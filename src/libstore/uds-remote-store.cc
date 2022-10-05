#include "uds-remote-store.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>


namespace nix {

UDSRemoteStore::UDSRemoteStore(const Params & params)
    : StoreConfig(params)
    , LocalFSStoreConfig(params)
    , RemoteStoreConfig(params)
    , UDSRemoteStoreConfig(params)
    , Store(params)
    , LocalFSStore(params)
    , RemoteStore(params)
{
}


UDSRemoteStore::UDSRemoteStore(
        const std::string scheme,
        std::string socket_path,
        const Params & params)
    : UDSRemoteStore(params)
{
    path.emplace(socket_path);
}


std::string UDSRemoteStore::getUri()
{
    if (path) {
        return std::string("unix://") + *path;
    } else {
        return "daemon";
    }
}


void UDSRemoteStore::Connection::closeWrite()
{
    shutdown(fd.get(), SHUT_WR);
}


ref<RemoteStore::Connection> UDSRemoteStore::openConnection()
{
    auto conn = make_ref<Connection>();

    /* Connect to a daemon that does the privileged work for us. */
    conn->fd = createUnixDomainSocket();

    nix::connect(conn->fd.get(), path ? *path : settings.nixDaemonSocketFile);

    conn->from.fd = conn->fd.get();
    conn->to.fd = conn->fd.get();

    conn->startTime = std::chrono::steady_clock::now();

    return conn;
}


static RegisterStoreImplementation<UDSRemoteStore, UDSRemoteStoreConfig> regUDSRemoteStore;


struct UDSNoFSRemoteStoreConfig : virtual RemoteStoreConfig
{
    UDSNoFSRemoteStoreConfig(const Store::Params & params)
        : StoreConfig(params)
        , RemoteStoreConfig(params)
    {
    }

    const std::string name() override { return "Local Daemon Store (remote fs)"; }
};

class UDSNoFSRemoteStore : public virtual UDSNoFSRemoteStoreConfig, public virtual RemoteStore
{
public:

    UDSNoFSRemoteStore(
            const std::string scheme,
            std::string socket_path,
            const Params & params)
        : StoreConfig(params)
        , RemoteStoreConfig(params)
        , UDSNoFSRemoteStoreConfig(params)
        , Store(params)
        , RemoteStore(params)
    {
        path = socket_path;
    }

    std::string getUri() override
    {
        return std::string("unix-nofs://") + path;
    }

    static std::set<std::string> uriSchemes()
    { return {"unix-nofs"}; }

    bool sameMachine() override
    { return false; }

private:

    std::string path;

    struct Connection : RemoteStore::Connection
    {
        AutoCloseFD fd;
        void closeWrite() override;
    };

    // FIXME extend daemon protocol, move implementation to RemoteStore
    std::optional<std::string> getBuildLog(const StorePath & path) override
    { unsupported("getBuildLog"); }

    ref<RemoteStore::Connection> openConnection() override
    {
        auto conn = make_ref<Connection>();

        /* Connect to a daemon that does the privileged work for us. */
        conn->fd = createUnixDomainSocket();

        nix::connect(conn->fd.get(), path );

        conn->from.fd = conn->fd.get();
        conn->to.fd = conn->fd.get();

        conn->startTime = std::chrono::steady_clock::now();

        return conn;
    }

};

void UDSNoFSRemoteStore::Connection::closeWrite()
{
    shutdown(fd.get(), SHUT_WR);
}

static RegisterStoreImplementation<UDSNoFSRemoteStore, UDSNoFSRemoteStoreConfig> regUDSNoFSRemoteStore;


}
