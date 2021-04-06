#include "derived-path.hh"
#include "store-api.hh"

#include <nlohmann/json.hpp>

namespace nix {

nlohmann::json DerivedPath::Opaque::toJSON(ref<Store> store) const
{
    nlohmann::json res;
    res["path"] = store->printStorePath(path);
    return res;
}

nlohmann::json DerivedPathWithHints::Built::toJSON(ref<Store> store) const
{
    nlohmann::json res;
    res["drvPath"] = store->printStorePath(drvPath);
    for (const auto& [output, path] : outputs) {
        res["outputs"][output] = path ? store->printStorePath(*path) : "";
    }
    return res;
}

nlohmann::json derivedPathsWithHintsToJSON(const DerivedPathsWithHints & buildables, ref<Store> store)
{
    auto res = nlohmann::json::array();
    for (const DerivedPathWithHints & buildable : buildables) {
        std::visit([&res, store](const auto & buildable) {
            res.push_back(buildable.toJSON(store));
        }, buildable.raw());
    }
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
        outputs = tokenizeString<std::set<string>>(outputsS);
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

}
