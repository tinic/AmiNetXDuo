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

# REFUSE TO MEASURE A TREE THE BUILD DID NOT FINISH.
#
# `cmake --build` can return non-zero with the library and device still
# linked -- one unrelated target failing is enough -- and the binaries left
# behind are then whatever the PREVIOUS build produced.  An A/B script that
# does not check the build's exit code measures those and prints a clean
# rate=PASS for them.  That happened twice on this rig in one sitting, and
# both times the numbers looked entirely reasonable, which is the danger.
#
# The build's exit code is not visible from here, but staleness is: if any
# source is newer than the artefact that is about to be measured, that
# artefact does not correspond to this tree.  Cheap, and it catches the case
# no amount of care in the caller does.
_stale=""
for _art in "$BUILD/src/bsdsocket/bsdsocket.library" \
            "$BUILD/src/netdev/anxnet.device"; do
    [ -e "$_art" ] || { echo "rate=error reason=missing_artefact file=$_art" >&2; exit 1; }
    _newer=$(find src port include third_party/netxduo/common/src \
                  -name '*.[ch]' -newer "$_art" -print -quit 2>/dev/null || true)
    [ -n "$_newer" ] && _stale="$_stale $_art(newer: $_newer)"
done
if [ -n "$_stale" ]; then
    echo "rate=error reason=stale_build build=$BUILD" >&2
    printf '  %s\n' $_stale >&2
    echo "  A source file is newer than the binary about to be measured, so" >&2
    echo "  that binary is from an earlier build.  Rebuild and check the exit" >&2
    echo "  code before measuring." >&2
    exit 1
fi

# ------------------------------------------------------------------ measure --

#
# A LIVE EMULATOR MAKES THIS MEASURE THE WRONG BOOT, AND IT HAS.
#
# Every arm writes build/amiberry-serial-$TAG.log and this harness reads what
# it finds there.  A leftover emulator from an earlier job -- one that survived
# a pkill, which run-lossgate.sh's arms routinely do, because bash defers
# SIGTERM while a foreground child runs and the loop is then reparented to
# init -- keeps writing the SAME log under the SAME tag.  The rounds below then
# read a boot from the other job's build, and the number is a fabrication that
# looks exactly like a measurement.
#
# tests/perf/run-rate-ab.sh already refuses on this; the check belongs HERE
# too, because that is where the reading happens and this file has other
# callers.  It names the count so the operator can see what to kill:
#
#   ps -eo pid,ppid,args | grep -E 'run-lossgate|amiberry|serial-time'
#   kill -9 <the loop script>          then the emulator, then the reader
#
# See tools/check-rate.sh's own note on medians: this is the same class of
# defect, a number arriving from somewhere other than the build under test.
#
STALE=$(ps -eo args 2>/dev/null |
        grep -E 'amiberry/build/amiberry|serial-timestamp\.py' |
        grep -cv grep)
if [ "${STALE:-0}" != "0" ] && [ "${AMINETXDUO_RATE_ALLOW_STALE:-0}" = "0" ]; then
    echo "rate=error reason=stale_emulator count=$STALE" >&2
    echo "  Another emulator is writing serial logs; these rounds would read" >&2
    echo "  its boots.  Kill the loop script by PID with -9 first, then" >&2
    echo "  amiberry, then serial-timestamp.py, and re-check with ps." >&2
    echo "  AMINETXDUO_RATE_ALLOW_STALE=1 overrides, for a rig that really is" >&2
    echo "  running two isolated guests." >&2
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

round_rate() {           # $1 = direction (tcp-rx / tcp-tx), $2 = log
    # AVERAGE EVERY TRANSFER IN THE ROUND, not just the first.
    # AMINETXDUO_IPERF_RX_REPEAT exists to run several receive transfers
    # inside one boot so the between-boot variance -- which is most of this
    # harness's ~2% spread -- can be averaged out (tests/tools/run-iperf.sh).
    # `head -1` threw every repeat away, so the option cost wall clock and
    # bought nothing, and nothing said so.  A single line averages to itself,
    # so callers that do not set the option see exactly what they saw.
    sed -n "s/^dir=$1 .*bits_per_sec=\([0-9]*\) .*/\1/p" "$2" |
        awk '{ s += $1; n++ } END { if (n) printf "%d\n", s / n }'
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
