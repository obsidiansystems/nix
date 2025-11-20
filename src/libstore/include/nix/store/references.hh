#pragma once
///@file

#include "nix/util/hash.hh"

namespace nix {

class RefScanSink : public Sink
{
    StringSet hashes;
    StringSet seen;

    Bytes tail;

public:

    RefScanSink(StringSet && hashes)
        : hashes(hashes)
    {
    }

    StringSet & getResult()
    {
        return seen;
    }

    void operator()(BytesView data) override;
};

struct RewritingSink : Sink
{
    const std::map<Bytes, Bytes> rewrites;
    Bytes::size_type maxRewriteSize;
    Bytes prev;
    Sink & nextSink;
    uint64_t pos = 0;

    std::vector<uint64_t> matches;

    RewritingSink(BytesView from, BytesView to, Sink & nextSink);
    RewritingSink(std::map<Bytes, Bytes> && rewrites, Sink & nextSink);

    void operator()(BytesView data) override;

    void flush();
};

struct HashModuloSink : AbstractHashSink
{
    HashSink hashSink;
    RewritingSink rewritingSink;

    HashModuloSink(HashAlgorithm ha, BytesView modulus);

    void operator()(BytesView data) override;

    HashResult finish() override;
};

} // namespace nix
