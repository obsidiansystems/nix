#pragma once

#include "store-installables.hh"
#include "eval.hh"
#include "flake/flake.hh"

#include <optional>

namespace nix {

struct DrvInfo;
struct SourceExprCommand;

namespace eval_cache { class EvalCache; class AttrCursor; }

struct App
{
    std::vector<StorePathWithOutputs> context;
    Path program;
    // FIXME: add args, sandbox settings, metadata, ...
};

struct UnresolvedApp
{
    App unresolved;
    App resolve(ref<Store> evalStore, ref<Store> store);
};

struct Installable : public virtual CoreInstallable
{
    virtual ~Installable() { }

    UnresolvedApp toApp(EvalState & state);

    virtual std::pair<Value *, Pos> toValue(EvalState & state)
    {
        throw Error("argument '%s' cannot be evaluated", what());
    }

    virtual std::vector<std::pair<std::shared_ptr<eval_cache::AttrCursor>, std::string>>
    getCursors(EvalState & state);

    std::pair<std::shared_ptr<eval_cache::AttrCursor>, std::string>
    getCursor(EvalState & state);

    virtual FlakeRef nixpkgsFlakeRef() const
    {
        return FlakeRef::fromAttrs({{"type","indirect"}, {"id", "nixpkgs"}});
    }
};

struct InstallableValue : Installable
{
    ref<EvalState> state;

    InstallableValue(ref<EvalState> state) : state(state) {}

    struct DerivationInfo
    {
        StorePath drvPath;
        std::string outputName;
    };

    virtual std::vector<DerivationInfo> toDerivations() = 0;

    DerivedPaths toDerivedPaths() override;

    StorePathSet toDrvPaths(ref<Store> store) override;
};

struct InstallableFlake : InstallableValue
{
    FlakeRef flakeRef;
    Strings attrPaths;
    Strings prefixes;
    const flake::LockFlags & lockFlags;
    mutable std::shared_ptr<flake::LockedFlake> _lockedFlake;

    InstallableFlake(
        SourceExprCommand * cmd,
        ref<EvalState> state,
        FlakeRef && flakeRef,
        std::string_view fragment,
        Strings attrPaths,
        Strings prefixes,
        const flake::LockFlags & lockFlags);

    std::string what() const override { return flakeRef.to_string() + "#" + *attrPaths.begin(); }

    std::vector<std::string> getActualAttrPaths();

    Value * getFlakeOutputs(EvalState & state, const flake::LockedFlake & lockedFlake);

    std::tuple<std::string, FlakeRef, DerivationInfo> toDerivation();

    std::vector<DerivationInfo> toDerivations() override;

    std::pair<Value *, Pos> toValue(EvalState & state) override;

    std::vector<std::pair<std::shared_ptr<eval_cache::AttrCursor>, std::string>>
    getCursors(EvalState & state) override;

    std::shared_ptr<flake::LockedFlake> getLockedFlake() const;

    FlakeRef nixpkgsFlakeRef() const override;
};

ref<eval_cache::EvalCache> openEvalCache(
    EvalState & state,
    std::shared_ptr<flake::LockedFlake> lockedFlake);

}
