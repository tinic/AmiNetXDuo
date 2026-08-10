#!/usr/bin/env bash
#
# The refused-connect leak of docs/RESEARCH.md 37.5 / 39, on its own.
#
#   tests/leak/run-leak.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# Seven arms of thirty-two socket lifecycles, about ninety seconds all told, on
# the SHARED emulator lane, no -x, so it does not block anybody's measurement.
#
# Read build/serial.log afterwards as well as the console output: the library's
# own "nx_tcp_socket_delete refused" warning goes to the serial port and is the
# leak seen from the inside.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=180
BUILD="${AMINETXDUO_BUILD:-build/cm}"

# refused_leak_test says "7 arms" in its own banner and marks each with
# `-- arm X`; the count is the evidence that the run did the work.
LEAK_ARMS=7

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/leak/refused_leak_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$EXE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build refused_leak_test bsdsocket_library" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

TAG="${AMINETXDUO_RUN_TAG:-leak}"
STAGE="$ROOT/build/leak-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"

UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"

# FS-UAE's own bsdsocket emulation off, as every other harness here does it,
# so a result cannot be the host-socket shim answering instead of ours.
BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

export AMINETXDUO_RUN_TAG="$TAG"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e

# ---------------------------------------------------------- the verdict ---
#
# This used to end in `exec <runner>`, so the script's exit status was the
# guest's own return code and a run that never reached the drive was a pass.
# refused_leak_test prints no check counter; it prints `-- arm X` per arm and
# one verdict line, so the arms are the evidence of work done and the verdict
# line is the result.  Both are required, and an absent transcript is named as
# an infrastructure failure rather than reported as clean.
. "$ROOT/tools/test-verdict.sh"

REPORT=$(verdict_find "leak" "$RUN_RC" 'refused_leak: (clean|LEAKED)' \
                      "$(verdict_hd_amiberry)/stdout.txt" \
                      "$(verdict_serial_amiberry)") || exit 1

echo "==> verdict from $REPORT"

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

ARMS=$(grep -c '^-- arm ' "$REPORT" || true)
if [ "$ARMS" -ge "$LEAK_ARMS" ]; then
    pass "all $ARMS arms ran"
else
    fail "only $ARMS of $LEAK_ARMS arms ran, the guest stopped early"
fi

if grep -q 'refused_leak: LEAKED' "$REPORT"; then
    fail "sockets were leaked"
    grep -n 'refused_leak: LEAKED' "$REPORT" | sed 's/^/       /' >&2
elif grep -q 'refused_leak: clean' "$REPORT"; then
    pass "no arm leaked"
else
    fail "the guest never printed its verdict, so it did not finish"
    tail -20 "$REPORT" | sed 's/^/       /' >&2
fi

if [ "$RUN_RC" != "0" ]; then
    case "$RUN_RC" in
        124) fail "the run TIMED OUT, the transcript above is a partial run" ;;
        4)   fail "illegal instruction: the guest is built for the wrong CPU" ;;
        *)   fail "the guest exited $RUN_RC" ;;
    esac
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "leak: FAILED" >&2
    exit 1
fi
echo "leak: PASSED, $ARMS arms, nothing leaked"
exit 0
