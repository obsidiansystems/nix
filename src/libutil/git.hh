#pragma once

#include "types.hh"
#include "serialise.hh"
#include "fs-sink.hh"

namespace nix {

void restoreGit(const Path & path, Source & source);

void parseGit(ParseSink & sink, Source & source);

void dumpGit(const Path & path, Sink & sink, PathFilter & filter = defaultPathFilter);

}
