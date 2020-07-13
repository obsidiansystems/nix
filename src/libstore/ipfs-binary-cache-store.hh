#pragma once

#include "binary-cache-store.hh"
#include "git.hh"

namespace nix {

MakeError(UploadToIPFS, Error);

class IPFSBinaryCacheStore : public Store
{

public:

    const Setting<std::string> compression{this, "xz", "compression", "NAR compression method ('xz', 'bzip2', or 'none')"};
    const Setting<Path> secretKeyFile{this, "", "secret-key", "path to secret key used to sign the binary cache"};
    const Setting<bool> parallelCompression{this, false, "parallel-compression",
        "enable multi-threading compression, available for xz only currently"};

private:

    std::unique_ptr<SecretKey> secretKey;
    std::string narMagic;

    std::string cacheUri;
    std::string daemonUri;

    std::string getIpfsPath() {
        auto state(_state.lock());
        return state->ipfsPath;
    }
    std::string initialIpfsPath;
    std::optional<string> optIpnsPath;

    struct State
    {
        std::string ipfsPath;
    };
    Sync<State> _state;

    // only enable trustless operations
    bool trustless = false;

public:

    IPFSBinaryCacheStore(const Params & params, const Path & _cacheUri);

    std::string getUri() override
    {
        return cacheUri;
    }

private:
    std::string putIpfsDag(nlohmann::json data, std::optional<std::string> hash = std::nullopt);

    nlohmann::json getIpfsDag(std::string objectPath);

    // Given a ipns path, checks if it corresponds to a DNSLink path, and in
    // case returns the domain
    static std::optional<string> isDNSLinkPath(std::string path);

    bool ipfsObjectExists(const std::string ipfsPath);

    std::optional<uint64_t> ipfsBlockStat(std::string ipfsPath);

    bool fileExists(const std::string & path)
    {
        return ipfsObjectExists(getIpfsPath() + "/" + path);
    }

    // Resolve the IPNS name to an IPFS object
    std::string resolveIPNSName(std::string ipnsPath);

    const std::string base16Alpha = "0123456789abcdef";
    std::string ipfsCidFormatBase16(std::string cid);

public:
    Path formatPathAsProtocol(Path path);

    // IPNS publish can be slow, we try to do it rarely.
    void sync() override;

private:

    void addLink(std::string name, std::string ipfsObject);

    std::string addFile(const std::string & data);

    void upsertFile(const std::string & path, const std::string & data, const std::string & mimeType);

    void getFile(const std::string & path,
        Callback<std::shared_ptr<std::string>> callback) noexcept;

    void getFile(const std::string & path, Sink & sink);

    std::shared_ptr<std::string> getFile(const std::string & path);

    void getIpfsObject(const std::string & ipfsPath,
        Callback<std::shared_ptr<std::string>> callback) noexcept;

    void getIpfsBlock(const std::string & path, Sink & sink);

    std::shared_ptr<std::string> getIpfsBlock(const std::string & path);

    void getIpfsBlock(const std::string & ipfsPath,
        Callback<std::shared_ptr<std::string>> callback) noexcept;

    void writeNarInfo(ref<NarInfo> narInfo);

    std::optional<std::string> getCidFromCA(ContentAddress ca);

    std::string putIpfsBlock(std::string s, std::string format, std::string mhtype);

    std::string addGit(Path path, std::string modulus, bool hasSelfReference);

    void unrewriteModulus(std::string & data, std::string hashPart);

    std::unique_ptr<Source> getGitObject(std::string path, std::string hashPart, bool hasSelfReference);

    void getGitEntry(ParseSink & sink, const Path & path,
        const Path & realStoreDir, const Path & storeDir,
        int perm, std::string name, Hash hash, std::string hashPart,
        bool hasSelfReference);

public:

    void addToStore(const ValidPathInfo & info, Source & narSource,
        RepairFlag repair, CheckSigsFlag checkSigs, std::shared_ptr<FSAccessor> accessor) override;

    bool isValidPathUncached(StorePathOrCA storePathOrCA) override;

    void narFromPath(StorePathOrCA storePathOrCA, Sink & sink) override;

    void queryPathInfoUncached(StorePathOrCA storePathOrCa,
        Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

    StorePath addToStore(const string & name, const Path & srcPath,
        FileIngestionMethod method, HashType hashAlgo, PathFilter & filter, RepairFlag repair) override;

    StorePath addTextToStore(const string & name, const string & s,
        const StorePathSet & references, RepairFlag repair) override;

    void addSignatures(const StorePath & storePath, const StringSet & sigs) override;

    virtual void addTempRoot(const StorePath & path) override;

    std::shared_ptr<std::string> getBuildLog(const StorePath & path) override
    { unsupported("getBuildLog"); }

    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv,
        BuildMode buildMode) override
    { unsupported("buildDerivation"); }

    void ensurePath(StorePathOrCA ca) override
    { unsupported("ensurePath"); }

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
    { unsupported("queryPathFromHashPart"); }

};

}
