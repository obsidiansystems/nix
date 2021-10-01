#include "derived-path.hh"
#include "store-api.hh"

#include <nlohmann/json.hpp>

namespace nix {

nlohmann::json DerivedPath::Opaque::toJSON(const Store & store) const
{
    nlohmann::json res;
    res["path"] = store.printStorePath(path);
    return res;
}

static void setOutputs(const Store & store, nlohmann::json & res, const std::pair<std::string, StorePath> & output)
{
    auto & [outputName, outputPath] = output;
    res["output"] = outputName;
    res["outputPath"] = store.printStorePath(outputPath);
}

static void setOutputs(const Store & store, nlohmann::json & res, const std::map<std::string, StorePath> & outputs)
{
    for (const auto & [outputName, outputPath] : outputs) {
        res["outputs"][outputName] = store.printStorePath(outputPath);
    }
}

nlohmann::json BuiltPath::Built::toJSON(const Store & store) const
{
    nlohmann::json res;
    res["drvPath"] = drvPath->toJSON(store);
    setOutputs(store, res, outputs);
    return res;
}

nlohmann::json SingleBuiltPath::Built::toJSON(const Store & store) const
{
    nlohmann::json res;
    res["drvPath"] = drvPath->toJSON(store);
    setOutputs(store, res, outputs);
    return res;
}

StorePath SingleBuiltPath::outPath() const
{
    return std::visit(
        overloaded{
            [](const SingleBuiltPath::Opaque & p) { return p.path; },
            [](const SingleBuiltPath::Built & b) { return b.outputs.second; },
        }, raw()
    );
}

StorePathSet BuiltPath::outPaths() const
{
    return std::visit(
        overloaded{
            [](const BuiltPath::Opaque & p) { return StorePathSet{p.path}; },
            [](const BuiltPath::Built & b) {
                StorePathSet res;
                for (auto & [_, path] : b.outputs)
                    res.insert(path);
                return res;
            },
        }, raw()
    );
}

nlohmann::json SingleBuiltPath::toJSON(const Store & store) const
{
    return std::visit([&](const auto & buildable) {
        return buildable.toJSON(store);
    }, raw());
}

nlohmann::json BuiltPath::toJSON(const Store & store) const
{
    return std::visit([&](const auto & buildable) {
        return buildable.toJSON(store);
    }, raw());
}

nlohmann::json derivedPathsWithHintsToJSON(const BuiltPaths & buildables, const Store & store)
{
    auto res = nlohmann::json::array();
    for (const BuiltPath & buildable : buildables)
        res.push_back(buildable.toJSON(store));
    return res;
}


std::string DerivedPath::Opaque::to_string(const Store & store) const
{
    return store.printStorePath(path);
}

std::string SingleDerivedPath::Built::to_string(const Store & store) const
{
    return drvPath->to_string(store) + "!" + outputs;
}

std::string DerivedPath::Built::to_string(const Store & store) const
{
    return drvPath->to_string(store)
        + "!"
        + (outputs.empty() ? std::string { "*" } : concatStringsSep(",", outputs));
}

std::string SingleDerivedPath::to_string(const Store & store) const
{
    return std::visit(
        [&](const auto & req) { return req.to_string(store); },
        raw());
}

std::string DerivedPath::to_string(const Store & store) const
{
    return std::visit(
        [&](const auto & req) { return req.to_string(store); },
        this->raw());
}


DerivedPath SingleDerivedPath::to_multi() const
{
    return std::visit(overloaded {
        [&](const SingleDerivedPath::Opaque & bo) -> DerivedPath {
            return bo;
        },
        [&](const SingleDerivedPath::Built & bfd) -> DerivedPath {
            return DerivedPath::Built { bfd.drvPath, { bfd.outputs } };
        },
    }, this->raw());
}


DerivedPath::Opaque DerivedPath::Opaque::parse(const Store & store, std::string_view s)
{
    return {store.parseStorePath(s)};
}

SingleDerivedPath::Built SingleDerivedPath::Built::parse(const Store & store, std::string_view drvS, std::string_view output)
{
    auto drvPath = std::make_shared<SingleDerivedPath>(
        SingleDerivedPath::parse(store, drvS));
    return { std::move(drvPath), std::string { output } };
}

DerivedPath::Built DerivedPath::Built::parse(const Store & store, std::string_view drvS, std::string_view outputsS)
{
    auto drvPath = std::make_shared<SingleDerivedPath>(
        SingleDerivedPath::parse(store, drvS));
    std::set<string> outputs;
    if (outputsS != "*")
        outputs = tokenizeString<std::set<string>>(outputsS, ",");
    return { std::move(drvPath), std::move(outputs) };
}

SingleDerivedPath SingleDerivedPath::parse(const Store & store, std::string_view s)
{
    size_t n = s.rfind("!");
    return n == s.npos
        ? (SingleDerivedPath) DerivedPath::Opaque::parse(store, s)
        : (SingleDerivedPath) SingleDerivedPath::Built::parse(store, s.substr(0, n), s.substr(n + 1));
}

DerivedPath DerivedPath::parse(const Store & store, std::string_view s)
{
    size_t n = s.rfind("!");
    return n == s.npos
        ? (DerivedPath) DerivedPath::Opaque::parse(store, s)
        : (DerivedPath) DerivedPath::Built::parse(store, s.substr(0, n), s.substr(n + 1));
}

RealisedPath::Set BuiltPath::toRealisedPaths(Store & store) const
{
    RealisedPath::Set res;
    std::visit(
        overloaded{
            [&](const BuiltPath::Opaque & p) { res.insert(p.path); },
            [&](const BuiltPath::Built & p) {
                auto drvHashes =
                    staticOutputHashes(store, store.readDerivation(p.drvPath->outPath()));
                for (auto& [outputName, outputPath] : p.outputs) {
                    if (settings.isExperimentalFeatureEnabled(
                            "ca-derivations")) {
                        auto thisRealisation = store.queryRealisation(
                            DrvOutput{drvHashes.at(outputName), outputName});
                        assert(thisRealisation);  // We’ve built it, so we must h
                                                  // ve the realisation
                        res.insert(*thisRealisation);
                    } else {
                        res.insert(outputPath);
                    }
                }
            },
        },
        raw());
    return res;
}
}
