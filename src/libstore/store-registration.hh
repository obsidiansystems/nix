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
    std::function<std::shared_ptr<Store>(
        std::string_view scheme, std::string_view authorityPath, const Store::Params & params)>
        create;
    std::function<std::shared_ptr<StoreConfig>()> getConfig;
};

struct Implementations
{
    static std::vector<StoreFactory> * registered;

    template<typename T, typename TConfig>
    static void add()
    {
        if (!registered)
            registered = new std::vector<StoreFactory>();
        StoreFactory factory{
            .uriSchemes = TConfig::uriSchemes(),
            .create = ([](auto scheme, auto uri, auto & params) -> std::shared_ptr<Store> {
                return std::make_shared<T>(scheme, uri, params);
            }),
            .getConfig = ([]() -> std::shared_ptr<StoreConfig> { return std::make_shared<TConfig>(StringMap({})); })};
        registered->push_back(factory);
    }
};

template<typename T, typename TConfig>
struct RegisterStoreImplementation
{
    RegisterStoreImplementation()
    {
        Implementations::add<T, TConfig>();
    }
};

}
