#!/usr/bin/env bash
#
# Can a rude peer take the machine down?
#
#   tests/tls/run-hangup.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#
# WHAT THIS IS FOR
#
#   A 14 MHz 68020 spends tens of seconds on the asymmetric arithmetic in a
#   TLS handshake, and modern servers do not wait that long: they close, and
#   they close in the middle of OUR handshake, at whatever point our silence
#   ran out their patience.  Waiting for a real server to do that is slow,
#   depends on somebody else's timeout policy, and cannot be aimed at a
#   particular moment.  tests/tls/hangup-server.py does it on demand instead,
#   four ways, from the host side.
#
#   The bar is not "the handshake succeeds", none of these can succeed.  It
#   is that each one produces an ERROR THE CALLER CAN ACT ON and the next
#   command still runs.  A peer must not be able to take the machine down by
#   hanging up, and "the machine" includes the four other things a real Amiga
#   is doing at the time.
#
#   Expected, one line per behaviour, all with return code 10:
#
#     rst      the connection is closed
#     fin      the connection is closed
#     hang     the server stopped responding    (TIMEOUT 15, so ~15 s)
#     garbage  the connection is closed
#
#   `garbage` reports the close rather than a record-parser complaint because
#   the peer hangs up in the same breath as the nonsense, and nx_secure meets
#   NX_NOT_CONNECTED before it finishes disbelieving the record.  Either
#   answer is legible; what matters is that it is an answer.
#
# THIS IS A BASELINE, unlike run-fetch.sh and run-api.sh: the peer is a script
# in this tree, on loopback, so there is no internet, no third party and no
# certificate that can rotate underneath it.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"

while getopts "m:t:c:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir]" >&2; exit 2 ;;
    esac
done

command -v python3 >/dev/null 2>&1 || {
    echo "python3 is needed to run the rude peer" >&2; exit 2; }

# The guest reaches the host through SLIRP's gateway alias.
HOST=10.0.2.2

COMMANDS="$ROOT/build/hangup-commands.txt"
cat > "$COMMANDS" <<EOF
SYS:AddNetInterface eth0
SYS:fetch https://$HOST:4443/ QUIET
SYS:fetch https://$HOST:4444/ QUIET
SYS:fetch https://$HOST:4445/ QUIET TIMEOUT 15
SYS:fetch https://$HOST:4446/ QUIET
EOF

SERVER_LOG="$ROOT/build/hangup-server.log"

# The server reads stdin and stops at EOF, so the FIFO going away is what
# shuts it down, no pid file, and no stray listener left behind if this
# script is interrupted.
FIFO="$ROOT/build/hangup-server.fifo"
rm -f "$FIFO"
mkfifo "$FIFO"

python3 "$ROOT/tests/tls/hangup-server.py" < "$FIFO" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
exec 9>"$FIFO"

cleanup() {
    exec 9>&- || true
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f "$FIFO"
}
trap cleanup EXIT INT TERM HUP

for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q '^ready$' "$SERVER_LOG" 2>/dev/null && break
    sleep 0.5
done
grep -q '^ready$' "$SERVER_LOG" || { echo "rude peer did not start:" >&2
                                     cat "$SERVER_LOG" >&2; exit 2; }
echo "==> rude peer up on 127.0.0.1:4443-4446"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

TAG="${AMINETXDUO_RUN_TAG:-hangup}"

set +e
AMINETXDUO_RUN_TAG="$TAG" \
AMINETXDUO_FETCH_COMMANDS="$COMMANDS" \
AMINETXDUO_BUILD="$BUILD" \
    "$ROOT/tests/tls/run-fetch.sh" -m "$MODEL" -t "$TIMEOUT" -b "$BUILD" "${CPUARG[@]}"
rc=$?
set -e

echo "---- what the rude peer saw ----"
cat "$SERVER_LOG"

# ---------------------------------------------------------- the verdict ---
#
# THIS FILE USED TO END `exit "$rc"`.
#
# run-fetch.sh does not score a custom command list -- it says so and returns
# 77 -- so the status forwarded here was a skip, and before that it was a 0
# that meant "ToolsSmoke's last command returned 0".  Neither is a fact about
# any of the four cases this harness exists for, and its row in
# tests/HARNESSES said as much: IT ASSERTS NOTHING TODAY.
#
# So the transcript is read instead.  run-fetch.sh's own status still matters
# for one thing -- whether the emulator came back at all -- and is reported
# beside the verdict rather than being it.
echo
echo "---- the verdict ----"
# shellcheck source=tests/tls/hangup-verdict.sh
. "$ROOT/tests/tls/hangup-verdict.sh"

REPORT="$ROOT/build/amiberry-testhd-$TAG/tools.txt"

printf 'run_rc=%s\n' "$rc"
case "$rc" in
    0|77) ;;
    *) printf 'reason=%s\n' "the emulator did not come back cleanly"
       printf 'RESULT=broken\n'
       exit 3 ;;
esac

if hangup_verdict "$REPORT" 15 "$SERVER_LOG"; then
    printf 'RESULT=pass\n'
    exit 0
fi
printf 'RESULT=fail\n'
exit 1
