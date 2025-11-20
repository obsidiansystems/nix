#pragma once
///@file

#include <memory>
#include <type_traits>

#include "nix/util/bytes.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"
#include "nix/util/file-descriptor.hh"

namespace boost::context {
struct stack_context;
}

namespace nix {

/**
 * Abstract destination of binary data.
 */
struct Sink
{
    virtual ~Sink() {}

    virtual void operator()(BytesView data) = 0;

    virtual bool good()
    {
        return true;
    }
};

/**
 * Just throws away data.
 */
struct NullSink : Sink
{
    void operator()(BytesView data) override {}
};

struct FinishSink : virtual Sink
{
    virtual void finish() = 0;
};

/**
 * A buffered abstract sink. Warning: a BufferedSink should not be
 * used from multiple threads concurrently.
 */
struct BufferedSink : virtual Sink
{
    size_t bufSize, bufPos;
    std::unique_ptr<std::byte[]> buffer;

    BufferedSink(size_t bufSize = 32 * 1024)
        : bufSize(bufSize)
        , bufPos(0)
        , buffer(nullptr)
    {
    }

    void operator()(BytesView data) override;

    void flush();

protected:

    virtual void writeUnbuffered(BytesView data) = 0;
};

/**
 * Abstract source of binary data.
 */
struct Source
{
    virtual ~Source() {}

    /**
     * Store exactly `data.size()` bytes in the buffer pointed to by
     * `data`. It blocks until all the requested data is available, or
     * throws an error if it is not going to be available.
     */
    void operator()(MutableBytesView data);

    /**
     * Store up to `data.size()` in the buffer pointed to by `data`, and
     * return the number of bytes stored. It blocks until at least one
     * byte is available.
     */
    virtual size_t read(MutableBytesView data) = 0;

    virtual bool good()
    {
        return true;
    }

    void drainInto(Sink & sink);

    std::string drain();

    virtual void skip(size_t len);
};

/**
 * A buffered abstract source. Warning: a BufferedSource should not be
 * used from multiple threads concurrently.
 */
struct BufferedSource : virtual Source
{
    size_t bufSize, bufPosIn, bufPosOut;
    std::unique_ptr<std::byte[]> buffer;

    BufferedSource(size_t bufSize = 32 * 1024)
        : bufSize(bufSize)
        , bufPosIn(0)
        , bufPosOut(0)
        , buffer(nullptr)
    {
    }

    size_t read(MutableBytesView data) override;

    /**
     * Return true if the buffer is not empty.
     */
    bool hasData();

protected:
    /**
     * Underlying read call, to be overridden.
     */
    virtual size_t readUnbuffered(MutableBytesView data) = 0;
};

/**
 * Source type that can be restarted.
 */
struct RestartableSource : virtual Source
{
    virtual void restart() = 0;
};

/**
 * A sink that writes data to a file descriptor.
 */
struct FdSink : BufferedSink
{
    Descriptor fd;
    size_t written = 0;

    FdSink()
        : fd(INVALID_DESCRIPTOR)
    {
    }

    FdSink(Descriptor fd)
        : fd(fd)
    {
    }

    FdSink(FdSink &&) = default;

    FdSink & operator=(FdSink && s)
    {
        flush();
        fd = s.fd;
        s.fd = INVALID_DESCRIPTOR;
        written = s.written;
        return *this;
    }

    ~FdSink();

    void writeUnbuffered(BytesView data) override;

    bool good() override;

private:
    bool _good = true;
};

/**
 * A source that reads data from a file descriptor.
 */
struct FdSource : BufferedSource, RestartableSource
{
    Descriptor fd;
    size_t read = 0;
    BackedStringView endOfFileError{"unexpected end-of-file"};
    bool isSeekable = true;

    FdSource()
        : fd(INVALID_DESCRIPTOR)
    {
    }

    FdSource(Descriptor fd)
        : fd(fd)
    {
    }

    FdSource(FdSource &&) = default;

    FdSource & operator=(FdSource && s) = default;

    bool good() override;
    void restart() override;

    /**
     * Return true if the buffer is not empty after a non-blocking
     * read.
     */
    bool hasData();

    void skip(size_t len) override;

protected:
    size_t readUnbuffered(MutableBytesView data) override;
private:
    bool _good = true;
};

/**
 * A sink that writes data to a Bytes vector.
 */
struct StringSink : Sink
{
    Bytes s;

    StringSink() {}

    explicit StringSink(const size_t reservedSize)
    {
        s.reserve(reservedSize);
    };

    StringSink(Bytes && s)
        : s(std::move(s)) {};

    void operator()(BytesView data) override;
};

/**
 * A source that reads data from a string.
 */
struct StringSource : RestartableSource
{
    BytesView s;
    size_t pos;

    // NOTE: Prevent unintentional dangling views when an implicit conversion
    // from std::string -> std::string_view occurs when the string is passed
    // by rvalue.
    StringSource(Bytes &&) = delete;

