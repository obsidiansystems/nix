#pragma once
/**
 * @file
 *
 * Infrastructure for "registering" store implementations. Used by the
 * store implementation definitions themselves but not by consumers of
 * those implementations.
 */

#include "store-api.hh"

namespace nix {

struct StoreFactory
{
    std::string doc;
    std::set<std::string> uriSchemes;
    config::SettingDescriptionMap configDescriptions;
    std::optional<ExperimentalFeature> experimentalFeature;
    /**
     * The `authorityPath` parameter is `<authority>/<path>`, or really
     * whatever comes after `<scheme>://` and before `?<query-params>`.
     */
    std::function<ref<StoreConfig>(
        std::string_view scheme, std::string_view authorityPath, const StoreReference::Params & params)>
        parseConfig;
};

struct Implementations
{
private:

    using V = std::vector<std::pair<std::string, StoreFactory>>;

public:

    static V * registered;

    template<typename TConfig>
    static void add()
    {
        if (!registered)
            registered = new V{};
        StoreFactory factory{
            .doc = TConfig::doc(),
            .uriSchemes = TConfig::uriSchemes(),
            .configDescriptions = TConfig::descriptions(),
            .experimentalFeature = TConfig::experimentalFeature(),
            .parseConfig = ([](auto scheme, auto uri, auto & params) -> ref<StoreConfig> {
                return make_ref<TConfig>(scheme, uri, params);
            }),
        };
        registered->push_back({TConfig::name(), std::move(factory)});
    }
};

template<typename TConfig>
struct RegisterStoreImplementation
{
    RegisterStoreImplementation()
    {
        Implementations::add<TConfig>();
    }
};

}
