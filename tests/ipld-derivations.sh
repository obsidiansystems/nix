#!/usr/bin/env bash

source common.sh

if [[ -z $(type -p ipfs) ]]; then
    echo "Ipfs not installed; skipping ipfs tests"
    exit 99
fi

startIpfs

simpleFileIpfs

drv=$(nix-instantiate --experimental-features ca-derivations ./ipfs-derivation-output.nix -A dependent)
nix --experimental-features 'nix-command ca-derivations' show-derivation --derivation "$drv"
echo $drv
cid=$(nix --experimental-features 'nix-command ca-derivations' ipld-drv export --derivation $drv)
echo cid: $cid

drv2=$(nix --experimental-features 'nix-command ca-derivations' ipld-drv import $cid)

[ $drv = $drv2 ]
