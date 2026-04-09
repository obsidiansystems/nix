#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/configuration.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"

namespace nix::go {

/**
 * Recursively hash the directory `path` using the Go module dirhash
 * algorithm.
 *
 * This is a port of `Hash1` from `golang.org/x/mod/sumdb/dirhash`:
 *
 *   1. Walk the directory tree rooted at `path`.
 *   2. For each regular file, compute its content hash.
 *   3. Build a manifest of `"<hex hash>  <relative path>\n"` lines, sorted
 *      lexicographically by slash-joined relative path (no leading slash).
 *   4. Hash the manifest. The result is the digest of the directory.
 *
 * `path` must be a directory; passing a regular file (or anything else)
 * throws. Only regular, non-executable files inside the tree are
 * supported — symlinks and executable files are rejected with an error,
 * since Go's dirhash format doesn't preserve those distinctions and
 * silently ingesting them would give a digest that doesn't faithfully
 * represent the input.
 *
 * @param ha must be `HashAlgorithm::SHA256`. Go's dirhash format only
 * defines `h1:` (SHA-256); other algorithms throw.
 *
 * @param filter Files for which `filter(path)` returns false are skipped.
 *
 * @param xpSettings Experimental feature settings; defaults to the
 * process-wide settings. Tests use this to enable `go-hashing` locally
 * without mutating global state.
 */
Hash dumpHash(
    HashAlgorithm ha,
    const SourcePath & path,
    PathFilter & filter = defaultPathFilter,
    const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

} // namespace nix::go
