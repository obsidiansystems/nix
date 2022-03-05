#pragma once

#include "store-command.hh"
#include "installables.hh"
#include "common-eval-args.hh"
#include "flake/lockfile.hh"

namespace nix {

class EvalState;
struct Pos;

struct EvalCommand : public virtual DrvCommand, MixEvalArgs
{
    EvalCommand();

    ~EvalCommand();

    ref<Store> getDrvStore() override;

    /* Alias with better name for this class. */
    inline ref<Store> getEvalStore() { return getDrvStore(); };

    ref<EvalState> getEvalState();

private:

    std::shared_ptr<EvalState> evalState;
};

struct MixFlakeOptions : virtual Args, EvalCommand
{
    flake::LockFlags lockFlags;

    MixFlakeOptions();

    virtual std::optional<FlakeRef> getFlakeRefForCompletion()
    { return {}; }
};

struct SourceExprCommand : public virtual ParseInstallableArgs, MixFlakeOptions
{
    std::optional<Path> file;
    std::optional<std::string> expr;

    SourceExprCommand();

    std::vector<std::shared_ptr<Installable>> parseInstallables(
        ref<Store> store, std::vector<std::string> ss);

    std::shared_ptr<Installable> parseInstallable(
        ref<Store> store, const std::string & installable);

    virtual Strings getDefaultFlakeAttrPaths();

    virtual Strings getDefaultFlakeAttrPathPrefixes();

    void completeInstallable(std::string_view prefix);

    std::vector<std::shared_ptr<CoreInstallable>> parseCoreInstallables(
        ref<Store> store, std::vector<std::string> ss) override;

    void completeCoreInstallable(std::string_view prefix) override {
        completeInstallable(prefix);
    }
};

/* A command that operates on a list of "installables", which can be
   store paths, attribute paths, Nix expressions, etc. */
struct InstallablesCommand : public virtual CoreInstallablesCommand, SourceExprCommand
{
    std::vector<std::shared_ptr<Installable>> installables;

    InstallablesCommand();

    void prepare() override;

    std::optional<FlakeRef> getFlakeRefForCompletion() override;
};

/* A command that operates on exactly one "installable" */
struct InstallableCommand : public virtual CoreInstallableCommand, SourceExprCommand
{
    std::shared_ptr<Installable> installable;

    InstallableCommand();

    void prepare() override;

    std::optional<FlakeRef> getFlakeRefForCompletion() override
    {
        return parseFlakeRef(_installable, absPath("."));
    }
};

/* Helper function to generate args that invoke $EDITOR on
   filename:lineno. */
Strings editorFor(const Pos & pos);

void completeFlakeRef(ref<Store> store, std::string_view prefix);

void completeFlakeRefWithFragment(
    ref<EvalState> evalState,
    flake::LockFlags lockFlags,
    Strings attrPathPrefixes,
    const Strings & defaultFlakeAttrPaths,
    std::string_view prefix);

}
