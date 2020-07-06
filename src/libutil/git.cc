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
#include "serialise.hh"

using namespace std::string_literals;

namespace nix {

// Converts a Path to a ParseSink
void restoreGit(const Path & path, Source & source, const Path & realStoreDir, const Path & storeDir,
    std::function<void (ParseSink & sink, const Path & path, const Path & realStoreDir, const Path & storeDir, int perm, std::string name, Hash hash)> addEntry) {
    RestoreSink sink;
    sink.dstPath = path;
    parseGit(sink, source, realStoreDir, storeDir, addEntry);
}

void parseGit(ParseSink & sink, Source & source, const Path & realStoreDir, const Path & storeDir,
    std::function<void (ParseSink & sink, const Path & path, const Path & realStoreDir, const Path & storeDir, int perm, std::string name, Hash hash)> addEntry)
{
    parseGitWithPath(sink, source, "", realStoreDir, storeDir, addEntry);
}

static string getStringUntil(Source & source, char byte)
{
    string s;
    unsigned char n[1];
    source(n, 1);
    while (*n != byte) {
        s += *n;
        source(n, 1);
    }
    return s;
}

static string getString(Source & source, int n)
{
    std::vector<unsigned char> v(n);
    source(v.data(), n);
    return std::string(v.begin(), v.end());
}

// Unfortunately, no access to libstore headers here.
static string getStoreEntry(const Path & storeDir, Hash hash, string name)
{
    Hash hash1 = hashString(htSHA256, "fixed:out:git:" + hash.to_string(Base::Base16, true) + ":");
    Hash hash2 = hashString(htSHA256, "output:out:" + hash1.to_string(Base::Base16, true) + ":" + storeDir + ":" + name);
    Hash hash3 = compressHash(hash2, 20);

    return hash3.to_string(Base::Base32, false) + "-" + name;
}

void addGitEntry(ParseSink & sink, const Path & path,
    const Path & realStoreDir, const Path & storeDir,
    int perm, std::string name, Hash hash)
{
    string entryName = getStoreEntry(storeDir, hash, "git");
    Path entry = absPath(realStoreDir + "/" + entryName);

    struct stat st;
    if (lstat(entry.c_str(), &st))
        throw SysError("getting attributes of path '%1%'", entry);

    if (S_ISREG(st.st_mode)) {
        if (perm == 40000)
            throw SysError("file is a file but expected to be a directory '%1%'", entry);

        if (perm == 120000)
            sink.createSymlink(path + "/" + name, readFile(entry));
        else {
            if (perm == 100755 || perm == 755)
                sink.createExecutableFile(path + "/" + name);
            else
                sink.createRegularFile(path + "/" + name);

            sink.copyFile(entry);
        }
    } else if (S_ISDIR(st.st_mode)) {
        if (perm != 40000)
            throw SysError("file is a directory but expected to be a file '%1%'", entry);

        sink.copyDirectory(realStoreDir + "/" + entryName, path + "/" + name);
    } else throw Error("file '%1%' has an unsupported type", entry);
}

void parseGitWithPath(ParseSink & sink, Source & source, const Path & path,
    const Path & realStoreDir, const Path & storeDir,
    std::function<void (ParseSink & sink, const Path & path, const Path & realStoreDir, const Path & storeDir, int perm, std::string name, Hash hash)> addEntry, int perm)
{
    auto type = getString(source, 5);

    if (type == "blob ") {
        unsigned long long size = std::stoi(getStringUntil(source, 0));

        if (perm == 120000) {
            assert(size < 4096);
            std::vector<unsigned char> target(size);
            source(target.data(), size);
            sink.createSymlink(path, std::string(target.begin(), target.end()));
        } else {
            if (perm == 100755 || perm == 755)
                sink.createExecutableFile(path);
            else
                sink.createRegularFile(path);

            sink.preallocateContents(size);

            unsigned long long left = size;
            std::vector<unsigned char> buf(65536);

            while (left) {
                checkInterrupt();
                auto n = buf.size();
                if ((unsigned long long) n > left) n = left;
                source(buf.data(), n);
                sink.receiveContents(buf.data(), n);
                left -= n;
            }
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
            if (perm != 100644 && perm != 100755 && perm != 644 && perm != 755 && perm != 40000 && perm != 120000)
              throw Error("Unknown Git permission: %d", perm);

            string name = getStringUntil(source, 0);
            left -= name.size();
            left -= 1;

            string hashs = getString(source, 20);
            left -= 20;

            Hash hash(htSHA1);
            std::copy(hashs.begin(), hashs.end(), hash.hash);

            addEntry(sink, path, realStoreDir, storeDir, perm, name, hash);
        }
    } else throw Error("input doesn't look like a Git object");
}

// TODO stream file into sink, rather than reading into vector
GitMode dumpGitBlob(const Path & path, const struct stat st, Sink & sink)
{
    std::string data;
    if (S_ISLNK(st.st_mode))
        data = readLink(path);
    else
        data = readFile(path);

    auto s = fmt("blob %d\0%s"s, std::to_string(st.st_size), data);

    vector<uint8_t> v;
    std::copy(s.begin(), s.end(), std::back_inserter(v));
    sink(v.data(), v.size());

    if (S_ISLNK(st.st_mode))
        return GitMode::Symlink;
    else if (st.st_mode & S_IXUSR)
        return GitMode::Executable;
    else
        return GitMode::Regular;
}

GitMode dumpGitTree(const GitTree & entries, Sink & sink)
{
    vector<uint8_t> v1;

    for (auto & i : entries) {
        unsigned int mode;
        switch (i.second.first) {
        case GitMode::Directory: mode = 40000; break;
        case GitMode::Executable: mode = 100755; break;
        case GitMode::Regular: mode = 100644; break;
        case GitMode::Symlink: mode = 120000; break;
        }
        auto name = i.first;
        if (i.second.first == GitMode::Directory)
            name.pop_back();
        auto s1 = fmt("%d %s", mode, name);
        std::copy(s1.begin(), s1.end(), std::back_inserter(v1));
        v1.push_back(0);
        std::copy(i.second.second.hash, i.second.second.hash + 20, std::back_inserter(v1));
    }

    vector<uint8_t> v2;
    auto s2 = fmt("tree %d"s, v1.size());
    std::copy(s2.begin(), s2.end(), std::back_inserter(v2));
    v2.push_back(0);
    std::copy(v1.begin(), v1.end(), std::back_inserter(v2));

    sink(v2.data(), v2.size());

    return GitMode::Directory;
}

GitMode dumpGitWithCustomHash(std::function<std::unique_ptr<AbstractHashSink>(void)> genHashSink, const Path & path, Sink & sink, PathFilter & filter)
{
    struct stat st;
    GitMode perm;
    if (lstat(path.c_str(), &st))
        throw SysError("getting attributes of path '%1%'", path);

    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
        perm = dumpGitBlob(path, st, sink);
    else if (S_ISDIR(st.st_mode)) {
        GitTree entries;
        for (auto & i : readDirectory(path)) {
            if (filter(path + "/" + i.name)) {
                auto hashSink = genHashSink();
                auto perm = dumpGitWithCustomHash(genHashSink, path + "/" + i.name, *hashSink, filter);
                auto hash = hashSink->finish().first;

                // correctly observe git order, see
                // https://github.com/mirage/irmin/issues/352
                auto name = i.name;
                if (perm == GitMode::Directory)
                    name += "/";

                entries.insert_or_assign(name, std::pair { perm, hash });
            }
        }
        perm = dumpGitTree(entries, sink);
    } else throw Error("file '%1%' has an unsupported type", path);

    return perm;
}


Hash dumpGitHashWithCustomHash(std::function<std::unique_ptr<AbstractHashSink>(void)> genHashSink, const Path & path, PathFilter & filter)
{
    auto hashSink = genHashSink();
    dumpGitWithCustomHash(genHashSink, path, *hashSink, filter);
    return hashSink->finish().first;
}

void dumpGit(HashType ht, const Path & path, Sink & sink, PathFilter & filter)
{
    assert(ht == htSHA1);
    dumpGitWithCustomHash([&]{ return std::make_unique<HashSink>(ht); }, path, sink, filter);
}

Hash dumpGitHash(HashType ht, const Path & path, PathFilter & filter)
{
    return dumpGitHashWithCustomHash([&]{ return std::make_unique<HashSink>(ht); }, path, filter);
}

}
