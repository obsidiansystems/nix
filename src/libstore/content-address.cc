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
        [](IPFSHash<IPFSGitTreeNode> ih) {
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

LegacyContentAddress parseLegacyContentAddress(std::string_view rawCa) {
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
        auto hash = IPFSHash<IPFSGitTreeNode>::from_string(rest);
        if (hash.hash.type != htSHA256)
            throw Error("This IPFS hash should have type SHA-256: %s", hash.to_string());
        return hash;
    } else
        throw UsageError("path-info content address prefix \"%s\" is unrecognized. Recogonized prefixes are \"text\", \"fixed\", or \"ipfs\"", prefix);
};

std::optional<LegacyContentAddress> parseLegacyContentAddressOpt(std::string_view rawCaOpt) {
    return rawCaOpt == "" ? std::optional<LegacyContentAddress> {} : parseLegacyContentAddress(rawCaOpt);
};

std::string renderLegacyContentAddress(std::optional<LegacyContentAddress> ca) {
    return ca ? renderLegacyContentAddress(*ca) : "";
}


// FIXME Deduplicate with store-api.cc path computation
std::string renderContentAddress(ContentAddress ca)
{
    std::string result = ca.name;

    auto dumpRefs = [&](auto references, bool hasSelfReference) {
        result += "refs:";
        result += std::to_string(references.size());
        for (auto & i : references) {
            result += ":";
            result += i.to_string();
        }
        if (hasSelfReference) result += ":self";
    };

    std::visit(overloaded {
        [&](TextInfo th) {
            result += "text:";
            dumpRefs(th.references, false);
            result += ":" + renderLegacyContentAddress(LegacyContentAddress {TextHash {
                .hash = th.hash,
            }});
        },
        [&](FixedOutputInfo fsh) {
            result += "fixed:";
            dumpRefs(fsh.references.references, fsh.references.hasSelfReference);
            result += ":" + renderLegacyContentAddress(LegacyContentAddress {FixedOutputHash {
                .method = fsh.method,
                .hash = fsh.hash
            }});
        },
        [&](IPFSHashWithOptValue<IPFSGitTreeNode> ih) {
            result += "ipfs:";
            result += ih.to_string();
        },
    }, ca.info);

    return result;
}


ContentAddress parseContentAddress(std::string_view rawCa)
{
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
        if (!splitPrefix(rest, "refs,"))
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
    ContentAddressWithoutName info = TextInfo { Hash(htSHA256), {} };

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
        info = IPFSHashWithOptValue<IPFSGitTreeNode> {
            IPFSHash<IPFSGitTreeNode>::from_string(rest)
        };
    } else
        throw UsageError("content address tag \"%s\" is unrecognized. Recogonized tages are \"text\", \"fixed\", or \"ipfs\"", tag);

    return ContentAddress {
        .name = std::string { name },
        .info = info,
    };
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
        [](IPFSHash<IPFSGitTreeNode> ih) {
            return nlohmann::json {
                { "type", "ipfs" },
                { "hash", ih },
            };
        },
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

template<typename T>
void to_json(nlohmann::json& j, const ContentAddressT<T> & c)
{
    j = nlohmann::json {
        { "name", c.name },
        { "info", c.info },
    };
}

template<typename T>
void from_json(const nlohmann::json& j, ContentAddressT<T> & c)
{
    c.name = j.at("name");
    from_json(j.at("info"), c.info);
}

template<template <typename> class Ref>
void to_json(nlohmann::json& j, const IPFSGitTreeNodeT<Ref> & c)
{
    // FIXME: ipfs sort order is weird, it always puts type before
    // references, so we rename it to qtype so it always comes
    // before references
    j = nlohmann::json {
        { "gitTree", IPFSHash<void> { c.gitTree } },
        { "zreferences", c.references },
    };
}

template<template <typename> class Ref>
void from_json(const nlohmann::json& j, IPFSGitTreeNodeT<Ref> & c)
{
    IPFSHash<void> temp {
        .hash = Hash { htSHA256 }, // dummy val
    };
    from_json(j.at("gitTree"), temp);
    c.gitTree = std::move(temp.hash);
    from_json(j.at("zreferences"), c.references);
}

template<typename T>
void to_json(nlohmann::json& j, const PathReferences<T> & references)
{
    // FIXME: ipfs sort order is weird, it always puts references
    // before hasSelfReference, so we rename it to zhasSelfReferences
    // so it always comes after references

    j = nlohmann::json {
        { "zhasSelfReference", references.hasSelfReference },
        { "references", references.references }
    };
}

template<typename T>
void from_json(const nlohmann::json& j, PathReferences<T> & references)
{
    std::set<T> refs;
    nlohmann::from_json(j.at("references"), refs);
    references = PathReferences<T> {
        .references = refs,
        .hasSelfReference = j.at("zhasSelfReference").get<bool>(),
    };
}

template
void to_json(nlohmann::json& j, const IPFSHash<IPFSGitTreeNode> & c);
template
void from_json(const nlohmann::json& j, IPFSHash<IPFSGitTreeNode> & c);

template
void to_json(nlohmann::json& j, const IPFSGitTreeNode & c);
template
void from_json(const nlohmann::json& j, IPFSGitTreeNode & c);

}
