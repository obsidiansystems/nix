#pragma once
///@file

#include "remote-store.hh"
#include "remote-store-connection.hh"
#include "indirect-root-store.hh"

namespace nix {

struct UDSRemoteStoreConfig :
    std::enable_shared_from_this<UDSRemoteStoreConfig>,
    Store::Config,
    LocalFSStore::Config,
    RemoteStore::Config
{
    static config::SettingDescriptionMap descriptions();

    UDSRemoteStoreConfig(const StoreReference::Params & params)
        : UDSRemoteStoreConfig{"unix", "", params}
    {}

    /**
     * @param authority is the socket path.
     */
    UDSRemoteStoreConfig(
        std::string_view scheme,
        std::string_view authority,
        const StoreReference::Params & params);

    const std::string name() const override { return "Local Daemon Store"; }

    std::string doc() const override;

    /**
     * The path to the unix domain socket.
     *
     * The default is `settings.nixDaemonSocketFile`, but we don't write
     * that below, instead putting in the constructor.
     */
    Path path;

    static std::set<std::string> uriSchemes()
    { return {"unix"}; }

    ref<Store> openStore() const override;
};

struct UDSRemoteStore :
    virtual IndirectRootStore,
    virtual RemoteStore
{
    using Config = UDSRemoteStoreConfig;

    ref<const Config> config;

    UDSRemoteStore(ref<const Config>);

    std::string getUri() override;

    ref<SourceAccessor> getFSAccessor(bool requireValidPath = true) override
    { return LocalFSStore::getFSAccessor(requireValidPath); }

    void narFromPath(const StorePath & path, Sink & sink) override
    { LocalFSStore::narFromPath(path, sink); }

    /**
     * Implementation of `IndirectRootStore::addIndirectRoot()` which
     * delegates to the remote store.
     *
     * The idea is that the client makes the direct symlink, so it is
     * owned managed by the client's user account, and the server makes
     * the indirect symlink.
     */
    void addIndirectRoot(const Path & path) override;

private:

    struct Connection : RemoteStore::Connection
    {
        AutoCloseFD fd;
        void closeWrite() override;
    };

    ref<RemoteStore::Connection> openConnection() override;
};

}
