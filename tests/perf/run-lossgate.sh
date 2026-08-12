#!/usr/bin/env bash
#
# The other half of the performance gate: throughput on a link that loses
# packets.
#
#   tests/perf/run-lossgate.sh -H user@host -A addr [-l PERCENT] [-r REPS]
#                              [-b BUILDDIR] [-T TAG] [-B] [-f BASELINE] [-h]
#
# WHY
#
#   Our lab rig measures zero retransmissions in every run.  A change to
#   acknowledgement or retransmission behaviour is therefore free on it, and
#   0.16.6 shipped two of them: the read direction fell 18% on real hardware
#   while every arm we ran said the opposite.  Loss is what makes that
#   behaviour cost something, and netem on the peer is how it gets induced.
#
#   READ AND WRITE ARE REPORTED SEPARATELY AND MUST STAY THAT WAY.  Across
#   that regression the write direction moved slightly UP while the read
#   direction fell by a fifth; one combined figure would have shown nothing.
#   Note also that FitzBench's write number is buffer acceptance rather than
#   wire throughput -- it is here to show that a change did not move it, not
#   as a rate.
#
# WHAT IT DOES TO THE PEER
#
#   A `prio' qdisc with netem on its third band, and a u32 filter that puts
#   ONLY frames addressed to the guest into that band.  Everything else the
#   peer sends is untouched.  It is still a change to the peer's root qdisc,
#   so the peer must be idle -- the script refuses to start if anything else
#   is serving on it.
#
#   `tc' needs CAP_NET_ADMIN.  Copy it and give the copy the capability rather
#   than modifying anything packaged:
#
#       cp /usr/sbin/tc ~/tc-cap && sudo /usr/sbin/setcap cap_net_admin+ep ~/tc-cap
#
#   AMINETXDUO_PEER_TC names it; the default is ~/tc-cap.
#
# WHAT IT CANNOT TELL YOU
#
#   The emulated card is an A2065 and the guest is not the machine a user has.
#   Absolute rates here are not a prediction of anything.  What carries is the
#   direction and the proportion of a change between two builds measured back
#   to back on this same rig.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# THE RIG MOVED UNDER THIS SCRIPT.  It was written against the WinUAE-over-ssh
# default; every measurement since is bridged Amiberry on the lab box, which
# run-fitzbench.sh spells -a -B <iface>.  Passed through rather than hardcoded
# so the script still works wherever it is pointed.
FBFLAGS="${AMINETXDUO_LOSSGATE_FBFLAGS:--a -B ens18}"
PEER="${AMINETXDUO_FITZ_PEER:-}"
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-}"
PEER_IF="${AMINETXDUO_PEER_IFACE:-ens18}"
PEER_TC="${AMINETXDUO_PEER_TC:-\$HOME/tc-cap}"
# 5%, AND MORE LOSS IS LESS NOISE.  At 1% a 4 MB transfer sees about thirty
# loss events, so which of them lands where -- in slow start, or as a tail loss
# needing an RTO -- is the whole measurement: nine arms on an idle box came
# back 456, 435, 384, 236, 197, 277 KB/s, and no honest tolerance over that
# could catch the 18% regression this exists for.  At 5% there are five times
# as many events, the law of large numbers applies, and consecutive arms agree
# to within a few percent.  The link is harsher and the figure is lower; the
# figure was never a prediction of anything, only a thing to compare.
LOSS=5
REPS=3
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TAG="${AMINETXDUO_RUN_TAG:-lossgate}"
RECORD=0
BASELINE="$ROOT/tests/perf/lossgate-baseline.txt"
KB=4096
# The widest tolerance -B will write.  See the refusal below.
MAXTOL="${AMINETXDUO_LOSSGATE_MAXTOL:-25}"

usage() {
    cat <<'EOF'
usage: tests/perf/run-lossgate.sh -H user@host -A addr [-l PERCENT] [-r REPS]
                                  [-b BUILDDIR] [-k KB] [-T TAG] [-B] [-f FILE]

  -H  the peer, over ssh.  A THIRD machine: not this one and not the host the
      emulator runs on.
  -A  the peer's address as the guest sees it
  -l  packet loss percent applied to peer -> guest frames (default 5)
  -r  repetitions; the median is compared (default 3)
  -k  transfer size in KB (default 4096)
  -B  record the current run as the new baseline
  -f  baseline file (default tests/perf/lossgate-baseline.txt)
EOF
}

while getopts "H:A:l:r:b:k:T:Bf:h" opt; do
    case "$opt" in
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        l) LOSS="$OPTARG" ;;
        r) REPS="$OPTARG" ;;
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

