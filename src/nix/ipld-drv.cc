#include "command.hh"
#include "store-api.hh"
#include "content-address.hh"
#include "ipfs.hh"
#include "ipfs-binary-cache-store.hh"


using namespace nix;


struct IPLDDerivation : TinyDerivation
{
    std::set<IPFSHash> inputSrcs;
    std::map<IPFSHash, StringSet> inputDrvs;
};


struct CmdIpldDrvImport : StoreCommand
{
    std::string caStr;

    CmdIpldDrvImport()
    {
        expectArg("ca", &caStr);
    }

    std::string description() override
    {
        return "Import the derivation graph identifed by the given CID";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store) override
    {
        auto ca = parseStorePathDescriptor(caStr);

        store->ensurePath(ca);

        std::cout
            << store->printStorePath(store->makeFixedOutputPathFromCA(ca))
            << std::endl;
    }
};


struct CmdIpldDrvExport : StoreCommand
{
    std::string caStr;

    CmdIpldDrvExport()
    {
        expectArg("ca", &caStr);
    }

    std::string description() override
    {
        return "Export the derivation graph rooted by the given path";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store) override
    {
        auto ca = parseStorePathDescriptor(caStr);

        store->ensurePath(ca);

        std::cout
            << store->printStorePath(store->makeFixedOutputPathFromCA(ca))
            << std::endl;
    }
};


struct CmdIpldDrv : virtual MultiCommand, virtual Command
{
    CmdIpldDrv()
        : MultiCommand({
                {"import", []() { return make_ref<CmdIpldDrvImport>(); }},
                {"export", []() { return make_ref<CmdIpldDrvExport>(); }},
            })
    {
    }

    std::string description() override
    {
        return "manage IPLD derivations";
    }

    void run() override
    {
        if (!command)
            throw UsageError("'nix ipld-drv' requires a sub-command.");
        command->second->prepare();
        command->second->run();
    }

    void printHelp(const string & programName, std::ostream & out) override
    {
        MultiCommand::printHelp(programName, out);
    }
};


static auto r = registerCommand<CmdIpldDrv>("ipld-drv");
