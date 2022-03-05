#include "store-installables.hh"
#include "installable-store-path.hh"
#include "store-command.hh"
#include "store-api.hh"
#include "build-result.hh"
#include "shared.hh"
#include "url.hh"

#include <regex>
#include <queue>

#include <nlohmann/json.hpp>

namespace nix {

ParseInstallableArgs::ParseInstallableArgs()
{
    addFlag({
        .longName = "derivation",
        .description = "Operate on the store derivation rather than its outputs.",
        .category = installablesCategory,
        .handler = {&operateOn, OperateOn::Derivation},
    });
}

DerivedPath CoreInstallable::toDerivedPath()
{
    auto buildables = toDerivedPaths();
    if (buildables.size() != 1)
        throw Error("installable '%s' evaluates to %d derivations, where only one is expected", what(), buildables.size());
    return std::move(buildables[0]);
}

static StorePath getDeriver(
    ref<Store> store,
    const CoreInstallable & i,
    const StorePath & drvPath)
{
    auto derivers = store->queryValidDerivers(drvPath);
    if (derivers.empty())
        throw Error("'%s' does not have a known deriver", i.what());
    // FIXME: use all derivers?
    return *derivers.begin();
}

std::vector<std::shared_ptr<CoreInstallable>> ParseInstallableArgs::parseCoreInstallables(
    ref<Store> store, std::vector<std::string> ss)
{
    std::vector<std::shared_ptr<CoreInstallable>> result;

    for (auto & s : ss) {
        result.push_back(std::make_shared<CoreInstallableStorePath>(
            CoreInstallableStorePath::parse(store, s)));
    }

    return result;
}

std::shared_ptr<CoreInstallable> ParseInstallableArgs::parseCoreInstallable(
    ref<Store> store, const std::string & installable)
{
    auto installables = parseCoreInstallables(store, {installable});
    assert(installables.size() == 1);
    return installables.front();
}

BuiltPaths getBuiltPaths(ref<Store> evalStore, ref<Store> store, const DerivedPaths & hopefullyBuiltPaths)
{
    BuiltPaths res;
    for (const auto & b : hopefullyBuiltPaths)
        std::visit(
            overloaded{
                [&](const DerivedPath::Opaque & bo) {
                    res.push_back(BuiltPath::Opaque{bo.path});
                },
                [&](const DerivedPath::Built & bfd) {
                    OutputPathMap outputs;
                    auto drv = evalStore->readDerivation(bfd.drvPath);
                    auto outputHashes = staticOutputHashes(*evalStore, drv); // FIXME: expensive
                    auto drvOutputs = drv.outputsAndOptPaths(*store);
                    for (auto & output : bfd.outputs) {
                        if (!outputHashes.count(output))
                            throw Error(
                                "the derivation '%s' doesn't have an output named '%s'",
                                store->printStorePath(bfd.drvPath), output);
                        if (settings.isExperimentalFeatureEnabled(Xp::CaDerivations)) {
                            auto outputId =
                                DrvOutput{outputHashes.at(output), output};
                            auto realisation =
                                store->queryRealisation(outputId);
                            if (!realisation)
                                throw Error(
                                    "cannot operate on an output of unbuilt "
                                    "content-addressed derivation '%s'",
                                    outputId.to_string());
                            outputs.insert_or_assign(
                                output, realisation->outPath);
                        } else {
                            // If ca-derivations isn't enabled, assume that
                            // the output path is statically known.
                            assert(drvOutputs.count(output));
                            assert(drvOutputs.at(output).second);
                            outputs.insert_or_assign(
                                output, *drvOutputs.at(output).second);
                        }
                    }
                    res.push_back(BuiltPath::Built{bfd.drvPath, outputs});
                },
            },
            b.raw());

    return res;
}

BuiltPaths CoreInstallable::build(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode,
    const std::vector<std::shared_ptr<CoreInstallable>> & installables,
    BuildMode bMode)
{
    if (mode == Realise::Nothing)
        settings.readOnlyMode = true;

    std::vector<DerivedPath> pathsToBuild;

    for (auto & i : installables) {
        auto b = i->toDerivedPaths();
        pathsToBuild.insert(pathsToBuild.end(), b.begin(), b.end());
    }

    switch (mode) {
    case Realise::Nothing:
    case Realise::Derivation:
        printMissing(store, pathsToBuild, lvlError);
        return getBuiltPaths(evalStore, store, pathsToBuild);
    case Realise::Outputs: {
        BuiltPaths res;
        for (auto & buildResult : store->buildPathsWithResults(pathsToBuild, bMode, evalStore)) {
            if (!buildResult.success())
                buildResult.rethrow();
            std::visit(overloaded {
                [&](const DerivedPath::Built & bfd) {
                    std::map<std::string, StorePath> outputs;
                    for (auto & path : buildResult.builtOutputs)
                        outputs.emplace(path.first.outputName, path.second.outPath);
                    res.push_back(BuiltPath::Built { bfd.drvPath, outputs });
                },
                [&](const DerivedPath::Opaque & bo) {
                    res.push_back(BuiltPath::Opaque { bo.path });
                },
            }, buildResult.path.raw());
        }
        return res;
    }
    default:
        assert(false);
    }
}

BuiltPaths CoreInstallable::toBuiltPaths(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode,
    OperateOn operateOn,
    const std::vector<std::shared_ptr<CoreInstallable>> & installables)
{
    if (operateOn == OperateOn::Output)
        return CoreInstallable::build(evalStore, store, mode, installables);
    else {
        if (mode == Realise::Nothing)
            settings.readOnlyMode = true;

        BuiltPaths res;
        for (auto & drvPath : CoreInstallable::toDerivations(store, installables, true))
            res.push_back(BuiltPath::Opaque{drvPath});
        return res;
    }
}

StorePathSet CoreInstallable::toStorePaths(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode, OperateOn operateOn,
    const std::vector<std::shared_ptr<CoreInstallable>> & installables)
{
    StorePathSet outPaths;
    for (auto & path : toBuiltPaths(evalStore, store, mode, operateOn, installables)) {
        auto thisOutPaths = path.outPaths();
        outPaths.insert(thisOutPaths.begin(), thisOutPaths.end());
    }
    return outPaths;
}

StorePath CoreInstallable::toStorePath(
    ref<Store> evalStore,
    ref<Store> store,
    Realise mode, OperateOn operateOn,
    std::shared_ptr<CoreInstallable> installable)
{
    auto paths = toStorePaths(evalStore, store, mode, operateOn, {installable});

    if (paths.size() != 1)
        throw Error("argument '%s' should evaluate to one store path", installable->what());

    return *paths.begin();
}

StorePathSet CoreInstallable::toDerivations(
    ref<Store> store,
    const std::vector<std::shared_ptr<CoreInstallable>> & installables,
    bool useDeriver)
{
    StorePathSet drvPaths;

    for (const auto & i : installables)
        for (const auto & b : i->toDerivedPaths())
            std::visit(overloaded {
                [&](const DerivedPath::Opaque & bo) {
                    if (!useDeriver)
                        throw Error("argument '%s' did not evaluate to a derivation", i->what());
                    drvPaths.insert(getDeriver(store, *i, bo.path));
                },
                [&](const DerivedPath::Built & bfd) {
                    drvPaths.insert(bfd.drvPath);
                },
            }, b.raw());

    return drvPaths;
}

CoreInstallablesCommand::CoreInstallablesCommand()
{
    expectArgs({
        .label = "installables",
        .handler = {&_installables},
        .completer = {[&](size_t, std::string_view prefix) {
            completeCoreInstallable(prefix);
        }}
    });
}

void CoreInstallablesCommand::prepare()
{
    if (_installables.empty() && useDefaultInstallables())
        // FIXME: commands like "nix install" should not have a
        // default, probably.
        _installables.push_back(".");
    coreInstallables = parseCoreInstallables(getStore(), _installables);
}

CoreInstallableCommand::CoreInstallableCommand()
{
    expectArgs({
        .label = "installable",
        .optional = true,
        .handler = {&_installable},
        .completer = {[&](size_t, std::string_view prefix) {
            completeCoreInstallable(prefix);
        }}
    });
}

void CoreInstallableCommand::prepare()
{
    coreInstallable = parseCoreInstallable(getStore(), _installable);
}

}
