#include <nlohmann/json.hpp>

#include "args.hh"
#include "content-address.hh"
#include "parser.hh"

namespace nix {

std::string FixedOutputHash::printMethodAlgo() const {
    return makeFileIngestionPrefix(method) + printHashType(hash.type);
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
             return "ipfs:"
                 + fsh.hash.to_string(Base32, true);
        }
    }, ca);
}

LegacyContentAddress parseLegacyContentAddress(std::string_view rawCa) {
    auto rest = rawCa;

    std::string_view prefix;
    {
        auto optPrefix = splitPrefixTo(rest, ':');
        if (!optPrefix)
            throw UsageError("not a content address because it is not in the form \"<prefix>:<rest>\": %s", rawCa);
        prefix = *optPrefix;
    }

    auto parseHashType_ = [&](){
        auto hashTypeRaw = splitPrefixTo(rest, ':');
        if (!hashTypeRaw)
            throw UsageError("content address hash must be in form \"<algo>:<hash>\", but found: %s", rawCa);
        HashType hashType = parseHashType(*hashTypeRaw);
        return std::move(hashType);
    };

    // Switch on prefix
    if (prefix == "text") {
        // No parsing of the method, "text" only support flat.
        HashType hashType = parseHashType_();
        if (hashType != htSHA256)
            throw Error("text content address hash should use %s, but instead uses %s",
                printHashType(htSHA256), printHashType(hashType));
        return TextHash {
            .hash = Hash::parseNonSRIUnprefixed(rest, std::move(hashType)),
        };
    } else if (prefix == "fixed") {
        // Parse method
        auto method = FileIngestionMethod::Flat;
        if (splitPrefix(rest, "r:"))
            method = FileIngestionMethod::Recursive;
        else if (splitPrefix(rest, "git:"))
            method = FileIngestionMethod::Git;
        HashType hashType = parseHashType_();
        return FixedOutputHash {
            .method = method,
            .hash = Hash::parseNonSRIUnprefixed(rest, std::move(hashType)),
        };
    } else if (prefix == "ipfs") {
        Hash hash = Hash::parseAnyPrefixed(rest);
        if (hash.type != htSHA256)
            throw Error("the ipfs hash should have type SHA256");
        return IPFSHash { hash };
    } else
        throw UsageError("content address prefix \"%s\" is unrecognized. Recogonized prefixes are \"text\" or \"fixed\"", prefix);
};

std::optional<LegacyContentAddress> parseLegacyContentAddressOpt(std::string_view rawCaOpt) {
    return rawCaOpt == "" ? std::optional<LegacyContentAddress> {} : parseLegacyContentAddress(rawCaOpt);
};

std::string renderLegacyContentAddress(std::optional<LegacyContentAddress> ca) {
    return ca ? renderLegacyContentAddress(*ca) : "";
}

std::string renderContentAddress(ContentAddress ca)
{
    return ca.name + ":" + std::visit(overloaded {
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
            std::string s = "";
            throw Error("ipfs info not handled");
            return s;
        },
        [](IPFSHash ic) {
            return renderLegacyContentAddress(std::variant<TextHash, FixedOutputHash, IPFSHash> { ic });
        }
    }, ca.info);

}

ContentAddress parseContentAddress(std::string_view rawCa)
{
    auto rest = rawCa;
    auto prefixSeparator = rest.find(':');
    if (prefixSeparator == string::npos)
        throw Error("unknown ca: '%s'", rawCa);
    auto name = std::string(rest.substr(0, prefixSeparator));
    rest = rest.substr(prefixSeparator + 1);

    if (hasPrefix(rest, "refs,")) {
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
                }
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
                }
            };
        } else throw Error("unknown content address type for '%s'", rawCa);
    } else {
        LegacyContentAddress ca = parseLegacyContentAddress(rest);
        if (std::holds_alternative<IPFSHash>(ca)) {
            auto hash = std::get<IPFSHash>(ca);
            return ContentAddress {
                .name = name,
                .info = hash
            };
        } else throw Error("unknown content address type for '%s'", rawCa);
    }
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
                { "algo", printHashType(foh.hash.type) },
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
            .hash = Hash::parseNonSRIUnprefixed(j.at("hash").get<std::string_view>(), htSHA256),
        };
    } else if (type == "fixed") {
        std::string_view methodRaw = j.at("method").get<std::string_view>();
        auto method = methodRaw == "flat" ? FileIngestionMethod::Flat
            : methodRaw == "recursive" ? FileIngestionMethod::Recursive
            : throw Error("invalid file ingestion method: %s", methodRaw);
        auto hashAlgo = parseHashType(j.at("algo").get<std::string>());
        ca = FixedOutputHash {
            .method = method,
            .hash = Hash::parseNonSRIUnprefixed(j.at("hash").get<std::string_view>(), hashAlgo),
        };
    } else
        throw Error("invalid type: %s", type);
}

// f01781114 is the cid prefix for a base16 cbor sha1. This hash
// stores the ContentAddress information.

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
            { "cid", nlohmann::json { { "/", "f01781114" + info.hash.to_string(Base16, false) } } }
        };
    } else throw Error("cannot convert to json");
}

void from_json(const nlohmann::json& j, ContentAddress & ca)
{
    std::string_view type = j.at("qtype").get<std::string_view>();
    if (type == "ipfs") {
        auto cid = j.at("cid").at("/").get<std::string_view>();
        if (cid.substr(0, 9) != "f01781114")
            throw Error("cid '%s' is wrong type for ipfs hash", cid);
        ca = ContentAddress {
            .name = j.at("name"),
            .info = IPFSInfo {
                Hash::parseAny(cid.substr(9), htSHA1),
                j.at("references").get<PathReferences<IPFSRef>>(),
            },
        };
    } else
        throw Error("invalid type: %s", type);
}

void to_json(nlohmann::json& j, const PathReferences<IPFSRef> & references)
{
    auto refs = nlohmann::json::array();
    for (auto & i : references.references) {
        refs.push_back(nlohmann::json {
            { "name", i.name },
            { "cid", nlohmann::json {{ "/", "f01711220" + i.hash.hash.to_string(Base16, false) }} }
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

void from_json(const nlohmann::json& j, PathReferences<IPFSRef> & references)
{
    std::set<IPFSRef> refs;
    for (auto & ref : j.at("references")) {
        auto cid = ref.at("cid").at("/").get<std::string>();
        refs.insert(IPFSRef {
                .name = ref.at("name").get<std::string>(),
                .hash = Hash::parseAny(std::string(cid, 9), htSHA256)
            });
    }
    references = PathReferences<IPFSRef> {
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
    if (j.is_null()) {
        c = std::nullopt;
    } else {
        // Dummy value to set tag bit.
        c = TextHash { .hash = Hash { htSHA256 } };
        from_json(j, *c);
    }
}

}
