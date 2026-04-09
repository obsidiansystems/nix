#include <map>

#include "nix/util/go.hh"
#include "nix/util/configuration.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/error.hh"

namespace nix::go {

using namespace nix;

/**
 * @note The map keys must be `std::string`, not `CanonPath`: we need
 * byte-wise sort order to match Go's `sort.Strings`, but
 * `std::map<CanonPath, ...>` would use `CanonPath::operator<=>`,
 * which sorts `/` as 0 and so puts directories immediately
 * before their children. That ordering disagrees with byte-wise
 * sort whenever a sibling sorts between e.g. `foo.bar` and
 * `foo/bar` (`.` < `/` byte-wise, but `/` < `.` for CanonPath).
 */
using DirEntries = std::map<std::string, Hash>;

static void
collect(HashAlgorithm ha, const SourcePath & path, const CanonPath & relPath, PathFilter & filter, DirEntries & out)
{
    auto st = path.lstat();

    switch (st.type) {

    case SourceAccessor::tRegular: {
        if (st.isExecutable)
            throw Error("Go dirhash does not support executable files: '%s'", path);
        std::string key{relPath.rel()};
        if (key.find('\n') != std::string::npos)
            throw Error("Go dirhash does not support filenames containing newlines: '%s'", key);
        out.insert_or_assign(std::move(key), hashString(ha, path.readFile()));
        break;
    }

    case SourceAccessor::tDirectory: {
        for (auto & [name, _] : path.readDirectory()) {
            auto child = path / name;
            if (!filter(child.path.abs()))
                continue;
            collect(ha, child, relPath / name, filter, out);
        }
        break;
    }

    case SourceAccessor::tSymlink:
        throw Error("Go dirhash does not support symlinks: '%s'", path);

    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
    default:
        throw Error("file '%s' has an unsupported type for Go hashing", path);
    }
}

Hash dumpHash(
    HashAlgorithm ha, const SourcePath & root, PathFilter & filter, const ExperimentalFeatureSettings & xpSettings)
{
    xpSettings.require(Xp::GoHashing);

    if (ha != HashAlgorithm::SHA256)
        throw Error("Go module hashing only supports SHA-256, but got: %s", printHashAlgo(ha));

    auto rootSt = root.lstat();
    if (rootSt.type != SourceAccessor::tDirectory)
        throw Error("Go dirhash requires a directory as the root, but '%s' is not a directory", root);

    DirEntries entries;
    collect(ha, root, CanonPath::root, filter, entries);

    std::string manifest;
    for (auto & [name, hash] : entries) {
        manifest += hash.to_string(HashFormat::Base16, false);
        manifest += "  ";
        manifest += name;
        manifest += '\n';
    }

    return hashString(ha, manifest);
}

} // namespace nix::go
