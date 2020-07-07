#pragma once

#include <memory>

#include "hash.hh"

namespace nix {

// Deref is a phantom parameter that just signifies what the hash should
// dereference too.
template<typename Deref>
struct IPFSHash {
    Hash hash;
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

}
