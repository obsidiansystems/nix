#include "command.hh"
#include "store-api.hh"
#include "content-address.hh"

using namespace nix;

struct CmdEnsureCid : StoreCommand
{
    std::string cid;
    std::string name;

    CmdEnsureCid()
    {
        expectArg("cid", &cid);
        expectArg("name", &name);
    }

    std::string description() override
    {
        return "put the cid into the store";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store) override
    {
        ContentAddress ca {
            .name = name,
            .info = IPFSCid {
                .cid = cid
            }
        };

        store->ensurePath(ca);

        std::cout
            << store->printStorePath(store->makeFixedOutputPathFromCA(ca))
            << std::endl;
    }
};

static auto r1 = registerCommand<CmdEnsureCid>("ensure-cid");

struct CmdGetCid : StorePathsCommand
{
    CmdGetCid()
    {
    }

    std::string description() override
    {
        return "get the cid of a path";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store, std::vector<StorePath> storePaths) override
    {
        for (auto & storePath : storePaths) {
            auto info = store->queryPathInfo(storePath)->fullContentAddressOpt(*store);
            if (!info)
                throw Error("path '%s' is not content addressed", store->printStorePath(storePath));
            auto cid = computeIPFSCid(*info);
            std::cout << cid << std::endl;
        }
    }
};

static auto r2 = registerCommand<CmdGetCid>("get-cid");
