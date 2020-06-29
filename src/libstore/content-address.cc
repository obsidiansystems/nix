#include "content-address.hh"

namespace nix {

std::string FixedOutputHash::printMethodAlgo() const {
    return makeFileIngestionPrefix(method) + printHashType(*hash.type);
}

std::string makeFileIngestionPrefix(const FileIngestionMethod m) {
    switch (m) {
    case FileIngestionMethod::Flat:
        return "";
    case FileIngestionMethod::Recursive:
        return "r:";
    default:
        throw Error("impossible, caught both cases");
    }
}

// FIXME Put this somewhere?
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

std::string renderMiniContentAddress(MiniContentAddress ca) {
    return std::visit(overloaded {
        [](TextHash th) {
            return "text:"
                + th.hash.to_string(Base32, true);
        },
        [](FixedOutputHash fsh) {
             return "fixed:"
                 + makeFileIngestionPrefix(fsh.method)
                 + fsh.hash.to_string(Base32, true);
        }
    }, ca);
}

MiniContentAddress parseMiniContentAddress(std::string_view rawCa) {
    auto prefixSeparator = rawCa.find(':');
    if (prefixSeparator != string::npos) {
        auto prefix = string(rawCa, 0, prefixSeparator);
        if (prefix == "text") {
            auto hashTypeAndHash = rawCa.substr(prefixSeparator+1, string::npos);
            Hash hash = Hash(string(hashTypeAndHash));
            if (*hash.type != htSHA256) {
                throw Error("parseMiniContentAddress: the text hash should have type SHA256");
            }
            return TextHash { hash };
        } else if (prefix == "fixed") {
            auto methodAndHash = rawCa.substr(prefixSeparator+1, string::npos);
            if (methodAndHash.substr(0,2) == "r:") {
                std::string_view hashRaw = methodAndHash.substr(2,string::npos);
                return FixedOutputHash {
                    .method = FileIngestionMethod::Recursive,
                    .hash = Hash(string(hashRaw)),
                };
            } else {
                std::string_view hashRaw = methodAndHash;
                return FixedOutputHash {
                    .method = FileIngestionMethod::Flat,
                    .hash = Hash(string(hashRaw)),
                };
            }
        } else {
            throw Error("parseMiniContentAddress: format not recognized; has to be text or fixed");
        }
    } else {
        throw Error("Not a content address because it lacks an appropriate prefix");
    }
};

std::optional<MiniContentAddress> parseMiniContentAddressOpt(std::string_view rawCaOpt) {
    return rawCaOpt == "" ? std::optional<MiniContentAddress> {} : parseMiniContentAddress(rawCaOpt);
};

std::string renderMiniContentAddress(std::optional<MiniContentAddress> ca) {
    return ca ? renderMiniContentAddress(*ca) : "";
}

std::string renderFullContentAddress(FullContentAddress ca)
{
    return "full:" + ca.name + ":" + std::visit(overloaded {
        [](TextInfo th) {
            std::string result = "refs," + std::to_string(th.references.size());
            for (auto & i : th.references) {
                result += ":";
                result += i.to_string();
            }
            result += ":" + renderMiniContentAddress(std::variant<TextHash, FixedOutputHash> {TextHash {
                    .hash = th.hash,
                }});
            return result;
        },
        [](FixedOutputInfo fsh) {
            std::string result = "refs," + std::to_string(fsh.references.references.size() + (fsh.references.hasSelfReference ? 1 : 0));
            for (auto & i : fsh.references.references) {
                result += ":";
                result += i.to_string();
            }
            if (fsh.references.hasSelfReference) result += ":self";
            result += ":" + renderMiniContentAddress(std::variant<TextHash, FixedOutputHash> {FixedOutputHash {
                    .method = fsh.method,
                    .hash = fsh.hash
                }});
            return result;
        }
    }, ca.info);

}

FullContentAddress parseFullContentAddress(std::string_view rawCa)
{
    auto prefixSeparator = rawCa.find(':');
    if (prefixSeparator != string::npos)
        throw Error("unknown ca: '%s'", rawCa);
    auto rest = rawCa.substr(prefixSeparator + 1);

    if (hasPrefix(rawCa, "full:")) {
        prefixSeparator = rest.find(':');
        auto name = std::string(rest.substr(0, prefixSeparator));
        rest = rest.substr(prefixSeparator + 1);

        if (!hasPrefix(rawCa, "refs,"))
            throw Error("could not parse ca ''", rawCa);
        prefixSeparator = rest.find(':');
        if (prefixSeparator == string::npos)
            throw Error("unknown ca: '%s'", rawCa);
        auto numReferences = std::stoi(std::string(rest.substr(5, prefixSeparator)));
        rest = rest.substr(prefixSeparator + 1);

        bool hasSelfReference = false;
        StorePathSet references;
        for (int i = 0; i < numReferences; i++) {
            prefixSeparator = rest.find(':');
            if (prefixSeparator == string::npos)
                throw Error("unexpected end of string in '%s'", rest);
            auto s = std::string(rest, 0, prefixSeparator);
            if (s == "self")
                hasSelfReference = true;
            else
                references.insert(StorePath(s));
            rest = rest.substr(prefixSeparator + 1);
        }
        MiniContentAddress ca = parseMiniContentAddress(rest);
        if (std::holds_alternative<TextHash>(ca)) {
            auto ca_ = std::get<TextHash>(ca);
            if (hasSelfReference)
                throw Error("text content address cannot have self reference");
            return FullContentAddress {
                .name = name,
                .info = TextInfo {
                    {.hash = ca_.hash,},
                    .references = references,
                },
            };
        } else if (std::holds_alternative<FixedOutputHash>(ca)) {
            auto ca_ = std::get<FixedOutputHash>(ca);
            return FullContentAddress {
                .name = name,
                .info = FixedOutputInfo {
                    {.method = ca_.method,
                     .hash = ca_.hash,},
                    .references = PathReferences<StorePath> {
                        .hasSelfReference = hasSelfReference,
                        .references = references,
                    },
                },
            };
        } else throw Error("unknown content address type");
    } else throw Error("unknown ca: '%s'", rawCa);
}

}
