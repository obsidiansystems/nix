source common.sh

ourSocket="$TEST_ROOT/our-socket"
storeArgs=(--store "unix://$ourSocket")
log=ssh-handshake-log.bin

if test -n "${_NIX_TEST_ACCEPT-}"; then
    startDaemon
    ./snoop-socket/snoop-socket "$NIX_DAEMON_SOCKET_PATH" "$ourSocket" > "$log" &
    pid=$!
    nix store info "${storeArgs[@]}"
    wait "$pid"
    rm "ourSocket" || true

    skipTest "regenerating golden masters"
else
    ./mock-daemon/mock-daemon "$ourSocket" &
    pid=$!
    expectStderr 1 nix store info "${storeArgs[@]}" | grepQuiet "Nix daemon disconnected unexpectedly"
    wait "$pid"
    rm "ourSocket" || true

    ./mock-daemon/mock-daemon "$ourSocket" "$log" 100 &
    pid=$!
    nix store info "${storeArgs[@]}"
    wait "$pid"
    rm "ourSocket" || true
fi
