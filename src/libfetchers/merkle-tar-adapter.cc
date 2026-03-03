#include "nix/fetchers/merkle-tar-adapter.hh"
#include "nix/util/sync.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/util.hh"

#include <future>
#include <map>
#include <optional>
#include <variant>
#include <vector>

namespace nix {

/**
 * A pending file system object - either finalized (hash), pending directory (map of children),
 * or pending file (placeholder until flushed).
 */
struct PendingFSO;
using PendingDir = std::map<std::string, PendingFSO>;

/**
 * Placeholder for a file that hasn't been flushed yet.
 * Holds the sink so it can be flushed later.
 */
struct PendingFilePlaceholder
{
    ref<merkle::RegularFileSinkWithFlush> sink;
};

struct PendingFSO
{
    merkle::Mode mode;
    std::variant<Hash, PendingDir, PendingFilePlaceholder> data;
};

/**
 * Adapter that implements TarSink (path-based) by building up a tree
 * structure and then flushing to merkle::DirectorySink/merkle::RegularFileSink.
 *
 * Flushing is done asynchronously: file flushes are started in parallel,
 * and directory flushes are triggered automatically when all their children
 * are complete, cascading up to the root.
 */
struct TarAdapterImpl : merkle::TarAdapter
{
    merkle::FileSinkBuilder & store;

    /**
     * Cache entry for sink flush results.
     * Stores the ref to keep the sink alive (preventing pointer reuse issues).
     */
    struct SinkCacheEntry
    {
        ref<merkle::RegularFileSinkWithFlush> sink;
        std::shared_future<Hash> future;
    };

    /**
     * Synchronized state for the pending tree.
     */
    struct State
    {
        /**
         * Root entry - could be a directory, file, or symlink.
         * Empty until first entry is created.
         */
        std::optional<PendingFSO> root;

        /**
         * Cache of sink flush results (for hardlinks sharing the same sink).
         * Uses shared_future so multiple threads can wait on the same flush.
         * Keyed by raw pointer, but the ref in the value keeps it alive.
         */
        std::map<merkle::RegularFileSinkWithFlush *, SinkCacheEntry> sinkFlushResults;
    };

    Sync<State> state_;

    /**
     * Thread pool for parallel flushing.
     */
    ThreadPool workers;

    TarAdapterImpl(merkle::FileSinkBuilder & store)
        : store(store)
    {
    }

    /**
     * Ensure root is a directory and return it.
     * Implements "last wins" semantics: if root is not a directory
     * (e.g., a Hash from a previously flushed file), replace it.
     */
    PendingDir & ensureRootDir(State & st)
    {
        if (st.root) {
            if (auto * dir = std::get_if<PendingDir>(&st.root->data))
                return *dir;
        }
        // Create or replace with new directory (last wins semantics)
        st.root = PendingFSO{merkle::Mode::Directory, PendingDir{}};
        return std::get<PendingDir>(st.root->data);
    }

    /**
     * Get or create the pending directory at the given path.
     */
    PendingDir & getDir(State & st, const CanonPath & path)
    {
        PendingDir * current = &ensureRootDir(st);

        for (auto & component : path) {
            auto it = current->find(std::string(component));
            if (it == current->end()) {
                // Create new pending directory
                auto [inserted, _] =
                    current->emplace(std::string(component), PendingFSO{merkle::Mode::Directory, PendingDir{}});
                current = &std::get<PendingDir>(inserted->second.data);
            } else {
                // If it's a directory, use it; otherwise replace with a new
                // directory (last wins semantics for file-to-directory conversion)
                if (auto * dir = std::get_if<PendingDir>(&it->second.data)) {
                    current = dir;
                } else {
                    it->second = PendingFSO{merkle::Mode::Directory, PendingDir{}};
                    current = &std::get<PendingDir>(it->second.data);
                }
            }
        }

        return *current;
    }

