#!/usr/bin/env bash
#
# Run the netstack bring-up test under WinUAE with an emulated ethernet card on
# SLIRP.  The WinUAE counterpart of tests/netstack/run-fsuae.sh.
#
#   tests/netstack/run-winuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                                [-N BOARD]
#
# -m defaults to A3000, because that is the machine this project is aimed at
# and the one FS-UAE cannot emulate the CPU of with any timing meaning.
#
# -N picks the card.  a2065 is the default and the only one we have a driver
# for; the rest are listed in tools/winuae-run.sh and all of them come up in
# WinUAE, but the SANA-II driver for each is a third-party binary that is not
# in this repository.  Passing -N ariadne without an ariadne.device in
# DEVS: gets you a machine with an Ariadne in it and a test that cannot open
# it -- which is a useful thing to see once, and nothing more.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
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
# The driver name is not the board key: Individual Computers ship
# x-surf-100.device, not xsurf100z2.device, and Hydra's card is amiganet.device.
case "$BOARD" in
    a2065)                 DRIVER=a2065.device ;;
    ariadne)               DRIVER=ariadne.device ;;
    ariadne2)              DRIVER=ariadne2.device ;;
    hydra)                 DRIVER=amiganet.device ;;
    xsurf)                 DRIVER=x-surf.device ;;
    xsurf100z2|xsurf100z3) DRIVER=x-surf-100.device ;;
    *)                     DRIVER="$BOARD.device" ;;
esac
DRIVER="${AMINETXDUO_SANA2_DRIVER_NAME:-$DRIVER}"

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
[ -n "$A2065" ] && cp "$A2065" "$STAGE/devs/a2065.device"

if [ "$DRIVER" != "a2065.device" ]; then
    sed "s/^DEVICE=.*/DEVICE=$DRIVER/" "$HERE/devs/NetInterfaces/eth0" \
        > "$STAGE/devs/NetInterfaces/eth0"
    DRV="${AMINETXDUO_SANA2_DRIVER:-}"
    if [ -n "$DRV" ] && [ -f "$DRV" ]; then
        # DEVS:Networks and not DEVS:, because that is where a third-party
        # driver is really installed and OpenDevice does not look there by
        # itself -- see docs/RESEARCH.md 44.9.
        mkdir -p "$STAGE/devs/Networks"
        cp "$DRV" "$STAGE/devs/Networks/$DRIVER"
    else
        echo "!! no $DRIVER staged: set AMINETXDUO_SANA2_DRIVER=<path> to one." >&2
        echo "!! The card will be in the machine and nothing will be able to" >&2
        echo "!! open it, which is what this run will show." >&2
    fi
fi

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netstack-winuae}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

exec "$ROOT/tools/winuae-run.sh" -N "$BOARD" -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs"
