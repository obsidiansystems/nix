#include "nix/util/serialise.hh"
#include "nix/util/compression.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"

#include <cstring>
#include <cerrno>
#include <memory>

#include <boost/coroutine2/coroutine.hpp>

#ifdef _WIN32
#  include <fileapi.h>
#  include <winsock2.h>
#  include "nix/util/windows-error.hh"
#else
#  include <poll.h>
#endif

namespace nix {

void BufferedSink::operator()(BytesView data)
{
    if (!buffer)
        buffer = decltype(buffer)(new std::byte[bufSize]);

    while (!data.empty()) {
        /* Optimisation: bypass the buffer if the data exceeds the
           buffer size. */
        if (bufPos + data.size() >= bufSize) {
            flush();
            writeUnbuffered(data);
            break;
        }
        /* Otherwise, copy the bytes to the buffer.  Flush the buffer
           when it's full. */
        size_t n = bufPos + data.size() > bufSize ? bufSize - bufPos : data.size();
        memcpy(buffer.get() + bufPos, data.data(), n);
        data = data.subspan(n);
        bufPos += n;
        if (bufPos == bufSize)
            flush();
    }
}

void BufferedSink::flush()
{
    if (bufPos == 0)
        return;
    size_t n = bufPos;
    bufPos = 0; // don't trigger the assert() in ~BufferedSink()
    writeUnbuffered(BytesView{buffer.get(), n});
}

FdSink::~FdSink()
{
    try {
        flush();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void FdSink::writeUnbuffered(BytesView data)
{
    written += data.size();
    try {
        writeFull(fd, data);
    } catch (SystemError & e) {
        _good = false;
        throw;
    }
}

bool FdSink::good()
{
    return _good;
}

void Source::operator()(MutableBytesView data)
{
    while (!data.empty()) {
        size_t n = read(data);
        data = data.subspan(n);
    }
}

void Source::drainInto(Sink & sink)
{
    std::array<std::byte, 8192> buf;
    while (true) {
        try {
            auto n = read(MutableBytesView{buf});
            sink(BytesView{buf.data(), n});
        } catch (EndOfFile &) {
            break;
        }
    }
}

std::string Source::drain()
{
    StringSink s;
    drainInto(s);
    return std::string(as_str(s.s));
}

void Source::skip(size_t len)
{
    std::array<std::byte, 8192> buf;
    while (len) {
        auto n = read(MutableBytesView{buf.data(), std::min(len, buf.size())});
        assert(n <= len);
        len -= n;
    }
}

size_t BufferedSource::read(MutableBytesView data)
{
    if (!buffer)
        buffer = decltype(buffer)(new std::byte[bufSize]);

    if (!bufPosIn)
        bufPosIn = readUnbuffered(MutableBytesView{buffer.get(), bufSize});

    /* Copy out the data in the buffer. */
    auto n = std::min(data.size(), bufPosIn - bufPosOut);
    memcpy(data.data(), buffer.get() + bufPosOut, n);
    bufPosOut += n;
    if (bufPosIn == bufPosOut)
        bufPosIn = bufPosOut = 0;
    return n;
}

bool BufferedSource::hasData()
{
    return bufPosOut < bufPosIn;
}

size_t FdSource::readUnbuffered(MutableBytesView data)
{
#ifdef _WIN32
    DWORD n;
    checkInterrupt();
    if (!::ReadFile(fd, data.data(), data.size(), &n, NULL)) {
        _good = false;
        throw windows::WinError("ReadFile when FdSource::readUnbuffered");
    }
#else
    ssize_t n;
    do {
        checkInterrupt();
        n = ::read(fd, data.data(), data.size());
    } while (n == -1 && errno == EINTR);
    if (n == -1) {
        _good = false;
        throw SysError("reading from file");
    }
    if (n == 0) {
        _good = false;
        throw EndOfFile(std::string(*endOfFileError));
    }
#endif
    read += n;
    return n;
}

bool FdSource::good()
{
    return _good;
}

bool FdSource::hasData()
{
    if (BufferedSource::hasData())
        return true;

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        int fd_ = fromDescriptorReadOnly(fd);
        FD_SET(fd_, &fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        auto n = select(fd_ + 1, &fds, nullptr, nullptr, &timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw SysError("polling file descriptor");
        }
        return FD_ISSET(fd, &fds);
    }
}

void FdSource::restart()
{
    if (!isSeekable)
        throw Error("can't seek to the start of a file");
    buffer.reset();
    read = bufPosIn = bufPosOut = 0;
    int fd_ = fromDescriptorReadOnly(fd);
    if (lseek(fd_, 0, SEEK_SET) == -1)
        throw SysError("seeking to the start of a file");
}

void FdSource::skip(size_t len)
{
    /* Discard data in the buffer. */
    if (len && buffer && bufPosIn - bufPosOut) {
        if (len >= bufPosIn - bufPosOut) {
            len -= bufPosIn - bufPosOut;
            bufPosIn = bufPosOut = 0;
        } else {
            bufPosOut += len;
            len = 0;
        }
    }

#ifndef _WIN32
    /* If we can, seek forward in the file to skip the rest. */
    if (isSeekable && len) {
        if (lseek(fd, len, SEEK_CUR) == -1) {
            if (errno == ESPIPE)
                isSeekable = false;
            else
                throw SysError("seeking forward in file");
        } else {
            read += len;
            return;
        }
    }
#endif

    /* Otherwise, skip by reading. */
    if (len)
        BufferedSource::skip(len);
}

size_t StringSource::read(MutableBytesView data)
{
    if (pos == s.size())
        throw EndOfFile("end of string reached");
    size_t n = std::min(data.size(), s.size() - pos);
    memcpy(data.data(), s.data() + pos, n);
    pos += n;
    return n;
}

void StringSource::skip(size_t len)
{
    const size_t remain = s.size() - pos;
    if (len > remain) {
        pos = s.size();
        throw EndOfFile("end of string reached");
    }
    pos += len;
}

CompressedSource::CompressedSource(RestartableSource & source, const std::string & compressionMethod)
    : compressedData([&]() {
        StringSink sink;
        auto compressionSink = makeCompressionSink(compressionMethod, sink);
        source.drainInto(*compressionSink);
        compressionSink->finish();
        return std::move(sink.s);
    }())
    , compressionMethod(compressionMethod)
    , stringSource(compressedData)
{
}

std::unique_ptr<FinishSink> sourceToSink(std::function<void(Source &)> fun)
{
    struct SourceToSink : FinishSink
    {
        typedef boost::coroutines2::coroutine<bool> coro_t;

        std::function<void(Source &)> fun;
        std::optional<coro_t::push_type> coro;

        SourceToSink(std::function<void(Source &)> fun)
            : fun(fun)
        {
        }

        BytesView cur;

        void operator()(BytesView in) override
        {
            if (in.empty())
                return;
            cur = in;

            if (!coro) {
                coro = coro_t::push_type([&](coro_t::pull_type & yield) {
                    LambdaSource source([&](MutableBytesView out) {
                        if (cur.empty()) {
                            yield();
                            if (yield.get())
                                throw EndOfFile("coroutine has finished");
                        }

                        size_t n = std::min(out.size(), cur.size());
                        memcpy(out.data(), cur.data(), n);
                        cur = cur.subspan(n);
                        return n;
                    });
                    fun(source);
                });
            }

            if (!*coro) {
                unreachable();
            }

            if (!cur.empty()) {
                (*coro)(false);
            }
        }

        void finish() override
        {
            if (coro && *coro)
                (*coro)(true);
        }
    };

    return std::make_unique<SourceToSink>(fun);
}

std::unique_ptr<Source> sinkToSource(std::function<void(Sink &)> fun, std::function<void()> eof)
{
    struct SinkToSource : Source
    {
        typedef boost::coroutines2::coroutine<BytesView> coro_t;

        std::function<void(Sink &)> fun;
        std::function<void()> eof;
        std::optional<coro_t::pull_type> coro;

        SinkToSource(std::function<void(Sink &)> fun, std::function<void()> eof)
            : fun(fun)
            , eof(eof)
        {
        }

        BytesView cur;

        size_t read(MutableBytesView data) override
        {
            bool hasCoro = coro.has_value();
            if (!hasCoro) {
                coro = coro_t::pull_type([&](coro_t::push_type & yield) {
                    LambdaSink sink([&](BytesView data) {
                        if (!data.empty()) {
                            yield(data);
                        }
                    });
                    fun(sink);
                });
            }

            if (cur.empty()) {
                if (hasCoro) {
                    (*coro)();
                }
                if (*coro) {
                    cur = coro->get();
                } else {
                    coro.reset();
                    eof();
                    unreachable();
                }
            }

            size_t n = std::min(data.size(), cur.size());
            memcpy(data.data(), cur.data(), n);
            cur = cur.subspan(n);

            return n;
        }
    };

    return std::make_unique<SinkToSource>(fun, eof);
}

void writePadding(size_t len, Sink & sink)
{
    if (len % 8) {
        std::byte zero[8] = {};
        sink(BytesView{zero, 8 - (len % 8)});
    }
}

void writeString(std::string_view data, Sink & sink)
{
    sink << data.size();
    sink(as_bytes(data));
    writePadding(data.size(), sink);
}

Sink & operator<<(Sink & sink, std::string_view s)
{
    writeString(s, sink);
    return sink;
}

template<class T>
void writeStrings(const T & ss, Sink & sink)
{
    sink << ss.size();
    for (auto & i : ss)
        sink << i;
}

Sink & operator<<(Sink & sink, const Strings & s)
{
    writeStrings(s, sink);
    return sink;
}

Sink & operator<<(Sink & sink, const StringSet & s)
{
    writeStrings(s, sink);
    return sink;
}

Sink & operator<<(Sink & sink, const Error & ex)
{
    auto & info = ex.info();
    sink << "Error" << info.level << "Error" // removed
         << info.msg.str() << 0              // FIXME: info.errPos
         << info.traces.size();
    for (auto & trace : info.traces) {
        sink << 0; // FIXME: trace.pos
        sink << trace.hint.str();
    }
    return sink;
}

void readPadding(size_t len, Source & source)
{
    if (len % 8) {
        std::byte zero[8];
        size_t n = 8 - (len % 8);
        source(MutableBytesView{zero, n});
        for (unsigned int i = 0; i < n; i++)
            if (zero[i] != std::byte{0})
                throw SerialisationError("non-zero padding");
    }
}

size_t readString(char * buf, size_t max, Source & source)
{
    auto len = readNum<size_t>(source);
    if (len > max)
        throw SerialisationError("string is too long");
    source(MutableBytesView{reinterpret_cast<std::byte *>(buf), len});
    readPadding(len, source);
    return len;
}

std::string readString(Source & source, size_t max)
{
    auto len = readNum<size_t>(source);
    if (len > max)
        throw SerialisationError("string is too long");
    std::string res(len, 0);
    source(MutableBytesView{reinterpret_cast<std::byte *>(res.data()), len});
    readPadding(len, source);
    return res;
}

Source & operator>>(Source & in, std::string & s)
{
    s = readString(in);
    return in;
}

template<class T>
T readStrings(Source & source)
{
    auto count = readNum<size_t>(source);
    T ss;
    while (count--)
        ss.insert(ss.end(), readString(source));
    return ss;
}

template Paths readStrings(Source & source);
template PathSet readStrings(Source & source);

Error readError(Source & source)
{
    auto type = readString(source);
    assert(type == "Error");
    auto level = (Verbosity) readInt(source);
    [[maybe_unused]] auto name = readString(source); // removed
    auto msg = readString(source);
    ErrorInfo info{
        .level = level,
        .msg = HintFmt(msg),
    };
    auto havePos = readNum<size_t>(source);
    assert(havePos == 0);
    auto nrTraces = readNum<size_t>(source);
    for (size_t i = 0; i < nrTraces; ++i) {
        havePos = readNum<size_t>(source);
        assert(havePos == 0);
        info.traces.push_back(Trace{.hint = HintFmt(readString(source))});
    }
    return Error(std::move(info));
}

void StringSink::operator()(BytesView data)
{
    s.insert(s.end(), data.begin(), data.end());
}

size_t ChainSource::read(MutableBytesView data)
{
    if (useSecond) {
        return source2.read(data);
    } else {
        try {
            return source1.read(data);
        } catch (EndOfFile &) {
            useSecond = true;
            return this->read(data);
        }
    }
}

} // namespace nix
