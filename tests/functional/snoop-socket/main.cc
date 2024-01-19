#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "logging.hh"
#include "unix-domain-socket.hh"

using namespace nix;

int main(int argc, char **argv)
{
    assert(argc == 3);
    Path server_path { argv[1] };
    Path our_path { argv[2] };

    AutoCloseFD server = createUnixDomainSocket();
    connect(server.get(), server_path);

    AutoCloseFD our_server = createUnixDomainSocket(our_path, 0667);
    AutoCloseFD conn = accept(our_server.get(), nullptr, nullptr);

    bool to_client = true, from_client = true;

    while (to_client || from_client) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        FD_SET(server.get(), &read_fds);
        FD_SET(conn.get(), &read_fds);

        debug("select");
        auto count = select(std::max(server.get(), conn.get()) + 1, &read_fds, nullptr, nullptr, nullptr);

        assert(count >= 0);

        if (count == 0) continue;

        // Check for data on the server socket
        if (FD_ISSET(server.get(), &read_fds)) {
            char buffer[1024];
            debug("read from server");
            auto ret = read(server.get(), buffer, sizeof(buffer));
            assert(ret >= 0);

            if (ret == 0) {
                debug("Server closed connection");
                assert(!shutdown(conn.get(), SHUT_WR));
                to_client = false;
            } else {
                // Log intercepted data from the server
                assert(write(STDOUT_FILENO, buffer, ret) >= 0);
                // Forward intercepted data to the server
                debug("write to client");
                assert(write(conn.get(), buffer, ret) >= 0);
            }
        }

        // Check for data on the conn socket
        if (FD_ISSET(conn.get(), &read_fds)) {
            char buffer[1024];
            debug("read from conn");
            auto ret = read(conn.get(), buffer, sizeof(buffer));
            assert(ret >= 0);

            if (ret == 0) {
                debug("client closed connection");
                assert(!shutdown(server.get(), SHUT_WR));
                from_client = false;
            } else {
                // Forward intercepted data to the server
                debug("write to server");
                assert(write(server.get(), buffer, ret) >= 0);
            }
        }
    }
    return EXIT_SUCCESS;
}
