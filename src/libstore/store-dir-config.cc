#include "store-dir-config.hh"
#include "config-parse.hh"
#include "util.hh"

namespace nix {

const StoreDirConfigT<JustValue> storeDirConfigDefaults = {
    .storeDir =
        {
            .value = settings.nixStore,
        },
};

const StoreDirConfigT<SettingInfo> storeDirConfigDescriptions = {
    .storeDir =
        {
            .name = "store",
            .description = R"(
              Logical location of the Nix store, usually
              `/nix/store`. Note that you can only copy store paths
              between stores if they have the same `store` setting.
            )",
        },
};

StoreDirConfigT<JustValue> parseStoreDirConfig(const StoreReference::Params & params)
{
    constexpr auto & defaults = storeDirConfigDefaults;
    constexpr auto & descriptions = storeDirConfigDescriptions;

    return {
        CONFIG_ROW(storeDir),
    };
}

StoreDirConfig::StoreDirConfig(const StoreReference::Params & params)
    : StoreDirConfigT<JustValue>{parseStoreDirConfig(params)}
{
}

}
