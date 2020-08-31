#!/usr/bin/env bash

source common.sh


drv=$(nix-instantiate --experimental-features ca-derivations ./ipfs-derivation-output.nix -A root)
nix --experimental-features 'nix-command ca-derivations' show-derivation --derivation "$drv"
echo $drv
nix ipld-drv export --derivation $drv

# nix-store \
#     --export $(nix-build --experimental-features ca-derivations ./ipfs-derivation-output.nix -A root --no-out-link)

[ 1 = 1 ]
