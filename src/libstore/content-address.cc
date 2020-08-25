#include <nlohmann/json.hpp>

#include "args.hh"
#include "content-address.hh"
#include "split.hh"


namespace nix {

std::string FixedOutputHash::printMethodAlgo() const {
    return makeFileIngestionPrefix(method) + printHashType(hash.type);
}


std::string makeFileIngestionPrefix(FileIngestionMethod m) {
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

std::string makeContentAddressingPrefix(ContentAddressingMethod m) {
    return std::visit(overloaded {
        [](IsText _) -> std::string { return "text:"; },
        [](FileIngestionMethod m2) {
             /* Not prefixed for back compat with things that couldn't produce text before. */
            return makeFileIngestionPrefix(m2);
        },
        [](IsIPFS _) -> std::string { return "ipfs:"; },
    }, m);
}


std::string makeFixedOutputCA(FileIngestionMethod method, const Hash & hash)
{
    return "fixed:"
        + makeFileIngestionPrefix(method)
        + hash.to_string(Base32, true);
}

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
        [](IPFSHash ih) {
            // FIXME do Base-32 for consistency
            return "ipfs:" + ih.to_string();
        }
    }, ca);
}

static HashType parseHashType_(std::string_view & rest) {
    auto hashTypeRaw = splitPrefixTo(rest, ':');
    if (!hashTypeRaw)
        throw UsageError("hash must be in form \"<algo>:<hash>\", but found: %s", rest);
    return parseHashType(*hashTypeRaw);
};

static FileIngestionMethod parseFileIngestionMethod_(std::string_view & rest) {
    if (splitPrefix(rest, "r:"))
        return FileIngestionMethod::Recursive;
    else if (splitPrefix(rest, "git:"))
        return FileIngestionMethod::Git;
    return FileIngestionMethod::Flat;
}

ContentAddress parseContentAddress(std::string_view rawCa) {
    auto rest = rawCa;

    std::string_view prefix;
    {
        auto optPrefix = splitPrefixTo(rest, ':');
        if (!optPrefix)
            throw UsageError("not a path-info content address because it is not in the form \"<prefix>:<rest>\": %s", rawCa);
        prefix = *optPrefix;
    }

    // Switch on prefix
    if (prefix == "text") {
        // No parsing of the method, "text" only support flat.
        HashType hashType = parseHashType_(rest);
        if (hashType != htSHA256)
            throw Error("text content address hash should use %s, but instead uses %s",
                printHashType(htSHA256), printHashType(hashType));
        return TextHash {
            .hash = Hash::parseNonSRIUnprefixed(rest, std::move(hashType)),
        };
    } else if (prefix == "fixed") {
        auto method = parseFileIngestionMethod_(rest);
        HashType hashType = parseHashType_(rest);
        return FixedOutputHash {
            .method = method,
            .hash = Hash::parseNonSRIUnprefixed(rest, std::move(hashType)),
        };
    } else if (prefix == "ipfs") {
        auto hash = IPFSHash::from_string(rest);
        if (hash.hash.type != htSHA256)
            throw Error("This IPFS hash should have type SHA-256: %s", hash.to_string());
        return hash;
    } else
        throw UsageError("path-info content address prefix \"%s\" is unrecognized. Recogonized prefixes are \"text\", \"fixed\", or \"ipfs\"", prefix);
};

std::optional<ContentAddress> parseContentAddressOpt(std::string_view rawCaOpt) {
    return rawCaOpt == "" ? std::optional<ContentAddress> {} : parseContentAddress(rawCaOpt);
};

std::string renderContentAddress(std::optional<ContentAddress> ca) {
    return ca ? renderContentAddress(*ca) : "";
}


// FIXME Deduplicate with store-api.cc path computation
std::string renderStorePathDescriptor(StorePathDescriptor ca)
{
    std::string result { ca.name };
    result += ":";

    auto dumpRefs = [&](auto references, bool hasSelfReference) {
        result += "refs:";
        result += std::to_string(references.size());
        for (auto & i : references) {
            result += ":";
            result += i.to_string();
        }
        if (hasSelfReference) result += ":self";
        result += ":";
    };

    std::visit(overloaded {
        [&](TextInfo th) {
            result += "text:";
            dumpRefs(th.references, false);
            result += th.hash.to_string(Base32, true);
        },
        [&](FixedOutputInfo fsh) {
            result += "fixed:";
            dumpRefs(fsh.references.references, fsh.references.hasSelfReference);
            result += makeFileIngestionPrefix(fsh.method);
            result += fsh.hash.to_string(Base32, true);
        },
        [](IPFSInfo fsh) {
            throw Error("ipfs info not handled");
        },
        [&](IPFSHash ih) {
            result += "ipfs:";
            result += ih.to_string();
        },
    }, ca.info);

    return result;
}


