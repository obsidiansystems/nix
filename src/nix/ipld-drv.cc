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


struct CmdIpldDrvImport : StorePathCommand
{
    std::string description() override
    {
        return "Import the derivation graph identifed by the given CID";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> localStore, const StorePath & drvPath) override
    {
        auto ipfsStore = std::make_shared<IPFSBinaryCacheStore>({ }, "ipfs://");


        // Recursively read and convert derivation to IPLDDerivation, and export
        //
        //  - read drv path into Derivation
        //
        //  - convert Derivation to IPLDDerivation, two files are different:
        //    - inputSrcs:
        //       - queryPathInfo to insure that inputSrcs are ipfs: or git: (i.e. stuff we can do trustlessly),
        //    - inputDrvs:
        //       - recur
        //
        //  - Serialize IPLDDerivation to JSON and export
        //    - See IPFS store code as guide, especially narinfo export.
        //    - See show-derivation code for example JSON searlization we should match

        //std::cout
        //    << final ipfs CID goes here!;
        //    << std::endl;
    }
};


struct CmdIpldDrvExport : StoreCommand
{
    std::string caStr;

    CmdIpldDrvExport()
    {
        expectArg("cid", &caStr);
    }

    std::string description() override
    {
        return "Export the derivation graph rooted by the given path";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store_) override
    {
        auto ipfsStore = std::make_shared<IPFSBinaryCacheStore>({ }, "ipfs://");

        // Recursively read and convert IPLDDerivation to Derivations

        /// - Where does the derivation initially come from? I think it's caStr
        ///
        /// - When we say "Recursively read", what are the actual tools with
        /// - which I can read chunks of this? Is this like parsing? In that
        /// case, _what's the format_? Maybe the one in show-derivation? nar-info?

        size_t pos = 0;
        while (pos < caStr.size()) {

        }

        //  - read and deserialized CID into IPLDDerivation
        /// But what's a CID, is this a cad? Or is it there an identifier?
        //
        //     - Copy narinfo import code for deserialization
        //
        //  - convert IPLDDerivation to Derivation
        //     - inputSrcs
        //        - add to store, c.f. how we import paths trustlessly in ipfs binary cache
        //     - inputDrvs
        //        - recur
        //  - local store writeDerivation to disk

        //std::cout
        //    << store->printStorePath(final DRV path goe here)
        //    << std::endl;
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
