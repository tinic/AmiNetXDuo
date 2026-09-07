#!/usr/bin/env bash
#
# IS THE RECEIVE PATH CPU-BOUND, AND BY HOW MUCH?  One build, one wire, two CPU
# clocks.  The ratio between them splits the frame into the part that scales
# with the processor and the part the bus owns.
#
#   tests/perf/run-cpuscale.sh [-b BUILDDIR] [-B IFACE] [-P PEER]
#                              [-m MULTIPLIER] [-r ROUNDS]
#
# WHY IT EXISTS.  "Receive is at its floor on this rig" was the campaign's
# standing conclusion, argued from a profile with no idle rows.  It had never
# been tested directly, and the direct test takes one sitting:
#
#     rx  base 5,917,563    cpu_multiplier=16  20,038,603   3.39x
#     tx  base 3,279,734    cpu_multiplier=16  10,504,104   3.20x
#
# SIXTEEN TIMES THE PROCESSOR BUYS 3.4 TIMES THE RATE, and Amdahl on the frame
# gives the split: T_new/T_old = f/M + (1-f), so f = 75%.  THREE QUARTERS OF A
# RECEIVED FRAME IS CPU AND SCALES; the other quarter is the a2065's window and
# does not, which is the same quarter the profile calls per-byte -- the copies
# read out of board SRAM.  So CPU work on this path DOES pay, and the per-frame
# commits that measured +0.04% removed too little to see, which is a different
# thing from a floor.
#
# It also says the base rate was never a wire limit: the fast arm reads 20
# Mbit/s over emulated 10 Mbit Ethernet, so the bridge does not enforce wire
# speed and nothing is being clipped at 10.
#
# The fast arm is the quieter instrument, which is worth knowing on its own:
# 0.05% spread across three rounds against the base arm's 4.30%.
#
# cpu_multiplier goes in through AMINETXDUO_AMIBERRY_EXTRA (amiberry-run.sh:552)
# rather than -k, because run-iperf.sh owns the amiberry-run.sh invocation.
# Chip RAM deliberately does not scale with the multiplier -- that is what makes
# the ratio mean something.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 1

BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_RATE_IFACE:-}"
PEER="${AMINETXDUO_RATE_PEER:-}"
MULT=16
ROUNDS=3

while getopts "b:B:P:m:r:h" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        m) MULT="$OPTARG" ;;
        r) ROUNDS="$OPTARG" ;;
        h) sed -n '3,9p' "$0"; exit 0 ;;
        *) sed -n '3,9p' "$0" >&2; exit 2 ;;
    esac
done

if [ -z "$IFACE" ] || [ -z "$PEER" ]; then
    echo "cpuscale=skipped reason=no_bridged_rig"
    exit 0
fi

live=$(ps -eo args | grep -E 'amiberry/build/amiberry|serial-timestamp\.py' |
       grep -cv grep)
[ "$live" = "0" ] || { echo "cpuscale=fail reason=stale_emulator count=$live"; exit 8; }

one() {                                 # $1 label  $2 extra  $3 round
    local out="/tmp/cpuscale-$1-r$3.log" rc rx tx
    AMINETXDUO_RUN_TAG="cpuscale$1" AMINETXDUO_AMIBERRY_EXTRA="$2" \
        tests/tools/run-iperf.sh -b "$BUILD" -B "$IFACE" -P "$PEER" \
        < /dev/null > "$out" 2>&1
    rc=$?
    rx=$(sed -n 's/^dir=tcp-rx .*bits_per_sec=\([0-9]*\) .*/\1/p' "$out" | head -1)
    tx=$(sed -n 's/^dir=tcp-tx .*bits_per_sec=\([0-9]*\) .*/\1/p' "$out" | head -1)
    echo "sample arm=$1 round=$3 rc=$rc rx=${rx:-none} tx=${tx:-none}"
}

r=1
while [ "$r" -le "$ROUNDS" ]; do
    if [ $((r % 2)) = 1 ]; then
        one base ""                        "$r"
        one fast "cpu_multiplier=$MULT"    "$r"
    else
        one fast "cpu_multiplier=$MULT"    "$r"
        one base ""                        "$r"
    fi
    r=$((r + 1))
done

echo "cpuscale=done multiplier=$MULT rounds=$ROUNDS"
echo "  ratio = fast/base.  f = (1 - 1/ratio) / (1 - 1/$MULT) is the CPU share"
echo "  of a frame; 1 - f is the a2065 window and does not scale."
