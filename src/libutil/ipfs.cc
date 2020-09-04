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

IPFSHash IPFSHash::from_string(std::string_view cid_)
{
    auto cid = ipfsCidFormatBase16(cid_);
    auto prefix = cid.substr(0, 9);
    HashType algo = prefix == "f01781114" ? htSHA1
        : prefix == "f01711220" ? htSHA256
        : throw Error("cid '%s' is wrong type for ipfs hash", cid);
    return IPFSHash {
        Hash::parseNonSRIUnprefixed(cid.substr(9), algo)
    };
}

constexpr std::string_view base16Alpha = "0123456789abcdef";

std::string ipfsCidFormatBase16(std::string_view cid)
{
    if (cid[0] == 'f') return std::string { cid };
    assert(cid[0] == 'b');
    std::string newCid = "f";
    unsigned short remainder;
    for (size_t i = 1; i < cid.size(); i++) {
        if (cid[i] >= 'a' && cid[i] <= 'z')
            remainder = (remainder << 5) | (cid[i] - 'a');
        else if (cid[i] >= '2' && cid[i] <= '7')
            remainder = (remainder << 5) | (26 + cid[i] - '2');
        else throw Error("unknown character: '%c'", cid[i]);;
        if ((i % 4) == 0)
            newCid += base16Alpha[(remainder >> 4) & 0xf];
        newCid += base16Alpha[(remainder >> (i % 4)) & 0xf];
    }
    return newCid;
}

}
