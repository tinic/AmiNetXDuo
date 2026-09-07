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
# WHICH INSTRUMENT THIS IS.  iperf with no induced delay, which is the
# CPU-BOUND one: the guest reads about 6.1 of the wire's 10 Mbit/s, so the
# guest is the limit and a CPU saving has nowhere to hide.  Its weakness is
# spread, ~5% within an arm, and the answer to that is ROUNDS, not delay.
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

BASE_REF=""
HEAD_REF=""
ROUNDS="${AMINETXDUO_RATE_ROUNDS:-5}"
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

for d in "$BASE_DIR" "$HEAD_DIR"; do
    [ -d "$d/.git" ] || { echo "rate_ab=fail reason=no_worktree dir=$d"; exit 2; }
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

run_arm() {                             # $1 dir  $2 label  $3 pass  $4 position
    cd "$1" || return 9
    local out="/tmp/rate-ab-$2-p$3.log"
    AMINETXDUO_RATE_ROUNDS="$ROUNDS" \
        tools/check-rate.sh -b build/ab -B "$IFACE" -P "$PEER" \
        < /dev/null > "$out" 2>&1
    local rc=$?
    local rx tx
    rx=$(sed -n 's/^rate=[a-z]* dir=tcp-rx median=\([0-9]*\) .*/\1/p' "$out" | head -1)
    tx=$(sed -n 's/^rate=[a-z]* dir=tcp-tx median=\([0-9]*\) .*/\1/p' "$out" | head -1)
    echo "sample arm=$2 pass=$3 pos=$4 rc=$rc rounds=$ROUNDS" \
         "rx=${rx:-none} tx=${tx:-none}"
    [ "$rc" = 0 ] || sed -n 's/^\(rate=fail.*\)/  \1/p' "$out" | head -3
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

echo "rate_ab=done passes=$PASSES rounds=$ROUNDS base=$BASE_REF head=$HEAD_REF"
