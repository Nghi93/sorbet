#include "serialize.h"
#include "absl/base/casts.h"
#include "ast/Helpers.h"
#include "core/ErrorQueue.h"
#include "core/GlobalState.h"
#include "core/Symbols.h"
#include "lib/lizard_compress.h"
#include "lib/lizard_decompress.h"

template class std::vector<ruby_typer::u4>;

namespace ruby_typer {
namespace core {
namespace serialize {
const u4 Serializer::VERSION;
const u1 Serializer::GLOBAL_STATE_COMPRESSION_DEGREE;
const u1 Serializer::FILE_COMPRESSION_DEGREE;

void Serializer::Pickler::putStr(const absl::string_view s) {
    putU4(s.size());

    for (char c : s) {
        putU1(absl::bit_cast<u1>(c));
    }
}

constexpr size_t SIZE_BYTES = sizeof(int) / sizeof(u1);

std::vector<u1> Serializer::Pickler::result(int compressionDegree) {
    if (zeroCounter != 0) {
        data.emplace_back(zeroCounter);
        zeroCounter = 0;
    }
    const size_t max_dst_size = Lizard_compressBound(data.size());
    std::vector<u1> compressed_data;
    compressed_data.resize(2048 + max_dst_size); // give extra room for compression
                                                 // Lizard_compressBound returns size of data if compression
                                                 // succeeds. It seems to be written for big inputs
                                                 // and returns too small sizes for small inputs,
                                                 // where compressed size is bigger than original size
    int resultCode = Lizard_compress((const char *)data.data(), (char *)(compressed_data.data() + SIZE_BYTES * 2),
                                     data.size(), (compressed_data.size() - SIZE_BYTES * 2), compressionDegree);
    if (resultCode == 0) {
        // did not compress!
        Error::raise("uncompressable picker?");
    } else {
        memcpy(compressed_data.data(), &resultCode, SIZE_BYTES); // ~200K of our stdlib
        int uncompressedSize = data.size();
        memcpy(compressed_data.data() + SIZE_BYTES, &uncompressedSize,
               SIZE_BYTES);                                     // 172817 ints(x4), ~675K of our stdlib
        int actualCompressedSize = resultCode + SIZE_BYTES * 2; // SIZE_BYTES * 2 are for sizes
        compressed_data.resize(actualCompressedSize);
    }
    return compressed_data;
}

Serializer::UnPickler::UnPickler(const u1 *const compressed) : pos(0) {
    int compressedSize;
    memcpy(&compressedSize, compressed, SIZE_BYTES);
    int uncompressedSize;
    memcpy(&uncompressedSize, compressed + SIZE_BYTES, SIZE_BYTES);

    data.resize(uncompressedSize);

    int resultCode = Lizard_decompress_safe((const char *)(compressed + 2 * SIZE_BYTES), (char *)this->data.data(),
                                            compressedSize, uncompressedSize);
    if (resultCode != uncompressedSize) {
        Error::raise("incomplete decompression");
    }
}

absl::string_view Serializer::UnPickler::getStr() {
    int sz = getU4();
    absl::string_view result((char *)&data[pos], sz);
    pos += sz;

    return result;
}

void Serializer::Pickler::putU1(u1 u) {
    if (zeroCounter != 0) {
        data.push_back(zeroCounter);
        zeroCounter = 0;
    }
    data.push_back(u);
}

u1 Serializer::UnPickler::getU1() {
    ENFORCE(zeroCounter == 0);
    auto res = data[pos++];
    return res;
}

void Serializer::Pickler::putU4(u4 u) {
    if (u == 0) {
        if (zeroCounter != 0) {
            if (zeroCounter == UCHAR_MAX) {
                data.push_back(UCHAR_MAX);
                zeroCounter = 0;
                putU4(u);
                return;
            }
            zeroCounter++;
            return;
        } else {
            data.push_back(0);
            zeroCounter = 1;
        }
    } else {
        if (zeroCounter != 0) {
            data.push_back(zeroCounter);
            zeroCounter = 0;
        }
        while (u > 127) {
            data.push_back(128 | (u & 127));
            u = u >> 7;
        }
        data.push_back(u & 127);
    }
}

u4 Serializer::UnPickler::getU4() {
    if (zeroCounter != 0) {
        zeroCounter--;
        return 0;
    }
    u1 r = data[pos++];
    if (r == 0) {
        zeroCounter = data[pos++];
        zeroCounter--;
        return r;
    } else {
        u4 res = r & 127;
        u4 vle = r;
        if ((vle & 128) == 0) {
            goto done;
        }

        vle = data[pos++];
        res |= (vle & 127) << 7;
        if ((vle & 128) == 0) {
            goto done;
        }

        vle = data[pos++];
        res |= (vle & 127) << 14;
        if ((vle & 128) == 0) {
            goto done;
        }

        vle = data[pos++];
        res |= (vle & 127) << 21;
        if ((vle & 128) == 0) {
            goto done;
        }

        vle = data[pos++];
        res |= (vle & 127) << 28;
        if ((vle & 128) == 0) {
            goto done;
        }

    done:
        return res;
    }
}

void Serializer::Pickler::putS8(const int64_t i) {
    auto u = absl::bit_cast<u8>(i);
    while (u > 127) {
        putU1((u & 127) | 128);
        u = u >> 7;
    }
    putU1(u & 127);
}

int64_t Serializer::UnPickler::getS8() {
    u8 res = 0;
    u8 vle = 128;
    int i = 0;
    while (vle & 128) {
        vle = getU1();
        res |= (vle & 127) << (i * 7);
        i++;
    }
    return absl::bit_cast<int64_t>(res);
}

void Serializer::pickle(Pickler &p, const File &what) {
    p.putU1((u1)what.source_type);
    p.putStr(what.path());
    p.putStr(what.source());
}

std::shared_ptr<File> Serializer::unpickleFile(UnPickler &p) {
    auto t = (File::Type)p.getU1();
    auto path = (std::string)p.getStr();
    auto source = (std::string)p.getStr();
    return std::make_shared<File>(move(path), move(source), t);
}

void Serializer::pickle(Pickler &p, const Name &what) {
    p.putU1(what.kind);
    switch (what.kind) {
        case NameKind::UTF8:
            p.putStr(what.raw.utf8);
            break;
        case NameKind::UNIQUE:
            p.putU1(what.unique.uniqueNameKind);
            p.putU4(what.unique.original._id);
            p.putU4(what.unique.num);
            break;
        case NameKind::CONSTANT:
            p.putU4(what.cnst.original.id());
            break;
    }
}

Name Serializer::unpickleName(UnPickler &p, GlobalState &gs) {
    Name result;
    result.kind = (NameKind)p.getU1();
    switch (result.kind) {
        case NameKind::UTF8:
            result.kind = NameKind::UTF8;
            result.raw.utf8 = gs.enterString(p.getStr());
            break;
        case NameKind::UNIQUE:
            result.unique.uniqueNameKind = (UniqueNameKind)p.getU1();
            result.unique.original = NameRef(gs, p.getU4());
            result.unique.num = p.getU4();
            break;
        case NameKind::CONSTANT:
            result.cnst.original = NameRef(gs, p.getU4());
            break;
    }
    return result;
}

void Serializer::pickle(Pickler &p, Type *what) {
    if (what == nullptr) {
        p.putU4(0);
        return;
    }
    if (auto *c = cast_type<ClassType>(what)) {
        p.putU4(1);
        p.putU4(c->symbol._id);
    } else if (auto *o = cast_type<OrType>(what)) {
        p.putU4(2);
        pickle(p, o->left.get());
        pickle(p, o->right.get());
    } else if (auto *c = cast_type<LiteralType>(what)) {
        p.putU4(3);
        pickle(p, c->underlying.get());
        p.putS8(c->value);
    } else if (auto *a = cast_type<AndType>(what)) {
        p.putU4(4);
        pickle(p, a->left.get());
        pickle(p, a->right.get());
    } else if (auto *arr = cast_type<TupleType>(what)) {
        p.putU4(5);
        pickle(p, arr->underlying.get());
        p.putU4(arr->elems.size());
        for (auto &el : arr->elems) {
            pickle(p, el.get());
        }
    } else if (auto *hash = cast_type<ShapeType>(what)) {
        p.putU4(6);
        pickle(p, hash->underlying.get());
        p.putU4(hash->keys.size());
        ENFORCE(hash->keys.size() == hash->values.size());
        for (auto &el : hash->keys) {
            pickle(p, el.get());
        }
        for (auto &el : hash->values) {
            pickle(p, el.get());
        }
    } else if (auto *hash = cast_type<MagicType>(what)) {
        p.putU4(7);
    } else if (auto *alias = cast_type<AliasType>(what)) {
        p.putU4(8);
        p.putU4(alias->symbol._id);
    } else if (auto *lp = cast_type<LambdaParam>(what)) {
        p.putU4(9);
        p.putU4(lp->definition._id);
    } else if (auto *at = cast_type<AppliedType>(what)) {
        p.putU4(10);
        p.putU4(at->klass._id);
        p.putU4(at->targs.size());
        for (auto &t : at->targs) {
            pickle(p, t.get());
        }
    } else if (auto *tp = cast_type<TypeVar>(what)) {
        p.putU4(11);
        p.putU4(tp->sym._id);
    } else {
        Error::notImplemented();
    }
}

std::shared_ptr<Type> Serializer::unpickleType(UnPickler &p, GlobalState *gs) {
    auto tag = p.getU4(); // though we formally need only u1 here, benchmarks suggest that size difference after
                          // compression is small and u4 is 10% faster
    switch (tag) {
        case 0: {
            std::shared_ptr<Type> empty;
            return empty;
        }
        case 1:
            return std::make_shared<ClassType>(SymbolRef(gs, p.getU4()));
        case 2:
            return OrType::make_shared(unpickleType(p, gs), unpickleType(p, gs));
        case 3: {
            std::shared_ptr<LiteralType> result = std::make_shared<LiteralType>(true);
            result->underlying = unpickleType(p, gs);
            result->value = p.getS8();
            return result;
        }
        case 4:
            return AndType::make_shared(unpickleType(p, gs), unpickleType(p, gs));
        case 5: {
            auto underlying = unpickleType(p, gs);
            int sz = p.getU4();
            std::vector<std::shared_ptr<Type>> elems(sz);
            for (auto &elem : elems) {
                elem = unpickleType(p, gs);
            }
            auto result = std::make_shared<TupleType>(underlying, move(elems));
            return result;
        }
        case 6: {
            auto underlying = unpickleType(p, gs);
            int sz = p.getU4();
            std::vector<std::shared_ptr<LiteralType>> keys(sz);
            std::vector<std::shared_ptr<Type>> values(sz);
            for (auto &key : keys) {
                auto tmp = unpickleType(p, gs);
                if (auto *lit = cast_type<LiteralType>(tmp.get())) {
                    key = std::shared_ptr<LiteralType>(tmp, lit);
                }
            }
            for (auto &value : values) {
                value = unpickleType(p, gs);
            }
            auto result = std::make_shared<ShapeType>(keys, values);
            result->underlying = underlying;
            return result;
        }
        case 7:
            return std::make_shared<MagicType>();
        case 8:
            return std::make_shared<AliasType>(SymbolRef(gs, p.getU4()));
        case 9: {
            return std::make_shared<LambdaParam>(SymbolRef(gs, p.getU4()));
        }
        case 10: {
            SymbolRef klass(gs, p.getU4());
            int sz = p.getU4();
            std::vector<std::shared_ptr<Type>> targs(sz);
            for (auto &t : targs) {
                t = unpickleType(p, gs);
            }
            return std::make_shared<AppliedType>(klass, targs);
        }
        case 11: {
            SymbolRef sym(gs, p.getU4());
            return std::make_shared<TypeVar>(sym);
        }
        default:
            Error::raise("Uknown type tag {}", tag);
    }
}

void Serializer::pickle(Pickler &p, const Symbol &what) {
    p.putU4(what.owner._id);
    p.putU4(what.name._id);
    p.putU4(what.superClass._id);
    p.putU4(what.flags);
    p.putU4(what.uniqueCounter);
    p.putU4(what.argumentsOrMixins.size());
    for (SymbolRef s : what.argumentsOrMixins) {
        p.putU4(s._id);
    }
    p.putU4(what.typeParams.size());
    for (SymbolRef s : what.typeParams) {
        p.putU4(s._id);
    }
    p.putU4(what.members.size());
    for (auto member : what.members) {
        p.putU4(member.first.id());
        p.putU4(member.second._id);
    }

    pickle(p, what.resultType.get());
    p.putU4(what.definitionLoc.file.id());
    p.putU4(what.definitionLoc.begin_pos);
    p.putU4(what.definitionLoc.end_pos);
}

Symbol Serializer::unpickleSymbol(UnPickler &p, GlobalState *gs) {
    Symbol result;
    result.owner = SymbolRef(gs, p.getU4());
    result.name = NameRef(*gs, p.getU4());
    result.superClass = SymbolRef(gs, p.getU4());
    result.flags = p.getU4();
    result.uniqueCounter = p.getU4();
    int argumentsOrMixinsSize = p.getU4();
    result.argumentsOrMixins.reserve(argumentsOrMixinsSize);
    for (int i = 0; i < argumentsOrMixinsSize; i++) {
        result.argumentsOrMixins.emplace_back(SymbolRef(gs, p.getU4()));
    }
    int typeParamsSize = p.getU4();

    result.typeParams.reserve(typeParamsSize);
    for (int i = 0; i < typeParamsSize; i++) {
        result.typeParams.emplace_back(SymbolRef(gs, p.getU4()));
    }

    int membersSize = p.getU4();
    result.members.reserve(membersSize);
    for (int i = 0; i < membersSize; i++) {
        auto name = NameRef(*gs, p.getU4());
        auto sym = SymbolRef(gs, p.getU4());
        result.members.emplace_back(name, sym);
    }
    result.resultType = unpickleType(p, gs);

    result.definitionLoc.file = FileRef(p.getU4());
    result.definitionLoc.begin_pos = p.getU4();
    result.definitionLoc.end_pos = p.getU4();
    return result;
}

Serializer::Pickler Serializer::pickle(const GlobalState &gs) {
    Pickler result;
    result.putU4(VERSION);
    result.putU4(gs.files.size());
    int i = -1;
    for (auto &f : gs.files) {
        ++i;
        if (i != 0) {
            pickle(result, *f);
        }
    }

    result.putU4(gs.names.size());
    i = -1;
    for (const Name &n : gs.names) {
        ++i;
        if (i != 0) {
            pickle(result, n);
        }
    }

    result.putU4(gs.symbols.size());
    for (const Symbol &s : gs.symbols) {
        pickle(result, s);
    }

    result.putU4(gs.names_by_hash.size());
    for (const auto &s : gs.names_by_hash) {
        result.putU4(s.first);
        result.putU4(s.second);
    }
    return result;
}

int nearestPowerOf2(int from) {
    int i = 1;
    while (i < from) {
        i = i * 2;
    }
    return i;
}

void Serializer::unpickleGS(UnPickler &p, GlobalState &result) {
    if (p.getU4() != VERSION) {
        Error::raise("Payload version mismatch");
    }

    std::vector<std::shared_ptr<File>> files(move(result.files));
    files.clear();
    std::vector<Name> names(move(result.names));
    names.clear();
    std::vector<Symbol> symbols(move(result.symbols));
    symbols.clear();
    std::vector<std::pair<unsigned int, unsigned int>> names_by_hash(move(result.names_by_hash));
    names_by_hash.clear();

    result.trace("Reading files");

    int filesSize = p.getU4();
    files.reserve(filesSize);
    for (int i = 0; i < filesSize; i++) {
        if (i == 0) {
            files.emplace_back();
        } else {
            files.emplace_back(unpickleFile(p));
        }
    }

    result.trace("Reading names");

    int namesSize = p.getU4();
    ENFORCE(namesSize > 0);
    names.reserve(nearestPowerOf2(namesSize));
    for (int i = 0; i < namesSize; i++) {
        if (i == 0) {
            names.emplace_back();
            names.back().kind = NameKind::UTF8;
            names.back().raw.utf8 = absl::string_view();
        } else {
            names.emplace_back(unpickleName(p, result));
        }
    }

    result.trace("Reading symbols");

    int symbolSize = p.getU4();
    ENFORCE(symbolSize > 0);
    symbols.reserve(symbolSize);
    for (int i = 0; i < symbolSize; i++) {
        symbols.emplace_back(unpickleSymbol(p, &result));
    }

    result.trace("Reading name table");

    int namesByHashSize = p.getU4();
    names_by_hash.reserve(names.capacity() * 2);
    for (int i = 0; i < namesByHashSize; i++) {
        auto hash = p.getU4();
        auto value = p.getU4();
        names_by_hash.emplace_back(std::make_pair(hash, value));
    }
    result.trace("moving");
    result.files = move(files);
    result.names = move(names);
    result.symbols = move(symbols);
    result.names_by_hash = move(names_by_hash);
    result.sanityCheck();
}

std::vector<u1> Serializer::store(GlobalState &gs) {
    Pickler p = pickle(gs);
    return p.result(GLOBAL_STATE_COMPRESSION_DEGREE);
}

void Serializer::loadGlobalState(GlobalState &gs, const u1 *const data) {
    gs.trace("Starting global state deserialization");
    ENFORCE(gs.files.empty() && gs.names.empty() && gs.symbols.empty(), "Can't load into a non-empty state");
    UnPickler p(data);
    gs.trace("Decompression done");
    unpickleGS(p, gs);
    gs.trace("Done reading GS");
}

template <class T> void pickleTree(Serializer::Pickler &p, std::unique_ptr<T> &t) {
    T *raw = t.get();
    std::unique_ptr<ast::Expression> tmp(t.release());
    Serializer::pickle(p, tmp);
    t.reset(raw);
    tmp.release();
}

void pickleAstHeader(Serializer::Pickler &p, u1 tag, ast::Expression *tree) {
    p.putU1(tag);
    p.putU4(tree->loc.begin_pos);
    p.putU4(tree->loc.end_pos);
}

void Serializer::pickle(Pickler &p, const std::unique_ptr<ast::Expression> &what) {
    if (what == nullptr) {
        p.putU1(1);
        return;
    }

    typecase(what.get(),
             [&](ast::Send *s) {
                 pickleAstHeader(p, 2, s);
                 p.putU4(s->fun._id);
                 p.putU4(s->flags);
                 p.putU4(s->args.size());
                 pickle(p, s->recv);
                 pickleTree(p, s->block);
                 for (auto &arg : s->args) {
                     pickle(p, arg);
                 }
             },
             [&](ast::Block *a) {
                 pickleAstHeader(p, 3, a);
                 p.putU4(a->symbol._id);
                 p.putU4(a->args.size());
                 pickle(p, a->body);
                 for (auto &arg : a->args) {
                     pickle(p, arg);
                 };
             },
             [&](ast::Literal *a) {
                 pickleAstHeader(p, 4, a);
                 serialize::Serializer::pickle(p, a->value.get());
             },
             [&](ast::While *a) {
                 pickleAstHeader(p, 5, a);
                 pickle(p, a->cond);
                 pickle(p, a->body);
             },
             [&](ast::Return *a) {
                 pickleAstHeader(p, 6, a);
                 pickle(p, a->expr);
             },
             [&](ast::If *a) {
                 pickleAstHeader(p, 7, a);
                 pickle(p, a->cond);
                 pickle(p, a->thenp);
                 pickle(p, a->elsep);
             },

             [&](ast::ConstantLit *a) {
                 pickleAstHeader(p, 8, a);
                 p.putU4(a->cnst._id);
                 pickle(p, a->scope);
             },
             [&](ast::Ident *a) {
                 pickleAstHeader(p, 9, a);
                 p.putU4(a->symbol._id);
             },
             [&](ast::Local *a) {
                 pickleAstHeader(p, 10, a);
                 p.putU4(a->localVariable._name._id);
                 p.putU4(a->localVariable.unique);
             },
             [&](ast::Self *a) {
                 pickleAstHeader(p, 11, a);
                 p.putU4(a->claz._id);
             },
             [&](ast::Assign *a) {
                 pickleAstHeader(p, 12, a);
                 pickle(p, a->lhs);
                 pickle(p, a->rhs);
             },
             [&](ast::InsSeq *a) {
                 pickleAstHeader(p, 13, a);
                 p.putU4(a->stats.size());
                 pickle(p, a->expr);
                 for (auto &st : a->stats) {
                     pickle(p, st);
                 }
             },

             [&](ast::Next *a) {
                 pickleAstHeader(p, 14, a);
                 pickle(p, a->expr);
             },

             [&](ast::Break *a) {
                 pickleAstHeader(p, 15, a);
                 pickle(p, a->expr);
             },

             [&](ast::Retry *a) { pickleAstHeader(p, 16, a); },

             [&](ast::Hash *h) {
                 pickleAstHeader(p, 17, h);
                 ENFORCE(h->values.size() == h->keys.size());
                 p.putU4(h->values.size());
                 for (auto &v : h->values) {
                     pickle(p, v);
                 }
                 for (auto &k : h->keys) {
                     pickle(p, k);
                 }
             },

             [&](ast::Array *a) {
                 pickleAstHeader(p, 18, a);
                 p.putU4(a->elems.size());
                 for (auto &e : a->elems) {
                     pickle(p, e);
                 }
             },

             [&](ast::Cast *c) {
                 pickleAstHeader(p, 19, c);
                 p.putU4(c->cast._id);
                 serialize::Serializer::pickle(p, c->type.get());
                 pickle(p, c->arg);
             },

             [&](ast::EmptyTree *n) { pickleAstHeader(p, 20, n); },
             [&](ast::ClassDef *c) {
                 pickleAstHeader(p, 21, c);
                 p.putU1(c->kind);
                 p.putU4(c->symbol._id);
                 p.putU4(c->ancestors.size());
                 p.putU4(c->singleton_ancestors.size());
                 p.putU4(c->rhs.size());
                 pickle(p, c->name);
                 for (auto &anc : c->ancestors) {
                     pickle(p, anc);
                 }
                 for (auto &anc : c->singleton_ancestors) {
                     pickle(p, anc);
                 }
                 for (auto &anc : c->rhs) {
                     pickle(p, anc);
                 }
             },
             [&](ast::MethodDef *c) {
                 pickleAstHeader(p, 22, c);
                 p.putU1(c->isSelf);
                 p.putU4(c->name._id);
                 p.putU4(c->symbol._id);
                 p.putU4(c->args.size());
                 pickle(p, c->rhs);
                 for (auto &a : c->args) {
                     pickle(p, a);
                 }
             },
             [&](ast::Rescue *a) {
                 pickleAstHeader(p, 23, a);
                 p.putU4(a->rescueCases.size());
                 pickle(p, a->ensure);
                 pickle(p, a->else_);
                 pickle(p, a->body);
                 for (auto &rc : a->rescueCases) {
                     pickleTree(p, rc);
                 }
             },
             [&](ast::RescueCase *a) {
                 pickleAstHeader(p, 24, a);
                 p.putU4(a->exceptions.size());
                 pickle(p, a->var);
                 pickle(p, a->body);
                 for (auto &ex : a->exceptions) {
                     pickle(p, ex);
                 }
             },
             [&](ast::RestArg *a) {
                 pickleAstHeader(p, 25, a);
                 pickleTree(p, a->expr);
             },
             [&](ast::KeywordArg *a) {
                 pickleAstHeader(p, 26, a);
                 pickleTree(p, a->expr);
             },
             [&](ast::ShadowArg *a) {
                 pickleAstHeader(p, 27, a);
                 pickleTree(p, a->expr);
             },
             [&](ast::BlockArg *a) {
                 pickleAstHeader(p, 28, a);
                 pickleTree(p, a->expr);
             },
             [&](ast::OptionalArg *a) {
                 pickleAstHeader(p, 29, a);
                 pickleTree(p, a->expr);
                 pickle(p, a->default_);
             },
             [&](ast::Yield *a) {
                 pickleAstHeader(p, 30, a);
                 pickle(p, a->expr);
             },
             [&](ast::ZSuperArgs *a) { pickleAstHeader(p, 31, a); },
             [&](ast::HashSplat *a) {
                 pickleAstHeader(p, 32, a);
                 pickle(p, a->arg);
             },
             [&](ast::ArraySplat *a) {
                 pickleAstHeader(p, 33, a);
                 pickle(p, a->arg);
             },
             [&](ast::ConstDef *a) {
                 pickleAstHeader(p, 34, a);
                 p.putU4(a->symbol._id);
                 pickle(p, a->rhs);
             },
             [&](ast::UnresolvedIdent *a) {
                 pickleAstHeader(p, 35, a);
                 p.putU1((int)a->kind);
                 p.putU4(a->name._id);
             },

             [&](ast::Expression *n) { Error::raise("Unimplemented AST Node: ", n->nodeName()); });
}

std::unique_ptr<ast::Expression> Serializer::unpickleExpr(serialize::Serializer::UnPickler &p, GlobalState &gs,
                                                          FileRef file) {
    u1 kind = p.getU1();
    if (kind == 1) {
        return nullptr;
    }
    core::Loc loc;
    loc.file = file;
    loc.begin_pos = p.getU4();
    loc.end_pos = p.getU4();

    switch (kind) {
        case 2: {
            NameRef fun = unpickleNameRef(p, gs);
            auto flags = p.getU4();
            auto argsSize = p.getU4();
            auto recv = unpickleExpr(p, gs, file);
            auto blkt = unpickleExpr(p, gs, file);
            std::unique_ptr<ast::Block> blk;
            if (blkt) {
                blk.reset(static_cast<ast::Block *>(blkt.release()));
            }
            ast::Send::ARGS_store store(argsSize);
            for (auto &expr : store) {
                expr = unpickleExpr(p, gs, file);
            }
            return ast::MK::Send(loc, move(recv), fun, move(store), flags, move(blk));
        }
        case 3: {
            SymbolRef sym(gs, p.getU4());
            auto argsSize = p.getU4();
            auto body = unpickleExpr(p, gs, file);
            ast::MethodDef::ARGS_store args(argsSize);
            for (auto &arg : args) {
                arg = unpickleExpr(p, gs, file);
            }
            return ast::MK::Block(loc, move(body), sym, move(args));
        }
        case 4: {
            auto tpe = unpickleType(p, &gs);
            return ast::MK::Literal(loc, tpe);
        }
        case 5: {
            auto cond = unpickleExpr(p, gs, file);
            auto body = unpickleExpr(p, gs, file);
            return ast::MK::While(loc, move(cond), move(body));
        }
        case 6: {
            auto expr = unpickleExpr(p, gs, file);
            return ast::MK::Return(loc, move(expr));
        }
        case 7: {
            auto cond = unpickleExpr(p, gs, file);
            auto thenp = unpickleExpr(p, gs, file);
            auto elsep = unpickleExpr(p, gs, file);
            return ast::MK::If(loc, move(cond), move(thenp), move(elsep));
        }
        case 8: {
            NameRef cnst = unpickleNameRef(p, gs);
            auto scope = unpickleExpr(p, gs, file);
            return ast::MK::Constant(loc, move(scope), cnst);
        }
        case 9: {
            SymbolRef sym(gs, p.getU4());
            return ast::MK::Ident(loc, sym);
        }
        case 10: {
            NameRef nm = unpickleNameRef(p, gs);
            auto unique = p.getU4();
            core::LocalVariable lv(nm, unique);
            return std::make_unique<ast::Local>(loc, lv);
        }
        case 11: {
            SymbolRef clazId(gs, p.getU4());
            return std::make_unique<ast::Self>(loc, clazId);
        }
        case 12: {
            auto lhs = unpickleExpr(p, gs, file);
            auto rhs = unpickleExpr(p, gs, file);
            return ast::MK::Assign(loc, move(lhs), move(rhs));
        }
        case 13: {
            auto insSize = p.getU4();
            auto expr = unpickleExpr(p, gs, file);
            ast::InsSeq::STATS_store stats(insSize);
            for (auto &stat : stats) {
                stat = unpickleExpr(p, gs, file);
            }
            return ast::MK::InsSeq(loc, move(stats), move(expr));
        }
        case 14: {
            auto expr = unpickleExpr(p, gs, file);
            return ast::MK::Next(loc, move(expr));
        }
        case 15: {
            auto expr = unpickleExpr(p, gs, file);
            return ast::MK::Break(loc, move(expr));
        }
        case 16: {
            return std::make_unique<ast::Retry>(loc);
        }
        case 17: {
            auto sz = p.getU4();
            ast::Hash::ENTRY_store keys(sz);
            ast::Hash::ENTRY_store values(sz);
            for (auto &value : values) {
                value = unpickleExpr(p, gs, file);
            }
            for (auto &key : keys) {
                key = unpickleExpr(p, gs, file);
            }
            return ast::MK::Hash(loc, move(keys), move(values));
        }
        case 18: {
            auto sz = p.getU4();
            ast::Array::ENTRY_store elems(sz);
            for (auto &elem : elems) {
                elem = unpickleExpr(p, gs, file);
            }
            return ast::MK::Array(loc, move(elems));
        }
        case 19: {
            NameRef kind(gs, p.getU4());
            auto type = unpickleType(p, &gs);
            auto arg = unpickleExpr(p, gs, file);
            return std::make_unique<ast::Cast>(loc, move(type), move(arg), kind);
        }
        case 20: {
            return ast::MK::EmptyTree(loc);
        }
        case 21: {
            auto kind = p.getU1();
            SymbolRef symbol(gs, p.getU4());
            auto ancestorsSize = p.getU4();
            auto singletonAncestorsSize = p.getU4();
            auto rhsSize = p.getU4();
            auto name = unpickleExpr(p, gs, file);
            ast::ClassDef::ANCESTORS_store ancestors(ancestorsSize);
            for (auto &anc : ancestors) {
                anc = unpickleExpr(p, gs, file);
            }
            ast::ClassDef::ANCESTORS_store singletonAncestors(singletonAncestorsSize);
            for (auto &sanc : singletonAncestors) {
                sanc = unpickleExpr(p, gs, file);
            }
            ast::ClassDef::RHS_store rhs(rhsSize);
            for (auto &r : rhs) {
                r = unpickleExpr(p, gs, file);
            }
            auto ret = ast::MK::Class(loc, move(name), move(ancestors), move(rhs), (ast::ClassDefKind)kind);
            ret->singleton_ancestors = move(singletonAncestors);
            ret->symbol = symbol;
            return ret;
        }
        case 22: {
            auto isSelf = p.getU1();
            NameRef name = unpickleNameRef(p, gs);
            SymbolRef symbol(gs, p.getU4());
            auto argsSize = p.getU4();
            auto rhs = unpickleExpr(p, gs, file);
            ast::MethodDef::ARGS_store args(argsSize);
            for (auto &arg : args) {
                arg = unpickleExpr(p, gs, file);
            }
            auto ret = ast::MK::Method(loc, name, move(args), move(rhs), isSelf);
            ret->symbol = symbol;
            return ret;
        }
        case 23: {
            auto rescueCasesSize = p.getU4();
            auto ensure = unpickleExpr(p, gs, file);
            auto else_ = unpickleExpr(p, gs, file);
            auto body_ = unpickleExpr(p, gs, file);
            ast::Rescue::RESCUE_CASE_store cases(rescueCasesSize);
            for (auto &case_ : cases) {
                auto t = unpickleExpr(p, gs, file);
                case_.reset(static_cast<ast::RescueCase *>(t.release()));
            }
            return std::make_unique<ast::Rescue>(loc, move(body_), move(cases), move(else_), move(ensure));
        }
        case 24: {
            auto exceptionsSize = p.getU4();
            auto var = unpickleExpr(p, gs, file);
            auto body = unpickleExpr(p, gs, file);
            ast::RescueCase::EXCEPTION_store exceptions(exceptionsSize);
            for (auto &ex : exceptions) {
                ex = unpickleExpr(p, gs, file);
            }
            return std::make_unique<ast::RescueCase>(loc, move(exceptions), move(var), move(body));
        }
        case 25: {
            auto tmp = unpickleExpr(p, gs, file);
            std::unique_ptr<ast::Reference> ref(static_cast<ast::Reference *>(tmp.release()));
            return std::make_unique<ast::RestArg>(loc, move(ref));
        }
        case 26: {
            auto tmp = unpickleExpr(p, gs, file);
            std::unique_ptr<ast::Reference> ref(static_cast<ast::Reference *>(tmp.release()));
            return std::make_unique<ast::KeywordArg>(loc, move(ref));
        }
        case 27: {
            auto tmp = unpickleExpr(p, gs, file);
            std::unique_ptr<ast::Reference> ref(static_cast<ast::Reference *>(tmp.release()));
            return std::make_unique<ast::ShadowArg>(loc, move(ref));
        }
        case 28: {
            auto tmp = unpickleExpr(p, gs, file);
            std::unique_ptr<ast::Reference> ref(static_cast<ast::Reference *>(tmp.release()));
            return std::make_unique<ast::BlockArg>(loc, move(ref));
        }
        case 29: {
            auto tmp = unpickleExpr(p, gs, file);
            std::unique_ptr<ast::Reference> ref(static_cast<ast::Reference *>(tmp.release()));
            auto default_ = unpickleExpr(p, gs, file);
            return std::make_unique<ast::OptionalArg>(loc, move(ref), move(default_));
        }
        case 30: {
            auto expr = unpickleExpr(p, gs, file);
            return ast::MK::Yield(loc, move(expr));
        }
        case 31: {
            return std::make_unique<ast::ZSuperArgs>(loc);
        }
        case 32: {
            auto expr = unpickleExpr(p, gs, file);
            return std::make_unique<ast::HashSplat>(loc, move(expr));
        }
        case 33: {
            auto expr = unpickleExpr(p, gs, file);
            return std::make_unique<ast::ArraySplat>(loc, move(expr));
        }
        case 34: {
            SymbolRef symbol(gs, p.getU4());
            auto rhs = unpickleExpr(p, gs, file);
            return std::make_unique<ast::ConstDef>(loc, symbol, move(rhs));
        }
        case 35: {
            auto kind = (ast::UnresolvedIdent::VarKind)p.getU1();
            NameRef name = unpickleNameRef(p, gs);
            return std::make_unique<ast::UnresolvedIdent>(loc, kind, name);
        }
    }
    Error::raise("Not handled {}", kind);
}

std::unique_ptr<ast::Expression> Serializer::loadExpression(GlobalState &gs, const u1 *const p) {
    serialize::Serializer::UnPickler up(p);
    FileRef fileId(up.getU4());
    return unpickleExpr(up, gs, fileId);
}

std::vector<u1> Serializer::store(GlobalState &gs, std::unique_ptr<ast::Expression> &e) {
    serialize::Serializer::Pickler pickler;
    pickler.putU4(e->loc.file.id());
    pickle(pickler, e);
    return pickler.result(FILE_COMPRESSION_DEGREE);
}

NameRef Serializer::unpickleNameRef(Serializer::UnPickler &p, GlobalState &gs) {
    NameRef name(NameRef::WellKnown{}, p.getU4());
    ENFORCE(name.data(gs).ref(gs) == name);
    return name;
}
} // namespace serialize

} // namespace core
} // namespace ruby_typer
