#!/usr/bin/env bash
#
# Run the netstack bring-up test under Amiberry on Linux.
#
#   tests/netstack/run-amiberry.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                                  [-N BOARD] [-B BACKEND]
#
# The Linux counterpart of tests/netstack/run-fsuae.sh and run-winuae.sh.
#
# -N picks the card and -B picks what it is wired to.  The default is the
# A2065 on SLIRP, which is what the other two harnesses do.  -B <interface>
# puts the guest on the host's own LAN instead, with its own MAC and a lease
# from the real DHCP server -- see tools/amiberry-run.sh for what that needs
# from the host.
#
# Every driver except a2065.device is a third-party binary this repository does
# not carry.  tools/fetch-sana2-drivers.sh downloads the two whose licences
# permit it; anything else needs AMINETXDUO_SANA2_DRIVER=<path>.  Without one
# the card is in the machine and nothing can open it, which the run then shows.
#
# a2065.device is not ours to ship either: AMINETXDUO_A2065=<path>, or drop a
# copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/netstack"
MODEL=A1200
TIMEOUT=0
CPU=""
BOARD=a2065
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:c:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

# cnet.device dumps every PCMCIA CIS tuple it walks to the serial port -- about
# 127,000 lines of it, since it reads attribute memory to the end and most of it
# is CISTPL_NULL.  That is the run, not the network: three minutes of serial at
# the emulated UART's rate.  So the PCMCIA board gets its own default rather
# than making every other board wait for it.
if [ "$TIMEOUT" = 0 ]; then
    case "$BOARD" in
        ne2000_pcmcia) TIMEOUT=420 ;;
        *)             TIMEOUT=180 ;;
    esac
fi

EXE="$ROOT/$BUILD/tests/netstack/netstack_test"
[ -f "$EXE" ] || { echo "build $BUILD/tests/netstack/netstack_test first" >&2; exit 2; }

. "$ROOT/tools/sana2-stage.sh"

# A driver the fetch script is allowed to download is used without being asked
# for, so the common case is one command.  An explicit AMINETXDUO_SANA2_DRIVER
# still wins, and a board whose driver cannot be fetched is left to say so.
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$("$ROOT/tools/fetch-sana2-drivers.sh" --print-path "$_want" 2>/dev/null || true)
    [ -n "$_have" ] && [ -f "$_have" ] && export AMINETXDUO_SANA2_DRIVER="$_have"
fi

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ] && [ "$BOARD" = "a2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
    [ -n "$A2065" ] && [ -f "$A2065" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
        exit 2
    }
fi

STAGE="$ROOT/build/amiberry-netstack-stage-$BOARD"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$HERE/devs" "$STAGE/devs"
[ -z "$A2065" ] || cp "$A2065" "$STAGE/devs/a2065.device"

sana2_stage "$BOARD" "$STAGE/devs"
echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netstack-$BOARD}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

exec "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
     -t "$TIMEOUT" "${CPUARG[@]}" "$EXE" "$STAGE/devs"
