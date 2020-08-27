#pragma once

#include "hash.hh"

namespace nix {

// hash of some IPFSInfo
struct IPFSHash {
    Hash hash;

    bool operator ==(IPFSHash otherHash) const noexcept {
        return hash == otherHash.hash;
    };
    bool operator !=(IPFSHash otherHash) const noexcept {
        return hash != otherHash.hash;
    };

    std::string to_string() const;
    static IPFSHash from_string(std::string_view cid);
};

}
