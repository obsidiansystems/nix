#pragma once

#include "types.hh"
#include "serialise.hh"
#include "fs-sink.hh"

namespace nix {

enum struct GitMode {
    Directory,
    Executable,
    Regular,
};

void addGitEntry(ParseSink & sink, const Path & path,
    const Path & realStoreDir, const Path & storeDir,
    int perm, std::string name, Hash hash);

void restoreGit(const Path & path, Source & source, const Path & realStoreDir, const Path & storeDir,
    std::function<void (ParseSink & sink, const Path & path, const Path & realStoreDir, const Path & storeDir, int perm, std::string name, Hash hash)> addEntry = addGitEntry);

void parseGit(ParseSink & sink, Source & source, const Path & realStoreDir, const Path & storeDir,
    std::function<void (ParseSink & sink, const Path & path, const Path & realStoreDir, const Path & storeDir, int perm, std::string name, Hash hash)> addEntry = addGitEntry);

// Dumps a single file to a sink
GitMode dumpGitBlob(const Path & path, const struct stat st, Sink & sink);

typedef std::map<string, std::pair<GitMode, Hash>> GitTree;

// Dumps a representation of a git tree to a sink
GitMode dumpGitTree(const GitTree & entries, Sink & sink);

// Recursively dumps path, hashing as we go
Hash dumpGitHash(HashType ht, const Path & path, PathFilter & filter = defaultPathFilter);

void dumpGit(HashType ht, const Path & path, Sink & sink, PathFilter & filter = defaultPathFilter);

void parseGitInternal(ParseSink & sink, Source & source, const Path & path,
    const Path & realStoreDir, const Path & storeDir,
    std::function<void (ParseSink & sink, const Path & path, const Path & realStoreDir, const Path & storeDir, int perm, std::string name, Hash hash)> addEntry);

}
