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
    std::vector<uint8_t> pack() const;
    nlohmann::json packForCBOR() const;
};

namespace untyped {
	std::string toString(Hash);
	Hash fromString(std::string_view);
    std::vector<uint8_t>pack(Hash);
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

template<typename Deref>
std::string IPFSHash<Deref>::to_string() const {
    return untyped::toString(hash);
}

template<typename Deref>
IPFSHash<Deref> IPFSHash<Deref>::from_string(std::string_view cid) {
	return IPFSHash<Deref> {
		.hash = untyped::fromString(cid)
	};
}

template<typename Deref>
std::vector<uint8_t> IPFSHash<Deref>::pack() const {
	return untyped::pack(hash);
}

template<typename Deref>
nlohmann::json IPFSHash<Deref>::packForCBOR() const {
    return nlohmann::json::binary(pack(), 42);
}

template<typename Deref>
void to_json(nlohmann::json& j, const IPFSHash<Deref> & c) {
    j = nlohmann::json {
        { "/", c.to_string() },
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
