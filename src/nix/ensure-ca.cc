#include "command.hh"
#include "store-api.hh"
#include "content-address.hh"

using namespace nix;

struct CmdEnsureCA : StoreCommand
{
    std::string caStr;
    std::string name;

    CmdEnsureCA()
    {
        expectArg("ca", &caStr);
        expectArg("name", &name);
    }

    std::string description() override
    {
        return "put the ca in the store";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store) override
    {
        auto ca = parseContentAddress(caStr);

        auto path = store->makeFixedOutputPathFromCA(name, ca);
        store->ensurePath(path, ca);

        std::cout << store->printStorePath(path) << std::endl;;
    }
};

static auto r = registerCommand<CmdEnsureCA>("ensure-ca");
