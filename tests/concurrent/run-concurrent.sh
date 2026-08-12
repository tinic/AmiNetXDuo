#!/usr/bin/env bash
#
# Run tests/concurrent under FS-UAE: eight applications, each with its own
# bsdsocket.library base, inside the stack at once.
#
#   tests/concurrent/run-concurrent.sh [-m model] [-t seconds] [-b builddir]
#
# WHY THIS IS NOT IN EMULATOR_TESTS
#
# The harness reaches bsdsocket.library through its LVOs, so the library and a
# SANA-II driver both have to be staged, unlike ram_driver_test and soak_test,
# which link the stack into themselves and need no driver at all. The only
# driver that brings an interface up under FS-UAE here is a2065.device, which is
# Commodore's and not redistributable, so this cannot run in public CI for the
# same reason bsdsocktest and netstack_test cannot.
#
# cnet.device is PCMCIA and was brought up under Amiberry rather than FS-UAE;
# making it work here is what would move this test into CI, and it would take
# bsdsocktest with it.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

MODEL="A1200"
BUILD="build/ci/default"

# -t is derived, not picked. The harness runs two internal deadlines back to
# back, servers reaching listen(), then everything finishing, and boot and
# the DHCP attempt against SLIRP come out of a budget it does not control. The
# same three numbers are in concurrent_test.c (CT_DEADLINE_SECS, CT_BOOT_SECS,
# CT_BUDGET_SECS) and it prints the total on startup, so a -t below its own
# needs shows up as a mismatch in the log rather than as a hang.
#
# Raising CONCURRENT_DEADLINE in CMake without raising this cuts the harness
# off before its own deadline fires, which is what makes a wedge unreadable:
# the run dies with no WEDGED line naming the application that stopped.
BOOT_SECS=90
DEADLINE_SECS="${CONCURRENT_DEADLINE:-60}"
MARGIN_SECS=30
TIMEOUT=$(( BOOT_SECS + 2 * DEADLINE_SECS + MARGIN_SECS ))

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/concurrent/concurrent_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$EXE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build concurrent_test bsdsocket_library" >&2; exit 2; }
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

TAG="${AMINETXDUO_RUN_TAG:-conc}"
STAGE="$ROOT/build/conc-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"

UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"

export AMINETXDUO_RUN_TAG="$TAG"

# SCORED FROM THE VERDICT LINE, NOT FROM THE EXIT STATUS
#
# The last CloseLibrary() drops the final netstack reference and tears the
# stack down, and that hangs: on Commodore's a2065.device 2.16 an AbortIO() on
# a pending SANA-II CMD_READ is never honoured, so the reader's WaitIO() never
# returns. tests/libraries/library_test.c hit the same wall and documents it.
#
# So the harness never returns, s/Startup-Sequence never writes DH0:.done, and
# the runner reports a timeout no matter how the test went. The harness
# prints its verdict BEFORE closing for exactly this reason; that line is the
# result, and a timeout with a verdict above it is the expected shape of a
# passing run rather than a failure.
#
# A timeout with NO verdict is a real failure and still scores as one.
OUT=$("$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
      "$EXE" "$STAGE/devs" "$STAGE/libs" 2>&1) && rc=0 || rc=$?
printf '%s\n' "$OUT"

VERDICT=$(printf '%s\n' "$OUT" | grep -E '^[0-9]+ checks, [0-9]+ failures' | tail -1)

echo
if [ -z "$VERDICT" ]; then
    echo "==> NO VERDICT, the harness did not reach its summary."
    echo "==> Read the serial trace above: the last [ct] line is how far it got."
    exit "${rc:-1}"
fi

echo "==> $VERDICT"
case "$VERDICT" in
    *PASS) echo "==> PASS (the trailing timeout is the a2065 teardown hang above)"
           exit 0 ;;
    *)     exit 20 ;;
esac
