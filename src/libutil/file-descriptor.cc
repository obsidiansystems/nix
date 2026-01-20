#include "file-descriptor-impl.hh"
#include "nix/util/serialise.hh"
#include "nix/util/util.hh"
#include "nix/util/signals.hh"

#include <span>
#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#  include <winnt.h>
#  include <fileapi.h>
#else
#  include <poll.h>
#endif

namespace nix {

namespace {

enum class PollDirection { In, Out };

/**
 * Retry an I/O operation if it fails with EAGAIN/EWOULDBLOCK.
 *
 * On Unix, polls the fd and retries. On Windows, just calls `f` once.
 *
 * This retry logic is needed to handle non-blocking reads/writes. This
 * is needed in the buildhook, because somehow the json logger file
 * descriptor ends up being non-blocking and breaks remote-building.
 *
 * @todo Get rid of buildhook and remove this logic again
 * (https://github.com/NixOS/nix/issues/12688)
 */
template<typename F>
auto retryOnBlock([[maybe_unused]] Descriptor fd, [[maybe_unused]] PollDirection dir, F && f) -> decltype(f())
{
#ifndef _WIN32
    while (true) {
        try {
            return std::forward<F>(f)();
        } catch (SystemError & e) {
            if (e.is(std::errc::resource_unavailable_try_again) || e.is(std::errc::operation_would_block)) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = dir == PollDirection::In ? POLLIN : POLLOUT;
                if (poll(&pfd, 1, -1) == -1)
                    throw SysError("poll on file descriptor failed");
                continue;
            }
            throw;
        }
    }
#else
    return std::forward<F>(f)();
#endif
}

} // namespace

void readFull(Descriptor fd, char * buf, size_t count)
{
    while (count) {
        checkInterrupt();
        auto res = retryOnBlock(
            fd, PollDirection::In, [&]() { return read(fd, {reinterpret_cast<std::byte *>(buf), count}); });
        if (res == 0)
            throw EndOfFile("unexpected end-of-file");
        count -= res;
        buf += res;
    }
}

std::string readLine(Descriptor fd, bool eofOk, char terminator)
{
    std::string s;
    while (1) {
        checkInterrupt();
        char ch;
        // FIXME: inefficient
        auto rd =
            retryOnBlock(fd, PollDirection::In, [&]() { return read(fd, {reinterpret_cast<std::byte *>(&ch), 1}); });
        if (rd == 0) {
            if (eofOk)
                return s;
            else
                throw EndOfFile("unexpected EOF reading a line");
        } else {
            if (ch == terminator)
                return s;
            s += ch;
        }
    }
}

void writeFull(Descriptor fd, std::string_view s, bool allowInterrupts)
{
    while (!s.empty()) {
        if (allowInterrupts)
            checkInterrupt();
        auto res = retryOnBlock(fd, PollDirection::Out, [&]() {
            return write(fd, {reinterpret_cast<const std::byte *>(s.data()), s.size()});
        });
        if (res > 0)
            s.remove_prefix(res);
    }
}

void writeLine(Descriptor fd, std::string s)
{
    s += '\n';
    writeFull(fd, s);
}

std::string readFile(Descriptor fd)
{
    auto size = getFileSize(fd);
    // We can't rely on size being correct, most files in /proc have a nominal size of 0
    return drainFD(fd, {.size = size, .expected = false});
}

/**
 * Common implementation for drainFD variants.
 *
 * @param fd File descriptor to drain
 * @param opts Options controlling the drain behavior
 * @param transferFn Function that transfers data, called with optional remaining byte count.
 *                   Returns number of bytes transferred, 0 on EOF.
 */
static void drainFDImpl([[maybe_unused]] Descriptor fd, DrainFdSinkOpts opts, auto && transferFn)
{
#ifndef _WIN32
    // silence GCC maybe-uninitialized warning in finally
    int saved = 0;

    if (!opts.block) {
        saved = fcntl(fd, F_GETFL);
        if (fcntl(fd, F_SETFL, saved | O_NONBLOCK) == -1)
            throw SysError("making file descriptor non-blocking");
    }

    Finally finally([&]() {
        if (!opts.block) {
            if (fcntl(fd, F_SETFL, saved) == -1)
                throw SysError("making file descriptor blocking");
        }
    });
#endif

    size_t bytesRead = 0;
    while (1) {
        checkInterrupt();

        std::optional<size_t> remaining;
        if (opts.expectedSize) {
            remaining = *opts.expectedSize - bytesRead;
            if (*remaining == 0)
                break;
        }

        size_t n;
        try {
            n = transferFn(remaining);
        } catch (SystemError & e) {
#ifndef _WIN32
            if (!opts.block
                && (e.is(std::errc::resource_unavailable_try_again) || e.is(std::errc::operation_would_block)))
                break;
#endif
            throw;
        }

        if (n == 0) {
            if (opts.expectedSize && bytesRead < *opts.expectedSize)
                throw EndOfFile("unexpected end-of-file");
            break;
        }

        bytesRead += n;
    }
}

void drainFD(Descriptor fd, FdSink & sink, DrainFdSinkOpts opts)
{
    sink.flush();
    drainFDImpl(fd, opts, [fd, &sink](std::optional<size_t>) {
        auto n = splice(fd, sink.fd);
        sink.written += n;
        return n;
    });
}

void drainFD(Descriptor fd, Sink & sink, DrainFdSinkOpts opts)
{
    // Use optimized splice path if sink is an FdSink
    if (auto fdSink = dynamic_cast<FdSink *>(&sink)) {
        drainFD(fd, *fdSink, opts);
        return;
    }

    std::array<std::byte, 64 * 1024> buf;
    drainFDImpl(fd, opts, [fd, &sink, &buf](std::optional<size_t> remaining) {
        auto toRead = remaining ? std::min(buf.size(), *remaining) : buf.size();
        auto n = read(fd, std::span(buf.data(), toRead));
        if (n > 0)
            sink(std::string_view(reinterpret_cast<const char *>(buf.data()), n));
        return n;
    });
}

