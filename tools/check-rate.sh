#!/usr/bin/env bash
#
# What the link actually carries, against what it carried at the last tag.
#
#   tools/check-rate.sh [-b BUILDDIR] [-B IFACE] [-P PEER] [-n ROUNDS] [--update]
#
# 0.26.0 and 0.26.1 shipped a receive path that ran at a quarter of 0.25.5's
# rate.  Every gate in this directory was green for both, because not one of
# them measured a byte per second: `grep fitzbench tools/ci.sh` was zero and
# tests/perf/run-fitzbench.sh had only ever been run by hand.  A user on an
# A3000 found it instead.
#
# THE MEDIAN OF N ROUNDS, NOT ONE RUN.  A stall released by a retransmit timer
# is erratic -- the build that shipped measured 2.02, 3.74 and 2.94 Mbit/s on
# the same rig in the same hour, an 85 per cent spread, while the fixed one
# measured 4.82, 4.88 and 4.76.  One sample of the broken build can land above
# one sample of the good one; their medians cannot.
#
# THE FLOOR IS A PERCENTAGE, NOT AN ABSOLUTE.  The number a rig produces
# depends on the rig; what may not change is the ratio to the last tag.
#
# SLIRP CANNOT ANSWER THIS.  run-iperf.sh's guest-as-server arm skips there --
# "a SLIRP guest cannot be called in to" -- and the receive direction is the
# whole point, so this needs a bridged interface and a peer that can call in.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

BASELINE="tests/perf/rate-baseline.txt"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_RATE_IFACE:-}"
PEER="${AMINETXDUO_RATE_PEER:-}"
ROUNDS="${AMINETXDUO_RATE_ROUNDS:-3}"
TOLERANCE="${AMINETXDUO_RATE_TOLERANCE:-25}"     # per cent below baseline
UPDATE=0

while [ $# -gt 0 ]; do
    case "$1" in
        -b) BUILD="$2"; shift 2 ;;
        -B) IFACE="$2"; shift 2 ;;
        -P) PEER="$2"; shift 2 ;;
        -n) ROUNDS="$2"; shift 2 ;;
        --update) UPDATE=1; shift ;;
        *) sed -n '3,5p' "$0" >&2; exit 2 ;;
    esac
done

if [ -z "$IFACE" ] || [ -z "$PEER" ]; then
    echo "rate=skipped reason=no_bridged_rig"
    echo "  Set AMINETXDUO_RATE_IFACE to a host NIC amiberry may bridge through"
    echo "  and AMINETXDUO_RATE_PEER to a machine that can call in to the guest."
    echo "  On the lab rig: -B ens18 -P playhouse2.local.tinic.net."
    exit 0
fi

[ -r "$BASELINE" ] || { echo "rate=error reason=no_baseline file=$BASELINE" >&2; exit 1; }

# ------------------------------------------------------------------ measure --

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

round_rate() {           # $1 = direction (tcp-rx / tcp-tx), $2 = log
    sed -n "s/^dir=$1 .*bits_per_sec=\([0-9]*\) .*/\1/p" "$2" | head -1
}

declare -A samples
for dir in tcp-rx tcp-tx; do samples[$dir]=""; done

r=1
while [ "$r" -le "$ROUNDS" ]; do
    if ! tests/tools/run-iperf.sh -b "$BUILD" -B "$IFACE" -P "$PEER" \
            > "$TMP/round$r.log" 2>&1; then
        echo "rate=error reason=harness_failed round=$r" >&2
        tail -20 "$TMP/round$r.log" >&2
        exit 1
    fi
    for dir in tcp-rx tcp-tx; do
        v=$(round_rate "$dir" "$TMP/round$r.log")
        [ -n "$v" ] || { echo "rate=error reason=no_${dir}_line round=$r" >&2; exit 1; }
        samples[$dir]="${samples[$dir]} $v"
        echo "rate_sample dir=$dir round=$r bits_per_sec=$v"
    done
    r=$((r + 1))
done

median() {               # median of the whitespace-separated numbers in $1
    printf '%s\n' $1 | sort -n | awk '{a[NR]=$1} END{print (NR%2) ? a[(NR+1)/2] : int((a[NR/2]+a[NR/2+1])/2)}'
}

# ------------------------------------------------------------------- verdict --

if [ "$UPDATE" = 1 ]; then
    {
        echo "# Receive and transmit rates, bits per second, the MEDIAN of"
        echo "# $ROUNDS rounds.  tools/check-rate.sh --update writes this; raising a"
        echo "# number belongs in the commit that earned it, with the rig named."
        echo "#"
        echo "# rig: $(hostname), -B $IFACE -P $PEER, $(git describe --tags --always 2>/dev/null)"
        for dir in tcp-rx tcp-tx; do
            echo "$dir $(median "${samples[$dir]}")"
        done
    } > "$BASELINE"
    echo "rate=updated file=$BASELINE"
    cat "$BASELINE"
    exit 0
fi

rc=0
for dir in tcp-rx tcp-tx; do
    got=$(median "${samples[$dir]}")
    want=$(sed -n "s/^$dir  *\([0-9]*\).*/\1/p" "$BASELINE" | head -1)
    if [ -z "$want" ]; then
        echo "rate=error reason=no_baseline_for dir=$dir" >&2
        rc=1
        continue
    fi
    floor=$(( want * (100 - TOLERANCE) / 100 ))
    pct=$(( got * 100 / want ))
    if [ "$got" -lt "$floor" ]; then
        echo "rate=SLOWER dir=$dir median=$got baseline=$want floor=$floor pct=$pct"
        echo "  $dir is $((100 - pct)) per cent below the last recorded rate."
        echo "  Either the change costs that, or it is a defect.  If it is the"
        echo "  price of something, say what in the commit and re-record with"
        echo "  tools/check-rate.sh --update."
        rc=1
    else
        echo "rate=ok dir=$dir median=$got baseline=$want floor=$floor pct=$pct"
    fi
done

[ "$rc" = 0 ] && echo "rate=PASS rounds=$ROUNDS tolerance=${TOLERANCE}%"
exit "$rc"
