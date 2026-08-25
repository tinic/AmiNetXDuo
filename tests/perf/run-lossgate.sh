#!/usr/bin/env bash
#
# The other half of the performance gate: throughput on a link that loses
# packets.
#
#   tests/perf/run-lossgate.sh -H user@host -A addr [-l PERCENT] [-r REPS]
#                              [-d MS] [-j MS] [-o PERCENT]
#                              [-b BUILDDIR] [-T TAG] [-B] [-f BASELINE] [-h]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

FBFLAGS="${AMINETXDUO_LOSSGATE_FBFLAGS:--a -B ens18}"
PEER="${AMINETXDUO_FITZ_PEER:-}"
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-}"
PEER_IF="${AMINETXDUO_PEER_IFACE:-ens18}"
PEER_TC="${AMINETXDUO_PEER_TC:-\$HOME/tc-cap}"
LOSS=5
DELAY="${AMINETXDUO_LOSSGATE_DELAY:-0}"
JITTER="${AMINETXDUO_LOSSGATE_JITTER:-0}"
REORDER="${AMINETXDUO_LOSSGATE_REORDER:-0}"
REPS=3
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TAG="${AMINETXDUO_RUN_TAG:-lossgate}"
RECORD=0
REPS_GIVEN=0
BASELINE="$ROOT/tests/perf/lossgate-baseline.txt"
KB=4096
# The widest tolerance -B will write.  See the refusal below.
MAXTOL="${AMINETXDUO_LOSSGATE_MAXTOL:-25}"
FLOOR="${AMINETXDUO_LOSSGATE_FLOOR:-15}"
DIR=rx
TXRAND="${AMINETXDUO_LOSSGATE_TXRAND:-determ}"

usage() {
    cat <<'EOF'
usage: tests/perf/run-lossgate.sh -H user@host -A addr [-l PERCENT] [-r REPS]
                                  [-b BUILDDIR] [-k KB] [-T TAG] [-B] [-f FILE]
                                  [-D rx|tx|both]

  -H  the peer, over ssh.  A THIRD machine: not this one and not the host the
      emulator runs on.
  -A  the peer's address as the guest sees it
  -l  packet loss percent (default 5)
  -D  which direction loses: rx = peer -> guest (default, and what every
      recorded baseline was measured on), tx = guest -> peer, both
  -d  one-way delay peer -> guest in ms; this is the round trip (default 0)
  -j  jitter on -d in ms (default 0).  netem reorders around it.
  -o  reorder percent; needs -d (default 0)
  -r  repetitions; the median is compared (default 3)
  -k  transfer size in KB (default 4096)
  -B  record the current run as the new baseline
  -f  baseline file (default tests/perf/lossgate-baseline.txt)
EOF
}

while getopts "H:A:l:d:j:o:r:b:k:T:Bf:D:h" opt; do
    case "$opt" in
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        l) LOSS="$OPTARG" ;;
        D) DIR="$OPTARG" ;;
        d) DELAY="$OPTARG" ;;
        j) JITTER="$OPTARG" ;;
        o) REORDER="$OPTARG" ;;
        r) REPS="$OPTARG"; REPS_GIVEN=1 ;;
        b) BUILD="$OPTARG" ;;
        k) KB="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        B) RECORD=1 ;;
        f) BASELINE="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

[ -n "$PEER" ] && [ -n "$PEER_ADDR" ] || { usage >&2; exit 2; }

case "$DIR" in rx|tx|both) ;; *) echo "-D takes rx, tx or both" >&2; exit 2 ;; esac

if [ "$REORDER" != "0" ] && [ "$DELAY" = "0" ]; then
    echo "-o $REORDER needs -d: netem reorders by releasing a frame early," >&2
    echo "and with no delay there is nothing for it to be released ahead of." >&2
    exit 2
fi

IMPAIR="delay ${DELAY}ms jitter ${JITTER}ms reorder ${REORDER}%"

if [ "$DIR" = "tx" ] && [ "$IMPAIR" != "delay 0ms jitter 0ms reorder 0%" ]; then
    echo "-D tx impairs the guest -> peer direction, which netem does not" >&2
    echo "reach, so -d/-j/-o have nothing to act on.  Use -D both to keep" >&2
    echo "them, or drop them." >&2
    exit 2