    StringSource(BytesView s)
        : s{s}
        , pos(0)
    {
    }

    StringSource(const Bytes & str)
        : StringSource(BytesView{str})
    {
    }

    size_t read(MutableBytesView data) override;

    void skip(size_t len) override;

    void restart() override
    {
        pos = 0;
    }
};

/**
 * Compresses a RestartableSource using the specified compression method.
 *
 * @note currently this buffers the entire compressed data stream in memory. In the future it may instead compress data
 * on demand, lazily pulling from the original `RestartableSource`. In that case, the `size()` method would go away
 * because we would not in fact know the compressed size in advance.
 */
struct CompressedSource : RestartableSource
{
private:
    Bytes compressedData;
    std::string compressionMethod;
    StringSource stringSource;

public:
    /**
     * Compress a RestartableSource using the specified compression method.
     *
     * @param source The source data to compress
     * @param compressionMethod The compression method to use (e.g., "xz", "br")
     */
    CompressedSource(RestartableSource & source, const std::string & compressionMethod);

    size_t read(MutableBytesView data) override
    {
        return stringSource.read(data);
    }

    void restart() override
    {
        stringSource.restart();
    }

    uint64_t size() const
    {
        return compressedData.size();
    }

    std::string_view getCompressionMethod() const
    {
        return compressionMethod;
    }
};

/**
 * A sink that writes all incoming data to two other sinks.
 */
struct TeeSink : Sink
{
    Sink &sink1, &sink2;

    TeeSink(Sink & sink1, Sink & sink2)
        : sink1(sink1)
        , sink2(sink2)
    {
    }

    virtual void operator()(BytesView data) override
    {
        sink1(data);
        sink2(data);
    }
};

/**
 * Adapter class of a Source that saves all data read to a sink.
 */
struct TeeSource : Source
{
    Source & orig;
    Sink & sink;

    TeeSource(Source & orig, Sink & sink)
        : orig(orig)
        , sink(sink)
    {
    }

    size_t read(MutableBytesView data) override
    {
        size_t n = orig.read(data);
        sink(data.subspan(0, n));
        return n;
    }
};

/**
 * A reader that consumes the original Source until 'size'.
 */
struct SizedSource : Source
{
    Source & orig;
    size_t remain;

    SizedSource(Source & orig, size_t size)
        : orig(orig)
        , remain(size)
    {
    }

    size_t read(MutableBytesView data) override
    {
        if (this->remain <= 0) {
            throw EndOfFile("sized: unexpected end-of-file");
        }
        size_t len = std::min(data.size(), this->remain);
        size_t n = this->orig.read(data.subspan(0, len));
        this->remain -= n;
        return n;
    }

    /**
     * Consume the original source until no remain data is left to consume.
     */
    size_t drainAll()
    {
        std::vector<std::byte> buf(8192);
        size_t sum = 0;
        while (this->remain > 0) {
            size_t n = read(MutableBytesView{buf});
            sum += n;
        }
        return sum;
    }
};

/**
 * A sink that that just counts the number of bytes given to it
 */
struct LengthSink : Sink
{
    uint64_t length = 0;

    void operator()(BytesView data) override
    {
        length += data.size();
    }
};

/**
 * A wrapper source that counts the number of bytes read from it.
 */
struct LengthSource : Source
{
    Source & next;

    LengthSource(Source & next)
        : next(next)
    {
    }

    uint64_t total = 0;

    size_t read(MutableBytesView data) override
    {
        auto n = next.read(data);
        total += n;
        return n;
    }
};

/**
 * Convert a function into a sink.
 */
struct LambdaSink : Sink
{
    typedef std::function<void(BytesView data)> data_t;
    typedef std::function<void()> cleanup_t;

    data_t dataFun;
    cleanup_t cleanupFun;

    LambdaSink(
        const data_t & dataFun, const cleanup_t & cleanupFun = []() {})
        : dataFun(dataFun)
        , cleanupFun(cleanupFun)
    {
    }

    ~LambdaSink()
    {
        cleanupFun();
    }

    void operator()(BytesView data) override
    {
        dataFun(data);
    }
};

/**
 * Convert a function into a source.
 */
struct LambdaSource : Source
{
    typedef std::function<size_t(MutableBytesView)> lambda_t;

    lambda_t lambda;

    LambdaSource(const lambda_t & lambda)
        : lambda(lambda)
    {
    }

    size_t read(MutableBytesView data) override
    {
        return lambda(data);
    }
};

/**
 * Chain two sources together so after the first is exhausted, the second is
 * used
 */
struct ChainSource : Source
{
    Source &source1, &source2;
    bool useSecond = false;

    ChainSource(Source & s1, Source & s2)
        : source1(s1)
        , source2(s2)
    {
    }

