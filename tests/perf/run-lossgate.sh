#!/usr/bin/env bash
#
# The other half of the performance gate: throughput on a link that loses
# packets.
#
#   tests/perf/run-lossgate.sh -H user@host -A addr [-l PERCENT] [-r REPS]
#                              [-d MS] [-j MS] [-o PERCENT]
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
#   -d PUTS DELAY ON THE SAME BAND, and it is the impairment a user actually
#   has: the lab link is 0.2 ms, where a retransmission costs nothing and a
#   delayed ACK held for a tick is invisible.  20 ms is a LAN-to-LAN hop and
#   100 ms is a continent.  Only peer->guest frames are delayed, so the delay
#   IS the round trip as both ends measure it.
#
#   -j adds jitter and -o adds reordering.  netem's queue is sorted by send
#   time, so jitter reorders on its own; -o is the explicit knob for when
#   that is the thing being varied rather than a side effect.  Reordering
#   needs a delay to be reordered around and the script refuses -o without -d.
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
# One-way delay peer->guest, in milliseconds, and its jitter; and the
# percentage of frames netem lets past the delay immediately, which is what
# reordering is.  All zero is the link this gate was recorded on.
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
# THE NARROWEST, AND IT IS NOT A STATISTIC.  A recording measures how far the
# arms of ONE run scatter; what the gate has to survive is how far the MEDIAN
# moves between runs, on a different netem seed and a different guest, and no
# single run can see that.  The floor is the place to put it, and 5% was a
# guess: a five-arm recording said read was repeatable to 4.3%, and the very
# next three-arm run came in 7.1% low and failed on nothing at all.  15% is
# above the drift this rig has been seen to have and below the 18% regression
# the gate exists to catch, which is the whole window there is.
FLOOR="${AMINETXDUO_LOSSGATE_FLOOR:-15}"
# Which direction loses.  rx is peer -> guest and is what every read curve in
# this tree was measured on, so it stays the default: changing it would make
# every recorded baseline the measurement of a different link.  tx is
# guest -> peer, the direction that makes this stack the SENDER, and until it
# existed no run here had ever retransmitted anything outbound.
DIR=rx
# tx cannot be netem: netem only shapes egress, and the guest's frames reach
# the peer on ingress.  tc's gact action can drop on the ingress hook, and its
# `determ' mode drops every Nth packet, which gives the tx direction the same
# repeatability the netem seed gives rx -- the two builds being compared meet
# the same losses in the same places.  netrand is the random shape, for when
# the periodicity is the thing in question.
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

# netem reorders by letting some frames past the delay; with no delay there is
# nothing to overtake and it silently does nothing.  Refuse rather than run an
# arm whose name says reordering and whose link has none.
if [ "$REORDER" != "0" ] && [ "$DELAY" = "0" ]; then
    echo "-o $REORDER needs -d: netem reorders by releasing a frame early," >&2
    echo "and with no delay there is nothing for it to be released ahead of." >&2
    exit 2
fi

# The impairment in one string, for the netem line, the baseline header and
# the guard that compares them.  Every arm the delay knob exists for is a
# different link, and a 20 ms baseline read against a 0 ms run reports the rig
# as a regression exactly the way a 1% run against a 5% baseline does.
IMPAIR="delay ${DELAY}ms jitter ${JITTER}ms reorder ${REORDER}%"

# Delay, jitter and reordering are netem's, and netem is on the peer's egress,
# which is the rx direction only.  -D tx takes the root qdisc out of the run
# entirely, so asking for both is asking for a link this cannot build -- and
# the arm would run, and report a figure, on a link with no delay in it.
if [ "$DIR" = "tx" ] && [ "$IMPAIR" != "delay 0ms jitter 0ms reorder 0%" ]; then
    echo "-D tx impairs the guest -> peer direction, which netem does not" >&2
    echo "reach, so -d/-j/-o have nothing to act on.  Use -D both to keep" >&2
    echo "them, or drop them." >&2
    exit 2
fi

# THE SAME LINK AND THE SAME NUMBER OF ARMS, OR IT IS NOT A COMPARISON.
#
# Throughput under loss is a function of the loss rate and the transfer size,
# so a 1% run read against a 5% baseline reports the difference between two
# rigs as a regression in the stack.
#
# The ARM COUNT belongs here for a less obvious reason: the read figure is
# bimodal, so the median of three arms and the median of nine are different
# estimators of the same thing.  A baseline recorded over nine, compared
# against a run of three, went red at -16.7% on a build with nothing wrong
# with it.  If the caller did not ask for a rep count, take the baseline's.
#
# Read here, before the arms run, because it decides how many of them there
# are.  Recording skips all of it: -B is where a baseline comes from.
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
        # The delay and reorder half of the same guard.  A baseline written
        # before this line existed has no Impairment line, and the link it was
        # recorded on had none either, so its absence means all zero.
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
    # A baseline written before -D existed carries no direction line and was
    # recorded on rx, which is what the default still is.
    WANTDIR=$(sed -n 's/^# Direction: \([a-z]*\).*/\1/p' "$BASELINE" | head -1)
    if [ "${WANTDIR:-rx}" != "$DIR" ]; then
        echo "$BASELINE was recorded with -D ${WANTDIR:-rx} and this run is" >&2
        echo "-D $DIR.  Losing the other direction is a different link, not a" >&2
        echo "different build.  Match it, or record with -B." >&2
        exit 2
    fi
fi

