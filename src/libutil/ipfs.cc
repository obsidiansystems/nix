#include "ipfs.hh"

namespace nix::untyped {

// "f01711220" base-16, sha-256
// "f01781114" base-16, sha-1

std::string toString(Hash hash) {
    std::string prefix;
    switch (hash.type) {
    case htSHA1: prefix = "f01711220";
    case htSHA256: prefix = "f01781114";
    default: throw Error("hash type '%s' we don't yet export to IPFS", printHashType(hash.type));
    }
    return prefix + hash.to_string(Base16, false);
}

Hash fromString(std::string_view cid) {
    auto prefix = cid.substr(0, 9);
    HashType algo = prefix == "f01711220" ? htSHA256
        : prefix == "f01781114" ? htSHA1
        : throw Error("cid '%s' is wrong type for ipfs hash", cid);
    return Hash::parseNonSRIUnprefixed(cid.substr(9), algo);
}

std::vector<uint8_t> pack(Hash hash)
{
	auto cid = toString(hash);
    std::vector<uint8_t> result;
    assert(cid[0] == 'f');
    result.push_back(0x00);
    result.push_back(std::stoi(cid.substr(1, 2), nullptr, 16));
    result.push_back(std::stoi(cid.substr(3, 2), nullptr, 16));
    result.push_back(std::stoi(cid.substr(5, 2), nullptr, 16));
    result.push_back(std::stoi(cid.substr(7, 2), nullptr, 16));
    for (unsigned int i = 0; i < hash.hashSize; i++)
        result.push_back(hash.hash[i]);
    return result;
}

}