    size_t read(MutableBytesView data) override;
};

std::unique_ptr<FinishSink> sourceToSink(std::function<void(Source &)> fun);

/**
 * Convert a function that feeds data into a Sink into a Source. The
 * Source executes the function as a coroutine.
 */
std::unique_ptr<Source> sinkToSource(
    std::function<void(Sink &)> fun, std::function<void()> eof = []() { throw EndOfFile("coroutine has finished"); });

void writePadding(size_t len, Sink & sink);
void writeString(std::string_view s, Sink & sink);

inline Sink & operator<<(Sink & sink, uint64_t n)
{
    std::byte buf[8];
    buf[0] = std::byte(n & 0xff);
    buf[1] = std::byte((n >> 8) & 0xff);
    buf[2] = std::byte((n >> 16) & 0xff);
    buf[3] = std::byte((n >> 24) & 0xff);
    buf[4] = std::byte((n >> 32) & 0xff);
    buf[5] = std::byte((n >> 40) & 0xff);
    buf[6] = std::byte((n >> 48) & 0xff);
    buf[7] = std::byte((n >> 56) & 0xff);
    sink(BytesView{buf});
    return sink;
}

Sink & operator<<(Sink & in, const Error & ex);
Sink & operator<<(Sink & sink, std::string_view s);
Sink & operator<<(Sink & sink, const Strings & s);
Sink & operator<<(Sink & sink, const StringSet & s);

MakeError(SerialisationError, Error);

template<typename T>
T readNum(Source & source)
{
    std::byte buf[8];
    source(MutableBytesView{buf, sizeof(buf)});

    auto n = readLittleEndian<uint64_t>(reinterpret_cast<unsigned char *>(buf));

    if (n > (uint64_t) std::numeric_limits<T>::max())
        throw SerialisationError("serialised integer %d is too large for type '%s'", n, typeid(T).name());

    return (T) n;
}

inline unsigned int readInt(Source & source)
{
    return readNum<unsigned int>(source);
}

inline uint64_t readLongLong(Source & source)
{
    return readNum<uint64_t>(source);
}

void readPadding(size_t len, Source & source);
size_t readString(char * buf, size_t max, Source & source);
std::string readString(Source & source, size_t max = std::numeric_limits<size_t>::max());
template<class T>
T readStrings(Source & source);

Source & operator>>(Source & in, std::string & s);

template<typename T>
Source & operator>>(Source & in, T & n)
{
    n = readNum<T>(in);
    return in;
}

template<typename T>
Source & operator>>(Source & in, bool & b)
{
    b = readNum<uint64_t>(in);
    return in;
}

Error readError(Source & source);

/**
 * An adapter that converts a std::basic_istream into a source.
 */
struct StreamToSourceAdapter : Source
{
    std::shared_ptr<std::basic_istream<char>> istream;

    StreamToSourceAdapter(std::shared_ptr<std::basic_istream<char>> istream)
        : istream(istream)
    {
    }

    size_t read(MutableBytesView data) override
    {
        if (!istream->read(reinterpret_cast<char *>(data.data()), data.size())) {
            if (istream->eof()) {
                if (istream->gcount() == 0)
                    throw EndOfFile("end of file");
            } else
                throw Error("I/O error in StreamToSourceAdapter");
        }
        return istream->gcount();
    }
};

/**
 * A source that reads a distinct format of concatenated chunks back into its
 * logical form, in order to guarantee a known state to the original stream,
 * even in the event of errors.
 *
 * Use with FramedSink, which also allows the logical stream to be terminated
 * in the event of an exception.
 */
struct FramedSource : Source
{
    Source & from;
    bool eof = false;
    Bytes pending;
    size_t pos = 0;

    FramedSource(Source & from)
        : from(from)
    {
    }

    ~FramedSource()
    {
        try {
            if (!eof) {
                while (true) {
                    auto n = readInt(from);
                    if (!n)
                        break;
                    Bytes data(n);
                    from(MutableBytesView{data});
                }
            }
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }

    size_t read(MutableBytesView data) override
    {
        if (eof)
            throw EndOfFile("reached end of FramedSource");

        if (pos >= pending.size()) {
            size_t len = readInt(from);
            if (!len) {
                eof = true;
                return 0;
            }
            pending = Bytes(len);
            pos = 0;
            from(MutableBytesView{pending.data(), len});
        }

        auto n = std::min(data.size(), pending.size() - pos);
        memcpy(data.data(), pending.data() + pos, n);
        pos += n;
        return n;
    }
};

/**
 * Write as chunks in the format expected by FramedSource.
 *
 * The `checkError` function can be used to terminate the stream when you
 * detect that an error has occurred. It does so by throwing an exception.
 */
struct FramedSink : nix::BufferedSink
{
    BufferedSink & to;
    std::function<void()> checkError;

    FramedSink(BufferedSink & to, std::function<void()> && checkError)
        : to(to)
        , checkError(checkError)
    {
    }

    ~FramedSink()
    {
        try {
            to << 0;
            to.flush();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }

    void writeUnbuffered(BytesView data) override
    {
        /* Don't send more data if an error has occurred. */
        checkError();

        to << data.size();
        to(data);
    };
};

} // namespace nix
