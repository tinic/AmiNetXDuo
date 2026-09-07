#!/usr/bin/env bash
#
# IS THE RECEIVE PATH CPU-BOUND, AND BY HOW MUCH?  One build, one wire, two CPU
# clocks.  The ratio between them splits the frame into the part that scales
# with the processor and the part the bus owns.
#
#   tests/perf/run-cpuscale.sh [-b BUILDDIR] [-B IFACE] [-P PEER]
#                              [-m MULTIPLIER] [-r ROUNDS]
#
# SWEEP AT LEAST TWO MULTIPLIERS.  The baseline is not at 1 and one ratio
# cannot tell you where it is.
#
# WHY IT EXISTS.  "Receive is at its floor on this rig" was the campaign's
# standing conclusion, argued from a profile with no idle rows.  It had never
# been tested directly, and the direct test takes one sitting:
#
#     multiplier   base rx      fast rx       ratio
#      4           5,950,800    5,919,794     0.99
#      8           5,965,073   10,809,784     1.81
#     16           5,917,563   20,038,603     3.39
#
# THE FOUR ARM IS THE ANSWER TO A QUESTION NOBODY ASKED AND THE KEY TO THE REST:
# it changes NOTHING, because the A1200 quickstart already runs at
# cpu_multiplier=4.  So the real speedup factors are 2x and 4x, not 8x and 16x,
# and Amdahl -- T_new/T_old = f/M + (1-f) -- reads:
#
#     M = 2, ratio 1.81  ->  f = 90%
#     M = 4, ratio 3.39  ->  f = 94%
#
# NINE TENTHS OF A RECEIVED FRAME IS CPU AND SCALES.  Only about a tenth is the
# a2065's window, which the multiplier deliberately does not touch.  Two points
# agreeing to four points of f is what makes it a measurement rather than one
# ratio and an assumption.
#
# A single point WOULD have got this wrong: taken alone, the 16 arm divided by
# a nominal 16 gives f = 75%, and the whole error is not knowing where the
# baseline sits on the same axis.  Sweep at least two multipliers.
#
# So CPU work on this path pays almost in full, and the per-frame commits that
# measured +0.04% removed too little to see -- which is a different thing from
# a floor.
#
# It also says the base rate was never a wire limit: the fast arm reads 20
# Mbit/s over emulated 10 Mbit Ethernet, so the bridge does not enforce wire
# speed and nothing is being clipped at 10.
#
# The fast arm is quieter, and the size of that has to be quoted carefully.
# WITHIN ONE INVOCATION OF ONE ARM -- same build, same tag, three rounds -- it
# spread 0.05% against the base arm's 4.30%.  ACROSS AN A/B it does not: two
# builds in two worktrees, alternated, three rounds each at multiplier 16,
# spread 0.62% and 1.31%.  Build, tag and worktree carry variance of their own
# and the multiplier does not remove it.
#
# So: 4.30% -> about 1%, a factor of four, not eighty.  Good enough to resolve
# an effect of two or three per cent and not one of a few tenths.
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
