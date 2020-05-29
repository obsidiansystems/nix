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

static void parse(ParseSink & sink, Source & source, const Path & path, const Path & storeDir);

// Converts a Path to a ParseSink
void restoreGit(const Path & path, Source & source, const Path & storeDir) {
    RestoreSink sink;
    sink.dstPath = path;
    parseGit(sink, source, storeDir);
}

void parseGit(ParseSink & sink, Source & source, const Path & storeDir) {
    parse(sink, source, "", storeDir);
}

string getStringUntil(Source & source, char byte) {
    string s;
    unsigned char n[1];
    source(n, 1);
    while (*n != byte) {
        s += *n;
        source(n, 1);
    }
    return s;
}

string getString(Source & source, int n){
    std::vector<unsigned char> v(n);
    source(v.data(), n);
    return std::string(v.begin(), v.end());
}

static void parse(ParseSink & sink, Source & source, const Path & path, const Path & storeDir) {
    auto type = getString(source, 5);

    if (type == "blob ") {
        sink.createRegularFile(path);

        unsigned long long size = std::stoi(getStringUntil(source, 0));

        sink.preallocateContents(size);

        unsigned long long left = size;
        std::vector<unsigned char> buf(65536);

        while (left) {
            checkInterrupt();
            auto n = buf.size();
            if ((unsigned long long)n > left) n = left;
            source(buf.data(), n);
            sink.receiveContents(buf.data(), n);
            left -= n;
        }
    } else if (type == "tree ") {
        unsigned long long size = std::stoi(getStringUntil(source, 0));
        unsigned long long left = size;

        sink.createDirectory(path);

        while (left) {
            string perms = getStringUntil(source, ' ');
            left -= perms.size();
            left -= 1;

            int perm = std::stoi(perms);
            if (perm != 100644 && perm != 100755 && perm != 644 && perm != 755 && perm != 40000)
              throw Error(format("Unknown Git permission: %d") % perm);

            string name = getStringUntil(source, 0);
            left -= name.size();
            left -= 1;

            string hashs = getString(source, 20);
            left -= 20;

            Hash hash1(htSHA1);
            std::copy(hashs.begin(), hashs.end(), hash1.hash);

            Hash hash2 = hashString(htSHA256, "fixed:out:git:" + hash1.to_string(Base16) + ":");
            Hash hash3 = hashString(htSHA256, "output:out:" + hash2.to_string(Base16) + ":" + storeDir + ":" + name);
            Hash hash4 = compressHash(hash3, 20);

            string entryName = hash4.to_string(Base32, false) + "-" + name;
            Path entry = storeDir + "/" + entryName;

            struct stat st;
            if (lstat(entry.c_str(), &st))
                throw SysError(format("getting attributes of path '%1%'") % entry);

            if (S_ISREG(st.st_mode)) {
                if (perm == 40000)
                    throw SysError(format("file is a file but expected to be a directory '%1%'") % entry);

                if (perm == 100755 || perm == 755)
                    sink.createExecutableFile(path + "/" + name);
                else
                    sink.createRegularFile(path + "/" + name);

                unsigned long long size = st.st_size;
                sink.preallocateContents(size);

                unsigned long long left = size;
                std::vector<unsigned char> buf(65536);

                StringSink ssink;
                readFile(entry, ssink);
                AutoCloseFD fd = open(entry.c_str(), O_RDONLY | O_CLOEXEC);

                while (left) {
                    checkInterrupt();
                    auto n = buf.size();
                    if ((unsigned long long)n > left) n = left;
                    ssink(buf.data(), n);
                    sink.receiveContents(buf.data(), n);
                    left -= n;
                }
            } else if (S_ISDIR(st.st_mode)) {
                if (perm != 40000)
                    throw SysError(format("file is a directory but expected to be a file '%1%'") % entry);

                sink.createSymlink(path + "/" + name, "../" + entryName);
            } else throw Error(format("file '%1%' has an unsupported type") % entry);
        }
    } else throw Error("input doesn't look like a Git object");
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
                s1 += (format("%06d %s\0%s"s) % perm % i.first % hash.hash).str();
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
