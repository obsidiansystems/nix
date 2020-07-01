#include "command.hh"
#include "store-api.hh"
#include "content-address.hh"

using namespace nix;

struct CmdEnsureCA : StoreCommand
{
    std::string caStr;

    CmdEnsureCA()
    {
        expectArg("ca", &caStr);
    }

    std::string description() override
    {
        return "put the ca into the store";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store) override
    {
        auto ca = parseContentAddress(caStr);

        auto path = store->makeFixedOutputPathFromCA(ca);
        store->ensurePath(path, ca);

        std::cout << store->printStorePath(path) << std::endl;
    }
};

static auto r = registerCommand<CmdEnsureCA>("ensure-ca");
