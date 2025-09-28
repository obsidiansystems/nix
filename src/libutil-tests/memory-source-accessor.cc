#include <string_view>

#include "nix/util/bytes.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/tests/json-characterization.hh"

namespace nix {

namespace memory_source_accessor {

using namespace std::literals::string_view_literals;

using File = MemorySourceAccessor::File;

ref<MemorySourceAccessor> exampleSimple()
{
    auto sc = make_ref<MemorySourceAccessor>();
    sc->root = File{File::Regular{
        .executable = false,
        .contents = to_owned(as_bytes("asdf")),
    }};
    return sc;
}

ref<MemorySourceAccessor> exampleComplex()
{
    auto files = make_ref<MemorySourceAccessor>();
    files->root = File::Directory{
        .entries{
            {
                to_owned(as_bytes("foo"sv)),
                File::Regular{
                    .contents = to_owned(as_bytes("hello\n\0\n\tworld!"sv)),
                },
            },
            {
                to_owned(as_bytes("bar"sv)),
                File::Directory{
                    .entries =
                        {
                            {
                                to_owned(as_bytes("baz"sv)),
                                File::Regular{
                                    .executable = true,
                                    .contents = to_owned(as_bytes("good day,\n\0\n\tworld!"sv)),
                                },
                            },
                            {
                                to_owned(as_bytes("quux"sv)),
                                File::Symlink{
                                    .target = to_owned(as_bytes("/over/there"sv)),
                                },
                            },
                        },
                },
            },
        },
    };
    return files;
}

/**
 * Invalid UTF-8 test sequence:
 *
 * - `"\x41\x42"`: ASCII characters 'A' and 'B' (valid UTF-8).
 *
 * - `"\xF0\x90\x80"`: Incomplete 4-byte UTF-8 sequence (invalid - missing 4th byte).
 *
 * - `"\x43\x44"`: ASCII characters 'C' and 'D' (valid UTF-8).
 *
 * - `"\xEF\xBF\xBD"`: UTF-8 encoding of Unicode Replacement Character U+FFFD (valid UTF-8).
 *
 * - `"\x45\x46"`: ASCII characters 'E' and 'F' (valid UTF-8).
 */
static const BytesView invalidUtf8 = as_bytes("\x41\x42\xF0\x90\x80\x43\x44\xEF\xBF\xBD\x45\x46"sv);

/**
 * Regular file with invalid UTF-8 in contents.
 */
static ref<MemorySourceAccessor> exampleInvalidUnicodeRegular()
{
    auto sc = make_ref<MemorySourceAccessor>();
    sc->root = File{File::Regular{
        .executable = false,
        .contents = to_owned(invalidUtf8),
    }};
    return sc;
}

/**
 * Directory with invalid UTF-8 in entry names.
 */
static ref<MemorySourceAccessor> exampleInvalidUnicodeDirectory()
{
    auto sc = make_ref<MemorySourceAccessor>();
    sc->root = File::Directory{
        .entries{
            {
                to_owned(as_bytes("validname"sv)),
                File::Regular{
                    .contents = to_owned(as_bytes("valid content"sv)),
                },
            },
            {
                to_owned(invalidUtf8),
                File::Regular{
                    .contents = to_owned(as_bytes("file with invalid unicode name"sv)),
                },
            },
        },
    };
    return sc;
}

/**
 * Symlink with invalid UTF-8 in target path.
 */
static ref<MemorySourceAccessor> exampleInvalidUnicodeSymlink()
{
    auto sc = make_ref<MemorySourceAccessor>();
    sc->root = File{File::Symlink{
        .target = to_owned(invalidUtf8),
    }};
    return sc;
}

} // namespace memory_source_accessor

/* ----------------------------------------------------------------------------
 * JSON
 * --------------------------------------------------------------------------*/

class MemorySourceAccessorTest : public virtual CharacterizationTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "memory-source-accessor";

public:

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / testStem;
    }
};

using nlohmann::json;

struct MemorySourceAccessorJsonTest : MemorySourceAccessorTest,
                                      JsonCharacterizationTest<MemorySourceAccessor>,
                                      ::testing::WithParamInterface<std::pair<std::string_view, MemorySourceAccessor>>
{};

TEST_P(MemorySourceAccessorJsonTest, from_json)
{
    auto & [name, expected] = GetParam();
    /* Cannot use `readJsonTest` because need to compare `root` field of
       the source accessors for equality. */
    readTest(Path{name} + ".json", [&](const auto & encodedRaw) {
        auto encoded = json::parse(encodedRaw);
        auto decoded = static_cast<MemorySourceAccessor>(encoded);
        ASSERT_EQ(decoded.root, expected.root);
    });
}

TEST_P(MemorySourceAccessorJsonTest, to_json)
{
    auto & [name, value] = GetParam();
    writeJsonTest(name, value);
}

INSTANTIATE_TEST_SUITE_P(
    MemorySourceAccessorJSON,
    MemorySourceAccessorJsonTest,
    ::testing::Values(
        std::pair{
            "simple",
            *memory_source_accessor::exampleSimple(),
        },
        std::pair{
            "complex",
            *memory_source_accessor::exampleComplex(),
        },
        std::pair{
            "invalid-unicode-regular",
            *memory_source_accessor::exampleInvalidUnicodeRegular(),
        },
        std::pair{
            "invalid-unicode-directory",
            *memory_source_accessor::exampleInvalidUnicodeDirectory(),
        },
        std::pair{
            "invalid-unicode-symlink",
            *memory_source_accessor::exampleInvalidUnicodeSymlink(),
        }));

} // namespace nix
