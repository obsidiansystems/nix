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
    const F<Strings> remoteProgram;
};

struct SSHStoreConfig : virtual RemoteStoreConfig, virtual CommonSSHStoreConfig, SSHStoreConfigT<config::JustValue>
{
    struct Descriptions : virtual RemoteStoreConfig::Descriptions,
                          virtual CommonSSHStoreConfig::Descriptions,
                          SSHStoreConfigT<config::SettingInfo>
    {
        Descriptions();
    };

    static const Descriptions descriptions;

    SSHStoreConfig(std::string_view scheme, std::string_view authority, const StoreReference::Params & params);

    const std::string name() override
    {
        return "Experimental SSH Store";
    }

    static std::set<std::string> uriSchemes()
    {
        return {"ssh-ng"};
    }

    std::string doc() override;

    ref<Store> openStore() const override;
};

struct MountedSSHStoreConfig : virtual SSHStoreConfig, virtual LocalFSStore::Config
{
    struct Descriptions : virtual SSHStoreConfig::Descriptions, virtual LocalFSStore::Config::Descriptions
    {
        Descriptions();
    };

    static const Descriptions descriptions;

    MountedSSHStoreConfig(std::string_view scheme, std::string_view host, const StoreReference::Params & params);

    const std::string name() override
    {
        return "Experimental SSH Store with filesystem mounted";
    }

    static std::set<std::string> uriSchemes()
    {
        return {"mounted-ssh-ng"};
    }

    std::string doc() override;

    std::optional<ExperimentalFeature> experimentalFeature() const override
    {
        return ExperimentalFeature::MountedSSHStore;
    }

    ref<Store> openStore() const override;
};

}
