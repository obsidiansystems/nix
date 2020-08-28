#include "command.hh"
#include "store-api.hh"
#include "content-address.hh"
#include "ipfs.hh"
#include "ipfs-binary-cache-store.hh"


using namespace nix;


struct IPLDDerivation : TinyDerivation
{
    StringSet outputs;
    std::set<IPFSRef> inputSrcs;
    std::map<IPFSRef, StringSet> inputDrvs;
};

void to_json(nlohmann::json& j, const IPLDDerivation & drv) {
    j = nlohmann::json {
        { "name", drv.name },
        { "platform", drv.platform },
        { "builder", drv.builder },
        { "args", drv.args },
        { "env", drv.env },
        { "outputs", drv.outputs },
        { "inputSrcs", drv.inputSrcs },
        { "inputDrvs", drv.inputDrvs },
    };
}

struct CmdIpldDrvExport : StorePathCommand
{
    std::string description() override
    {
        return "Export the derivation graph rooted by the given path";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> localStore, const StorePath & drvPath0) override
    {
        auto ipfsStore = std::make_shared<IPFSBinaryCacheStore>( Store::Params { }, "ipfs://" );

        std::function<IPFSRef(const StorePath &)> convertDerivation = [&](const StorePath & drvPath) -> IPFSRef {
            Derivation drv = localStore->readDerivation(drvPath);

            IPLDDerivation ipldDrv;

            static_cast<TinyDerivation &>(ipldDrv) = drv;

            for (auto & [name, derivationOutput] : drv.outputs) {
                auto drvOutputCAFloating = std::get_if<DerivationOutputCAFloating>(&derivationOutput.output);

                if (!drvOutputCAFloating)
                    throw UsageError("In order to upload a derivation as IPLD the outputs should be content addressed and floating");
                if (!std::get_if<IsIPFS>(&drvOutputCAFloating->method))
                    throw UsageError("In order to upload a derivation as IPLD the outputs should be content addressed as IPFS and floating");
                if (drvOutputCAFloating->hashType != htSHA256)
                    throw UsageError("In order to upload a derivation as IPLD the outputs should have a SHA256 hash");

                ipldDrv.outputs.insert(name);
            }

            auto err  = UsageError("In order to upload a derivation as IPLD the paths it references must be content addressed");
            auto err2 = UsageError("In order to upload a derivation as IPLD the paths it references must be content addressed as IPFS");
            for (auto & inputSource : drv.inputSrcs) {

                auto caOpt = localStore->queryPathInfo(inputSource)->optCA();
                if (!caOpt) throw err;

                auto pref = std::get_if<IPFSHash>(&*caOpt);
                if (!pref) throw err2;

                copyPaths(localStore, ref { ipfsStore }, { drvPath });

                ipldDrv.inputSrcs.insert(IPFSRef {
                    .name = std::string(inputSource.name()),
                    .hash = *pref,
                });
            }

            for (auto & [inputDrvPath, outputs] : drv.inputDrvs) {
                ipldDrv.inputDrvs.insert_or_assign(
                    convertDerivation(inputDrvPath),
                    outputs
                );
            }

            nlohmann::json serializeDrv = ipldDrv;
            string ipfsHash = ipfsStore->putIpfsDag(serializeDrv, "sha2-256");
            return {
                .name = drv.name,
                .hash = IPFSHash::from_string(ipfsHash),
            };
        };

        auto [_, ipfsHash] = convertDerivation(drvPath0);

        std::cout << ipfsHash.to_string() << std::endl;
    }
};


struct CmdIpldDrvImport : StoreCommand
{
    std::string cidStr;

    CmdIpldDrvImport()
    {
        expectArg("cid", &cidStr);
    }

    std::string description() override
    {
        return "Import the derivation graph identifed by the given CID";
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store_) override
    {
        auto ipfsStore = std::make_shared<IPFSBinaryCacheStore>( Store::Params { }, "ipfs://" );

        // Recursively read and convert IPLDDerivation to Derivations

        /// - Where does the derivation initially come from? I think it's cidStr
        ///
        /// - When we say "Recursively read", what are the actual tools with
        /// - which I can read chunks of this? Is this like parsing? In that
        /// case, _what's the format_? Maybe the one in show-derivation? nar-info?

        size_t pos = 0;
        while (pos < cidStr.size()) {

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
                {"export", []() { return make_ref<CmdIpldDrvExport>(); }},
                {"import", []() { return make_ref<CmdIpldDrvImport>(); }},
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
