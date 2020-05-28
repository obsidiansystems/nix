#include <cerrno>
#include <algorithm>
#include <vector>
#include <map>

#include <strings.h> // for strcasecmp

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include "util.hh"
#include "config.hh"
#include "hash.hh"

#include "git.hh"

using namespace std::string_literals;

namespace nix {

static void parse(ParseSink & sink, Source & source, const Path & path);

// Converts a Path to a ParseSink
void restoreGit(const Path & path, Source & source) {

    RestoreSink sink;
    sink.dstPath = path;
    parseGit(sink, source);

}

void parseGit(ParseSink & sink, Source & source) {
    parse(sink, source, "");
}

static void parse(ParseSink & sink, Source & source, const Path & path) {
    uint8_t buf[4];

    std::basic_string_view<uint8_t> buf_v {
        (const uint8_t *) & buf,
        std::size(buf)
    };
    source(buf_v);
    if (buf_v.compare((const uint8_t *)"blob")) {
        uint8_t space;
        source(& space, 1);
    }
    else {
    }
}

// Internal version, returns the perm.
unsigned int dumpGitInternal(const Path & path, Sink & sink, PathFilter & filter)
{
    struct stat st;
    if (lstat(path.c_str(), &st))
        throw SysError(format("getting attributes of path '%1%'") % path);

    if (S_ISREG(st.st_mode)) {
        auto s = (format("blob %d\0%s"s) % std::to_string(st.st_size) % readFile(path)).str();

        vector<unsigned char> v;
        std::copy(s.begin(), s.end(), std::back_inserter(v));
        sink(v.data(), v.size());
        return st.st_mode & S_IXUSR
            ? 100755
            : 100644;
    }

    else if (S_ISDIR(st.st_mode)) {
        std::string s1 = "";

        std::map<string, string> entries;
        for (auto & i : readDirectory(path))
            entries[i.name] = i.name;

        for (auto & i : entries)
            if (filter(path + "/" + i.first)) {
                HashSink hashSink(htSHA1);
                unsigned int perm = dumpGitInternal(path + "/" + i.first, hashSink, filter);
                auto hash = hashSink.finish().first;
                s1 += (format("%6d %s\0%s"s) % perm % i.first % hash.hash).str();
            }

        std::string s2 = (format("tree %d\0%s"s) % s1.size() % s1).str();

        vector<unsigned char> v;
        std::copy(s2.begin(), s2.end(), std::back_inserter(v));
        sink(v.data(), v.size());
        return 40000;
    }

    else throw Error(format("file '%1%' has an unsupported type") % path);
}

void dumpGit(const Path & path, Sink & sink, PathFilter & filter)
{
    dumpGitInternal(path, sink, filter);
}

}
