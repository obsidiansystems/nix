#include "nix/store/references.hh"
#include "nix/store/path.hh"
#include "nix/util/hash.hh"
#include "nix/util/base-nix-32.hh"

#include <map>
#include <cstdlib>
#include <mutex>
#include <algorithm>

namespace nix {

static constexpr auto refLength = StorePath::HashLen;

static void search(std::string_view s, StringSet & hashes, StringSet & seen)
{
    for (size_t i = 0; i + refLength <= s.size();) {
        int j;
        bool match = true;
        for (j = refLength - 1; j >= 0; --j)
            if (!BaseNix32::lookupReverse(s[i + j])) {
                i += j + 1;
                match = false;
                break;
            }
        if (!match)
            continue;
        std::string ref(s.substr(i, refLength));
        if (hashes.erase(ref)) {
            debug("found reference to '%1%' at offset '%2%'", ref, i);
            seen.insert(ref);
        }
        ++i;
    }
}

void RefScanSink::operator()(BytesView data)
{
    /* It's possible that a reference spans the previous and current
       fragment, so search in the concatenation of the tail of the
       previous fragment and the start of the current fragment. */
    Bytes s(tail);
    auto tailLen = std::min(data.size(), refLength);
    s.insert(s.end(), data.begin(), data.begin() + tailLen);
    search(as_str(s), hashes, seen);

    search(as_str(data), hashes, seen);

    auto rest = refLength - tailLen;
    if (rest < tail.size())
        tail = Bytes(tail.begin() + (tail.size() - rest), tail.end());
    auto tailData = data.subspan(data.size() - tailLen);
    tail.insert(tail.end(), tailData.begin(), tailData.end());
}

RewritingSink::RewritingSink(BytesView from, BytesView to, Sink & nextSink)
    : RewritingSink(std::map<Bytes, Bytes>{{Bytes(from.begin(), from.end()), Bytes(to.begin(), to.end())}}, nextSink)
{
}

RewritingSink::RewritingSink(std::map<Bytes, Bytes> && rewrites, Sink & nextSink)
    : rewrites(std::move(rewrites))
    , nextSink(nextSink)
{
    Bytes::size_type maxRewriteSize = 0;
    for (auto & [from, to] : this->rewrites) {
        assert(from.size() == to.size());
        maxRewriteSize = std::max(maxRewriteSize, from.size());
    }
    this->maxRewriteSize = maxRewriteSize;
}

void RewritingSink::operator()(BytesView data)
{
    Bytes s(prev);
    s.insert(s.end(), data.begin(), data.end());

    // Rewrite bytes
    for (auto & [from, to] : rewrites) {
        size_t pos = 0;
        while ((pos = std::search(s.begin() + pos, s.end(), from.begin(), from.end()) - s.begin()) < s.size()) {
            std::copy(to.begin(), to.end(), s.begin() + pos);
            matches.push_back(this->pos + pos);
            pos += from.size();
        }
    }

    prev = s.size() < maxRewriteSize ? s
           : maxRewriteSize == 0     ? Bytes{}
                                     : Bytes(s.begin() + (s.size() - maxRewriteSize + 1), s.end());

    auto consumed = s.size() - prev.size();

    pos += consumed;

    if (consumed)
        nextSink(BytesView{s.data(), consumed});
}

void RewritingSink::flush()
{
    if (prev.empty())
        return;
    pos += prev.size();
    nextSink(prev);
    prev.clear();
}

HashModuloSink::HashModuloSink(HashAlgorithm ha, BytesView modulus)
    : hashSink(ha)
    , rewritingSink(modulus, Bytes(modulus.size(), std::byte{0}), hashSink)
{
}

void HashModuloSink::operator()(BytesView data)
{
    rewritingSink(data);
}

HashResult HashModuloSink::finish()
{
    rewritingSink.flush();

    /* Hash the positions of the self-references. This ensures that a
       NAR with self-references and a NAR with some of the
       self-references already zeroed out do not produce a hash
       collision. FIXME: proof. */
    for (auto & pos : rewritingSink.matches)
        hashSink(as_bytes(fmt("|%d", pos)));

    auto h = hashSink.finish();
    return {.hash = h.hash, .numBytesDigested = rewritingSink.pos};
}

} // namespace nix
