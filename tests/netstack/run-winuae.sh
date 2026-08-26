#!/usr/bin/env bash
#
# Run the netstack bring-up test under WinUAE with an emulated ethernet card on
# SLIRP.  The WinUAE counterpart of tests/netstack/run-amiberry.sh.
#
#   tests/netstack/run-winuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                                [-N BOARD]
#
# No SANA-II driver is in this repository: a2065.device needs AMINETXDUO_A2065 or
# a copy in build/a2065.device, and every other board (-N, listed in
# tools/winuae-run.sh) needs AMINETXDUO_SANA2_DRIVER=<path>.  Without one the
# card is in the machine and nothing can open it.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/netstack"
MODEL=A3000
TIMEOUT=180
CPU=""
BOARD=a2065
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:c:b:N:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-N board]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/netstack/netstack_test"
[ -f "$EXE" ] || { echo "build $BUILD/tests/netstack/netstack_test first" >&2; exit 2; }

# The driver name in DEVS:NetInterfaces/eth0 has to match the card, so the
# interface file is rewritten for anything other than the A2065.  Nothing else
# about the test changes: SANA-II is SANA-II.
. "$ROOT/tools/sana2-stage.sh"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ] && [ "$BOARD" = "a2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
    [ -n "$A2065" ] && [ -f "$A2065" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
        exit 2
    }
fi

STAGE="$ROOT/build/winuae-netstack-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$HERE/devs" "$STAGE/devs"
[ -z "$A2065" ] || cp "$A2065" "$STAGE/devs/a2065.device"

sana2_stage "$BOARD" "$STAGE/devs"
echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netstack-winuae}"

# ---------------------------------------------------------- the verdict ---
#
# This used to end in `exec <runner>`, so the script's exit status was the
# guest's own return code: a guest that opened nothing, ran no checks and
# returned 0 was a pass, and so was one whose transcript never arrived.
# tools/test-verdict.sh reads the guest's own counters instead, puts a floor
# under the number of checks, and fails loudly and by name when there is no
# transcript at all.
. "$ROOT/tools/test-verdict.sh"

verdict() {
    # 0 pass, 1 fail, 77 the guest skipped: all three are carried out.
    verdict_guest "netstack" 12 "$1" \
        "$(verdict_hd_winuae)/stdout.txt" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
"$ROOT/tools/winuae-run.sh" -N "$BOARD" -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
