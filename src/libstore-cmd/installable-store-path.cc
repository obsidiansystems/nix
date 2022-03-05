#include "installable-store-path.hh"
#include "derivations.hh"

namespace nix {

// FIXME copied
static StorePath getDeriver(
    ref<Store> store,
    const CoreInstallableStorePath & i,
    const StorePath & drvPath)
{
    auto derivers = store->queryValidDerivers(drvPath);
    if (derivers.empty())
        throw Error("'%s' does not have a known deriver", i.what());
    // FIXME: use all derivers?
    return *derivers.begin();
}

std::string CoreInstallableStorePath::what() const
{
    return store->printStorePath(storePath);
}

DerivedPaths CoreInstallableStorePath::toDerivedPaths()
{
    if (storePath.isDerivation()) {
        auto drv = store->readDerivation(storePath);
        return {
            DerivedPath::Built {
                .drvPath = storePath,
                .outputs = drv.outputNames(),
            }
        };
    } else {
        return {
            DerivedPath::Opaque {
                .path = storePath,
            }
        };
    }
}

StorePathSet CoreInstallableStorePath::toDrvPaths(ref<Store> store)
{
    if (storePath.isDerivation()) {
        return {storePath};
    } else {
        return {getDeriver(store, *this, storePath)};
    }
}

std::optional<StorePath> CoreInstallableStorePath::getStorePath()
{
    return storePath;
}

CoreInstallableStorePath CoreInstallableStorePath::parse(ref<Store> store, std::string_view s)
{
    return CoreInstallableStorePath { store, store->followLinksToStorePath(s) };
}

}
