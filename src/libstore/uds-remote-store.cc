#include "uds-remote-store.hh"
#include "unix-domain-socket.hh"
#include "worker-protocol.hh"
#include "store-registration.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef _WIN32
# include <winsock2.h>
# include <afunix.h>
#else
# include <sys/socket.h>
# include <sys/un.h>
#endif

namespace nix {

UDSRemoteStoreConfig::UDSRemoteStoreConfig(
    std::string_view scheme,
    std::string_view authority,
    const StoreReference::Params & params)
    : StoreConfig(params)
    , LocalFSStoreConfig(params)
    , RemoteStoreConfig(params)
    , path{authority.empty() ? settings.nixDaemonSocketFile : authority}
{
    if (uriSchemes().count(std::string{scheme}) == 0) {
        throw UsageError("Scheme must be 'unix'");
    }
}


std::string UDSRemoteStoreConfig::doc()
{
    return
        #include "uds-remote-store.md"
        ;
}


UDSRemoteStore::UDSRemoteStore(const Config & config)
    : Store::Config{config}
    , LocalFSStore::Config{config}
    , RemoteStore::Config{config}
    , UDSRemoteStore::Config{config}
    , Store(static_cast<const Store::Config &>(*this))
    , LocalFSStore(static_cast<const LocalFSStore::Config &>(*this))
    , RemoteStore(static_cast<const RemoteStore::Config &>(*this))
{
}


std::string UDSRemoteStore::getUri()
{
    return path == settings.nixDaemonSocketFile
        ? // FIXME: Not clear why we return daemon here and not default
          // to settings.nixDaemonSocketFile
          //
          // unix:// with no path also works. Change what we return?
          "daemon"
        : std::string(*uriSchemes().begin()) + "://" + path;
}


void UDSRemoteStore::Connection::closeWrite()
{
    shutdown(toSocket(fd.get()), SHUT_WR);
}


ref<RemoteStore::Connection> UDSRemoteStore::openConnection()
{
    auto conn = make_ref<Connection>();

    /* Connect to a daemon that does the privileged work for us. */
    conn->fd = createUnixDomainSocket();

    nix::connect(toSocket(conn->fd.get()), path);

    conn->from.fd = conn->fd.get();
    conn->to.fd = conn->fd.get();

    conn->startTime = std::chrono::steady_clock::now();

    return conn;
}


void UDSRemoteStore::addIndirectRoot(const Path & path)
{
    auto conn(getConnection());
    conn->to << WorkerProto::Op::AddIndirectRoot << path;
    conn.processStderr();
    readInt(conn->from);
}


static RegisterStoreImplementation<UDSRemoteStore> regUDSRemoteStore;

}
