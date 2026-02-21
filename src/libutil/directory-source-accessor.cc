// FIXME: detect in meson
#define HAVE_OPENAT2 1
#define HAVE_OPENAT 1

#include "util-config-private.hh"
#include "nix/util/signals.hh"
#include "nix/util/directory-source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/file-system.hh"

#if defined(__linux__) && HAVE_OPENAT2
#  include <sys/syscall.h>
#  include <linux/openat2.h>
#endif

#include <atomic>
#include <ranges>

namespace nix {

/**
 * Convert a `struct stat` to a `SourceAccessor::Stat`.
 */
static SourceAccessor::Stat makeStat(const struct ::stat & st)
{
    return SourceAccessor::Stat{
        .type = S_ISREG(st.st_mode)   ? SourceAccessor::tRegular
                : S_ISDIR(st.st_mode) ? SourceAccessor::tDirectory
                : S_ISLNK(st.st_mode) ? SourceAccessor::tSymlink
                : S_ISCHR(st.st_mode) ? SourceAccessor::tChar
                : S_ISBLK(st.st_mode) ? SourceAccessor::tBlock
                :
#ifdef S_ISSOCK
                S_ISSOCK(st.st_mode) ? SourceAccessor::tSocket
                :
#endif
                S_ISFIFO(st.st_mode) ? SourceAccessor::tFifo
                                     : SourceAccessor::tUnknown,
        .fileSize = S_ISREG(st.st_mode) ? std::optional<uint64_t>(st.st_size) : std::nullopt,
        .isExecutable = S_ISREG(st.st_mode) && st.st_mode & S_IXUSR,
    };
}

#if HAVE_OPENAT

namespace {

class DirFdSourceAccessor : public SourceAccessor
{
    /**
     * File descriptor of the root directory (or parent directory when
     * `rootBasename` is set).
     */
    AutoCloseFD dirFd;

    /**
     * Path corresponding to the accessor.
     * @warning Do not use for any file operations!
     */
    std::filesystem::path root;

    /**
     * When the root is a file or symlink rather than a directory,
     * this holds the filename within the parent directory (`dirFd`).
     * Path operations on non-root paths will fail in this mode.
     */
    std::optional<std::string> rootBasename;

    /**
     * The most recent mtime seen by lstat(). This is a hack to
     * support dumpPathAndGetMtime(). Should remove this eventually.
     */
    time_t mtime = 0;

    bool trackLastModified = false;

