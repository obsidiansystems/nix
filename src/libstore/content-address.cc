#include <nlohmann/json.hpp>

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
    case FileIngestionMethod::Git:
        return "git:";
    }
    abort();
}

// FIXME Put this somewhere?
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

std::string renderContentAddress(ContentAddress ca) {
    return std::visit(overloaded {
        [](TextHash th) {
            return "text:"
                + th.hash.to_string(Base32, true);
        },
        [](FixedOutputHash fsh) {
             return "fixed:"
                 + makeFileIngestionPrefix(fsh.method)
                 + fsh.hash.to_string(Base32, true);
        },
        [](IPFSHash fsh) {
             return "ipfs:"
                 + fsh.hash.to_string(Base32, true);
        }
    }, ca);
}

ContentAddress parseContentAddress(std::string_view rawCa) {
    auto prefixSeparator = rawCa.find(':');
    if (prefixSeparator != string::npos) {
        auto prefix = string(rawCa, 0, prefixSeparator);
        if (prefix == "text") {
            auto hashTypeAndHash = rawCa.substr(prefixSeparator+1, string::npos);
            Hash hash = Hash(string(hashTypeAndHash));
            if (*hash.type != htSHA256) {
                throw Error("the text hash should have type SHA256");
            }
            return TextHash { hash };
        } else if (prefix == "fixed") {
            auto methodAndHash = rawCa.substr(prefixSeparator+1, string::npos);
            if (methodAndHash.substr(0, 2) == "r:") {
                std::string_view hashRaw = methodAndHash.substr(2, string::npos);
                return FixedOutputHash {
                    .method = FileIngestionMethod::Recursive,
                    .hash = Hash(string(hashRaw)),
                };
            } else if (methodAndHash.substr(0, 4) == "git:") {
                std::string_view hashRaw = methodAndHash.substr(4, string::npos);
                return FixedOutputHash {
                    .method = FileIngestionMethod::Git,
                    .hash = Hash(string(hashRaw)),
                };
            } else {
                std::string_view hashRaw = methodAndHash;
                return FixedOutputHash {
                    .method = FileIngestionMethod::Flat,
                    .hash = Hash(string(hashRaw)),
                };
            }
        } else if (prefix == "ipfs") {
            Hash hash = Hash(rawCa.substr(prefixSeparator+1, string::npos));
            if (*hash.type != htSHA256)
                throw Error("the text hash should have type SHA256");
            return IPFSHash { hash };
        } else {
            throw Error("format not recognized; has to be text or fixed");
        }
    } else {
        throw Error("Not a content address because it lacks an appropriate prefix");
    }
};

std::optional<ContentAddress> parseContentAddressOpt(std::string_view rawCaOpt) {
    return rawCaOpt == "" ? std::optional<ContentAddress> {} : parseContentAddress(rawCaOpt);
};

std::string renderContentAddress(std::optional<ContentAddress> ca) {
    return ca ? renderContentAddress(*ca) : "";
}

