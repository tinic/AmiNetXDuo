#!/usr/bin/env bash
#
# Run tests/stack under FS-UAE: the API called from the stack a Shell command
# actually gets.
#
#   tests/stack/run-stack.sh [-m model] [-t seconds] [-b builddir] [-e]
#
#     -e  under Enforcer + MungWall (tools/enforcer-run.sh -m) instead of a
#         plain boot. That is the instrument for this: a Process's stack is an
#         AllocMem block, so overrunning it lands in MungWall's guard band and
#         is reported instead of merely corrupting something.
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
# It also needs a real ROM to mean what it says, for two reasons measured here:
#
#   * AROS rounds NP_StackSize up to 16 KB, so under the AROS ROM the worker
#     never gets the 4 KB it asked for. The harness reports the granted size and
#     holds the bar at 4096 anyway, but it is not measuring a Shell stack.
#   * the reverse lookup (getnameinfo on an unroutable address) has not been
#     seen to return under AROS on SLIRP, 600 s and still waiting, where on
#     Kickstart 3.1 it comes back and is the deepest phase of the run. An AROS
#     run therefore fails at that phase whatever the library does.
#
# Kickstart 3.1 under Enforcer + MungWall (-e) is the run that means something.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

MODEL="A1200"
TIMEOUT=900
BUILD="build/ci/default"
ENFORCE=0

# BRIDGED, NEVER SLIRP.  -B names the host NIC the guest bridges onto, and
# tools/amiberry-run.sh defaults to slirp when nobody says.  Nothing here puts
# a workload on the link, but a run on SLIRP exercises the emulator's own
# TCP/IP rather than a SANA-II driver, so the backend is named rather than
# inherited.
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:B:e" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        e) ENFORCE=1 ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-B backend] [-e]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/stack/stack_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$EXE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build stack_test bsdsocket_library" >&2; exit 2; }
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

TAG="${AMINETXDUO_RUN_TAG:-stack}"
STAGE="$ROOT/build/stack-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"

UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"

# For the -e lane only.  tools/enforcer-run.sh is the one runner left that
# drives fs-uae (Enforcer needs a real MMU), and its base_dir is this
# directory, so this is where its bsdsocket.library emulation is switched off
# before it can answer for the library under test.  The Amiberry lane never
# reads it.
BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

export AMINETXDUO_RUN_TAG="$TAG"

# ---------------------------------------------------------- the verdict ---
#
# This used to end in `exec <runner>`, so the script's exit status was the
# guest's own return code: a guest that measured nothing and returned 0 was a
# pass, and so was one whose transcript never arrived.  stack_test prints
# `stack: N checks, M failures`, which is what is read instead.
#
# enforcer-run.sh stages under build/testhd-$TAG, amiberry-run.sh under
# build/amiberry-testhd-$TAG, so both are offered and the first one with a
# summary in it wins.
. "$ROOT/tools/test-verdict.sh"

verdict() {
    # 0 pass, 1 fail, 77 the guest skipped: all three are carried out.
    verdict_guest "stack" 12 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$ROOT/build/testhd-$TAG/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

if [ "$ENFORCE" = "1" ]; then
    set +e
    "$ROOT/tools/enforcer-run.sh" -m -n -t "$TIMEOUT" \
         "$EXE" "$STAGE/devs" "$STAGE/libs"
    RUN_RC=$?
    set -e
    verdict "$RUN_RC"
fi

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