    /**
     * Add an entry at the given path.
     */
    void addEntry(State & st, const CanonPath & path, PendingFSO entry)
    {
        if (path.isRoot()) {
            st.root = std::move(entry);
            return;
        }

        auto parent = path.parent();
        auto name = std::string(path.baseName().value());

        PendingDir & dir = parent ? getDir(st, *parent) : ensureRootDir(st);
        dir.insert_or_assign(name, std::move(entry));
    }

    /**
     * Navigate to the entry at path and return a pointer to it.
     * Returns nullptr if not found.
     */
    PendingFSO * navigateTo(State & st, const CanonPath & path)
    {
        if (!st.root)
            return nullptr;

        if (path.isRoot())
            return &*st.root;

        auto * dir = std::get_if<PendingDir>(&st.root->data);
        if (!dir)
            return nullptr;

        PendingDir * current = dir;

        auto it = path.begin();
        auto end = path.end();

        while (it != end) {
            auto component = *it++;
            auto found = current->find(std::string(component));
            if (found == current->end())
                return nullptr;

            if (it == end) {
                return &found->second;
            }

            if (auto * subdir = std::get_if<PendingDir>(&found->second.data))
                current = subdir;
            else
                return nullptr;
        }

        return nullptr;
    }

    /**
     * Look up a finalized entry at the given path (for hardlinks).
     */
    const PendingFSO * lookupEntry(State & st, const CanonPath & path)
    {
        return navigateTo(st, path);
    }

    void createDirectory(const CanonPath & path) override
    {
        auto st = state_.lock();
        if (path.isRoot()) {
            ensureRootDir(*st);
            return;
        }
        getDir(*st, path);
    }

    void createRegularFile(const CanonPath & path, bool isExecutable, fun<void(Sink &)> callback) override
    {
        auto sink = store.makeRegularFileSink();
        callback(*sink);

        auto mode = isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular;

        {
            auto st = state_.lock();
            if (path.isRoot()) {
                st->root = PendingFSO{mode, PendingFilePlaceholder{sink}};
            } else {
                auto parent = path.parent();
                if (parent)
                    getDir(*st, *parent);
                else
                    ensureRootDir(*st);
                addEntry(*st, path, PendingFSO{mode, PendingFilePlaceholder{sink}});
            }
        }

        // Enqueue flush job
        workers.enqueue([this, path, sink]() {
            flushFile(path, sink);
        });
    }

    /**
     * Flush a file sink and update the tree.
     * Handles hardlinks by ensuring each sink is only flushed once.
     */
    void flushFile(const CanonPath & path, ref<merkle::RegularFileSinkWithFlush> sink)
    {
        std::shared_future<Hash> future;
        bool shouldFlush = false;
        std::promise<Hash> promise;

        {
            auto st = state_.lock();
            auto it = st->sinkFlushResults.find(&*sink);
            if (it != st->sinkFlushResults.end()) {
                // Another thread is handling this sink
                future = it->second.future;
            } else {
                // We're responsible for flushing this sink
                future = promise.get_future().share();
                st->sinkFlushResults.emplace(&*sink, SinkCacheEntry{sink, future});
                shouldFlush = true;
            }
        }

        if (shouldFlush) {
            Hash hash = std::move(*sink).flush();
            promise.set_value(hash);
        }

        completeFileEntry(path, sink, future.get());
    }

    void createSymlink(const CanonPath & path, const std::string & target) override
    {
        auto entry = store.makeSymlink(target);
        auto st = state_.lock();
        addEntry(*st, path, PendingFSO{entry.mode, entry.hash});
    }

