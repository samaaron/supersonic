#!/usr/bin/env bash
# Transport harness (macOS/Linux) — boots the real SuperSonic binary once per
# command transport (UDP, TCP, UDS stream, UDS datagram), sends /status over
# that transport with the transport_probe client, and requires a reply.
# Headless throughout, so it runs on CI runners with no audio device.
#
#   test/transport-harness/run.sh [path-to-SuperSonic-binary]
#
# Exit 0 = every transport answered; nonzero = at least one failed (each
# failure prints the server log). Windows (UDP/TCP/named pipe) is run.ps1.
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$REPO/build/native/SuperSonic_artefacts/Release/SuperSonic}"
WORK="$(mktemp -d /tmp/ss-harness-XXXXXX)"
trap 'kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT

if [ ! -x "$BIN" ]; then
    echo "harness: binary not found: $BIN" >&2
    exit 2
fi

echo "harness: building transport_probe…"
(cd "$REPO/rust" && cargo build -q --example transport_probe -p supersonic-osc-net) || exit 2
PROBE="$REPO/rust/target/debug/examples/transport_probe"

SERVER_PID=""
FAILURES=0

# boot <log> <server-args…> — start the server headless, remember its pid.
boot() {
    local log="$1"; shift
    "$BIN" --headless -u 0 "$@" > "$log" 2>&1 &
    SERVER_PID=$!
}

# probe_retry <proto> <target> — the server binds its transport shortly after
# boot; retry the probe until it answers or ~10s elapse.
probe_retry() {
    for _ in $(seq 1 20); do
        if "$PROBE" "$1" "$2" 2>/dev/null; then
            return 0
        fi
        kill -0 "$SERVER_PID" 2>/dev/null || return 1  # server died
        sleep 0.5
    done
    return 1
}

check() {
    local name="$1" proto="$2" target="$3" log="$4"; shift 4
    boot "$log" "$@"
    if probe_retry "$proto" "$target"; then
        echo "PASS $name"
    else
        echo "FAIL $name — server log:"
        sed 's/^/  | /' "$log"
        FAILURES=$((FAILURES + 1))
    fi
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
}

# Distinct ports/paths per transport so a leaked server can't cross-answer.
UDP_PORT=$((20000 + RANDOM % 20000))
TCP_PORT=$((20000 + RANDOM % 20000))

check "udp"       udp       "127.0.0.1:$UDP_PORT" "$WORK/udp.log"  -u "$UDP_PORT"
check "tcp"       tcp       "127.0.0.1:$TCP_PORT" "$WORK/tcp.log"  --tcp "$TCP_PORT" -B 127.0.0.1
check "uds"       uds       "$WORK/uds.sock"      "$WORK/uds.log"  --uds "$WORK/uds.sock"
check "uds-dgram" uds-dgram "$WORK/dg.sock"       "$WORK/dg.log"   --uds-dgram "$WORK/dg.sock"

if [ "$FAILURES" -gt 0 ]; then
    echo "harness: $FAILURES transport(s) failed"
    exit 1
fi
echo "harness: all transports answered"
