#include "args.hh"
#include "content-address.hh"
#include "split.hh"

namespace nix {

std::string FixedOutputHash::printMethodAlgo() const
{
    return makeFileIngestionPrefix(method) + printHashType(hash.type);
}


std::string makeFileIngestionPrefix(FileIngestionMethod m)
{
    switch (m) {
    case FileIngestionMethod::Flat:
        return "";
    case FileIngestionMethod::Recursive:
        return "r:";
    }
    abort();
}

std::string makeContentAddressingPrefix(ContentAddressMethod m) {
    return std::visit(overloaded {
        [](TextHashMethod _) -> std::string { return "text:"; },
        [](FileIngestionMethod m2) {
             /* Not prefixed for back compat with things that couldn't produce text before. */
            return makeFileIngestionPrefix(m2);
        },
    }, m);
}

ContentAddressMethod parseContentAddressingPrefix(std::string_view & m)
{
    ContentAddressMethod method = FileIngestionMethod::Flat;
    if (splitPrefix(m, "r:"))
        method = FileIngestionMethod::Recursive;
    else if (splitPrefix(m, "text:"))
        method = TextHashMethod {};
    return method;
}


std::string makeFixedOutputCA(FileIngestionMethod method, const Hash & hash)
{
    return "fixed:"
        + makeFileIngestionPrefix(method)
        + hash.to_string(Base32, true);
}

std::string renderContentAddress(ContentAddress ca)
{
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

static HashType parseHashType_(std::string_view & rest) {
    auto hashTypeRaw = splitPrefixTo(rest, ':');
    if (!hashTypeRaw)
        throw UsageError("hash must be in form \"<algo>:<hash>\", but found: %s", rest);
    return parseHashType(*hashTypeRaw);
};

static FileIngestionMethod parseFileIngestionMethod_(std::string_view & rest) {
    if (splitPrefix(rest, "r:"))
        return FileIngestionMethod::Recursive;
    return FileIngestionMethod::Flat;
}

std::string renderContentAddressMethodAndHash(ContentAddressMethod cam, HashType ht)
{
    return std::visit(overloaded {
        [&](TextHashMethod & th) {
            return std::string{"text:"} + printHashType(ht);
        },
        [&](FileIngestionMethod & fim) {
            return "fixed:" + makeFileIngestionPrefix(fim) + printHashType(ht);
        }
    }, cam);
}

/*
  Parses content address strings up to the hash.
 */
static std::pair<ContentAddressMethod, HashType> parseContentAddressMethodPrefix(std::string_view & rest)
{
    std::string_view wholeInput { rest };

    std::string_view prefix;
    {
        auto optPrefix = splitPrefixTo(rest, ':');
        if (!optPrefix)
            throw UsageError("not a path-info content address because it is not in the form \"<prefix>:<rest>\": %s", wholeInput);
        prefix = *optPrefix;
    }

    // Switch on prefix
    if (prefix == "text") {
        // No parsing of the ingestion method, "text" only support flat.
        HashType hashType = parseHashType_(rest);
        return {
            TextHashMethod {},
            std::move(hashType),
        };
    } else if (prefix == "fixed") {
        auto method = parseFileIngestionMethod_(rest);
        HashType hashType = parseHashType_(rest);
        return {
            std::move(method),
            std::move(hashType),
        };
    } else
        throw UsageError("path-info content address prefix \"%s\" is unrecognized. Recogonized prefixes are \"text\" or \"fixed\"", prefix);
}

ContentAddress parseContentAddress(std::string_view rawCa) {
    auto rest = rawCa;

    auto [caMethod, hashType] = parseContentAddressMethodPrefix(rest);

    return std::visit(overloaded {
        [&](TextHashMethod _) {
            return ContentAddress(TextHash {
                .hash = Hash::parseNonSRIUnprefixed(rest, hashType)
            });
        },
        [&](FileIngestionMethod fim) {
            return ContentAddress(FixedOutputHash {
                .method = fim,
                .hash = Hash::parseNonSRIUnprefixed(rest, hashType),
            });
        },
    }, caMethod);
}

std::pair<ContentAddressMethod, HashType> parseContentAddressMethod(std::string_view caMethod)
{
    std::string_view asPrefix {std::string{caMethod} + ":"};
    return parseContentAddressMethodPrefix(asPrefix);
}

std::optional<ContentAddress> parseContentAddressOpt(std::string_view rawCaOpt)
{
    return rawCaOpt == "" ? std::optional<ContentAddress>() : parseContentAddress(rawCaOpt);
};

std::string renderContentAddress(std::optional<ContentAddress> ca)
{
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
    } else
        throw UsageError("content address tag \"%s\" is unrecognized. Recogonized tages are \"text\" or \"fixed\"", tag);

    return StorePathDescriptor {
        .name = std::string { name },
        .info = info,
    };
}

ContentAddressWithReferences contentAddressFromMethodHashAndRefs(
    ContentAddressMethod method, Hash && hash, PathReferences<StorePath> && refs)
{
    return std::visit(overloaded {
        [&](TextHashMethod _) -> ContentAddressWithReferences {
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
    }, method);
}

ContentAddressMethod getContentAddressMethod(const ContentAddressWithReferences & ca)
{
    return std::visit(overloaded {
        [](TextInfo th) -> ContentAddressMethod {
            return TextHashMethod {};
        },
        [](FixedOutputInfo fsh) -> ContentAddressMethod {
            return fsh.method;
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
    }, ca);
}

std::string printMethodAlgo(const ContentAddressWithReferences & ca) {
    return makeContentAddressingPrefix(getContentAddressMethod(ca))
        + printHashType(getContentAddressHash(ca).type);
}

}