StorePathDescriptor parseStorePathDescriptor(std::string_view rawCa)
{
    warn("%s", rawCa);
    auto rest = rawCa;

    std::string_view name;
    std::string_view tag;
    {
        auto optName = splitPrefixTo(rest, ':');
        auto optTag = splitPrefixTo(rest, ':');
        if (!(optTag && optName))
            throw UsageError("not a content address because it is not in the form \"<name>:<tag>:<rest>\": %s", rawCa);
        tag = *optTag;
        name = *optName;
    }

    auto parseRefs = [&]() -> PathReferences<StorePath> {
        if (!splitPrefix(rest, "refs:"))
            throw Error("Invalid CA \"%s\", \"%s\" should begin with \"refs:\"", rawCa, rest);
        PathReferences<StorePath> ret;
        size_t numReferences = 0;
        {
            auto countRaw = splitPrefixTo(rest, ':');
            if (!countRaw)
                throw UsageError("Invalid count");
            numReferences = std::stoi(std::string { *countRaw });
        }
        for (size_t i = 0; i < numReferences; i++) {
            auto s = splitPrefixTo(rest, ':');
            if (!s)
                throw UsageError("Missing reference no. %d", i);
            ret.references.insert(StorePath(*s));
        }
        if (splitPrefix(rest, "self:"))
            ret.hasSelfReference = true;
        return ret;
    };

    // Dummy value
    ContentAddressWithReferences info = TextInfo { Hash(htSHA256), {} };

    // Switch on tag
    if (tag == "text") {
        auto refs = parseRefs();
        if (refs.hasSelfReference)
            throw UsageError("Text content addresses cannot have self references");
        auto hashType = parseHashType_(rest);
        if (hashType != htSHA256)
            throw Error("Text content address hash should use %s, but instead uses %s",
                printHashType(htSHA256), printHashType(hashType));
        info = TextInfo {
            {
                .hash = Hash::parseNonSRIUnprefixed(rest, std::move(hashType)),
            },
            refs.references,
        };
    } else if (tag == "fixed") {
        auto refs = parseRefs();
        auto method = parseFileIngestionMethod_(rest);
        auto hashType = parseHashType_(rest);
        info = FixedOutputInfo {
            {
                .method = method,
                .hash = Hash::parseNonSRIUnprefixed(rest, std::move(hashType)),
            },
            refs,
        };
    } else if (tag == "ipfs") {
        info = IPFSHash::from_string(rest);
    } else
        throw UsageError("content address tag \"%s\" is unrecognized. Recogonized tages are \"text\", \"fixed\", or \"ipfs\"", tag);

    return StorePathDescriptor {
        .name = std::string { name },
        .info = info,
    };
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
                { "algo", printHashType(foh.hash.type) },
                { "hash", foh.hash.to_string(Base32, false) },
            };
        },
        [](IPFSHash ih) {
            return nlohmann::json {
                { "type", "ipfs" },
                { "hash", ih.to_string() },
            };
        },
    }, ca);
}

void from_json(const nlohmann::json& j, ContentAddress & ca) {
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
// stores the StorePathDescriptor information.

void to_json(nlohmann::json& j, const StorePathDescriptor & ca)
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
            { "cid", nlohmann::json {
                { "/", (IPFSHash { .hash = info.hash }).to_string() }
            } }
        };
    } else throw Error("cannot convert to json");
}

