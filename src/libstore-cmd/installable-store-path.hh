#pragma once

#include "store-installables.hh"

namespace nix {

struct CoreInstallableStorePath : public virtual CoreInstallable
{
    ref<Store> store;
    StorePath storePath;

    CoreInstallableStorePath(ref<Store> store, StorePath && storePath)
        : store(store), storePath(std::move(storePath)) { }

    std::string what() const override;

    DerivedPaths toDerivedPaths() override;

    StorePathSet toDrvPaths(ref<Store> store) override;

    std::optional<StorePath> getStorePath() override;

    static CoreInstallableStorePath parse(
        ref<Store> store, std::string_view s);
};

BuiltPaths getBuiltPaths(
    ref<Store> evalStore,
    ref<Store> store,
    const DerivedPaths & hopefullyBuiltPaths);

}
