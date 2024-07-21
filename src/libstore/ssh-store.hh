#pragma once
///@file

#include "common-ssh-store-config.hh"
#include "store-api.hh"
#include "local-fs-store.hh"
#include "remote-store.hh"

namespace nix {

template<template<typename> class F>
struct SSHStoreConfigT
{
    F<Strings> remoteProgram;
};

struct SSHStoreConfig : std::enable_shared_from_this<SSHStoreConfig>,
                        Store::Config,
                        RemoteStore::Config,
                        CommonSSHStoreConfig,
                        SSHStoreConfigT<config::JustValue>
{
    static config::SettingDescriptionMap descriptions();

    std::optional<LocalFSStore::Config> mounted;

    SSHStoreConfig(std::string_view scheme, std::string_view authority, const StoreReference::Params & params);

    const std::string name() const override;

    static std::set<std::string> uriSchemes()
    {
        return {"ssh-ng"};
    }

    std::string doc() const override;

    std::optional<ExperimentalFeature> experimentalFeature() const override
    {
        return mounted ? std::optional{ExperimentalFeature::MountedSSHStore} : std::nullopt;
    }

    ref<Store> openStore() const override;
};

}