    void createHardlink(const CanonPath & path, const CanonPath & target) override
    {
        auto st = state_.lock();
        auto * entry = lookupEntry(*st, target);

        if (!entry)
            throw Error("hardlink target '%s' not found", target);

        if (auto * hash = std::get_if<Hash>(&entry->data)) {
            // Target already flushed, just copy the hash
            addEntry(*st, path, PendingFSO{entry->mode, *hash});
        } else if (auto * placeholder = std::get_if<PendingFilePlaceholder>(&entry->data)) {
            // Share the sink with the target
            auto sink = placeholder->sink;
            auto mode = entry->mode;
            addEntry(*st, path, PendingFSO{mode, PendingFilePlaceholder{sink}});

            // Enqueue flush job (will share result with original via sinkFlushResults)
            workers.enqueue([this, path, sink]() {
                flushFile(path, sink);
            });
        } else {
            throw Error("hardlink target '%s' is a directory", target);
        }
    }

    /**
     * Check if a directory is complete (all children are Hashes or complete subdirectories).
     */
    bool isDirComplete(PendingDir & dir)
    {
        for (auto & [name, fso] : dir) {
            if (std::holds_alternative<Hash>(fso.data))
                continue;
            if (auto * subdir = std::get_if<PendingDir>(&fso.data)) {
                if (isDirComplete(*subdir))
                    continue;
            }
            return false;
        }
        return true;
    }

    /**
     * Tree structure for collecting directory info for flushing.
     * Stores either a leaf (mode + hash) or a subtree to flush.
     */
    struct DirTreeEntry;
    using DirTreeChildren = std::map<std::string, DirTreeEntry>;

    struct DirTreeEntry
    {
        merkle::Mode mode;
        std::variant<Hash, DirTreeChildren> data;
    };

    /**
     * Collect a complete directory's info into a tree for flushing outside lock.
     */
    DirTreeChildren collectDirChildren(PendingDir & dir)
    {
        DirTreeChildren result;
        for (auto & [name, child] : dir) {
            if (auto * hash = std::get_if<Hash>(&child.data)) {
                result.emplace(name, DirTreeEntry{child.mode, *hash});
            } else {
                auto & subdir = std::get<PendingDir>(child.data);
                result.emplace(name, DirTreeEntry{child.mode, collectDirChildren(subdir)});
            }
        }
        return result;
    }

    /**
     * Recursively flush directory children and return their entries.
     * Called outside lock.
     */
    std::vector<std::tuple<std::string, merkle::Mode, Hash>> flushDirChildren(DirTreeChildren & children)
    {
        std::vector<std::tuple<std::string, merkle::Mode, Hash>> result;
        for (auto & [name, entry] : children) {
            Hash hash = std::visit(
                overloaded{
                    [](Hash & h) { return h; },
                    [this](DirTreeChildren & subchildren) {
                        auto flushedChildren = flushDirChildren(subchildren);
                        return flushCompleteDir(std::move(flushedChildren));
                    },
                },
                entry.data);
            result.emplace_back(name, entry.mode, hash);
        }
        return result;
    }

    /**
     * Flush a directory that has all Hash children.
     * Called without lock held.
     */
    Hash flushCompleteDir(std::vector<std::tuple<std::string, merkle::Mode, Hash>> children)
    {
        auto dirSink = store.makeDirectorySink();
        for (auto & [name, mode, hash] : children) {
            dirSink->insertChild(name, merkle::TreeEntry{mode, hash});
        }
        return std::move(*dirSink).flush();
    }

    /**
     * Called when a file flush job completes.
     * Verifies the sink still matches (entry wasn't replaced) before updating.
     */
    void completeFileEntry(const CanonPath & path, ref<merkle::RegularFileSinkWithFlush> sink, Hash hash)
    {
        auto st = state_.lock();

        if (path.isRoot()) {
            // Verify sink matches (entry wasn't replaced by a later file)
            if (!st->root)
                return;
            auto * placeholder = std::get_if<PendingFilePlaceholder>(&st->root->data);
            if (!placeholder || &*placeholder->sink != &*sink)
                return;
            st->root->data = hash;
            return;
        }

        auto * entry = navigateTo(*st, path);
        if (!entry)
            return;
        // Verify sink matches (entry wasn't replaced by a later file)
        auto * placeholder = std::get_if<PendingFilePlaceholder>(&entry->data);
        if (!placeholder || &*placeholder->sink != &*sink)
            return;
        entry->data = hash;

        // Check if parent directory is now complete
        auto parent = path.parent();
        checkAndFlushParent(*st, parent ? *parent : CanonPath::root);
    }