std::string renderContentAddressWithNameAndReferences(ContentAddressWithNameAndReferences ca)
{
    return "full:" + ca.name + ":" + std::visit(overloaded {
        [](TextInfo th) {
            std::string result = "refs," + std::to_string(th.references.size());
            for (auto & i : th.references) {
                result += ":";
                result += i.to_string();
            }
            result += ":" + renderContentAddress(std::variant<TextHash, FixedOutputHash, IPFSHash> {TextHash {
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
            result += ":" + renderContentAddress(std::variant<TextHash, FixedOutputHash, IPFSHash> {FixedOutputHash {
                    .method = fsh.method,
                    .hash = fsh.hash
                }});
            return result;
        },
        [](IPFSInfo fsh) {
            std::string result = "refs," + std::to_string(fsh.references.references.size() + (fsh.references.hasSelfReference ? 1 : 0));
            for (auto & i : fsh.references.references) {
                result += ":";
                result += i.to_string();
            }
            if (fsh.references.hasSelfReference) result += ":self";
            result += ":" + renderContentAddress(std::variant<TextHash, FixedOutputHash, IPFSHash> {IPFSHash {
                    .hash = fsh.hash
                }});
            return result;
        }
    }, ca.info);

}

ContentAddressWithNameAndReferences parseContentAddressWithNameAndReferences(std::string_view rawCa)
{
    auto prefixSeparator = rawCa.find(':');
    if (prefixSeparator == string::npos)
        throw Error("unknown ca: '%s'", rawCa);
    auto rest = rawCa.substr(prefixSeparator + 1);

    if (hasPrefix(rawCa, "full:")) {
        prefixSeparator = rest.find(':');
        auto name = std::string(rest.substr(0, prefixSeparator));
        rest = rest.substr(prefixSeparator + 1);

        if (!hasPrefix(rest, "refs,"))
            throw Error("could not parse ca '%s'", rawCa);
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
        ContentAddress ca = parseContentAddress(rest);
        if (std::holds_alternative<TextHash>(ca)) {
            auto ca_ = std::get<TextHash>(ca);
            if (hasSelfReference)
                throw Error("text content address cannot have self reference");
            return ContentAddressWithNameAndReferences {
                .name = name,
                .info = TextInfo {
                    {.hash = ca_.hash,},
                    .references = references,
                },
            };
        } else if (std::holds_alternative<FixedOutputHash>(ca)) {
            auto ca_ = std::get<FixedOutputHash>(ca);
            return ContentAddressWithNameAndReferences {
                .name = name,
                .info = FixedOutputInfo {
                    {.method = ca_.method,
                     .hash = ca_.hash,},
                    .references = PathReferences<StorePath> {
                        .references = references,
                        .hasSelfReference = hasSelfReference,
                    },
                },
            };
        } else if (std::holds_alternative<IPFSHash>(ca)) {
            auto ca_ = std::get<IPFSHash>(ca);
            return ContentAddressWithNameAndReferences {
                .name = name,
                .info = IPFSInfo {
                    {.hash = ca_.hash,},
                    .references = PathReferences<StorePath> {
                        .references = references,
                        .hasSelfReference = hasSelfReference,
                    },
                },
            };
        } else throw Error("unknown content address type");
    } else throw Error("unknown ca: '%s'", rawCa);
}

void to_json(nlohmann::json& j, const ContentAddress & ca) {
    j = std::visit(overloaded {
        [](TextHash th) {
            return nlohmann::json {
                { "type", "text" },
                { "hash", th.hash.to_string(Base32, false) },
            };
        },
        [](FixedOutputHash foh) {
            return nlohmann::json {
                { "type", "fixed" },
                { "method", foh.method == FileIngestionMethod::Flat ? "flat" : "recursive" },
                { "algo", printHashType(*foh.hash.type) },
                { "hash", foh.hash.to_string(Base32, false) },
            };
        },
        [](IPFSHash foh) {
            return nlohmann::json {
                { "type", "ipfs" },
                { "hash", foh.hash.to_string(Base32, false) },
            };
        }
    }, ca);
}

void from_json(const nlohmann::json& j, ContentAddress & ca) {
    std::string_view type = j.at("type").get<std::string_view>();
    if (type == "text") {
        ca = TextHash {
            .hash = Hash { j.at("hash").get<std::string_view>(), htSHA256 },
        };
    } else if (type == "fixed") {
        std::string_view methodRaw = j.at("method").get<std::string_view>();
        auto method = methodRaw == "flat" ? FileIngestionMethod::Flat
            : methodRaw == "recursive" ? FileIngestionMethod::Recursive
            : throw Error("invalid file ingestion method: %s", methodRaw);
        auto hashAlgo = parseHashType(j.at("algo").get<std::string>());
        ca = FixedOutputHash {
            .method = method,
            .hash = Hash { j.at("hash").get<std::string_view>(), hashAlgo },
        };
    } else
        throw Error("invalid type: %s", type);
}

// Needed until https://github.com/nlohmann/json/pull/2117

void to_json(nlohmann::json& j, const std::optional<ContentAddress> & c) {
    if (!c)
        j = nullptr;
    else
        to_json(j, *c);
}

void from_json(const nlohmann::json& j, std::optional<ContentAddress> & c) {
    if (j.is_null())
        c = std::nullopt;
    else
        c = j.get<ContentAddress>();
}

}
