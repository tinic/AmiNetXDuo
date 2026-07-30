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
# SANA-II driver both have to be staged -- unlike ram_driver_test and soak_test,
# which link the stack into themselves and need no driver at all. The only
# driver that brings an interface up under FS-UAE here is a2065.device, which is
# Commodore's and not redistributable, so this cannot run in public CI for the
# same reason bsdsocktest and netstack_test cannot.
#
# tools/fetch-sana2-drivers.sh fetches two drivers whose licences do permit it
# (cnet.device, hydra.device). cnet is PCMCIA and was brought up under Amiberry
# rather than FS-UAE; making one of them work here is what would move this test
# into CI, and it would take bsdsocktest with it.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

MODEL="A1200"
BUILD="build/ci/default"

# -t is derived, not picked. The harness runs two internal deadlines back to
# back -- servers reaching listen(), then everything finishing -- and boot and
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
    [ -f "$f" ] || { echo "missing $f -- build concurrent_test bsdsocket_library" >&2; exit 2; }
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

# FS-UAE's own bsdsocket emulation off, as every other harness here does it, so
# a result cannot be the host-socket shim answering instead of ours -- which
# would be a particularly bad way to pass a test about our own bracket.
BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

export AMINETXDUO_RUN_TAG="$TAG"

exec "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
