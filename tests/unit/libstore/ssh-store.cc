#include <gtest/gtest.h>

#include "ssh-store.hh"

namespace nix {

TEST(SSHStore, constructConfig)
{
    SSHStoreConfig config{
        "ssh",
        "localhost",
        StoreReference::Params{
            {
                "remote-program",
                {
                    "foo",
                    "bar",
                },
            },
        },
    };

    EXPECT_EQ(
        config.remoteProgram.get(),
        (Strings{
            "foo",
            "bar",
        }));
}

TEST(MountedSSHStore, constructConfig)
{
    MountedSSHStoreConfig config{
        "mounted-ssh",
        "localhost",
        StoreReference::Params{
            {
                "remote-program",
                {
                    "foo",
                    "bar",
                },
            },
        },
    };

    EXPECT_EQ(
        config.remoteProgram.get(),
        (Strings{
            "foo",
            "bar",
        }));
}

}
