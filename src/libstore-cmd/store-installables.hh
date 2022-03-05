#pragma once

#include "util.hh"
#include "path.hh"
#include "path-with-outputs.hh"
#include "derived-path.hh"
#include "store-api.hh"

#include <optional>

namespace nix {

struct DrvInfo;

enum class Realise {
    /* Build the derivation. Postcondition: the
       derivation outputs exist. */
    Outputs,
    /* Don't build the derivation. Postcondition: the store derivation
       exists. */
    Derivation,
    /* Evaluate in dry-run mode. Postcondition: nothing. */
    // FIXME: currently unused, but could be revived if we can
    // evaluate derivations in-memory.
    Nothing
};

/* How to handle derivations in commands that operate on store paths. */
enum class OperateOn {
    /* Operate on the output path. */
    Output,
    /* Operate on the .drv path. */
    Derivation
};

struct CoreInstallable
{
    virtual ~CoreInstallable() { }

    virtual std::string what() const = 0;

    virtual DerivedPaths toDerivedPaths() = 0;

    virtual StorePathSet toDrvPaths(ref<Store> store)
    {
        throw Error("'%s' cannot be converted to a derivation path", what());
    }

    DerivedPath toDerivedPath();

    /* Return a value only if this installable is a store path or a
       symlink to it. */
    virtual std::optional<StorePath> getStorePath()
    {
        return {};
    }

    static BuiltPaths build(
        ref<Store> evalStore,
        ref<Store> store,
        Realise mode,
        const std::vector<std::shared_ptr<CoreInstallable>> & installables,
        BuildMode bMode = bmNormal);

    static std::set<StorePath> toStorePaths(
        ref<Store> evalStore,
        ref<Store> store,
        Realise mode,
        OperateOn operateOn,
        const std::vector<std::shared_ptr<CoreInstallable>> & installables);

    static StorePath toStorePath(
        ref<Store> evalStore,
        ref<Store> store,
        Realise mode,
        OperateOn operateOn,
        std::shared_ptr<CoreInstallable> installable);

    static std::set<StorePath> toDerivations(
        ref<Store> store,
        const std::vector<std::shared_ptr<CoreInstallable>> & installables,
        bool useDeriver = false);

    static BuiltPaths toBuiltPaths(
        ref<Store> evalStore,
        ref<Store> store,
        Realise mode,
        OperateOn operateOn,
        const std::vector<std::shared_ptr<CoreInstallable>> & installables);
};

BuiltPaths getBuiltPaths(
    ref<Store> evalStore,
    ref<Store> store,
    const DerivedPaths & hopefullyBuiltPaths);

}
