#pragma once

#include "store-installables.hh"
#include "args.hh"
#include "path.hh"

#include <optional>

namespace nix {

extern std::string programPath;

extern char * * savedArgv;

class Store;

static constexpr Command::Category catSecondary = 100;
static constexpr Command::Category catUtility = 101;
static constexpr Command::Category catNixInstallation = 102;

static constexpr auto installablesCategory = "Options that change the interpretation of installables";

struct NixMultiCommand : virtual MultiCommand, virtual Command
{
    nlohmann::json toJSON() override;
};

/* A command that requires a Nix store. */
struct StoreCommand : virtual Command
{
    StoreCommand();
    void run() override;
    ref<Store> getStore();
    virtual ref<Store> createStore();
    virtual void run(ref<Store>) = 0;

private:
    std::shared_ptr<Store> _store;
};

/* A command that copies something between `--from` and `--to`
   stores. */
struct CopyCommand : virtual StoreCommand
{
    std::string srcUri, dstUri;

    CopyCommand();

    ref<Store> createStore() override;

    ref<Store> getDstStore();
};

struct DrvCommand : virtual StoreCommand
{
    DrvCommand() = default;

    ~DrvCommand() = default;

    virtual ref<Store> getDrvStore();

protected:
    std::shared_ptr<Store> drvStore;
};

struct ParseInstallableArgs : virtual Args, virtual DrvCommand
{
    // FIXME: move this; not all commands (e.g. 'nix run') use it.
    OperateOn operateOn = OperateOn::Output;

    ParseInstallableArgs();

    virtual std::vector<std::shared_ptr<CoreInstallable>> parseCoreInstallables(
        ref<Store> store, std::vector<std::string> ss);

    virtual std::shared_ptr<CoreInstallable> parseCoreInstallable(
        ref<Store> store, const std::string & installable);

    virtual void completeCoreInstallable(std::string_view prefix) { }
};

/* A core command that operates on a list of "installables", which can
   be store paths, attribute paths etc. */
struct CoreInstallablesCommand : virtual Args, ParseInstallableArgs
{
    std::vector<std::shared_ptr<CoreInstallable>> coreInstallables;

    CoreInstallablesCommand();

    void prepare() override;

    virtual bool useDefaultInstallables() { return true; }

protected:

    std::vector<std::string> _installables;
};

/* A core command that operates on exactly one "installable" */
struct CoreInstallableCommand : virtual Args, ParseInstallableArgs
{
    std::shared_ptr<CoreInstallable> coreInstallable;

    CoreInstallableCommand();

    void prepare() override;

protected:

    std::string _installable{"."};
};

/* A command that operates on zero or more store paths. */
struct BuiltPathsCommand : public CoreInstallablesCommand
{
private:

    bool recursive = false;
    bool all = false;

protected:

    Realise realiseMode = Realise::Derivation;

public:

    BuiltPathsCommand(bool recursive = false);

    using StoreCommand::run;

    virtual void run(ref<Store> store, BuiltPaths && paths) = 0;

    void run(ref<Store> store) override;

    bool useDefaultInstallables() override { return !all; }
};

struct StorePathsCommand : public BuiltPathsCommand
{
    StorePathsCommand(bool recursive = false);

    using BuiltPathsCommand::run;

    virtual void run(ref<Store> store, std::vector<StorePath> && storePaths) = 0;

    void run(ref<Store> store, BuiltPaths && paths) override;
};

/* A command that operates on exactly one store path. */
struct StorePathCommand : public StorePathsCommand
{
    using StorePathsCommand::run;

    virtual void run(ref<Store> store, const StorePath & storePath) = 0;

    void run(ref<Store> store, std::vector<StorePath> && storePaths) override;
};

/* A helper class for registering commands globally. */
struct RegisterCommand
{
    typedef std::map<std::vector<std::string>, std::function<ref<Command>()>> Commands;
    static Commands * commands;

    RegisterCommand(std::vector<std::string> && name,
        std::function<ref<Command>()> command)
    {
        if (!commands) commands = new Commands;
        commands->emplace(name, command);
    }

    static nix::Commands getCommandsFor(const std::vector<std::string> & prefix);
};

template<class T>
static RegisterCommand registerCommand(const std::string & name)
{
    return RegisterCommand({name}, [](){ return make_ref<T>(); });
}

template<class T>
static RegisterCommand registerCommand2(std::vector<std::string> && name)
{
    return RegisterCommand(std::move(name), [](){ return make_ref<T>(); });
}

struct MixProfile : virtual StoreCommand
{
    std::optional<Path> profile;

    MixProfile();

    /* If 'profile' is set, make it point at 'storePath'. */
    void updateProfile(const StorePath & storePath);

    /* If 'profile' is set, make it point at the store path produced
       by 'buildables'. */
    void updateProfile(const BuiltPaths & buildables);
};

struct MixDefaultProfile : MixProfile
{
    MixDefaultProfile();
};

struct MixEnvironment : virtual Args {

    StringSet keep, unset;
    Strings stringsEnv;
    std::vector<char*> vectorEnv;
    bool ignoreEnvironment;

    MixEnvironment();

    /* Modify global environ based on ignoreEnvironment, keep, and unset. It's expected that exec will be called before this class goes out of scope, otherwise environ will become invalid. */
    void setEnviron();
};

std::string showVersions(const std::set<std::string> & versions);

void printClosureDiff(
    ref<Store> store,
    const StorePath & beforePath,
    const StorePath & afterPath,
    std::string_view indent);

}