fi

if [ "$RECORD" = 0 ]; then
    [ -f "$BASELINE" ] ||
        { echo "no baseline at $BASELINE -- record one with -B" >&2; exit 2; }
    WANT=$(sed -n 's/^# Recorded with \([0-9.]*\)% peer-to-guest loss, \([0-9]*\) KB, \([0-9]*\) reps.*/\1 \2 \3/p' \
           "$BASELINE" | head -1)
    if [ -n "$WANT" ]; then
        set -- $WANT
        if [ "$1" != "$LOSS" ] || [ "$2" != "$KB" ]; then
            echo "$BASELINE was recorded at $1% loss over $2 KB and this run" >&2
            echo "is ${LOSS}% over $KB KB.  Those are different links; the" >&2
            echo "comparison would report the rig as a regression.  Match it" >&2
            echo "with -l and -k, or record a new baseline with -B." >&2
            exit 2
        fi
        WANTIMP=$(sed -n 's/^# Impairment: //p' "$BASELINE" | head -1)
        WANTIMP="${WANTIMP:-delay 0ms jitter 0ms reorder 0%}"
        if [ "$WANTIMP" != "$IMPAIR" ]; then
            echo "$BASELINE was recorded with '$WANTIMP' and this run is" >&2
            echo "'$IMPAIR'.  Those are different links; match them with" >&2
            echo "-d/-j/-o, or record a new baseline with -B." >&2
            exit 2
        fi
        if [ "$REPS_GIVEN" = 0 ] && [ -n "${3:-}" ]; then
            REPS="$3"
            echo "==> $REPS arms, which is what $BASELINE was recorded over"
        fi
    fi
    WANTDIR=$(sed -n 's/^# Direction: \([a-z]*\).*/\1/p' "$BASELINE" | head -1)
    if [ "${WANTDIR:-rx}" != "$DIR" ]; then
        echo "$BASELINE was recorded with -D ${WANTDIR:-rx} and this run is" >&2
        echo "-D $DIR.  Losing the other direction is a different link, not a" >&2
        echo "different build.  Match it, or record with -B." >&2
        exit 2
    fi
fi

[ -f "$ROOT/build/fitz/Fitz/fitz" ] || {
    echo "no $ROOT/build/fitz/Fitz/fitz -- run tests/endurance/fetch-fitz.sh" >&2
    echo "This measures throughput against Fitz, which is fetched rather than" >&2
    echo "vendored, so a fresh checkout does not have it." >&2
    exit 2
}

BUSY=$(ssh "$PEER" "ps -eo args= | grep '[f]itz-serve' | wc -l" 2>/dev/null || echo 0)
[ "$BUSY" = "0" ] || {
    echo "the peer already has $BUSY fitz-serve process(es) running." >&2
    echo "Something else is using it; this changes its root qdisc, so wait." >&2
    exit 2
}

GUEST=""

# ------------------------------------------------------------------ netem --

peer_tc() { ssh "$PEER" "$PEER_TC $*"; }

netem_off() {
    peer_tc "qdisc del dev $PEER_IF root" >/dev/null 2>&1 || true
    peer_tc "qdisc del dev $PEER_IF ingress" >/dev/null 2>&1 || true
}

SEED="${AMINETXDUO_LOSSGATE_SEED:-20260811}"

tx_loss_on() {
    local guest="$1" nth
    nth=$(awk -v l="$LOSS" 'BEGIN { printf "%d", (l + 0 > 0) ? (100.0 / l) + 0.5 : 0 }')
    [ "$nth" -ge 2 ] || {
        echo "==> ${LOSS}% is not a drop tc gact can express; tx loss is off" >&2
        return; }
    peer_tc "qdisc add dev $PEER_IF handle ffff: ingress"
    peer_tc "filter add dev $PEER_IF parent ffff: protocol ip prio 1 u32 \
             match ip src $guest/32 action pass random $TXRAND drop $nth"
    echo "==> peer $PEER_IF: 1 in $nth of $guest -> peer dropped on ingress ($TXRAND)"
}

