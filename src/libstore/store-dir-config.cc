#include "store-dir-config.hh"
#include "config-parse-impl.hh"
#include "util.hh"

namespace nix {

const StoreDirConfigT<config::JustValue> storeDirConfigDefaults = {
    ._storeDir = {.value = settings.nixStore},
};

const StoreDirConfigT<config::SettingInfo> storeDirConfigDescriptions = {
    ._storeDir =
        {
            .name = "store",
            .description = R"(
              Logical location of the Nix store, usually
              `/nix/store`. Note that you can only copy store paths
              between stores if they have the same `store` setting.
            )",
        },
};

StoreDirConfigT<config::JustValue> parseStoreDirConfig(const StoreReference::Params & params)
{
    constexpr auto & defaults = storeDirConfigDefaults;
    constexpr auto & descriptions = storeDirConfigDescriptions;

    return {
        CONFIG_ROW(_storeDir),
    };
}

StoreDirConfig::StoreDirConfig(const StoreReference::Params & params)
    : StoreDirConfigT<config::JustValue>{parseStoreDirConfig(params)}
{
}

}