    /**
     * Called when a directory at `path` has been completed with `hash`.
     * Updates the tree and propagates completion up to parents.
     *
     * Note: The entry might have been replaced by a later tar entry while this
     * job was running (last wins semantics). In that case, we silently skip
     * the update since our result is no longer relevant.
     */
    void completeDirEntry(const CanonPath & path, Hash hash)
    {
        auto st = state_.lock();

        if (path.isRoot()) {
            // Verify entry is still a PendingDir (wasn't replaced)
            if (!st->root)
                return;
            if (!std::holds_alternative<PendingDir>(st->root->data))
                return;
            st->root->data = hash;
            return;
        }

        auto * entry = navigateTo(*st, path);
        if (!entry)
            return;
        // Verify entry is still a PendingDir (wasn't replaced)
        if (!std::holds_alternative<PendingDir>(entry->data))
            return;
        entry->data = hash;

        // Check if parent directory is now complete
        auto parent = path.parent();
        checkAndFlushParent(*st, parent ? *parent : CanonPath::root);
    }

    /**
     * Check if the directory at `dirPath` is complete, and if so, enqueue its flush.
     * Must be called with lock held.
     */
    void checkAndFlushParent(State & st, const CanonPath & dirPath)
    {
        PendingFSO * dirEntry = navigateTo(st, dirPath);
        if (!dirEntry)
            return;

        auto * dir = std::get_if<PendingDir>(&dirEntry->data);
        if (!dir)
            return;

        if (!isDirComplete(*dir))
            return;

        // Collect children info for flushing (may include subdirs to flush recursively)
        DirTreeChildren children = collectDirChildren(*dir);
        CanonPath dirPathCopy = dirPath;

        // Enqueue directory flush
        workers.enqueue([this, children = std::move(children), dirPath = std::move(dirPathCopy)]() mutable {
            auto flushedChildren = flushDirChildren(children);
            Hash dirHash = flushCompleteDir(std::move(flushedChildren));
            completeDirEntry(dirPath, dirHash);
        });
    }

    merkle::TreeEntry flush() override
    {
        // Check for special cases that can be handled without the thread pool
        std::optional<std::pair<merkle::Mode, DirTreeChildren>> completeTree;
        {
            auto st = state_.lock();
            if (!st->root)
                throw Error("tar archive is empty");

            // Handle case of already-complete root (symlink only, no files)
            if (auto * hash = std::get_if<Hash>(&st->root->data)) {
                return merkle::TreeEntry{st->root->mode, *hash};
            }

            // Handle directory tree with no pending files (only dirs)
            if (auto * dir = std::get_if<PendingDir>(&st->root->data)) {
                if (isDirComplete(*dir)) {
                    completeTree = {st->root->mode, collectDirChildren(*dir)};
                }
            }
        }

        // Flush complete tree outside lock
        if (completeTree) {
            auto & [mode, children] = *completeTree;
            auto flushedChildren = flushDirChildren(children);
            Hash dirHash = flushCompleteDir(std::move(flushedChildren));
            return merkle::TreeEntry{mode, dirHash};
        }

        // Process all work items (files and cascading directories)
        workers.process();

        auto st = state_.lock();
        if (!std::holds_alternative<Hash>(st->root->data))
            throw Error("flush completed but root is not a hash");
        return merkle::TreeEntry{st->root->mode, std::get<Hash>(st->root->data)};
    }
};

} // namespace nix

namespace nix::merkle {

ref<TarAdapter> makeTarSink(FileSinkBuilder & store)
{
    return make_ref<nix::TarAdapterImpl>(store);
}

} // namespace nix::merkle
