#include <gtest/gtest.h>

#include "legacy-ssh-store.hh"

namespace nix {

TEST(LegacySSHStore, constructConfig)
{
    LegacySSHStoreConfig config{
        "ssh",
        "localhost",
        StoreReference::Params{
            {
                "remote-program",
                // TODO #11106, no more split on space
                "foo bar",
            },
        }};
    EXPECT_EQ(
        config.remoteProgram.get(),
        (Strings{
            "foo",
            "bar",
        }));
}
}
