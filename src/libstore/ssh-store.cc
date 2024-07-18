#include "ssh-store.hh"
#include "local-fs-store.hh"
#include "remote-store-connection.hh"
#include "source-accessor.hh"
#include "archive.hh"
#include "worker-protocol.hh"
#include "worker-protocol-impl.hh"
#include "pool.hh"
#include "ssh.hh"
#include "config-parse-impl.hh"
#include "store-registration.hh"

namespace nix {

SSHStoreConfig::Descriptions::Descriptions()
    : Store::Config::Descriptions{Store::Config::descriptions}
    , CommonSSHStoreConfig::Descriptions{CommonSSHStoreConfig::descriptions}
    , SSHStoreConfigT<config::SettingInfo>{
        .remoteProgram{
            .name = "remote-program",
            .description = "Path to the `nix-daemon` executable on the remote machine.",
        },
    }
{}


const SSHStoreConfig::Descriptions SSHStoreConfig::descriptions{};


decltype(SSHStoreConfig::defaults) SSHStoreConfig::defaults = {
    .remoteProgram = {{"nix-daemon"}},
};


SSHStoreConfig::SSHStoreConfig(
    std::string_view scheme,
    std::string_view authority,
    const StoreReference::Params & params)
    : Store::Config{params}
    , RemoteStore::Config{params}
    , CommonSSHStoreConfig{scheme, authority, params}
    , SSHStoreConfigT<config::JustValue>{
        CONFIG_ROW(remoteProgram),
    }
{
}


std::string SSHStoreConfig::doc()
{
    return
      #include "ssh-store.md"
      ;
}

struct SSHStore :
    public virtual SSHStoreConfig,
    public virtual RemoteStore
{
    using Config = SSHStoreConfig;

    SSHStore(const Config & config)
        : Store::Config{config}
        , RemoteStore::Config{config}
        , CommonSSHStoreConfig{config}
        , SSHStore::Config{config}
        , Store{static_cast<const Store::Config &>(*this)}
        , RemoteStore{static_cast<const RemoteStore::Config &>(*this)}
        , master(createSSHMaster(
            // Use SSH master only if using more than 1 connection.
            connections->capacity() > 1))
    {
    }

    std::string getUri() override
    {
        return *uriSchemes().begin() + "://" + host;
    }

    // FIXME extend daemon protocol, move implementation to RemoteStore
    std::optional<std::string> getBuildLogExact(const StorePath & path) override
    { unsupported("getBuildLogExact"); }

protected:

    struct Connection : RemoteStore::Connection
    {
        std::unique_ptr<SSHMaster::Connection> sshConn;

        void closeWrite() override
        {
            sshConn->in.close();
        }
    };

    ref<RemoteStore::Connection> openConnection() override;

    std::string host;

    std::vector<std::string> extraRemoteProgramArgs;

    SSHMaster master;

    void setOptions(RemoteStore::Connection & conn) override
    {
        /* TODO Add a way to explicitly ask for some options to be
           forwarded. One option: A way to query the daemon for its
           settings, and then a series of params to SSHStore like
           forward-cores or forward-overridden-cores that only
           override the requested settings.
        */
    };
};


MountedSSHStoreConfig::MountedSSHStoreConfig(
    std::string_view scheme,
    std::string_view host,
    const StoreReference::Params & params)
    : Store::Config{params}
    , RemoteStore::Config{params}
    , CommonSSHStoreConfig{scheme, host, params}
    , SSHStoreConfig{scheme, host, params}
    , LocalFSStoreConfig{params}
{
}

std::string MountedSSHStoreConfig::doc()
{
    return
      #include "mounted-ssh-store.md"
      ;
}


/**
 * The mounted ssh store assumes that filesystems on the remote host are
 * shared with the local host. This means that the remote nix store is
 * available locally and is therefore treated as a local filesystem
 * store.
 *
 * MountedSSHStore is very similar to UDSRemoteStore --- ignoring the
 * superficial differnce of SSH vs Unix domain sockets, they both are
 * accessing remote stores, and they both assume the store will be
 * mounted in the local filesystem.
 *
 * The difference lies in how they manage GC roots. See addPermRoot
 * below for details.
 */
struct MountedSSHStore : virtual MountedSSHStoreConfig, virtual SSHStore, virtual LocalFSStore
{
    using Config = MountedSSHStoreConfig;

    MountedSSHStore(const Config & config)
        : Store::Config{config}
        , RemoteStore::Config{config}
        , CommonSSHStoreConfig{config}
        , SSHStore::Config{config}
        , LocalFSStore::Config{config}
        , MountedSSHStore::Config{config}
        , Store{static_cast<const Store::Config &>(*this)}
        , RemoteStore{static_cast<const RemoteStore::Config &>(*this)}
        , SSHStore{static_cast<const SSHStore::Config &>(*this)}
        , LocalFSStore{static_cast<const LocalFSStore::Config &>(*this)}
    {
        extraRemoteProgramArgs = {
            "--process-ops",
        };
    }

    std::string getUri() override
    {
        return *uriSchemes().begin() + "://" + host;
    }

    void narFromPath(const StorePath & path, Sink & sink) override
    {
        return LocalFSStore::narFromPath(path, sink);
    }

    ref<SourceAccessor> getFSAccessor(bool requireValidPath) override
    {
        return LocalFSStore::getFSAccessor(requireValidPath);
    }

    std::optional<std::string> getBuildLogExact(const StorePath & path) override
    {
        return LocalFSStore::getBuildLogExact(path);
    }

    /**
     * This is the key difference from UDSRemoteStore: UDSRemote store
     * has the client create the direct root, and the remote side create
     * the indirect root.
     *
     * We could also do that, but the race conditions (will the remote
     * side see the direct root the client made?) seems bigger.
     *
     * In addition, the remote-side will have a process associated with
     * the authenticating user handling the connection (even if there
     * is a system-wide daemon or similar). This process can safely make
     * the direct and indirect roots without there being such a risk of
     * privilege escalation / symlinks in directories owned by the
     * originating requester that they cannot delete.
     */
    Path addPermRoot(const StorePath & path, const Path & gcRoot) override
    {
        auto conn(getConnection());
        conn->to << WorkerProto::Op::AddPermRoot;
        WorkerProto::write(*this, *conn, path);
        WorkerProto::write(*this, *conn, gcRoot);
        conn.processStderr();
        return readString(conn->from);
    }
};

ref<RemoteStore::Connection> SSHStore::openConnection()
{
    auto conn = make_ref<Connection>();
    Strings command = remoteProgram.get();
    command.push_back("--stdio");
    if (remoteStore.get() != "") {
        command.push_back("--store");
        command.push_back(remoteStore.get());
    }
    command.insert(command.end(),
        extraRemoteProgramArgs.begin(), extraRemoteProgramArgs.end());
    conn->sshConn = master.startCommand(std::move(command));
    conn->to = FdSink(conn->sshConn->in.get());
    conn->from = FdSource(conn->sshConn->out.get());
    return conn;
}

static RegisterStoreImplementation<SSHStore> regSSHStore;
static RegisterStoreImplementation<MountedSSHStore> regMountedSSHStore;

}
