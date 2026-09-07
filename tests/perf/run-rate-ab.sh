#!/usr/bin/env bash
#
# TWO REFS, ALTERNATED, CLEAN BUILD EACH, MEDIANS PRINTED.  The A/B this
# campaign runs constantly and had re-implemented in a scratch file every time.
#
#   tests/perf/run-rate-ab.sh -a BASE_REF -b HEAD_REF [-n ROUNDS] [-p PASSES]
#                             [-B IFACE] [-P PEER] [-x BASE_DIR] [-y HEAD_DIR]
#
# WHY THIS IS A FILE AND NOT A SCRATCH SCRIPT.  Four hand-rolled versions of it
# in one day, and the method rules it has to obey are exactly the ones that get
# dropped when it is retyped:
#
#   * a clean build per arm, in its OWN worktree, so neither arm can be a stale
#     object from the other.  A stale library once made a real +1.5% read as
#     "positions disagree" and the change was written off for a day.
#   * the md5 of both built images PRINTED BEFORE A SINGLE ROUND RUNS, so the
#     log says what was measured rather than what was meant.
#   * ALTERNATE WHICH ARM GOES FIRST.  Position is worth about a per cent on
#     this rig, the same size as the effects being chased, so an unalternated
#     A/B measures position.
#   * a median of N rounds, never one run.  A single run has lied twice.
#
# HOW MANY ROUNDS.  MEASURED, on 2026-09-07: 30 runs of two builds gave a
# per-run sd of 2.06-2.54%, and 12 more at AMINETXDUO_IPERF_SECS=12 gave
# 1.65%.  Resampling 5-round median A/Bs out of data with NO effect in it
# spans -0.97% to +2.32%.  So:
#
#     effect   rounds/arm at SECS=3   at SECS=12
#       3%              4                  2
#       2%              8                  5
#       1%             33                 21
#      <1%          not measurable on this rig
#
# THE DEFAULT FIVE IS A TOLERANCE GATE, NOT AN INSTRUMENT.  It resolves 3% and
# nothing smaller, and below that it will report "+2%" for nothing at all --
# it did exactly that three times in one day, on a pair that 30 runs scored at
# +0.29%, p=0.74.  Pass -n 15 or more for anything under 3%, and take the
# longer transfer while doing it.
#
# THE LONGER TRANSFER IS NEARLY FREE AND IS ON BY DEFAULT HERE.  Raising the
# transfer from run-iperf.sh's 3 s default to 12 s cut the sd 1.25x for a few
# seconds a round.  Note what that ratio says: four times the transfer cut the
# spread by 1.25, where pure within-run sampling noise would have cut it by 2.
# MOST OF THE VARIANCE IS BETWEEN-RUN -- boot-to-boot state, host scheduling,
# the bridge -- not the transfer.  Duration is therefore nearly exhausted as a
# lever; the next real one is several transfers inside ONE boot, which
# run-iperf.sh can already report (guest_val takes an nth-occurrence index,
# run-iperf.sh:477) and which needs the peer taught to send more than once.
#
# WHICH INSTRUMENT THIS IS.  iperf with no induced delay, which is the
# CPU-BOUND one: the guest reads about 6.1 of the wire's 10 Mbit/s, so the
# guest is the limit and a CPU saving has nowhere to hide.  Its weakness is
# spread, and the answer to that is ROUNDS, not delay.
# tests/perf/run-lossgate.sh -M -d 50 -l 0 is the other one: a 50 ms round trip
# collapses the scatter to well under a per cent, but it also leaves the guest
# about a third idle, so it resolves a LATENCY change and hides a CPU one.
# Today's eight per-frame commits measured 543.0 against 543.0 there.
#
# It refuses to start while an emulator is live, because a stale one writes the
# same serial log and the arms would read each other's boots.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

#
# THE RIG'S ENVIRONMENT, BECAUSE WITHOUT IT AMIBERRY HAS NO ROM.
#
# Every hand-rolled version of this script began `. ~/amiga-assets/env.sh` and
# this one did not, so amiberry answered "No boot ROM" in three and a half
# seconds a round -- and run-iperf.sh then re-read a tools.txt from hours
# earlier and reported ITS rate as the round's.  Thirty-six rounds returned
# bit-identical numbers before the ephemeral port gave it away.  Both halves
# are fixed: run-iperf.sh deletes the transcript before booting, and this
# sources the environment the rig keeps for it.
#
AB_ENV="${AMINETXDUO_AB_ENV:-$HOME/amiga-assets/env.sh}"
# shellcheck disable=SC1090
[ ! -r "$AB_ENV" ] || . "$AB_ENV"

