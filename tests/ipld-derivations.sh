#!/usr/bin/env bash

source common.sh


drv=$(nix-instantiate --experimental-features ca-derivations ./ipfs-derivation-output.nix -A dependent)
nix --experimental-features 'nix-command ca-derivations' show-derivation --derivation "$drv"
echo $drv
cid=$(nix ipld-drv export --derivation $drv)
echo cid: $cid

drv2=$(nix ipld-drv import $cid)

[ $drv = $drv2 ]