std::string drainFD(Descriptor fd, DrainFdOpts opts)
{
    // the parser needs two extra bytes to append terminating characters, other users will
    // not care very much about the extra memory.
    size_t reserveSize = opts.expected ? 0 : opts.size;
    StringSink sink(reserveSize + 2);
    DrainFdSinkOpts sinkOpts{
        .expectedSize = opts.expected ? std::optional<size_t>(opts.size) : std::nullopt,
#ifndef _WIN32
        .block = opts.block,
#endif
    };
    drainFD(fd, sink, sinkOpts);
    return std::move(sink.s);
}

static void copyFdRangeNoSendfile(Descriptor fd, off_t offset, size_t nbytes, Sink & sink)
{
    auto left = nbytes;
    std::array<std::byte, 64 * 1024> buf;

    while (left) {
        auto limit = std::min<size_t>(left, buf.size());
        auto n = readOffset(fd, offset, std::span(buf.data(), limit));
        if (n == 0)
            throw EndOfFile("unexpected end-of-file");
        assert(n <= left);
        sink(std::string_view(reinterpret_cast<const char *>(buf.data()), n));
        offset += n;
        left -= n;
    }
}

void copyFdRange(Descriptor fd, off_t offset, size_t nbytes, Sink & sink)
{
    // Use optimized sendfile path if sink is an FdSink
    if (auto fdSink = dynamic_cast<FdSink *>(&sink)) {
        copyFdRange(fd, offset, nbytes, *fdSink);
        return;
    }

    // Fallback: regular read/write loop
    copyFdRangeNoSendfile(fd, offset, nbytes, sink);
}

void copyFdRange(Descriptor fd, off_t offset, size_t nbytes, FdSink & sink)
{
    // Flush any buffered data first
    sink.flush();

    off_t original_offset = offset;
    try {
        auto left = nbytes;
        while (left) {
            checkInterrupt();
            size_t n;
            try {
                n = sendFile(sink.fd, fd, offset, left);
            } catch (SystemError & e) {
                if (e.is(std::errc::interrupted)) {
                    continue;
                }
                throw;
            }
            if (n == 0) {
                throw EndOfFile("unexpected end-of-file");
            }
            assert(n <= left);
            sink.written += n;
            offset += n;
            left -= n;
        }
    } catch (SystemError & e) {
        if (e.is(std::errc::invalid_argument) || e.is(std::errc::function_not_supported)) {
            // sendfile not supported, fall back to regular read/write
            size_t bytes_sent = offset - original_offset;
            copyFdRangeNoSendfile(fd, offset, nbytes - bytes_sent, sink);
        } else {
            throw;
        }
    }
}

size_t sendFileFallback(Descriptor out_fd, Descriptor in_fd, off_t offset, size_t count)
{
    std::array<std::byte, 64 * 1024> buf;
    size_t total = 0;
    while (total < count) {
        checkInterrupt();
        auto toRead = std::min(buf.size(), count - total);
        auto n = readOffset(in_fd, offset + total, std::span(buf.data(), toRead));
        if (n == 0)
            throw EndOfFile("unexpected EOF in sendFile");
        writeFull(out_fd, std::string_view(reinterpret_cast<char *>(buf.data()), n), false);
        total += n;
    }
    return total;
}

size_t spliceFallback(Descriptor from, Descriptor to)
{
    std::array<std::byte, 64 * 1024> buf;
    auto n = read(from, buf);
    if (n == 0)
        return 0;
    writeFull(to, std::string_view(reinterpret_cast<char *>(buf.data()), n), false);
    return n;
}

//////////////////////////////////////////////////////////////////////

AutoCloseFD::AutoCloseFD()
    : fd{INVALID_DESCRIPTOR}
{
}

AutoCloseFD::AutoCloseFD(Descriptor fd)
    : fd{fd}
{
}

// NOTE: This can be noexcept since we are just copying a value and resetting
// the file descriptor in the rhs.
AutoCloseFD::AutoCloseFD(AutoCloseFD && that) noexcept
    : fd{that.fd}
{
    that.fd = INVALID_DESCRIPTOR;
}

AutoCloseFD & AutoCloseFD::operator=(AutoCloseFD && that)
{
    close();
    fd = that.fd;
    that.fd = INVALID_DESCRIPTOR;
    return *this;
}

AutoCloseFD::~AutoCloseFD()
{
    try {
        close();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

Descriptor AutoCloseFD::get() const
{
    return fd;
}

void AutoCloseFD::close()
{
    if (fd != INVALID_DESCRIPTOR) {
        if (
#ifdef _WIN32
            ::CloseHandle(fd)
#else
            ::close(fd)
#endif
            == -1)
            /* This should never happen. */
            throw NativeSysError("closing file descriptor %1%", fd);
        fd = INVALID_DESCRIPTOR;
    }
}

void AutoCloseFD::startFsync() const
{
#ifdef __linux__
    if (fd != -1) {
        /* Ignore failure, since fsync must be run later anyway. This is just a performance optimization. */
        ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
    }
#endif
}

AutoCloseFD::operator bool() const
{
    return fd != INVALID_DESCRIPTOR;
}

Descriptor AutoCloseFD::release()
{
    Descriptor oldFD = fd;
    fd = INVALID_DESCRIPTOR;
    return oldFD;
}

//////////////////////////////////////////////////////////////////////

void Pipe::close()
{
    readSide.close();
    writeSide.close();
}

} // namespace nix