BASE_REF=""
HEAD_REF=""
ROUNDS="${AMINETXDUO_RATE_ROUNDS:-5}"
# 12 s, not run-iperf.sh's 3 s default: measured to cut the per-run sd from
# 2.06% to 1.65%.  Exported below so every round of both arms gets it.
SECS="${AMINETXDUO_IPERF_SECS:-12}"
PASSES=2
IFACE="${AMINETXDUO_RATE_IFACE:-}"
PEER="${AMINETXDUO_RATE_PEER:-}"
BASE_DIR="${AMINETXDUO_AB_BASE_DIR:-$HOME/anxd-base}"
HEAD_DIR="${AMINETXDUO_AB_HEAD_DIR:-$HOME/anxd-head}"

usage() {
    sed -n '3,10p' "$0" >&2
}

while getopts "a:b:n:p:B:P:x:y:h" opt; do
    case "$opt" in
        a) BASE_REF="$OPTARG" ;;
        b) HEAD_REF="$OPTARG" ;;
        n) ROUNDS="$OPTARG" ;;
        p) PASSES="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        x) BASE_DIR="$OPTARG" ;;
        y) HEAD_DIR="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

[ -n "$BASE_REF" ] && [ -n "$HEAD_REF" ] || { usage; exit 2; }

if [ -z "$IFACE" ] || [ -z "$PEER" ]; then
    echo "rate_ab=skipped reason=no_bridged_rig"
    echo "  -B a host NIC amiberry may bridge through, -P a machine that can"
    echo "  call in to the guest.  On the lab rig: -B ens18 -P playhouse2."
    exit 0
fi

#
# A LINKED WORKTREE'S .git IS A FILE, NOT A DIRECTORY, and `[ -d ]` refused the
# only two directories this script is ever pointed at.  ~/anxd-base/.git on the
# rig is 53 bytes of "gitdir: /home/turo/anxd-e2e/.git/worktrees/anxd-base" --
# which is the whole point of using worktrees here, so the test has to be the
# one git itself answers.
#
for d in "$BASE_DIR" "$HEAD_DIR"; do
    git -C "$d" rev-parse --git-dir > /dev/null 2>&1 ||
        { echo "rate_ab=fail reason=no_worktree dir=$d"; exit 2; }
done
[ "$BASE_DIR" != "$HEAD_DIR" ] ||
    { echo "rate_ab=fail reason=one_worktree_two_arms"; exit 2; }

# A live emulator writes the serial log these arms read.  See the header.
live=$(ps -eo args | grep -E 'amiberry/build/amiberry|serial-timestamp\.py' |
       grep -cv grep)
if [ "$live" != "0" ]; then
    echo "rate_ab=fail reason=stale_emulator count=$live"
    exit 8
fi

build_arm() {                           # $1 dir  $2 ref  $3 label
    cd "$1" || return 9
    git fetch -q origin || return 1
    git reset --hard -q HEAD
    git clean -qfd src bench port tools tests 2>/dev/null
    git checkout -q --detach "$2" || { echo "rate_ab=fail reason=checkout arm=$3"; return 1; }
    rm -rf build/ab
    cmake -S . -B build/ab \
          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
          -DCMAKE_BUILD_TYPE=Release > "/tmp/rate-ab-$3-cfg.log" 2>&1 ||
        { echo "rate_ab=fail reason=configure arm=$3"; tail -8 "/tmp/rate-ab-$3-cfg.log"; return 1; }
    cmake --build build/ab --parallel 8 > "/tmp/rate-ab-$3-build.log" 2>&1 ||
        { echo "rate_ab=fail reason=build arm=$3"; tail -8 "/tmp/rate-ab-$3-build.log"; return 1; }
    echo "arm=$3 tree=$(git log --oneline -1 | cut -c1-9)" \
         "lib=$(md5sum build/ab/src/bsdsocket/bsdsocket.library | cut -c1-12)" \
         "dev=$(md5sum build/ab/src/netdev/anxnet.device | cut -c1-12)"
}

build_arm "$BASE_DIR" "$BASE_REF" BASE || exit 1
build_arm "$HEAD_DIR" "$HEAD_REF" HEAD || exit 1

