#pragma once
///@file

#include "common-ssh-store-config.hh"
#include "store-api.hh"
#include "ssh.hh"
#include "callback.hh"
#include "pool.hh"

namespace nix {

template<template<typename> class F>
struct LegacySSHStoreConfigT
{
    const F<Strings> remoteProgram;
    const F<int> maxConnections;
};

struct LegacySSHStoreConfig :
    virtual CommonSSHStoreConfig,
    LegacySSHStoreConfigT<config::JustValue>
{
    struct Descriptions :
        virtual CommonSSHStoreConfig::Descriptions,
        LegacySSHStoreConfigT<config::SettingInfo>
    {
        Descriptions();
    };

    static const Descriptions descriptions;

    /**
     * Hack for getting remote build log output. Intentionally not a
     * documented user-visible setting.
     */
    Descriptor logFD = INVALID_DESCRIPTOR;

    LegacySSHStoreConfig(
        std::string_view scheme,
        std::string_view authority,
        const StoreReference::Params & params);

    const std::string name() override { return "SSH Store"; }

    static std::set<std::string> uriSchemes() { return {"ssh"}; }

    std::string doc() override;

    ref<Store> openStore() const override;
};

struct LegacySSHStore : public virtual LegacySSHStoreConfig, public virtual Store
{
    using Config = LegacySSHStoreConfig;

    struct Connection;

    ref<Pool<Connection>> connections;

    SSHMaster master;

    LegacySSHStore(const Config &);

    ref<Connection> openConnection();

    std::string getUri() override;

    void queryPathInfoUncached(const StorePath & path,
        Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

    void addToStore(const ValidPathInfo & info, Source & source,
        RepairFlag repair, CheckSigsFlag checkSigs) override;

    void narFromPath(const StorePath & path, Sink & sink) override;

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
    { unsupported("queryPathFromHashPart"); }

    StorePath addToStore(
        std::string_view name,
        const SourcePath & path,
        ContentAddressMethod method,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        PathFilter & filter,
        RepairFlag repair) override
    { unsupported("addToStore"); }

    virtual StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod = FileSerialisationMethod::NixArchive,
        ContentAddressMethod hashMethod = FileIngestionMethod::NixArchive,
        HashAlgorithm hashAlgo = HashAlgorithm::SHA256,
        const StorePathSet & references = StorePathSet(),
        RepairFlag repair = NoRepair) override
    { unsupported("addToStore"); }

public:

    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv,
        BuildMode buildMode) override;

    void buildPaths(const std::vector<DerivedPath> & drvPaths, BuildMode buildMode, std::shared_ptr<Store> evalStore) override;

    void ensurePath(const StorePath & path) override
    { unsupported("ensurePath"); }

    virtual ref<SourceAccessor> getFSAccessor(bool requireValidPath) override
    { unsupported("getFSAccessor"); }

    /**
     * The default instance would schedule the work on the client side, but
     * for consistency with `buildPaths` and `buildDerivation` it should happen
     * on the remote side.
     *
     * We make this fail for now so we can add implement this properly later
     * without it being a breaking change.
     */
    void repairPath(const StorePath & path) override
    { unsupported("repairPath"); }

    void computeFSClosure(const StorePathSet & paths,
        StorePathSet & out, bool flipDirection = false,
        bool includeOutputs = false, bool includeDerivers = false) override;

    StorePathSet queryValidPaths(const StorePathSet & paths,
        SubstituteFlag maybeSubstitute = NoSubstitute) override;

    void connect() override;

    unsigned int getProtocol() override;

    /**
     * The legacy ssh protocol doesn't support checking for trusted-user.
     * Try using ssh-ng:// instead if you want to know.
     */
    std::optional<TrustedFlag> isTrustedClient() override;

    void queryRealisationUncached(const DrvOutput &,
        Callback<std::shared_ptr<const Realisation>> callback) noexcept override
    // TODO: Implement
    { unsupported("queryRealisation"); }
};

}