tx_loss_report() {
    case "$DIR" in rx) return ;; esac
    echo "==> guest -> peer drops:"
    peer_tc "-s filter show dev $PEER_IF parent ffff:" 2>/dev/null |
        sed -n 's/^[ \t]*Sent \(.*\)/    \1/p'
}

# DELAY BEFORE LOSS on the netem line, because `reorder' is parsed as a
# modifier of the delay that precedes it and tc rejects the line otherwise.
NETEM_SPEC="loss ${LOSS}%"
if [ "$DELAY" != "0" ]; then
    NETEM_SPEC="delay ${DELAY}ms"
    [ "$JITTER" = "0" ] || NETEM_SPEC="$NETEM_SPEC ${JITTER}ms"
    [ "$REORDER" = "0" ] || NETEM_SPEC="$NETEM_SPEC reorder ${REORDER}%"
    NETEM_SPEC="$NETEM_SPEC loss ${LOSS}%"
fi

netem_on() {
    local guest="$1"
    netem_off
    case "$DIR" in tx|both) tx_loss_on "$guest" ;; esac
    case "$DIR" in tx) return ;; esac
    peer_tc "qdisc add dev $PEER_IF root handle 1: prio bands 3"
    peer_tc "qdisc add dev $PEER_IF parent 1:3 handle 30: netem $NETEM_SPEC \
             seed $SEED" 2>/dev/null || {
        echo "==> this peer's tc has no netem 'seed': the loss pattern is" \
             "random per run, and run-to-run drift will swamp the gate"
        peer_tc "qdisc add dev $PEER_IF parent 1:3 handle 30: netem $NETEM_SPEC"; }
    peer_tc "filter add dev $PEER_IF protocol ip parent 1: prio 1 u32 \
             match ip dst $guest/32 flowid 1:3"
    peer_tc "qdisc show dev $PEER_IF" | grep -q "netem" || {
        echo "netem is not on $PEER_IF after asking for: $NETEM_SPEC" >&2
        echo "the arm would measure a clean link under an impaired name." >&2
        exit 2; }
    echo "==> peer $PEER_IF: netem $NETEM_SPEC towards $guest, everything else clean"
    peer_tc "-s qdisc show dev $PEER_IF" | sed 's/^/    /'
}

trap netem_off EXIT INT TERM HUP

# --------------------------------------------------------------------- run --

OUT="$ROOT/build/lossgate-$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "==> warm-up arm, no loss, to learn the guest address"
WARM_RC=0
AMINETXDUO_FITZ_PEER="$PEER" AMINETXDUO_FITZ_PEER_ADDR="$PEER_ADDR" \
AMINETXDUO_RUN_TAG="$TAG-warm" \
    tests/perf/run-fitzbench.sh -H "$PEER" -A "$PEER_ADDR" -b "$BUILD" \
        $FBFLAGS -k "$KB" -r 1 -T "$TAG-warm" > "$OUT/warm.txt" 2>&1 || WARM_RC=$?

[ "$WARM_RC" = "0" ] || {
    echo "the warm-up arm failed (rc=$WARM_RC) on a link with no impairment" >&2
    echo "on it yet, so this is the rig and not a result." >&2
    echo "--- the last 15 lines of $OUT/warm.txt ---" >&2
    tail -15 "$OUT/warm.txt" >&2
    exit 2; }

GUEST=$(sed -n 's/.*address \([0-9][0-9.]*\).*/\1/p' "$OUT/warm.txt" | head -1)
[ -n "$GUEST" ] || GUEST=$(grep -oE '192\.168\.[0-9]+\.[0-9]+' "$OUT/warm.txt" \
                           | grep -v "^$PEER_ADDR$" | head -1 || true)
[ -n "$GUEST" ] || {
    echo "could not learn the guest's address from $OUT/warm.txt" >&2
    echo "the warm-up arm measured nothing, so this is the rig and not a" >&2
    echo "result.  The usual causes are a missing ingredient -- no ROM, no" >&2
    echo "a2065.device, no build/fitz -- or a guest that never got a lease." >&2
    echo "--- the last 15 lines of it ---" >&2
    tail -15 "$OUT/warm.txt" >&2
    exit 2; }
