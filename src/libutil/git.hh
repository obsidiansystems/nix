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

void restoreGit(const Path & path, Source & source, const Path & storeDir);

void parseGit(ParseSink & sink, Source & source, const Path & storeDir);

void dumpGit(const Path & path, Sink & sink, PathFilter & filter = defaultPathFilter);

}