    AutoCloseFD openFile(const CanonPath & path, int flags)
    {
        if (path.isRoot()) {
            if (rootBasename)
                return ::openat(dirFd.get(), rootBasename->c_str(), flags);
            return ::openat(dirFd.get(), ".", flags);
        }

        if (rootBasename)
            /* Root is a file/symlink; it has no children. */
            return AutoCloseFD();

#  if defined(__linux__) && HAVE_OPENAT2
        /* Cache the result of whether openat2 is not supported. */
        static std::atomic_flag openat2Unsupported = ATOMIC_FLAG_INIT;

        if (!openat2Unsupported.test()) {
            /* No glibc wrapper yet, but there's a patch:
             * https://patchwork.sourceware.org/project/glibc/patch/20251029200519.3203914-1-adhemerval.zanella@linaro.org/
             */
            auto how = ::open_how{
                .flags = static_cast<decltype(::open_how::flags)>(flags),
                /* Symlinks are disallowed. RESOLVE_BENEATH is a bit overkill, since
                   CanonPath has the invariant of not having any `..` components, but
                   that's good practice anyway. */
                .resolve = RESOLVE_NO_SYMLINKS | RESOLVE_BENEATH,
            };

            auto res = ::syscall(__NR_openat2, dirFd.get(), path.rel_c_str(), &how, sizeof(how));
            if (res < 0 && errno == ENOSYS) {
                /* Cache that the syscall is not supported and fall through to openat. */
                openat2Unsupported.test_and_set();
            } else if (res < 0 && (errno == ELOOP || errno == ENOTDIR)) {
                /* openat2 with RESOLVE_NO_SYMLINKS may return ELOOP or
                   ENOTDIR when a symlink is encountered. Fall through
                   to the loop-based approach which does precise
                   per-component symlink detection. */
            } else {
                return res;
            }
        }
#  endif

        AutoCloseFD parentFd;
        auto nrComponents = std::ranges::distance(path);
        auto components = std::views::take(path, nrComponents - 1); /* Everything but last component */
        auto getParentFd = [&]() { return parentFd ? parentFd.get() : dirFd.get(); };

        /* This rather convoluted loop is necessary to avoid TOCTOU when validating that
           no inner path component is a symlink. */
        for (auto it = components.begin(); it != components.end(); ++it) {
            std::string_view component = *it;
            auto currentDir = getParentFd();
            auto newFd = AutoCloseFD{::openat(
                currentDir,
                std::string(component).c_str(), /* Copy into a string to make NUL terminated. */
                O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};

            if (!newFd) {
                auto savedErrno = errno;

                /* Construct the CanonPath for error message. */
                auto path2 = std::ranges::fold_left(components.begin(), ++it, CanonPath::root, [](auto lhs, auto rhs) {
                    lhs.push(rhs);
                    return lhs;
                });

                if (savedErrno == ELOOP)
                    throw SymlinkNotAllowed(path2, "path '%s' is a symlink", showPath(path2));

                if (savedErrno == ENOTDIR) {
                    /* On some kernels, O_DIRECTORY | O_NOFOLLOW on a
                       symlink returns ENOTDIR instead of ELOOP. Check
                       whether the component is actually a symlink. */
                    struct ::stat stBuf;
                    if (::fstatat(currentDir, std::string(component).c_str(), &stBuf, AT_SYMLINK_NOFOLLOW) == 0
                        && S_ISLNK(stBuf.st_mode))
                        throw SymlinkNotAllowed(path2, "path '%s' is a symlink", showPath(path2));
                }

                errno = savedErrno;
                return AutoCloseFD();
            }

            parentFd = std::move(newFd);
        }

        auto result = ::openat(getParentFd(), std::string(path.baseName().value()).c_str(), flags);
        if (result < 0 && (errno == ELOOP || errno == ENOTDIR)) {
            /* Check if the last component is actually a symlink. */
            struct ::stat stBuf;
            if (::fstatat(getParentFd(), std::string(path.baseName().value()).c_str(), &stBuf, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(stBuf.st_mode))
                throw SymlinkNotAllowed(path, "path '%s' is a symlink", showPath(path));
        }
        return result;
    }

    std::optional<struct ::stat> maybeLstatImpl(const CanonPath & path)
    {
        std::optional<struct ::stat> st{std::in_place};

        if (path.isRoot()) {
            if (rootBasename) {
                if (::fstatat(dirFd.get(), rootBasename->c_str(), &*st, AT_SYMLINK_NOFOLLOW)) {
                    if (errno == ENOENT || errno == ENOTDIR)
                        return std::nullopt;
                    throw SysError("getting status of '%s'", showPath(path));
                }
            } else {
                if (::fstat(dirFd.get(), &*st)) /* Is already open. */
                    throw SysError("getting status of '%s'", showPath(path));
            }
            return st;
        }

        if (rootBasename)
            /* Root is a file/symlink; it has no children. */
            return std::nullopt;

        auto parentPath = path.parent().value();
        auto fd = openFile(parentPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_DIRECTORY);
        if (!fd) {
            if (errno == ENOENT || errno == ENOTDIR)
                return std::nullopt;
            throw SysError("opening parent path of '%s'", showPath(path));
        }

        /* Should have the same semantics as lstat on a path. */
        if (::fstatat(fd.get(), std::string(path.baseName().value()).c_str(), &*st, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT || errno == ENOTDIR)
                st.reset();
            else
                throw SysError("getting status of '%s'", showPath(path));
        }

        return st;
    }

public:
    /**
     * Construct an accessor for a directory root.
     */
    DirFdSourceAccessor(AutoCloseFD rootFd_, std::filesystem::path root_, bool trackLastModified)
        : dirFd(std::move(rootFd_))
        , root(std::move(root_))
        , trackLastModified(trackLastModified)
    {
        if (root != root.root_path()) /* Don't prefix root directory. */
            displayPrefix = root.string();
        else
            displayPrefix.clear();
    }

    /**
     * Construct an accessor for a file or symlink root. `parentFd` is
     * an fd to the parent directory, and `basename` is the name of
     * the entry within that directory.
     */
    DirFdSourceAccessor(
        AutoCloseFD parentFd_,
        std::filesystem::path root_,
        std::string basename_,
        bool trackLastModified)
        : dirFd(std::move(parentFd_))
        , root(std::move(root_))
        , rootBasename(std::move(basename_))
        , trackLastModified(trackLastModified)
    {
        if (root != root.root_path())
            displayPrefix = root.string();
        else
            displayPrefix.clear();
    }

    void readFile(const CanonPath & path, Sink & sink, std::function<void(uint64_t)> sizeCallback) override
    {
        auto fd = openFile(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (!fd)
            throw SysError("opening file '%s'", showPath(path));

        auto size = getFileSize(fd.get());
        sizeCallback(size);
        drainFD(fd.get(), sink, {.expectedSize = size});
    }

    bool pathExists(const CanonPath & path) override
    {
        return maybeLstatImpl(path).has_value();
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto st = maybeLstatImpl(path);
        if (!st)
            return std::nullopt;

        /* The contract is that trackLastModified implies that the caller uses the accessor
           from a single thread. Thus this is not a CAS loop. */
        if (trackLastModified)
            mtime = std::max(mtime, st->st_mtime);

        return makeStat(*st);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        if (rootBasename && !path.isRoot())
            throw Error("path '%s' does not exist (root is not a directory)", showPath(path));

        auto fd = openFile(path, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (!fd)
            throw SysError("opening directory '%1%'", path);

        auto dir = AutoCloseDir{::fdopendir(fd.get())};
        if (!dir)
            throw SysError("opening directory '%1%'", path);

        fd.release();

        DirEntries entries;
        struct dirent * dirent;

        while (errno = 0, dirent = ::readdir(dir.get())) {
            checkInterrupt();
            std::string name = dirent->d_name;
            if (name == "." || name == "..")
                continue;

            auto type = [&]() -> std::optional<Type> {
                switch (dirent->d_type) {
                case DT_REG:
                    return tRegular;
                case DT_DIR:
                    return tDirectory;
                case DT_LNK:
                    return tSymlink;
                case DT_BLK:
                    return tBlock;
                case DT_CHR:
                    return tChar;
                case DT_FIFO:
                    return tFifo;
                case DT_SOCK:
                    return tSocket;
                case DT_UNKNOWN:
                default:
                    return std::nullopt;
                }
            }();

            entries.emplace(std::move(name), type);
        }

        if (errno)
            throw SysError("reading directory '%1%'", showPath(path));

        return entries;
    }

    std::string readLink(const CanonPath & path) override
    {
        if (path.isRoot()) {
            if (rootBasename) {
                std::string target;
                target.resize(PATH_MAX);
                auto len = ::readlinkat(dirFd.get(), rootBasename->c_str(), target.data(), target.size());
                if (len < 0)
                    throw SysError("reading link '%1%'", showPath(path));
                target.resize(len);
                return target;
            }
            throw Error("file '%s' is not a symbolic link", path);
        }

        if (rootBasename)
            throw Error("path '%s' does not exist (root is not a directory)", showPath(path));

        auto parentPath = path.parent().value();
        auto basename = std::string(path.baseName().value());

        auto parentFd = openFile(
            parentPath,
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (!parentFd)
            throw SysError("opening path '%1%'", showPath(parentPath));

        std::string target;
        target.resize(PATH_MAX);
        auto len = ::readlinkat(parentFd.get(), basename.c_str(), target.data(), target.size());
        if (len < 0)
            throw SysError("reading link '%1%'", showPath(path));
        target.resize(len);
        return target;
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return root / path.rel();
    }

    std::optional<std::time_t> getLastModified() override
    {
        return trackLastModified ? std::optional{mtime} : std::nullopt;
    }
};

} // namespace

#else // !HAVE_OPENAT (Windows fallback)

namespace {

/**
 * Fallback filesystem source accessor for platforms without `openat`
 * (e.g. Windows). Uses `std::filesystem` calls with absolute paths.
 */
class SimpleFSSourceAccessor : public SourceAccessor
{
    std::filesystem::path root;

    time_t mtime = 0;

    bool trackLastModified = false;

    std::filesystem::path makeAbsPath(const CanonPath & path)
    {
        return root.empty()    ? (std::filesystem::path{path.abs()})
               : path.isRoot() ? root
                               : root / path.rel();
    }

public:
    SimpleFSSourceAccessor(std::filesystem::path root_, bool trackLastModified)
        : root(std::move(root_))
        , trackLastModified(trackLastModified)
    {
        assert(root.empty() || root.is_absolute());
        if (root != root.root_path())
            displayPrefix = root.string();
        else
            displayPrefix.clear();
    }

    void readFile(const CanonPath & path, Sink & sink, std::function<void(uint64_t)> sizeCallback) override
    {
        auto ap = makeAbsPath(path);

        AutoCloseFD fd = toDescriptor(open(
            ap.string().c_str(),
            O_RDONLY
#ifndef _WIN32
                | O_NOFOLLOW | O_CLOEXEC
#endif
            ));
        if (!fd)
            throw SysError("opening file '%1%'", ap.string());

        auto size = getFileSize(fd.get());
        sizeCallback(size);
        drainFD(fd.get(), sink, {.expectedSize = size});
    }

    bool pathExists(const CanonPath & path) override
    {
        return nix::pathExists(makeAbsPath(path).string());
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto st = nix::maybeLstat(makeAbsPath(path).string().c_str());
        if (!st)
            return std::nullopt;

        if (trackLastModified)
            mtime = std::max(mtime, st->st_mtime);

        return makeStat(*st);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        DirEntries res;
        for (auto & entry : DirectoryIterator{makeAbsPath(path)}) {
            checkInterrupt();
            auto type = [&]() -> std::optional<Type> {
                try {
                    if (entry.is_symlink())
                        return tSymlink;
                    if (entry.is_regular_file())
                        return tRegular;
                    if (entry.is_directory())
                        return tDirectory;
                    if (entry.is_character_file())
                        return tChar;
                    if (entry.is_block_file())
                        return tBlock;
                    if (entry.is_fifo())
                        return tFifo;
                    if (entry.is_socket())
                        return tSocket;
                    return tUnknown;
                } catch (std::filesystem::filesystem_error & e) {
                    if (e.code() == std::errc::permission_denied || e.code() == std::errc::operation_not_permitted)
                        return std::nullopt;
                    else
                        throw SystemError(e.code(), "getting status of '%s'", PathFmt(entry.path()));
                }
            }();
            res.emplace(entry.path().filename().string(), type);
        }
        return res;
    }

    std::string readLink(const CanonPath & path) override
    {
        return nix::readLink(makeAbsPath(path).string());
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return makeAbsPath(path);
    }

    std::optional<std::time_t> getLastModified() override
    {
        return trackLastModified ? std::optional{mtime} : std::nullopt;
    }
};

} // namespace

#endif

ref<SourceAccessor> makeDirectorySourceAccessor(AutoCloseFD fd, std::filesystem::path root, bool trackLastModified)
{
#if HAVE_OPENAT
    return make_ref<DirFdSourceAccessor>(std::move(fd), std::move(root), trackLastModified);
#else
    return make_ref<SimpleFSSourceAccessor>(std::move(root), trackLastModified);
#endif
}

ref<SourceAccessor> getFSSourceAccessor()
{
    static auto rootFS = ({
        std::filesystem::path rootPath{"/"};
        AutoCloseFD fd = toDescriptor(
            ::open(rootPath.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
        if (!fd)
            throw SysError("opening root filesystem");
#if HAVE_OPENAT
        make_ref<DirFdSourceAccessor>(std::move(fd), std::move(rootPath), false);
#else
        make_ref<SimpleFSSourceAccessor>(std::move(rootPath), false);
#endif
    });
    return rootFS;
}

ref<SourceAccessor> makeFSSourceAccessor(std::filesystem::path root, bool trackLastModified)
{
    AutoCloseFD fd = toDescriptor(
        ::open(
            root.c_str(),
            O_RDONLY |
#ifndef _WIN32
                O_NOFOLLOW | O_CLOEXEC
#endif
            ));

    if (!fd) {
        if (errno == ELOOP || errno == ENOENT) {
            /* The path itself is a symlink (ELOOP) or does not exist
               (ENOENT). Open the parent directory and store the
               basename so we can access it via `fstatat`/`readlinkat`. */
            auto parent = root.parent_path();
            auto basename = root.filename().string();
            AutoCloseFD parentFd = toDescriptor(
                ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
            if (!parentFd)
                throw SysError("opening directory %s", PathFmt(parent));
#if HAVE_OPENAT
            return make_ref<DirFdSourceAccessor>(
                std::move(parentFd), std::move(root), std::move(basename), trackLastModified);
#else
            return make_ref<SimpleFSSourceAccessor>(std::move(root), trackLastModified);
#endif
        }
        throw SysError("opening file %s", PathFmt(root));
    }

    struct ::stat st;
    if (::fstat(fd.get(), &st))
        throw SysError("getting status of %s", PathFmt(root));

    if (S_ISDIR(st.st_mode))
        return makeDirectorySourceAccessor(std::move(fd), std::move(root), trackLastModified);

    /* Non-directory file. Open the parent directory and store the
       basename so child-path accesses correctly fail. */
    auto parent = root.parent_path();
    auto basename = root.filename().string();
    fd.close();
    AutoCloseFD parentFd = toDescriptor(
        ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (!parentFd)
        throw SysError("opening directory %s", PathFmt(parent));
#if HAVE_OPENAT
    return make_ref<DirFdSourceAccessor>(
        std::move(parentFd), std::move(root), std::move(basename), trackLastModified);
#else
    return make_ref<SimpleFSSourceAccessor>(std::move(root), trackLastModified);
#endif
}

SourcePath createAtRoot(const std::filesystem::path & path, bool trackLastModified)
{
    std::filesystem::path path2 = absPath(path);
    return {
        makeFSSourceAccessor(path2.root_path(), trackLastModified),
        CanonPath{path2.relative_path().string()},
    };
}

} // namespace nix
