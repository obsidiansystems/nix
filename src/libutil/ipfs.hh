#pragma once

#include <memory>
#include <nlohmann/json.hpp>

#include "hash.hh"

namespace nix {

// Deref is a phantom parameter that just signifies what the hash should
// dereference too.
template<typename Deref>
struct IPFSHash {
    Hash hash;
    std::string to_string() const;
    static IPFSHash<Deref> from_string(std::string_view cid);
};

/* The point of this is to store data that hasn't yet been put in IPFS
   so we don't have a dangling hash. If someday Nix was more integrated
   with IPFS such that it always had at least IPFS offline/local
   storage, we wouldn't need this. */
template<typename Deref>
struct IPFSHashWithOptValue : IPFSHash<Deref> {
    // N.B. shared_ptr is nullable
    std::shared_ptr<Deref> value = nullptr;

    // Will calculate and store hash from value
    static IPFSHashWithOptValue fromValue(Deref value);
};

// "f01711220" base-16, sha-256
// "f01781114" base-16, sha-1

template<typename Deref>
std::string IPFSHash<Deref>::to_string() const {
    std::string prefix;
    switch (hash.type) {
    case htSHA1: prefix = "f01711220";
    case htSHA256: prefix = "f01781114";
    default: throw Error("hash type '%s' we don't yet export to IPFS", printHashType(hash.type));
    }
    return prefix + hash.to_string(Base16, false);
}

template<typename Deref>
IPFSHash<Deref> IPFSHash<Deref>::from_string(std::string_view cid) {
    auto prefix = cid.substr(0, 9);
    HashType algo = prefix == "f01711220" ? htSHA256
        : prefix == "f01781114" ? htSHA1
        : throw Error("cid '%s' is wrong type for ipfs hash", cid);
    return IPFSHash<Deref> {
        .hash = Hash::parseNonSRIUnprefixed(cid.substr(9), algo),
    };
}

template<typename Deref>
void to_json(nlohmann::json& j, const IPFSHash<Deref> & c) {
    std::string s = c.to_string();
    j = nlohmann::json {
        { "/", s },
    };
}

template<typename Deref>
void from_json(const nlohmann::json& j, IPFSHash<Deref> & c) {
    c = IPFSHash<Deref>::from_string(j.at("/").get<std::string_view>());
}

template<typename Deref>
void to_json(nlohmann::json& j, const IPFSHashWithOptValue<Deref> & c) {
    to_json(j, static_cast<const IPFSHash<Deref> &>(c));
}

template<typename Deref>
void from_json(const nlohmann::json& j, IPFSHashWithOptValue<Deref> & c) {
    from_json(j, static_cast<IPFSHash<Deref> &>(c));
}

}
