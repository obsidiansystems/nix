#!/usr/bin/env bash

source common.sh

clearStore

export NIX_VARLINK_REMOTE="$TEST_ROOT/varlink"
nix store varlink-daemon -l "$NIX_VARLINK_REMOTE" &
child=$!

while [[ ! -e "$NIX_VARLINK_REMOTE" ]]; do sleep 0.1; done

rootDir="$TEST_ROOT/varlink-trivial-external"
mkdir -p "$rootDir/foo"
echo "external" > "$rootDir/foo/bar"

out="$("$_NIX_TEST_BUILD_DIR"/varlink/test-varlink/varlink-trivial-external "$rootDir")"

test "$(cat "$NIX_STORE_DIR"/"$out"/foo/bar)" == "external"

kill -- $child
