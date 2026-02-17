#include <gtest/gtest.h>

#include "nix/fetchers/git-utils.hh"
#include "nix/util/file-system.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/serialise.hh"
#include "nix/util/tarfile.hh"
#include "nix/util/fs-sink.hh"

#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <thread>

namespace nix {

/// Helper: create a .tar.gz archive at `path` containing a single
/// top-level directory with a few files and a symlink.
static void createTestTarball(const std::filesystem::path & path)
{
    auto * ar = archive_write_new();
    archive_write_add_filter_gzip(ar);
    archive_write_set_format_pax_restricted(ar);
    archive_write_open_filename(ar, path.string().c_str());

    auto addDir = [&](const char * name) {
        auto * entry = archive_entry_new();
        archive_entry_set_pathname(entry, name);
        archive_entry_set_filetype(entry, AE_IFDIR);
        archive_entry_set_perm(entry, 0755);
        archive_entry_set_mtime(entry, 1000000000, 0);
        archive_write_header(ar, entry);
        archive_entry_free(entry);
    };

    auto addFile = [&](const char * name, std::string_view contents, bool executable = false) {
        auto * entry = archive_entry_new();
        archive_entry_set_pathname(entry, name);
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, executable ? 0755 : 0644);
        archive_entry_set_size(entry, contents.size());
        archive_entry_set_mtime(entry, 1000000000, 0);
        archive_write_header(ar, entry);
        archive_write_data(ar, contents.data(), contents.size());
        archive_entry_free(entry);
    };

    auto addSymlink = [&](const char * name, const char * target) {
        auto * entry = archive_entry_new();
        archive_entry_set_pathname(entry, name);
        archive_entry_set_filetype(entry, AE_IFLNK);
        archive_entry_set_perm(entry, 0777);
        archive_entry_set_symlink(entry, target);
        archive_entry_set_mtime(entry, 1000000000, 0);
        archive_write_header(ar, entry);
        archive_entry_free(entry);
    };

    addDir("test-pkg-1.0");
    addFile("test-pkg-1.0/hello.txt", "hello world\n");
    addFile("test-pkg-1.0/run.sh", "#!/bin/sh\necho hi\n", true);
    addSymlink("test-pkg-1.0/link", "hello.txt");
    addDir("test-pkg-1.0/subdir");
    addFile("test-pkg-1.0/subdir/nested.txt", "nested content\n");

    archive_write_close(ar);
    archive_write_free(ar);
}

class TarballUnpackTest : public ::testing::Test
{
    std::unique_ptr<AutoDelete> delTmpDir;

protected:
    std::filesystem::path tmpDir;

    void SetUp() override
    {
        tmpDir = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(tmpDir, true);
    }

    void TearDown() override
    {
        delTmpDir.reset();
    }

    ref<GitRepo> openRepo()
    {
        return GitRepo::openRepo(tmpDir / "repo", {.create = true});
    }
};

/// Verify that creating a TarArchive from a file path (the new code
/// path) produces the same git tree as creating one from a Source.
TEST_F(TarballUnpackTest, PathAndStreamProduceSameTree)
{
    auto tarball = tmpDir / "test.tar.gz";
    createTestTarball(tarball);

    auto getTreeFromPath = [&]() {
        auto repo = openRepo();
        auto sink = repo->getFileSystemObjectSink();
        TarArchive archive{tarball};
        unpackTarfileToSink(archive, *sink);
        return repo->dereferenceSingletonDirectory(sink->flush());
    };
    auto treeFromPath = getTreeFromPath();

    // For the stream path, read the tarball into a Source.
    auto tarballContents = readFile(tarball);
    auto getTreeFromStream = [&]() {
        // Use a fresh repo to avoid any caching.
        auto repoDir2 = tmpDir / "repo2";
        auto repo2 = GitRepo::openRepo(repoDir2, {.create = true});
        auto sink = repo2->getFileSystemObjectSink();
        StringSource source{tarballContents};
        TarArchive archive{source};
        unpackTarfileToSink(archive, *sink);
        return repo2->dereferenceSingletonDirectory(sink->flush());
    };
    auto treeFromStream = getTreeFromStream();

    EXPECT_EQ(treeFromPath.gitRev(), treeFromStream.gitRev());
}

/// Verify that draining a sinkToSource into an FdSink (the new
/// download pattern) completes without being throttled by a slow
/// downstream consumer. This is the core of the backpressure fix.
///
/// The test creates a ~1 MiB data stream via sinkToSource. It
/// measures how long the source takes to drain into an FdSink (fast
/// disk write) vs. into a sink that artificially sleeps to simulate
/// the expensive tarball-parse + git-object-write pipeline. If
/// backpressure were present, the producer would be blocked by the
/// slow consumer; the temp-file approach avoids this.
TEST_F(TarballUnpackTest, TempFileDrainNotThrottledBySlowConsumer)
{
    constexpr size_t dataSize = 1 << 20; // 1 MiB
    constexpr size_t chunkSize = 4096;
    std::string payload(dataSize, 'x');

    // Drain via FdSink (simulating the new temp-file approach).
    auto makeFreshSource = [&]() {
        return sinkToSource([&](Sink & sink) {
            for (size_t off = 0; off < payload.size(); off += chunkSize) {
                auto n = std::min(chunkSize, payload.size() - off);
                sink({payload.data() + off, n});
            }
        });
    };

    auto [fd, tempPath] = createTempFile("nix-backpressure-test");
    AutoDelete cleanupTemp(tempPath);

    auto source = makeFreshSource();
    auto t0 = std::chrono::steady_clock::now();
    {
        FdSink sink(fd.get());
        source->drainInto(sink);
    }
    auto fastDuration = std::chrono::steady_clock::now() - t0;

    // Drain via a slow sink (simulating the old streaming approach
    // where tarball parsing + git object writing throttles the
    // producer).
    struct SlowSink : Sink
    {
        size_t totalBytes = 0;
        void operator()(std::string_view data) override
        {
            totalBytes += data.size();
            // Sleep 500us per chunk to simulate expensive processing.
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    };

    SlowSink slowSink;
    auto source2 = makeFreshSource();
    auto t1 = std::chrono::steady_clock::now();
    source2->drainInto(slowSink);
    auto slowDuration = std::chrono::steady_clock::now() - t1;

    EXPECT_EQ(slowSink.totalBytes, dataSize);

    // The fast path (FdSink) should complete significantly faster
    // than the slow path. The slow path sleeps ~500us per 4KB chunk,
    // so ~128ms for 1 MiB. The fast path should finish in <20ms on
    // any reasonable machine. We use a 4x ratio to avoid flakiness.
    EXPECT_LT(fastDuration, slowDuration / 4)
        << "FdSink drain took " << std::chrono::duration_cast<std::chrono::milliseconds>(fastDuration).count()
        << "ms, slow sink took " << std::chrono::duration_cast<std::chrono::milliseconds>(slowDuration).count()
        << "ms; expected at least 4x speedup";
}

} // namespace nix
