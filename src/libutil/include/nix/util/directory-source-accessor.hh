#pragma once
/// @file

#include "nix/util/source-accessor.hh"
#include "nix/util/ref.hh"

#include <filesystem>

namespace nix {

struct SourcePath;

/**
 * Create a directory accessor rooted at @param root.
 *
 * On unix accesses are performed with openat. Linux uses openat2 if supported
 * by the kernel (>= 5.6).
 *
 * On Windows returns a simple filesystem accessor using std::filesystem.
 *
 * @param fd Descriptor of the directory.
 * @param root Filesystem path corresponding to fd. Must correspond to a directory.
 */
ref<SourceAccessor>
makeDirectorySourceAccessor(AutoCloseFD fd, std::filesystem::path root, bool trackLastModified = false);

/**
 * Create a `SourceAccessor` and `SourcePath` corresponding to
 * some native path.
 *
 * @param trackLastModified Whether the accessor should return a non-null `getLastModified`.
 * When true the accessor must be used only by a single thread.
 *
 * The accessor is rooted as far up the tree as possible (e.g. at
 * the filesystem root `/`). This allows more `..` parent accessing
 * to work.
 *
 * @note When `path` is trusted user input, canonicalize it using
 * `std::filesystem::canonical`, `makeParentCanonical`, `std::filesystem::weakly_canonical`, etc,
 * as appropriate for the use case. At least weak canonicalization is
 * required for the `SourcePath` to do anything useful at the location it
 * points to.
 *
 * @note A canonicalizing behavior is not built in so that
 * callers do not accidentally introduce symlink-related security vulnerabilities.
 * Furthermore, it cannot decide whether the file pointed to by
 * `path` should be resolved if it is itself a symlink.
 *
 * See
 * [`std::filesystem::path::root_path`](https://en.cppreference.com/w/cpp/filesystem/path/root_path)
 * and
 * [`std::filesystem::path::relative_path`](https://en.cppreference.com/w/cpp/filesystem/path/relative_path).
 */
SourcePath createAtRoot(const std::filesystem::path & path, bool trackLastModified = false);

} // namespace nix
