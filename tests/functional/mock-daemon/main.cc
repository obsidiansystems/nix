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
    assert(argc >= 2 && argc <= 4);
    Path socket_path { argv[1] };
    std::optional<Path> write_log_path;
    if (argc >= 3)
        write_log_path = Path { argv[2] };
    size_t write_count = argc >= 4 ? std::stoull(argv[3]) : 0;

    AutoCloseFD server = createUnixDomainSocket(socket_path, 0666);

    AutoCloseFD conn = accept(server.get(), nullptr, nullptr);
    assert(conn.get() != -1);

    fcntl(
        conn.get(),
        F_SETFL,
        fcntl(conn.get(), F_GETFL, 0) | O_NONBLOCK);

    AutoCloseFD write_log;

    auto no_write = [&]{
        debug("done writing");
        write_log.close();
        assert(!shutdown(conn.get(), SHUT_WR));
        write_count = 0;
    };

    if (write_log_path) {
        write_log = open(write_log_path->c_str(), O_RDONLY);
        assert(write_log.get() != -1);
    } else {
        no_write();
    }

    while (true) {
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(conn.get(), &read_fds);
        if (write_count)
            FD_SET(conn.get(), &write_fds);
        else
            no_write();

        debug("select");
        auto count = select(conn.get() + 1, &read_fds, &write_fds, nullptr, nullptr);

        assert(count >= 0);

        if (count > 0 && FD_ISSET(conn.get(), &read_fds)) {
            debug("read");
            char c;
            auto ret = read(conn.get(), &c, 1);
            assert(ret >= 0);
            if (ret == 0) break;
        }

        if (write_count && count > 0 && FD_ISSET(conn.get(), &write_fds)) {
            debug("write");
            char c;
            auto ret = read(write_log.get(), &c, 1);
            assert(ret >= 0);
            write_count = write_count - ret;
            if (ret == 0) write_count = 0;
        }
    }
    return EXIT_SUCCESS;
}
