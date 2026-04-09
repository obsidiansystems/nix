#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "nix/util/file-system.hh"
#include "nix/util/go.hh"
#include "nix/util/memory-source-accessor.hh"

#include "nix/util/tests/characterization.hh"

namespace nix {

class GoTest : public CharacterizationTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "go";

public:

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / std::string(testStem);
    }

    /**
     * Local override so we can run with `go-hashing` enabled without
     * mutating the process-wide settings.
     */
    ExperimentalFeatureSettings mockXpSettings;

private:

    void SetUp() override
    {
        mockXpSettings.set("experimental-features", "go-hashing");
    }
};

/**
 * Read the golden-master MemorySourceAccessor JSON, run `go::dumpHash`
 * over it, and compare the resulting digest's base16 form against the
 * golden-master `.txt`.
 */
struct GoCharacterizationTest : GoTest, ::testing::WithParamInterface<std::string_view>
{};

TEST_P(GoCharacterizationTest, dumpHash)
{
    auto stem = GetParam();

    // Load the file system fixture. The JSON is an *input* — not a
    // golden master — so we read it directly via `readFile` rather than
    // `readTest`, which would skip the body under `_NIX_TEST_ACCEPT=1`.
    // `MemorySourceAccessor` is not copy-assignable (its base has a
    // `const` member), so we copy `.root` out of the temporary that
    // `from_json` produces.
    auto accessor = make_ref<MemorySourceAccessor>();
    {
        auto encoded = nlohmann::json::parse(readFile(goldenMaster(std::string{stem} + ".json")));
        auto decoded = static_cast<MemorySourceAccessor>(encoded);
        accessor->root = std::move(decoded.root);
    }

    // Compute and compare the digest. The `.txt` file *is* the golden
    // master here, so `writeTest` is correct.
    writeTest(std::string{stem} + ".txt", [&]() {
        auto h = go::dumpHash(HashAlgorithm::SHA256, SourcePath{accessor}, defaultPathFilter, mockXpSettings);
        return h.to_string(HashFormat::Base16, false) + "\n";
    });
}

INSTANTIATE_TEST_SUITE_P(
    Go,
    GoCharacterizationTest,
    ::testing::Values(
        // An empty directory: hashes the empty manifest.
        std::string_view{"empty-dir"},
        // A directory containing a single regular file at the root.
        std::string_view{"single-file"},
        // A directory with nested subdirectories, exercising the
        // recursive walk and the sorted manifest format.
        std::string_view{"nested"},
        // Siblings whose names straddle `.` (0x2e) and `/` (0x2f), so
        // the byte-wise sort order baked into the manifest is
        // observable. CanonPath's `/`-as-zero ordering would produce a
        // different digest here.
        std::string_view{"sort-order"}));

/* ----------------------------------------------------------------------------
 * Error cases
 * --------------------------------------------------------------------------*/

class GoErrorTest : public ::testing::Test
{
protected:
    ExperimentalFeatureSettings mockXpSettings;

    void SetUp() override
    {
        mockXpSettings.set("experimental-features", "go-hashing");
    }
};

TEST_F(GoErrorTest, rejectsExperimentalFeatureDisabled)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->root = MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}};

    ExperimentalFeatureSettings emptyXpSettings;
    EXPECT_THROW(
        go::dumpHash(HashAlgorithm::SHA256, SourcePath{accessor}, defaultPathFilter, emptyXpSettings),
        MissingExperimentalFeature);
}

TEST_F(GoErrorTest, rejectsNonSha256)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->root = MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}};

    EXPECT_THROW(go::dumpHash(HashAlgorithm::SHA1, SourcePath{accessor}, defaultPathFilter, mockXpSettings), Error);
}

TEST_F(GoErrorTest, rejectsRegularFileAsRoot)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->root = MemorySourceAccessor::File{MemorySourceAccessor::File::Regular{
        .contents = "hi",
    }};

    EXPECT_THROW(go::dumpHash(HashAlgorithm::SHA256, SourcePath{accessor}, defaultPathFilter, mockXpSettings), Error);
}

TEST_F(GoErrorTest, rejectsExecutableFile)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->root = MemorySourceAccessor::File::Directory{
        .entries{
            {
                "foo",
                MemorySourceAccessor::File::Regular{
                    .executable = true,
                    .contents = "hi",
                },
            },
        },
    };

    EXPECT_THROW(go::dumpHash(HashAlgorithm::SHA256, SourcePath{accessor}, defaultPathFilter, mockXpSettings), Error);
}

TEST_F(GoErrorTest, rejectsSymlink)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->root = MemorySourceAccessor::File::Directory{
        .entries{
            {
                "link",
                MemorySourceAccessor::File::Symlink{
                    .target = "/somewhere",
                },
            },
        },
    };

    EXPECT_THROW(go::dumpHash(HashAlgorithm::SHA256, SourcePath{accessor}, defaultPathFilter, mockXpSettings), Error);
}

TEST_F(GoErrorTest, rejectsNewlineInName)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->root = MemorySourceAccessor::File::Directory{
        .entries{
            {
                "naughty\nname",
                MemorySourceAccessor::File::Regular{
                    .contents = "hi",
                },
            },
        },
    };

    EXPECT_THROW(go::dumpHash(HashAlgorithm::SHA256, SourcePath{accessor}, defaultPathFilter, mockXpSettings), Error);
}

} // namespace nix
