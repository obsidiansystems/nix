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
    } else
        throw UsageError("content address prefix \"%s\" is unrecognized. Recogonized prefixes are \"text\" or \"fixed\"", prefix);
};

std::optional<LegacyContentAddress> parseLegacyContentAddressOpt(std::string_view rawCaOpt) {
    return rawCaOpt == "" ? std::optional<LegacyContentAddress> {} : parseLegacyContentAddress(rawCaOpt);
};

std::string renderLegacyContentAddress(std::optional<LegacyContentAddress> ca) {
    return ca ? renderLegacyContentAddress(*ca) : "";
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
