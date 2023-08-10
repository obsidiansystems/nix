#include "uds-remote-store.hh"
#include "pool.hh"
#include "worker-protocol.hh"

namespace nix {

struct BuildInterposeUDSStoreConfig : virtual UDSRemoteStoreConfig
{
    BuildInterposeUDSStoreConfig(const Store::Params & params)
        : StoreConfig(params)
        , LocalFSStoreConfig(params)
        , RemoteStoreConfig(params)
        , UDSRemoteStoreConfig(params)
    { }

    const Setting<int> maxInterposeConnections{(StoreConfig*) this, 1,
            "max-interpose-connections", "maximum number of concurrent connections to the build daemon"};

    const std::string name() override { return UDSRemoteStoreConfig::name() + " with building interposed"; }

    const PathSetting buildInterposeSocket{(StoreConfig*) this, true, "",
        "build-interpose-socket", "Socket for logging builds"
    };
};

struct BuildInterposeUDSStore : virtual BuildInterposeUDSStoreConfig, virtual UDSRemoteStore
{
    static std::set<std::string> uriSchemes() { return {"build-interpose-unix"}; }

    struct InterposeConnection;

    ref<Pool<InterposeConnection>> interposeConnections;

    BuildInterposeUDSStore(
        const std::string & uriScheme,
        const std::string & ignore,
        const Params & params);

    void buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode, std::shared_ptr<Store> evalStore) override;

#if 0
    std::vector<BuildResult> buildPathsWithResults(
        const std::vector<DerivedPath> & paths,
        BuildMode buildMode,
        std::shared_ptr<Store> evalStore) override;

    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv,
        BuildMode buildMode) override;

    void ensurePath(const StorePath & path) override;

    void repairPath(const StorePath & path) override;
#endif
};

struct BuildInterposeUDSStore::InterposeConnection
{
    AutoCloseFD fd;
    FdSink to;
    FdSource from;

    InterposeConnection(AutoCloseFD && fd0)
    : fd(std::move(fd0))
    {
        to.fd = fd.get();
        from.fd = fd.get();
    }
};

BuildInterposeUDSStore::BuildInterposeUDSStore(
    const std::string & uriScheme,
    const std::string & ignore,
    const Params & params)
    : StoreConfig(params)
    , LocalFSStoreConfig(params)
    , RemoteStoreConfig(params)
    , UDSRemoteStoreConfig(params)
    , BuildInterposeUDSStoreConfig(params)
    , Store(params)
    , LocalFSStore(params)
    , RemoteStore(params)
    , UDSRemoteStore(params)
    , interposeConnections(make_ref<Pool<InterposeConnection>>(
        1, //std::max(1, (int) maxInterposeConnections),
        [this]() {
            AutoCloseFD socket = createUnixDomainSocket();
            nix::connect(socket.get(), buildInterposeSocket);
            return make_ref<InterposeConnection>(std::move(socket));
        },
        [](const ref<InterposeConnection> & r) {
            return
                r->to.good()
                && r->from.good();
        }
        ))
{
    if (ignore != "")
        throw Error("Invalid URL");
}

void BuildInterposeUDSStore::buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode, std::shared_ptr<Store> evalStore)
{
    auto conn(interposeConnections->get());
    conn->to << "Build interposer";
    worker_proto::write(*this, conn->to, reqs);
    conn->to.flush();
    auto res = readNum<bool>(conn->from);
    if (!res) {
        std::string msg;
        for (auto & req : reqs) {
            msg += " ";
            msg += req.to_string(*this);
        }
        throw Error("Build interposer Failed to Build%s", msg);
    }
    UDSRemoteStore::buildPaths(reqs, buildMode, evalStore);
}

static RegisterStoreImplementation<BuildInterposeUDSStore, BuildInterposeUDSStoreConfig> regBuildInterposeUDSStore;

}