# The peer's qdisc is machine-wide, so two of these running at once measure
# each other.  Refuse rather than produce a number nobody can trust.
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
}

netem_on() {
    local guest="$1"
    netem_off
    peer_tc "qdisc add dev $PEER_IF root handle 1: prio bands 3"
    peer_tc "qdisc add dev $PEER_IF parent 1:3 handle 30: netem loss ${LOSS}%"
    peer_tc "filter add dev $PEER_IF protocol ip parent 1: prio 1 u32 \
             match ip dst $guest/32 flowid 1:3"
    echo "==> peer $PEER_IF: ${LOSS}% loss towards $guest, everything else clean"
    peer_tc "-s qdisc show dev $PEER_IF" | sed 's/^/    /'
}

trap netem_off EXIT INT TERM HUP

# --------------------------------------------------------------------- run --

OUT="$ROOT/build/lossgate-$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

# The guest's address is not knowable before it takes a DHCP lease, and the
# filter needs it.  One warm-up arm with no loss gets it, and doubles as the
# control: a rig that cannot move bytes cleanly is not going to say anything
# useful about a lossy one.
echo "==> warm-up arm, no loss, to learn the guest address"
AMINETXDUO_FITZ_PEER="$PEER" AMINETXDUO_FITZ_PEER_ADDR="$PEER_ADDR" \
AMINETXDUO_RUN_TAG="$TAG-warm" \
    tests/perf/run-fitzbench.sh -H "$PEER" -A "$PEER_ADDR" -b "$BUILD" \
        $FBFLAGS -k "$KB" -r 1 -T "$TAG-warm" > "$OUT/warm.txt" 2>&1 || true

GUEST=$(sed -n 's/.*address \([0-9][0-9.]*\).*/\1/p' "$OUT/warm.txt" | head -1)
# `|| true` on the fallback, and it is not decoration.  Under `set -e` a grep
# that matches nothing makes the whole `[ -n ... ] || GUEST=$(...)` list fail,
# so the script exited 1 with NOTHING PRINTED -- and the two lines below, which
# exist for exactly this case, could never run.  The first thing that went
# wrong here (build/fitz not fetched, so the warm-up arm never booted) was
# reported as a silent exit.
[ -n "$GUEST" ] || GUEST=$(grep -oE '192\.168\.[0-9]+\.[0-9]+' "$OUT/warm.txt" \
                           | grep -v "^$PEER_ADDR$" | head -1 || true)
[ -n "$GUEST" ] || {
    echo "could not learn the guest's address from $OUT/warm.txt" >&2
    echo "the warm-up arm probably never got a DHCP lease -- read it." >&2
    echo "--- the last 15 lines of it ---" >&2
    tail -15 "$OUT/warm.txt" >&2
    exit 1; }
echo "==> guest is $GUEST"

netem_on "$GUEST"

: > "$OUT/samples.txt"
for rep in $(seq 1 "$REPS"); do
    echo "==> lossy arm $rep/$REPS"
    AMINETXDUO_FITZ_PEER="$PEER" AMINETXDUO_FITZ_PEER_ADDR="$PEER_ADDR" \
    AMINETXDUO_RUN_TAG="$TAG-$rep" \
        tests/perf/run-fitzbench.sh -H "$PEER" -A "$PEER_ADDR" -b "$BUILD" \
            $FBFLAGS -k "$KB" -r 1 -T "$TAG-$rep" > "$OUT/arm-$rep.txt" 2>&1 || true

    # Only the FITZ: arm, not the RAM: control that follows it in the same
    # boot -- and the read figure first, because the write one is buffer
    # acceptance.
    #
    # ONE SAMPLE PER REP PER METRIC.  run-fitzbench.sh prints the guest's
    # transcript and then prints it again in its summary, so every RESULT line
    # is in the file three times; without the `seen` guard one rep contributed
    # nine samples and the `n` this reports was three times the number of runs
    # behind it.
    #
    # AND THE NETSTAT COUNTER IS $1, NOT $3.  The line is
    # "0 retransmitted, 18 dropped on receipt": the old expression found the
    # field matching /^retransmit/ and printed the one AFTER it, which is the
    # dropped count, under the name `retransmits`.  It was also unguarded by
    # arm, so it took the snapshot printed BEFORE the transfer as a second
    # sample of the same metric and the median sat between two numbers that
    # measure different things.  Both are recorded now, each named what it is.
    # dropped-on-receipt is the one that moves: on the read direction the guest
    # is the receiver, so its own retransmit counter stays at zero and the
    # peer's retransmissions show up here as duplicates arriving.
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
    ' "$OUT/arm-$rep.txt" >> "$OUT/samples.txt"