echo "==> guest is $GUEST"

netem_on "$GUEST"

: > "$OUT/samples.txt"
ARMS_OK=0
ARMS_FAILED=0
for rep in $(seq 1 "$REPS"); do
    echo "==> lossy arm $rep/$REPS"
    ARM_RC=0
    AMINETXDUO_FITZ_PEER="$PEER" AMINETXDUO_FITZ_PEER_ADDR="$PEER_ADDR" \
    AMINETXDUO_RUN_TAG="$TAG-$rep" \
        tests/perf/run-fitzbench.sh -H "$PEER" -A "$PEER_ADDR" -b "$BUILD" \
            $FBFLAGS -k "$KB" -r 1 -T "$TAG-$rep" > "$OUT/arm-$rep.txt" 2>&1 ||
        ARM_RC=$?

    awk -v rep="$rep" '
        # fitzbench names the arm as "fitzbench: file=FITZ:fitzbench.dat".
        # It used to say "FitzBench FITZ:", which this matched on and which
        # nothing has printed for weeks -- so every sample came back empty and
        # the gate would have reported no data rather than a regression.
        /file=FITZ:/ { infitz = 1 }
        /file=RAM:/  { infitz = 0 }
        infitz && /RESULT read kbs_mean=/ && !seen["r"]++ {
            sub(/.*kbs_mean=/, ""); print rep, "read_kbs",  $1 }
        infitz && /RESULT write kbs_mean=/ && !seen["w"]++ {
            sub(/.*kbs_mean=/, ""); print rep, "write_kbs", $1 }
        infitz && /^[ \t]*[0-9]+ retransmitted, [0-9]+ dropped/ && !seen["d"]++ {
            print rep, "retransmitted", $1
            print rep, "dropped_rx",    $3 }
    ' "$OUT/arm-$rep.txt" > "$OUT/arm-$rep.samples"

    ARM_RATES=$(awk '$2 == "read_kbs" || $2 == "write_kbs" { n++ }
                     END { print n + 0 }' "$OUT/arm-$rep.samples")
    if [ "$ARM_RC" = "0" ] && [ "$ARM_RATES" = "2" ]; then
        cat "$OUT/arm-$rep.samples" >> "$OUT/samples.txt"
        ARMS_OK=$((ARMS_OK + 1))
        echo "arm_$rep=ok"
    else
        ARMS_FAILED=$((ARMS_FAILED + 1))
        echo "arm_$rep=failed rc=$ARM_RC rates=$ARM_RATES" >&2
        echo "    it contributes no sample; $OUT/arm-$rep.txt ends:" >&2
        tail -6 "$OUT/arm-$rep.txt" | sed 's/^/    /' >&2
    fi
done

tx_loss_report
netem_off

echo "arms_ok=$ARMS_OK"
echo "arms_failed=$ARMS_FAILED"
echo "arms_asked=$REPS"

[ "$ARMS_OK" != "0" ] || {
    echo "FAIL: no arm produced a figure; there is nothing to take a median of" >&2
    exit 2; }

awk '{ v[$2] = v[$2] " " $3 }
     END {
        for (k in v) {
            n = split(v[k], a, " ")
            for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++)
                if (a[j] + 0 < a[i] + 0) { t = a[i]; a[i] = a[j]; a[j] = t }
            med = (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
            lo = int(n / 4) + 1; hi = n - int(n / 4)
            spread = (med + 0 > 0) ? (a[hi] - a[lo]) * 100.0 / med : 0
            range  = (med + 0 > 0) ? (a[n] - a[1]) * 100.0 / med : 0
            printf "%s %.1f %.1f %d %.1f\n", k, med, spread, n, range
        }
     }' "$OUT/samples.txt" | sort > "$OUT/median.txt"

if [ "$RECORD" = "1" ]; then
    [ "$ARMS_FAILED" = "0" ] || {
        echo "==> NOT recorded: $ARMS_FAILED of $REPS arms measured nothing," >&2
        echo "    so this sweep is $ARMS_OK arms deep and says it is $REPS." >&2
        exit 2; }

    noisy=""
    {
        echo "# tests/perf/run-lossgate.sh baseline."
        echo "# NAME  DIRECTION  VALUE  TOLERANCE_PERCENT"
        echo "# Recorded with ${LOSS}% peer-to-guest loss, $KB KB, $REPS reps."
        echo "# Direction: $DIR"
        echo "# Impairment: $IMPAIR"
        echo "# Read and write are separate on purpose: the 0.16.6 regression"
        echo "# moved them in opposite directions."
        echo "# Tolerance: twice the interquartile spread over root(reps), floor ${FLOOR}%."
        while read -r name med spread n _range; do
            tol=$(awk -v s="$spread" -v n="$n" -v f="$FLOOR" 'BEGIN {
                    if (n + 0 < 1) n = 1
                    t = 2 * s / sqrt(n); if (t < f) t = f; printf "%.1f", t }')
            dir=higher
            case "$name" in retransmitted|dropped_rx) dir=lower ;; esac
            printf '%-14s %-7s %10s %6s\n' "$name" "$dir" "$med" "$tol"
            case "$name" in
                *_kbs) awk -v t="$tol" -v m="$MAXTOL" \
                           'BEGIN { exit !(t + 0 > m + 0) }' &&
                           noisy="$noisy $name(+-$tol%)" ;;
            esac
        done < "$OUT/median.txt"
    } > "$BASELINE.new"

    if [ -n "$noisy" ]; then
        echo "==> NOT recorded.  These came out too noisy to gate anything:" >&2
        echo "   $noisy" >&2
        echo "    against a ceiling of ${MAXTOL}%.  Raise -k (a bigger" >&2
        echo "    transfer costs almost nothing: an arm is mostly its boot)" >&2
        echo "    and -r, and run it on a machine with nothing else on it." >&2
        echo "    What it would have written is in $BASELINE.new" >&2
        exit 1
    fi

    mv "$BASELINE.new" "$BASELINE"
    echo "==> baseline written to $BASELINE"
    cat "$BASELINE"
    exit 0
