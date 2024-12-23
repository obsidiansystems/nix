#include "local-store.hh"
#include "pool.hh"
#include "worker-protocol.hh"

namespace nix {

struct BuildInterposeStoreConfig : virtual LocalStoreConfig
{
    BuildInterposeStoreConfig(const Store::Params & params)
        : StoreConfig(params)
        , LocalFSStoreConfig(params)
        , LocalStoreConfig(params)
    { }

    const Setting<int> maxConnections{(StoreConfig*) this, 1,
            "max-connections", "maximum number of concurrent connections to the build daemon"};

    const std::string name() override { return LocalStoreConfig::name() + " with building interposed"; }

    const PathSetting buildInterposeSocket{(StoreConfig*) this, true, "",
        "build-interpose-socket", "Socket for logging builds"
    };
};

struct BuildInterposeStore : virtual BuildInterposeStoreConfig, virtual LocalStore
{
    static std::set<std::string> uriSchemes() { return {"build-interpose"}; }

    using LocalStore::LocalStore;

    struct Connection;

    ref<Pool<Connection>> connections;

    BuildInterposeStore(
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

struct BuildInterposeStore::Connection
{
    AutoCloseFD fd;
    FdSink to;
    FdSource from;

    Connection(AutoCloseFD && fd0)
    : fd(std::move(fd0))
    {
        to.fd = fd.get();
        from.fd = fd.get();
    }
};

BuildInterposeStore::BuildInterposeStore(
    const std::string & uriScheme,
    const std::string & ignore,
    const Params & params)
    : StoreConfig(params)
    , LocalFSStoreConfig(params)
    , LocalStoreConfig(params)
    , BuildInterposeStoreConfig(params)
    , Store(params)
    , LocalFSStore(params)
    , LocalStore(params)
    , connections(make_ref<Pool<Connection>>(
        1, //std::max(1, (int) maxConnections),
        [this]() {
            AutoCloseFD socket = createUnixDomainSocket();
            nix::connect(socket.get(), buildInterposeSocket);
            return make_ref<Connection>(std::move(socket));
        },
        [](const ref<Connection> & r) {
            return
                r->to.good()
                && r->from.good();
        }
        ))
{
    if (ignore != "")
        throw Error("Invalid URL");
}

void BuildInterposeStore::buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode, std::shared_ptr<Store> evalStore)
{
    auto conn(connections->get());
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
    LocalStore::buildPaths(reqs, buildMode, evalStore);
}

static RegisterStoreImplementation<BuildInterposeStore, BuildInterposeStoreConfig> regBuildInterposeStore;

}
