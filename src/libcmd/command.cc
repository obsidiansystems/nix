#include "command.hh"
#include "store-api.hh"
#include "local-fs-store.hh"
#include "derivations.hh"
#include "nixexpr.hh"
#include "profiles.hh"

#include <nlohmann/json.hpp>

namespace nix {

EvalCommand::EvalCommand()
{
}

EvalCommand::~EvalCommand()
{
    if (evalState)
        evalState->printStats();
}

ref<Store> EvalCommand::getDrvStore()
{
    if (!drvStore)
        drvStore = evalStoreUrl ? openStore(*evalStoreUrl) : getStore();
    return ref<Store>(drvStore);
}

ref<EvalState> EvalCommand::getEvalState()
{
    if (!evalState)
        evalState =
            #if HAVE_BOEHMGC
            std::allocate_shared<EvalState>(traceable_allocator<EvalState>(),
                searchPath, getEvalStore(), getStore())
            #else
            std::make_shared<EvalState>(
                searchPath, getEvalStore(), getStore())
            #endif
            ;
    return ref<EvalState>(evalState);
}

Strings editorFor(const Pos & pos)
{
    auto editor = getEnv("EDITOR").value_or("cat");
    auto args = tokenizeString<Strings>(editor);
    if (pos.line > 0 && (
        editor.find("emacs") != std::string::npos ||
        editor.find("nano") != std::string::npos ||
        editor.find("vim") != std::string::npos))
        args.push_back(fmt("+%d", pos.line));
    args.push_back(pos.file);
    return args;
}

}