#
# IT CALLS run-iperf.sh, NOT check-rate.sh, AND THE DIFFERENCE IS THE POINT.
#
# check-rate.sh is a GATE: it compares a median against a recorded baseline and
# it DISCARDS any round whose harness returned non-zero.  On this rig every
# round returns non-zero, because run-iperf.sh asserts the PEER's side of each
# arm as well as the guest's and every peer .out file comes back empty:
#
#     FAIL: no TCP receive count to compare: guest '2412544' peer ''
#
# while the guest's own measurement is complete in the same run -- tcp-rx
# 5,869,937 bit/s, udp-rx 512 datagrams 0 lost.  So the gate throws away four
# good rate lines to report a cross-check it could not perform, and the CPU-
# bound instrument has been unusable for it.
#
# AN A/B DOES NOT WANT THAT CROSS-CHECK.  It wants the same guest, measured
# twice, on two builds; the peer-side agreement is the gate's business and its
# absence is identical in both arms.  So this reads the rate lines directly and
# takes its own median, and REPORTS the harness rc per round rather than
# obeying it -- `bad=` in the sample line says how many rounds had a non-zero
# harness while still producing a figure, so a reader can see it is the same in
# both arms rather than a difference between them.
#
# A round that produced NO rate line is still dropped: that is a round that
# measured nothing, not a round whose cross-check failed.
#
median_of() {                           # numbers on stdin
    sort -n | awk '{ v[NR] = $1 }
                   END { if (NR == 0) { print "none"; exit }
                         if (NR % 2) print v[(NR + 1) / 2]
                         else        printf "%d\n", (v[NR/2] + v[NR/2 + 1]) / 2 }'
}

run_arm() {                             # $1 dir  $2 label  $3 pass  $4 position
    cd "$1" || return 9
    local out base r rc bad=0 got=0
    base="/tmp/rate-ab-$2-p$3"
    : > "$base.rx"; : > "$base.tx"

    r=1
    while [ "$r" -le "$ROUNDS" ]; do
        out="$base-r$r.log"
        AMINETXDUO_IPERF_SECS="$SECS" \
        tests/tools/run-iperf.sh -b build/ab -B "$IFACE" -P "$PEER" \
            < /dev/null > "$out" 2>&1
        rc=$?
        [ "$rc" = 0 ] || bad=$((bad + 1))

        sed -n 's/^dir=tcp-rx .*bits_per_sec=\([0-9]*\) .*/\1/p' "$out" |
            head -1 >> "$base.rx"
        sed -n 's/^dir=tcp-tx .*bits_per_sec=\([0-9]*\) .*/\1/p' "$out" |
            head -1 >> "$base.tx"
        r=$((r + 1))
    done

    got=$(grep -c . "$base.rx")
    echo "sample arm=$2 pass=$3 pos=$4 rounds=$ROUNDS got=$got bad=$bad" \
         "rx=$(median_of < "$base.rx") tx=$(median_of < "$base.tx")"
    [ "$got" = "$ROUNDS" ] ||
        echo "  $((ROUNDS - got)) round(s) produced no rate line at all"

    #
    # IDENTICAL ROUNDS ARE A DEFECT REPORT, NOT A CLEAN MEASUREMENT.  A boot
    # that fails and a transcript that survives it give the same number every
    # time; that is how 36 rounds of this came back bit-identical.  A rig where
    # every round really does agree to the bit would rather be told twice than
    # have the next reader take a stale answer for a quiet one.
    #
    if [ "$got" -gt 2 ] && [ "$(sort -u "$base.rx" | grep -c .)" = 1 ]; then
        echo "  WARNING: all $got rounds returned the SAME rate to the bit."
        echo "  Check that each round actually booted -- a failed boot used to"
        echo "  re-read the previous run's transcript."
    fi
}

p=1
while [ "$p" -le "$PASSES" ]; do
    if [ $((p % 2)) = 1 ]; then
        run_arm "$BASE_DIR" BASE "$p" 1
        run_arm "$HEAD_DIR" HEAD "$p" 2
    else
        run_arm "$HEAD_DIR" HEAD "$p" 1
        run_arm "$BASE_DIR" BASE "$p" 2
    fi
    p=$((p + 1))
done

echo "rate_ab=done passes=$PASSES rounds=$ROUNDS secs=$SECS base=$BASE_REF head=$HEAD_REF"
# What this run could actually have resolved, from the sd measured on
# 2026-09-07 -- so a small number in the output is read as "under the
# floor" rather than as a result.
echo "rate_ab=resolves >=$(awk -v n="$((ROUNDS * PASSES))" \
    'BEGIN { printf "%.1f", 1.96 * 1.65 * sqrt(2.0 / n) }')% at p<0.05"
