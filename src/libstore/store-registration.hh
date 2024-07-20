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
    std::set<std::string> uriSchemes;
    /**
     * The `authorityPath` parameter is `<authority>/<path>`, or really
     * whatever comes after `<scheme>://` and before `?<query-params>`.
     */
    std::function<ref<StoreConfig>(
        std::string_view scheme, std::string_view authorityPath, const StoreReference::Params & params)>
        parseConfig;
    config::SettingDescriptionMap configDescriptions;
};

struct Implementations
{
    static std::vector<StoreFactory> * registered;

    template<typename TConfig>
    static void add()
    {
        if (!registered)
            registered = new std::vector<StoreFactory>();
        StoreFactory factory{
            .uriSchemes = TConfig::uriSchemes(),
            .parseConfig = ([](auto scheme, auto uri, auto & params) -> ref<StoreConfig> {
                return make_ref<TConfig>(scheme, uri, params);
            }),
            .configDescriptions = TConfig::descriptions(),
        };
        registered->push_back(factory);
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