# Fitz is fetched, not vendored, so on a fresh checkout it is not there.  It
# has to be named HERE rather than discovered by the warm-up arm failing to
# boot: without it the arm produces no address, the script says "the warm-up
# arm probably never got a DHCP lease" and exits 1, and tools/ci.sh:918 turns
# a 1 into "a metric moved past its tolerance".  That is a false report of a
# regression on a rig that never measured anything.  Exit 2 is what the rest
# of this file uses for "this box cannot run this test", and ci.sh already
# reads it as the rig refusing.  Found the first time the emulator workflow
# ran the arm, 2026-08-12.
# -f, not -x: it is an m68k binary the emulator reads as a file, and lha
# unpacks it 0600.  Nothing on this host ever executes it.
[ -f "$ROOT/build/fitz/Fitz/fitz" ] || {
    echo "no $ROOT/build/fitz/Fitz/fitz -- run tests/endurance/fetch-fitz.sh" >&2
    echo "This measures throughput against Fitz, which is fetched rather than" >&2
    echo "vendored, so a fresh checkout does not have it." >&2
    exit 2
}

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
    peer_tc "qdisc del dev $PEER_IF ingress" >/dev/null 2>&1 || true
}

# THE SAME LOSS PATTERN EVERY RUN, or the gate measures the pattern.  netem
# seeds itself from the clock, so which segments are dropped -- and above all
# whether any of them land close enough together to cost a retransmission
# timeout -- is different every time.  Arms inside one run share a qdisc and
# agreed to about 7%; consecutive runs, each with a fresh seed, came back 70,
# 65 and 57 KB/s on an idle rig, which is a fifth of the figure and more than
# the regression this exists to catch.  A fixed seed makes the two builds being
# compared meet the same link.
SEED="${AMINETXDUO_LOSSGATE_SEED:-20260811}"

# The guest -> peer direction, which netem cannot reach: it shapes egress, and
# these frames arrive.  An `ingress' qdisc is not the root one, so this leaves
# whatever the peer already had in place alone, and a u32 filter on the source
# address means nothing but the guest's own frames meet the action.  gact's
# `determ N' drops every Nth packet -- exactly LOSS percent, in the same places
# every run, which is the tx counterpart of the netem seed.
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

# What the tx side actually dropped.  netem prints its own count in the qdisc
# dump above; the ingress action keeps its in the filter's statistics, and a
# run that says the send side was exercised should be able to show the number.
tx_loss_report() {
    case "$DIR" in rx) return ;; esac
    echo "==> guest -> peer drops:"
    peer_tc "-s filter show dev $PEER_IF parent ffff:" 2>/dev/null |
        sed -n 's/^[ \t]*Sent \(.*\)/    \1/p'
}

# DELAY BEFORE LOSS on the netem line, because `reorder' is parsed as a
# modifier of the delay that precedes it and tc rejects the line otherwise.
# A netem queue holds delay*rate frames, so the default limit of 1000 is
# 1000 frames of headroom: at 100 ms and the A2065's rate it is never near it,
# and a limit that overflowed would add loss this run did not ask for.
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
    # iproute2 grew `seed` in 6.x.  Where it is older the run still works and
    # says that its numbers are not comparable with anybody else's.
    peer_tc "qdisc add dev $PEER_IF parent 1:3 handle 30: netem $NETEM_SPEC \
             seed $SEED" 2>/dev/null || {
        echo "==> this peer's tc has no netem 'seed': the loss pattern is" \
             "random per run, and run-to-run drift will swamp the gate"
        peer_tc "qdisc add dev $PEER_IF parent 1:3 handle 30: netem $NETEM_SPEC"; }
    peer_tc "filter add dev $PEER_IF protocol ip parent 1: prio 1 u32 \
             match ip dst $guest/32 flowid 1:3"
    # tc takes a bad netem line and leaves the prio qdisc standing, so the arm
    # runs on a clean link and reports a throughput figure under a name that
    # says impaired.  Read the qdisc back and refuse if netem is not on band 3.
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
# Exit 2, not 1.  A warm-up arm that produced no address measured nothing, so
# whatever went wrong is the rig and not the stack -- and tools/ci.sh:918 turns
# a 1 here into "a metric moved past its tolerance", which is a report of a
# throughput regression on a run that never moved a byte.  Both of the real
# causes seen on 2026-08-12 are ingredients: build/fitz not fetched, and
# AMINETXDUO_A2065 unset ("No a2065.device found").  2 is what the rest of this
# file uses for "this box cannot run this test".
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

tx_loss_report
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
        echo "# Direction: $DIR"
        echo "# Impairment: $IMPAIR"
        echo "# Read and write are separate on purpose: the 0.16.6 regression"
        echo "# moved them in opposite directions."
        echo "# Tolerance: twice the interquartile spread over root(reps), floor ${FLOOR}%."
        # OVER ROOT(REPS), and that is not a refinement.  The gate compares
        # MEDIANS, so the tolerance has to describe how far a median moves,
        # not how far one sample does -- and the range of samples GROWS with
        # the number of them, so the old `spread * 2` made -r 9 a looser gate
        # than -r 3.  More arms must tighten it.
        while read -r name med spread n _range; do
            tol=$(awk -v s="$spread" -v n="$n" -v f="$FLOOR" 'BEGIN {
                    if (n + 0 < 1) n = 1
                    t = 2 * s / sqrt(n); if (t < f) t = f; printf "%.1f", t }')
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
    # ONLY THE RATES VOTE.  The counters beside them are small integers --
    # nineteen dropped segments against twenty-three is four events -- and the
    # recorder already refuses to hold them to the rates' ceiling for that
    # reason.  Letting them decide the verdict anyway put a clean run, read
    # and write both inside 10%, on the floor over a count.  They are printed,
    # and they are not the verdict.
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
[ "$RC" = "0" ] && echo "==> PASS" || echo "==> FAIL"
exit "$RC"
