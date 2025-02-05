#include "nix/cmd/command.hh"
#include "nix/main/common-args.hh"
#include "nix/store/store-api.hh"
#include "nix/util/archive.hh"
#include "nix/util/git.hh"
#include "nix/util/posix-source-accessor.hh"
#include "nix/cmd/misc-store-flags.hh"

using namespace nix;

struct CmdAddToStore : MixDryRun, StoreCommand
{
    Path path;
    std::optional<std::string> namePart;
    ContentAddressMethod caMethod = ContentAddressMethod::Raw::NixArchive;
    HashAlgorithm hashAlgo = HashAlgorithm::SHA256;
    std::vector<std::string> referencesRaw;

    CmdAddToStore()
    {
        // FIXME: completion
        expectArg("path", &path);

        addFlag({
            .longName = "name",
            .shortName = 'n',
            .description = "Override the name component of the store path. It defaults to the base name of *path*.",
            .labels = {"name"},
            .handler = {&namePart},
        });

        addFlag(flag::contentAddressMethod(&caMethod));

        addFlag(flag::hashAlgo(&hashAlgo));

        addFlag({
            .longName = "reference",
            .description = "Make the newly created store object refer to this other store object",
            .labels = {"reference"},
            .handler = {&referencesRaw},
            .experimentalFeature = Xp::DynamicDerivations,
        });
    }

    void run(ref<Store> store) override
    {
        if (!namePart) namePart = baseNameOf(path);

        auto sourcePath = PosixSourceAccessor::createAtRoot(makeParentCanonical(path));

        StorePathSet references;
        for (auto & raw : referencesRaw) {
            references.insert(store->parseStorePath(raw));
        }

        auto storePath = dryRun
            ? store->computeStorePath(
                *namePart, sourcePath, caMethod, hashAlgo, references).first
            : store->addToStoreSlow(
                *namePart, sourcePath, caMethod, hashAlgo, references).path;

        logger->cout("%s", store->printStorePath(storePath));
    }
};

struct CmdAdd : CmdAddToStore
{
    std::string description() override
    {
        return "Add a file or directory to the Nix store";
    }

    std::string doc() override
    {
        return
          #include "add.md"
          ;
    }
};

struct CmdAddFile : CmdAddToStore
{
    CmdAddFile()
    {
        caMethod = ContentAddressMethod::Raw::Flat;
    }

    std::string description() override
    {
        return "Deprecated. Use [`nix store add --mode flat`](@docroot@/command-ref/new-cli/nix3-store-add.md) instead.";
    }
};

struct CmdAddPath : CmdAddToStore
{
    std::string description() override
    {
        return "Deprecated alias to [`nix store add`](@docroot@/command-ref/new-cli/nix3-store-add.md).";
    }
};

static auto rCmdAddFile = registerCommand2<CmdAddFile>({"store", "add-file"});
static auto rCmdAddPath = registerCommand2<CmdAddPath>({"store", "add-path"});
static auto rCmdAdd = registerCommand2<CmdAdd>({"store", "add"});
