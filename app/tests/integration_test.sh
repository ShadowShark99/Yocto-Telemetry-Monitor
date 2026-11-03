#!/bin/sh
# SPDX-License-Identifier: MIT
#
# End-to-end smoke test: start telemetryd, feed it from telemetry-sim over
# localhost UDP, and check the numbers in the status file add up.
#
# Runs on the build host (ctest) and on the target (ptest), so it sticks to
# POSIX sh and tools BusyBox provides.
#
# Usage: integration_test.sh TELEMETRYD TELEMETRY_SIM [PORT]
set -eu

DAEMON=$1
SIM=$2
PORT=${3:-45500}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/telemetry-it.XXXXXX")
STATUS="$WORK/status.json"
PID=""

cleanup() {
    if [ -n "$PID" ]; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*" >&2
    echo "--- daemon log ---" >&2
    cat "$WORK/daemon.log" >&2 || true
    echo "--- status file ---" >&2
    cat "$STATUS" >&2 || true
    exit 1
}

# Reads an integer field from the (pretty-printed) status file.
field() {
    sed -n "s/^ *\"$1\": \([0-9][0-9]*\).*/\1/p" "$STATUS" | head -n 1
}

wait_for() {
    # wait_for <tries> <command...>: retry every 0.2 s
    tries=$1
    shift
    while [ "$tries" -gt 0 ]; do
        if "$@"; then
            return 0
        fi
        tries=$((tries - 1))
        sleep 0.2
    done
    return 1
}

"$DAEMON" --udp-port "$PORT" --bind 127.0.0.1 --status-file "$STATUS" --log-level debug \
    >"$WORK/daemon.log" 2>&1 &
PID=$!

# The first status file appears after the first 1 s tick, so the socket is bound.
wait_for 50 test -s "$STATUS" || fail "daemon did not start"

SUMMARY=$("$SIM" --host 127.0.0.1 --port "$PORT" --sensors 2 --rate 50 --count 25 \
    --fault-rate 0.2 --drop-rate 0.1 --seed 7 --quiet)
echo "simulator: $SUMMARY"

SENT=$(echo "$SUMMARY" | sed -n 's/.*sent=\([0-9]*\).*/\1/p')
CORRUPTED=$(echo "$SUMMARY" | sed -n 's/.*corrupted=\([0-9]*\).*/\1/p')
DROPPED=$(echo "$SUMMARY" | sed -n 's/.*dropped=\([0-9]*\).*/\1/p')
EXPECTED_OK=$((SENT - CORRUPTED))

[ "$SENT" -gt 0 ] || fail "simulator sent nothing"
[ "$CORRUPTED" -gt 0 ] || fail "fault injection produced no corrupted packets"

packets_match() {
    [ "$(field packets_total)" = "$EXPECTED_OK" ] && [ "$(field parse_errors_total)" = "$CORRUPTED" ]
}
wait_for 25 packets_match ||
    fail "expected packets_total=$EXPECTED_OK parse_errors_total=$CORRUPTED, got $(field packets_total)/$(field parse_errors_total)"

grep -q '"id": "env-01"' "$STATUS" || fail "env-01 missing from status"
grep -q '"id": "env-02"' "$STATUS" || fail "env-02 missing from status"
grep -q '"checksum_mismatch":' "$STATUS" || fail "checksum errors not broken down"

# Dropped *and* corrupted packets both leave gaps in a node's sequence
# numbers: to the monitor, a packet that failed its checksum never arrived.
# Losses at the very start or end of a node's stream cannot be seen, hence <=.
LOST=$((DROPPED + CORRUPTED))
MISSED=$(sed -n 's/^ *"missed": \([0-9][0-9]*\).*/\1/p' "$STATUS" | awk '{ s += $1 } END { print s + 0 }')
[ "$MISSED" -le "$LOST" ] || fail "missed=$MISSED is more than dropped+corrupted=$LOST"
if [ "$LOST" -gt 4 ] && [ "$MISSED" -eq 0 ]; then
    fail "no packet loss detected although $LOST packets never arrived intact"
fi

# A clean shutdown on SIGTERM returns 0.
kill -TERM "$PID"
STATUS_CODE=0
wait "$PID" || STATUS_CODE=$?
PID=""
[ "$STATUS_CODE" -eq 0 ] || fail "daemon exited with $STATUS_CODE on SIGTERM"
grep -q "shutting down" "$WORK/daemon.log" || fail "no shutdown message"

echo "PASS: integration (ok=$EXPECTED_OK corrupted=$CORRUPTED dropped=$DROPPED missed=$MISSED)"
