#pragma once

#include "types.hh"
#include "serialise.hh"

namespace nix {

void dumpGit(const Path & path, Sink & sink, PathFilter & filter = defaultPathFilter);

}
