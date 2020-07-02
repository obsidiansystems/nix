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

std::string renderLegacyContentAddress(LegacyContentAddress ca) {
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
             return "ipfs-git:"
                 + fsh.hash.to_string(Base32, true);
        }
    }, ca);
}

LegacyContentAddress parseLegacyContentAddress(std::string_view rawCa) {
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
        } else if (prefix == "ipfs-git") {
            Hash hash = Hash(rawCa.substr(prefixSeparator+1, string::npos));
            if (*hash.type != htSHA1)
                throw Error("the ipfs hash should have type SHA1");
            return IPFSHash { hash };
        } else {
            throw Error("format not recognized; has to be text or fixed");
        }
    } else {
        throw Error("Not a content address because it lacks an appropriate prefix");
    }
};

std::optional<LegacyContentAddress> parseLegacyContentAddressOpt(std::string_view rawCaOpt) {
    return rawCaOpt == "" ? std::optional<LegacyContentAddress> {} : parseLegacyContentAddress(rawCaOpt);
};

std::string renderLegacyContentAddress(std::optional<LegacyContentAddress> ca) {
    return ca ? renderLegacyContentAddress(*ca) : "";
}

std::string renderContentAddress(ContentAddress ca)
{
    return "full:" + ca.name + ":" + std::visit(overloaded {
        [](TextInfo th) {
            std::string result = "refs," + std::to_string(th.references.size());
            for (auto & i : th.references) {
                result += ":";
                result += i.to_string();
            }
            result += ":" + renderLegacyContentAddress(std::variant<TextHash, FixedOutputHash, IPFSHash> {TextHash {
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
            result += ":" + renderLegacyContentAddress(std::variant<TextHash, FixedOutputHash, IPFSHash> {FixedOutputHash {
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
            result += ":" + renderLegacyContentAddress(std::variant<TextHash, FixedOutputHash, IPFSHash> {IPFSHash {
                    .hash = fsh.hash
                }});
            return result;
        }
    }, ca.info);

}

ContentAddress parseContentAddress(std::string_view rawCa)
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
        LegacyContentAddress ca = parseLegacyContentAddress(rest);
        if (std::holds_alternative<TextHash>(ca)) {
            auto ca_ = std::get<TextHash>(ca);
            if (hasSelfReference)
                throw Error("text content address cannot have self reference");
            return ContentAddress {
                .name = name,
                .info = TextInfo {
                    {.hash = ca_.hash,},
                    .references = references,
                },
            };
        } else if (std::holds_alternative<FixedOutputHash>(ca)) {
            auto ca_ = std::get<FixedOutputHash>(ca);
            return ContentAddress {
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
            return ContentAddress {
                .name = name,
                .info = IPFSGitTreeReference {
                    .hash = ca_.hash,
                },
            };
        } else throw Error("unknown content address type");
    } else throw Error("unknown ca: '%s'", rawCa);
}

void to_json(nlohmann::json& j, const LegacyContentAddress & ca) {
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

void from_json(const nlohmann::json& j, LegacyContentAddress & ca) {
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

// f01781114 is the cid prefix for a base16 cbor sha1. This hash
// stores the ContentAddress information. The hash (without the cid
// prefix) will be put directly in the store path hash.

void to_json(nlohmann::json& j, const ContentAddress & ca)
{
    if (std::holds_alternative<IPFSInfo>(ca.info)) {
        auto info = std::get<IPFSInfo>(ca.info);

        // FIXME: ipfs sort order is weird, it always puts type before
        // references, so we rename it to qtype so it always comes
        // before references
        j = nlohmann::json {
            { "qtype", "ipfs" },
            { "name", ca.name },
            { "references", info.references },
            { "cid", "f01781114" + info.hash.to_string(Base16, false) }
        };
    } else throw Error("cannot convert to json");
}

void from_json(const nlohmann::json& j, ContentAddress & ca)
{
    std::string_view type = j.at("qtype").get<std::string_view>();
    if (type == "ipfs") {
        auto cid = j.at("cid").get<std::string_view>();
        if (cid.substr(0, 9) != "f01781114")
            throw Error("cid '%s' is wrong type for ipfs hash", cid);
        ca = ContentAddress {
            .name = j.at("name"),
            .info = IPFSInfo {
                Hash { cid.substr(9), htSHA1 },
                j.at("references").get<PathReferences<StorePath>>(),
            },
        };
    } else
        throw Error("invalid type: %s", type);
}

void to_json(nlohmann::json& j, const PathReferences<StorePath> & references)
{
    auto refs = nlohmann::json::array();
    for (auto & i : references.references) {
        Hash hash { i.hashPart(), htSHA1 };
        refs.push_back(nlohmann::json {
            { "name", i.name() },
            { "cid", "f01781114" + hash.to_string(Base16, false) }
        });
    }

    // FIXME: ipfs sort order is weird, it always puts references
    // before hasSelfReference, so we rename it to zhasSelfReferences
    // so it always comes after references

    j = nlohmann::json {
        { "zhasSelfReference", references.hasSelfReference },
        { "references", refs }
    };
}

void from_json(const nlohmann::json& j, PathReferences<StorePath> & references)
{
    StorePathSet refs;
    for (auto & ref : j.at("references")) {
        auto name = ref.at("name").get<std::string>();
        auto cid = ref.at("cid").get<std::string>();
        if (cid.substr(0, 9) != "f01781114")
            throw Error("cid '%s' is wrong type for ipfs hash", cid);
        Hash hash { cid.substr(9), htSHA1 };
        refs.insert(StorePath(hash, name));
    }
    references = PathReferences<StorePath> {
        .references = refs,
        .hasSelfReference = j.at("zhasSelfReference").get<bool>(),
    };
}

// Needed until https://github.com/nlohmann/json/pull/2117

void to_json(nlohmann::json& j, const std::optional<LegacyContentAddress> & c) {
    if (!c)
        j = nullptr;
    else
        to_json(j, *c);
}

void from_json(const nlohmann::json& j, std::optional<LegacyContentAddress> & c) {
    if (j.is_null())
        c = std::nullopt;
    else
        c = j.get<LegacyContentAddress>();
}

}