void from_json(const nlohmann::json& j, StorePathDescriptor & ca)
{
    std::string_view type = j.at("qtype").get<std::string_view>();
    if (type == "ipfs") {
        auto cid = j.at("cid").at("/").get<std::string_view>();
        ca = StorePathDescriptor {
            .name = j.at("name"),
            .info = IPFSInfo {
                IPFSHash::from_string(cid).hash,
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
            { "cid", nlohmann::json {{ "/", i.hash.to_string() }} }
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
        auto cid = ref.at("cid").at("/").get<std::string_view>();
        refs.insert(IPFSRef {
            .name = std::move(ref.at("name").get<std::string>()),
            .hash = IPFSHash::from_string(cid).hash,
        });
    }
    references = PathReferences<IPFSRef> {
        .references = refs,
        .hasSelfReference = j.at("zhasSelfReference").get<bool>(),
    };
}

// Needed until https://github.com/nlohmann/json/pull/2117

void to_json(nlohmann::json& j, const std::optional<ContentAddress> & c) {
    if (!c)
        j = nullptr;
    else
        to_json(j, *c);
}

void from_json(const nlohmann::json& j, std::optional<ContentAddress> & c) {
    if (j.is_null()) {
        c = std::nullopt;
    } else {
        // Dummy value to set tag bit.
        c = TextHash { .hash = Hash { htSHA256 } };
        from_json(j, *c);
    }
}


}

// FIXME this file should not know about the store class in detail, but it is
// currently needed just for `contentAddressFromMethodHashAndRefs`.
#include "store-api.hh"

namespace nix {


ContentAddressWithReferences contentAddressFromMethodHashAndRefs(
    Store & store,
    ContentAddressingMethod method, Hash && hash, PathReferences<StorePath> && refs)
{
    return std::visit(overloaded {
        [&](IsText _) -> ContentAddressWithReferences {
            if (refs.hasSelfReference)
                throw UsageError("Cannot have a self reference with text hashing scheme");
            return TextInfo {
                { .hash = std::move(hash) },
                std::move(refs.references),
            };
        },
        [&](FileIngestionMethod m2) -> ContentAddressWithReferences {
            return FixedOutputInfo {
                {
                    .method = m2,
                    .hash = std::move(hash),
                },
                std::move(refs),
            };
        },
        [&](IsIPFS _) -> ContentAddressWithReferences {
            std::set<IPFSRef> ipfsRefs;
            auto err = UsageError("IPFS paths must only reference IPFS paths");
            for (auto & ref : refs.references) {
                auto & caOpt = store.queryPathInfo(ref)->ca;
                if (!caOpt) throw err;
                auto pref = std::get_if<IPFSHash>(&*store.queryPathInfo(ref)->ca);
                if (!pref) throw err;
                ipfsRefs.insert(IPFSRef {
                    .name = std::string(ref.name()),
                    .hash = *pref,
                });
            }
            return IPFSInfo {
                .hash = std::move(hash),
                PathReferences<IPFSRef> {
                    .references = std::move(ipfsRefs),
                    .hasSelfReference = refs.hasSelfReference,
                },
            };
        },
    }, method);
}

ContentAddressingMethod getContentAddressMethod(const ContentAddressWithReferences & ca)
{
    return std::visit(overloaded {
        [](TextInfo th) -> ContentAddressingMethod {
            return IsText {};
        },
        [](FixedOutputInfo fsh) -> ContentAddressingMethod {
            return fsh.method;
        },
        [](IPFSInfo ih) -> ContentAddressingMethod {
            return IsIPFS {};
        },
        [](IPFSHash ih) -> ContentAddressingMethod {
            return IsIPFS {};
        },
    }, ca);
}

Hash getContentAddressHash(const ContentAddress & ca)
{
    return std::visit(overloaded {
        [](TextHash th) {
            return th.hash;
        },
        [](FixedOutputHash fsh) {
            return fsh.hash;
        },
        [](IPFSHash ih) {
            return ih.hash;
        },
    }, ca);
}

Hash getContentAddressHash(const ContentAddressWithReferences & ca)
{
    return std::visit(overloaded {
        [](TextInfo th) {
            return th.hash;
        },
        [](FixedOutputInfo fsh) {
            return fsh.hash;
        },
        [](IPFSInfo ih) -> Hash {
            throw Error("ipfs info not handled");
        },
        [](IPFSHash ih) -> Hash {
            return ih.hash;
        },
    }, ca);
}

std::string printMethodAlgo(const ContentAddressWithReferences & ca) {
    return makeContentAddressingPrefix(getContentAddressMethod(ca))
        + printHashType(getContentAddressHash(ca).type);
}

}
