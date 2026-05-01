#include "nix/cmd/command.hh"
#include "nix/cmd/unix-socket-server.hh"
#include "nix/store/build/derivation-builder-varlink.hh"
#include "nix/store/store-api.hh"
#include "nix/util/signals.hh"

namespace nix {

struct CmdVarlinkDaemon : StoreConfigCommand
{
    std::optional<std::filesystem::path> listen = std::nullopt;

    CmdVarlinkDaemon()
    {
        addFlag({
            .longName = "listen",
            .shortName = 'l',
            .description = "Set the path to listen for the varlink socket",
            .labels = {"path"},
            .handler = {&listen},
        });
    }

    void run(ref<StoreConfig> storeConfig) override
    {
        if (!listen.has_value()) {
            listen = storeConfig->getStateDir() / "varlink" / "socket";
        }

        unix::serveUnixSocket(
            {
                .socketPath = listen.value(),
                .socketMode = 0666,
            },
            [&](AutoCloseFD remote, std::function<void()> closeListeners) {
                auto store = storeConfig->openStore();
                try {
                    FdSource from(remote.get());
                    FdSink to(remote.get());
                    processVarlinkConnection(*store, std::nullopt, make_ref<Sync<OutputPathMap>>(), from, to);
                    debug("terminated Varlink daemon connection");
                } catch (const Interrupted &) {
                    debug("interrupted Varlink daemon connection");
                } catch (SystemError &) {
                    ignoreExceptionExceptInterrupt();
                }
            });
    }
};

static auto rCmdVarlinkDaemon = registerCommand2<CmdVarlinkDaemon>({"store", "varlink-daemon"});

} // namespace nix
