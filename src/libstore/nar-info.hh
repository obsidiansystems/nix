#pragma once

#include "types.hh"
#include "hash.hh"
#include "store-api.hh"

namespace nix {

struct NarInfo : ValidPathInfo
{
    std::string url;
    std::string compression;
    std::optional<Hash> fileHash;
    uint64_t fileSize = 0;
    std::string system;

    NarInfo() = delete;
    NarInfo(StorePath && path, Hash narHash)
    	: ValidPathInfo(std::move(path), narHash)
    { }
    NarInfo(const ValidPathInfo & info, Hash narHash)
    	: ValidPathInfo(info, narHash)
    { }
    NarInfo(const Store & store, const std::string & s, const std::string & whence);

    std::string to_string(const Store & store) const;
};

}