done

netem_off

[ -s "$OUT/samples.txt" ] || { echo "FAIL: no arm produced a RESULT line" >&2; exit 1; }

# THE SPREAD IS THE INTERQUARTILE RANGE, not the full range.  The gate
# compares MEDIANS, and a median is robust: one arm that lands at half the
# others -- which is what a busy emulator host produces, and this one is shared
# -- barely moves it.  Measuring the dispersion with max-minus-min is not
# robust at all, so that same single arm tripled the tolerance and turned a
# usable gate into one nothing could ever breach.  Quartiles ignore the tails
# the median already ignores.
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
    # A TOLERANCE WIDE ENOUGH IS NOT A GATE.  The tolerance is twice the
    # observed spread, so a noisy run records a number no regression can
    # breach and the file still looks like a baseline: the first one recorded
    # here gave read_kbs +-282% and retransmits +-1450%, on 512 KB at 1% loss,
    # where a handful of loss events is the whole population and one RTO
    # halves the figure.  That is a gate that has already failed, in the
    # quiet direction, on the day it was written.
    #
    # So the recorder refuses.  The fix is more data per arm and more arms --
    # -k is nearly free because almost all of an arm is the boot, not the
    # transfer -- and the refusal says so.
    noisy=""
    {
        echo "# tests/perf/run-lossgate.sh baseline."
        echo "# NAME  DIRECTION  VALUE  TOLERANCE_PERCENT"
        echo "# Recorded with ${LOSS}% peer-to-guest loss, $KB KB, $REPS reps."
        echo "# Read and write are separate on purpose: the 0.16.6 regression"
        echo "# moved them in opposite directions."
        echo "# Tolerance: twice the interquartile spread over root(reps), floor 5%."
        # OVER ROOT(REPS), and that is not a refinement.  The gate compares
        # MEDIANS, so the tolerance has to describe how far a median moves,
        # not how far one sample does -- and the range of samples GROWS with
        # the number of them, so the old `spread * 2` made -r 9 a looser gate
        # than -r 3.  More arms must tighten it.
        while read -r name med spread n _range; do
            tol=$(awk -v s="$spread" -v n="$n" 'BEGIN {
                    if (n + 0 < 1) n = 1
                    t = 2 * s / sqrt(n); if (t < 5) t = 5; printf "%.1f", t }')
            dir=higher
            case "$name" in retransmitted|dropped_rx) dir=lower ;; esac
            printf '%-14s %-7s %10s %6s\n' "$name" "$dir" "$med" "$tol"
            # The ceiling is for the RATES, which are what this exists to
            # gate.  The counters beside them are small integers -- nineteen
            # dropped segments in a run -- and the square root of nineteen is
            # four, so their relative spread cannot be small however long the
            # run is.  Holding a count to a rate's ceiling refuses every
            # baseline forever.  They are recorded, they are compared, and
            # they are not the verdict.
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

[ -f "$BASELINE" ] || { echo "no baseline at $BASELINE -- record one with -B" >&2; exit 2; }

# THE SAME LINK, OR IT IS NOT A COMPARISON.  Throughput under loss is a
# function of the loss rate and the transfer size, so a 1% run read against a
# 5% baseline reports the difference between two rigs as a regression in the
# stack.  The recorder writes both into the header; this reads them back.
WANT=$(sed -n 's/^# Recorded with \([0-9.]*\)% peer-to-guest loss, \([0-9]*\) KB.*/\1 \2/p' \
       "$BASELINE" | head -1)
if [ -n "$WANT" ]; then
    set -- $WANT
    if [ "$1" != "$LOSS" ] || [ "$2" != "$KB" ]; then
        echo "$BASELINE was recorded at $1% loss over $2 KB and this run is" >&2
        echo "${LOSS}% over $KB KB.  Those are different links; the comparison" >&2
        echo "would report the rig as a regression.  Match it with -l and -k," >&2
        echo "or record a new baseline with -B." >&2
        exit 2
    fi
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
    [ "$verdict" != "FAIL" ] || RC=1
    printf '%-14s %10s %10s %8s%% %8s\n' "$name" "$base" "$now" "$pct" "$verdict"
done < "$BASELINE"

echo
awk '{ printf "    %-14s median %8s  iqr %s%%  range %s%%  over %s rep(s)\n", \
                   $1, $2, $3, $5, $4 }' \
    "$OUT/median.txt"

echo
[ "$RC" = "0" ] && echo "==> PASS" || echo "==> FAIL"
exit "$RC"
