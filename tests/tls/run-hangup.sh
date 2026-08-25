#!/usr/bin/env bash
# Can a rude peer take the machine down?
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
