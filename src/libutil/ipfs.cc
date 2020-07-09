#include "ipfs.hh"

namespace nix {

// "f01711220" base-16, sha-256
// "f01781114" base-16, sha-1

std::string IPFSHash::to_string() const
{
    std::string prefix;
    switch (hash.type) {
    case htSHA1: prefix = "f01781114"; break;
    case htSHA256: prefix = "f01711220"; break;
    default: throw Error("hash type '%s' we don't yet export to IPFS", printHashType(hash.type));
    }
    return prefix + hash.to_string(Base16, false);
}

IPFSHash IPFSHash::from_string(std::string_view cid)
{
    auto prefix = cid.substr(0, 9);
    HashType algo = prefix == "f01781114" ? htSHA1
        : prefix == "f01711220" ? htSHA256
        : throw Error("cid '%s' is wrong type for ipfs hash", cid);
    return IPFSHash {
        Hash::parseNonSRIUnprefixed(cid.substr(9), algo)
    };
}

}
