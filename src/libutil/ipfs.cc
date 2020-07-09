#include "ipfs.hh"

namespace nix::untyped {

// "f01711220" base-16, sha-256
// "f01781114" base-16, sha-1

std::string toString(Hash hash) const {
    std::string prefix;
    switch (hash.type) {
    case htSHA1: prefix = "f01711220";
    case htSHA256: prefix = "f01781114";
    default: throw Error("hash type '%s' we don't yet export to IPFS", printHashType(hash.type));
    }
    return prefix + hash.to_string(Base16, false);
}

IPFSHash<Deref> fromString(std::string_view cid) {
    auto prefix = cid.substr(0, 9);
    HashType algo = prefix == "f01711220" ? htSHA256
        : prefix == "f01781114" ? htSHA1
        : throw Error("cid '%s' is wrong type for ipfs hash", cid);
    return IPFSHash<Deref> {
        .hash = Hash::parseNonSRIUnprefixed(cid.substr(9), algo),
    };
}

static HashType getMultiHashTag(int tag)
{
    switch (tag) {
    case 0x11: {
        return htSHA1;
    }
    case 0x12: {
        return htSHA256;
    }
    default: {
        throw Error("tag '%i' is an unknown hash type", tag);
    }
    }
}

nlohmann::json pack(Hash hash) const {
{
	auto cid = to_string();
    std::vector<uint8_t> result;
    assert(cid[0] == 'f');
    result.push_back(0x00);
    result.push_back(std::stoi(cid.substr(1, 2), nullptr, 16));
    result.push_back(std::stoi(cid.substr(3, 2), nullptr, 16));
    result.push_back(std::stoi(cid.substr(5, 2), nullptr, 16));
    result.push_back(std::stoi(cid.substr(7, 2), nullptr, 16));
    HashType ht = getMultiHashTag(std::stoi(cid.substr(5, 2), nullptr, 16));
    Hash hash = Hash::parseAny(cid.substr(9), ht);
    for (unsigned int i = 0; i < hash.hashSize; i++)
        result.push_back(hash.hash[i]);
    return result;
}

