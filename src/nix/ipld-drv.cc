#include "command.hh"
#include "store-api.hh"
#include "content-address.hh"
#include "ipfs.hh"
#include "split.hh"
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

void from_json(nlohmann::json& j, IPLDDerivation & drv) {
    j.at("name").get_to(drv.name);
    j.at("platform").get_to(drv.platform);
    j.at("builder").get_to(drv.builder);
    j.at("args").get_to(drv.args);
    j.at("env").get_to(drv.env);
    j.at("outputs").get_to(drv.outputs);
    j.at("inputSrcs").get_to(drv.inputSrcs);
    j.at("inputDrvs").get_to(drv.inputDrvs);
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

                copyPaths(localStore, ref { ipfsStore }, { inputSource });

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
            std::string ipfsHashWithPrefix = ipfsStore->putIpfsDag(serializeDrv, "sha2-256");

            std::string_view ipfsHash { ipfsHashWithPrefix };
            assert(splitPrefix(ipfsHash, "/ipfs/"));
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

    void run(ref<Store> store) override
    {
        auto ipfsStore = std::make_shared<IPFSBinaryCacheStore>( Store::Params { }, "ipfs://" );

        std::function<StorePath (const IPFSHash &)> convertDerivation = [&](const IPFSHash & ipfsHash) -> StorePath {

            nlohmann::json jsonValue = ipfsStore->getIpfsDag(ipfsHash.to_string());

            IPLDDerivation ipldDrv;
            from_json(jsonValue, ipldDrv);

            Derivation drv;

            static_cast<TinyDerivation &>(drv) = ipldDrv;

            for (auto name : ipldDrv.outputs) {
                drv.outputs.insert_or_assign(
                    name,
                    DerivationOutput { DerivationOutputCAFloating { IsIPFS { } , htSHA256 } }
                );
            }

            for (IPFSRef inputSource : ipldDrv.inputSrcs) {
                StorePath storePath = store->makeFixedOutputPathFromCA(StorePathDescriptor {
                    inputSource.name,
                    inputSource.hash,
                });
                drv.inputSrcs.insert(storePath);
            }

            for (auto & [inputDrvPath, outputs] : ipldDrv.inputDrvs) {
                drv.inputDrvs.insert_or_assign(
                    convertDerivation(inputDrvPath.hash),
                    outputs
                );
            }

            return writeDerivation(*store, drv);

        };

        auto finalStorePath = convertDerivation(IPFSHash::from_string(cidStr));
        std::cout
           << store->printStorePath(finalStorePath)
           << std::endl;
    }
};


struct CmdIpldDrv : NixMultiCommand
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