fi

echo
printf '%-14s %10s %10s %9s %8s\n' METRIC BASELINE NOW CHANGE VERDICT
RC=0
while read -r name dir base tol; do
    case "$name" in '#'*|'') continue ;; esac
    now=$(awk -v n="$name" '$1 == n { print $2 }' "$OUT/median.txt")
    [ -n "$now" ] || { printf '%-14s %10s %10s %9s %8s\n' "$name" "$base" - - MISSING
                       RC=1; continue; }
    read -r pct verdict <<EOF
$(awk -v b="$base" -v n="$now" -v d="$dir" -v t="$tol" 'BEGIN {
        if (b + 0 == 0) { print "0.0 SKIP"; exit }
        p = (n - b) * 100.0 / b
        bad = (d == "higher") ? (p < -t) : (p > t)
        printf "%+.1f %s\n", p, bad ? "FAIL" : "ok"
     }')
EOF
    case "$name" in
        *_kbs) [ "$verdict" != "FAIL" ] || RC=1 ;;
        *)     [ "$verdict" != "FAIL" ] || verdict="high" ;;
    esac
    printf '%-14s %10s %10s %8s%% %8s\n' "$name" "$base" "$now" "$pct" "$verdict"
done < "$BASELINE"

echo
awk '{ printf "    %-14s median %8s  iqr %s%%  range %s%%  over %s rep(s)\n", \
                   $1, $2, $3, $5, $4 }' \
    "$OUT/median.txt"

echo
if [ "$ARMS_FAILED" != "0" ]; then
    echo "==> INCOMPLETE: $ARMS_FAILED of $REPS arms measured nothing, so the"
    echo "    medians above are over $ARMS_OK arm(s).  Whatever the comparison"
    echo "    says, this run is not a verdict on the tolerance."
    exit 2
fi
[ "$RC" = "0" ] && echo "==> PASS" || echo "==> FAIL"
exit "$RC"
