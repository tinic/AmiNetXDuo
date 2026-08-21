#!/usr/bin/env bash
#
# IS THE MACHINE STILL ON THE NETWORK WHILE IT IS DOING A TLS HANDSHAKE?
#
#   tests/tls/run-reachability.sh [-P user@peer] [-B BACKEND] [-b BUILDDIR]
#                                 [-m MODEL] [-u URL] [-i SECONDS]
#                                 [-x PERCENT] [-g SECONDS] [-t SECONDS]
#                                 [-k MHZ]
#
# WHY IT EXISTS
#
#   A 100.4 s `fetch https://www.gnu.org/` used to take the guest off the
#   network for about 44 s in the middle of it: the peer's neighbour entry
#   went REACHABLE -> STALE -> PROBE -> FAILED and stayed there, and every
#   inbound connection got EHOSTUNREACH until the handshake finished.  The
#   machine was not wedged -- it was inside the asymmetric arithmetic of a
#   certificate chain and never yielding long enough to answer an ARP.  That
#   affects httpd, WebDAV and ssh in the field, not just a test, and it is why
#   the release end-to-end arm was red: its drill runs while the guest is
#   doing its own https fetch.
#
#   It is fixed, and what fixes it is the stride of the crypto yield hook.  A
#   stride is a number somebody will change.  This is the gate that says so.
#
# THE BAR, AND WHERE THE NUMBERS COME FROM
#
#   Measured in this lab, at the 7 MHz default: repaired, 31 of 31 probes
#   answered and a longest gap of 2 s, which is the sampling period.  With the
#   yield hook made to give nothing back, 16 of 19 and a 13 s gap.  The
#   scratchpad rig this replaces measured the original defect at 7 of 24, with
#   stretches of 35 to 65 s in which the address could not be resolved.
#
#   So two things are asserted, not one:
#
#     -x  the share of probes that were ANSWERED, default 90 %.  Not 100:
#         the segment is a real one and a single lost frame under load must
#         not be a red line.
#     -g  the longest gap between two ANSWERED probes, default 8 s.  This is
#         the assertion that matters.  A machine that answers 90 % of probes
#         with the other 10 % in one 40-second block is exactly the defect,
#         and a percentage on its own cannot see the difference.
#
#         Measured off the timestamps and not off a probe count, because an
#         unanswered probe costs its own three-second connect timeout and a
#         count times an interval would understate every gap it is looking
#         for.  A healthy run reads 1 to 2 s, which is the sampling period.
#
#   The handshake here is about 18 s of wall clock -- www.gnu.org, three
#   certificates, TLS 1.3, on a stock A1200 -- so the probe runs once a second
#   rather than the two the scratchpad rig used, and a run gets about twenty
#   samples instead of ten.
#
# THE PROBE IS FROM ANOTHER MACHINE, AND IT HAS TO BE
#
#   Amiberry injects the guest's frames with pcap on the backend NIC, and
#   injected frames never enter the RX path of the host they were injected on.
#   So the emulator host can never see the guest answer anything, and a
#   sampler running there would report the guest permanently unreachable, with
#   the fix in or out.  -P names a machine on the same segment that is not
#   this one.
#
#   What it does, every -i seconds:
#
#     ip neigh show <guest>      the peer's own resolution state
#     /dev/tcp/<guest>/80        a connect.  Nothing is listening, so the
#                                answer is a REFUSAL -- and a refusal is an
#                                ANSWER: it took a SYN, a route, an ARP and
#                                a reply to produce.  Silence is the failure.
#
# THE BASELINE COMES FIRST.  The command list waits before it fetches, so the
# probes either side of the handshake are the same probes with the same peer,
# and a guest that was already unreachable is a REFUSED run rather than a red
# one.  Without that, a lab problem reads as a crypto regression.
#
# NOT A BASELINE TEST, unlike run-hangup.sh: the URL is on the real internet,
# behind somebody else's server and somebody else's certificate, because a
# three-certificate RSA chain is what makes the machine work long enough for
# this to be worth measuring.  -u names another.
#
# AND THE MACHINE IS THROTTLED TO 7 MHz, WHICH IS PART OF THE GATE
#
#   The clock is not a detail here, it is what makes the question answerable.
#   Measured, with the yield hook installed and made to give nothing back --
#   which is this defect, put back on purpose:
#
#     13 MHz (Amiberry's stock A1200)  18.4 s fetch, 92 % answered, 6 s gap
#     7 MHz  (an A500 or A600 clock)   31.1 s fetch, 84 % answered, 13 s gap
#
#   The 13 MHz arm PASSES a defect that is unmistakably present, because the
#   machine is only busy for nine seconds and nine seconds of silence is not
#   long enough to clear the bar.  A gate at that clock would have to be tuned
#   so tight that a healthy run's own two-second sampling period is inside it.
#   At 7 MHz the same build fails both bars with room either side, and 7 MHz
#   is a machine this project ships a TLS binary for.
#
#   So the throttle is a parameter of the measurement and not a convenience.
#   -k names another clock, and a faster one is a weaker gate.
#
# OUTPUT IS key=value.  Exit 0 the machine stayed reachable, 1 it did not,
# 2 a broken invocation or a lab that cannot run this, 3 the run produced no
# handshake to measure.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 1

PEER="${AMINETXDUO_REACH_PEER:-}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/tls}"
MODEL=A1200
URL="https://www.gnu.org/"
INTERVAL=1
FLOOR=90
MAXGAP=8
TIMEOUT=900
CLOCK=7

while getopts "P:B:b:m:u:i:x:g:t:k:" opt; do
    case "$opt" in
        P) PEER="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        u) URL="$OPTARG" ;;
        i) INTERVAL="$OPTARG" ;;
        x) FLOOR="$OPTARG" ;;
        g) MAXGAP="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac

kv()    { printf '%s=%s\n' "$1" "$2"; }
infra() { kv reason "$1"; kv RESULT refused; exit 2; }

# --------------------------------------------------------------- preflight --

case "$IFACE" in
    slirp|slirp_inbound)
        infra "slirp_cannot_be_probed_from_outside: -B <interface>" ;;
esac

[ -n "$PEER" ] ||
    infra "no peer; -P user@host names a machine on this segment that is not\
 this one, because this one cannot see the guest answer"

[ -f "$BUILDDIR/src/tlslib/tls.library" ] ||
    infra "no tls.library in $BUILD; there is no handshake to measure without it"
[ -f "$BUILDDIR/certificates" ] ||
    infra "no trust store in $BUILD; the chain would be refused before the\
 arithmetic that this measures"
[ -x "$BUILDDIR/src/tools/fetch" ] || infra "no $BUILD/src/tools/fetch"

[ -n "${AMINETXDUO_KICKSTART:-}${AMINETXDUO_KICKSTART_A1200:-}" ] ||
    infra "no boot ROM; export AMINETXDUO_KICKSTART (. ~/amiga-assets/env.sh)"

ssh -o BatchMode=yes -o ConnectTimeout=8 "$PEER" \
    'command -v ip >/dev/null && command -v timeout >/dev/null' 2>/dev/null ||
    infra "cannot reach $PEER over ssh, or it has no ip(8)/timeout(1)"

# The peer must not be this host: it would see nothing and report everything
# unreachable, which is a red run for a reason that is the rig.
PEER_ID=$(ssh -o BatchMode=yes "$PEER" 'cat /etc/machine-id 2>/dev/null || hostname')
HERE_ID=$(cat /etc/machine-id 2>/dev/null || hostname)
[ "$PEER_ID" != "$HERE_ID" ] ||
    infra "-P names this same machine; the probe has to come from another one"

# ------------------------------------------------------------------- the run --

TAG="${AMINETXDUO_RUN_TAG:-reachability}"
HD="$ROOT/build/amiberry-testhd-$TAG"
REPORT="$HD/tools.txt"
COMMANDS="$ROOT/build/reachability-commands.txt"
SAMPLES="$ROOT/build/reachability-samples.txt"
RUNLOG="$ROOT/build/reachability-run.log"

mkdir -p "$ROOT/build"
rm -rf "$HD"
: > "$SAMPLES"

# The wait is what gives the baseline somewhere to be taken.  It is not
# padding: without it there is no measurement of the same guest, through the
# same peer, while it is NOT doing crypto, and every number below would be
# uncalibrated.
BASELINE_WAIT=$((INTERVAL * 6 + 4))
cat > "$COMMANDS" <<EOF
SYS:AddNetInterface eth0
wait $BASELINE_WAIT
SYS:fetch $URL TIMEOUT 400 TO DH0:page.txt
EOF

echo "==> peer $PEER, bridge $IFACE, build $BUILD"
echo "==> $URL at ${CLOCK} MHz, probe every ${INTERVAL}s, floor ${FLOOR}%," \
     "longest gap ${MAXGAP}s"

env AMINETXDUO_RUN_TAG="$TAG" \
    AMINETXDUO_FETCH_COMMANDS="$COMMANDS" \
    AMINETXDUO_BUILD="$BUILD" \
    AMINETXDUO_AMIBERRY_BACKEND="$IFACE" \
    "$ROOT/tests/tls/run-fetch.sh" -m "$MODEL" -t "$TIMEOUT" -b "$BUILD" \
                                  -k "$CLOCK" \
    > "$RUNLOG" 2>&1 &
RUNPID=$!

SSHPID=""
# shellcheck disable=SC2329  # the trap below is the caller
cleanup() {
    [ -z "$SSHPID" ] || kill "$SSHPID" 2>/dev/null
    kill "$RUNPID" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP

# ---- the guest's address, off its own report ------------------------------

GUESTIP=""
while kill -0 "$RUNPID" 2>/dev/null; do
    if [ -s "$REPORT" ]; then
        GUESTIP=$(sed -n 's/^.*online, address \([0-9][0-9.]*\).*$/\1/p' \
                      "$REPORT" | head -1)
        [ -n "$GUESTIP" ] && break
    fi
    sleep 1
done

if [ -z "$GUESTIP" ]; then
    wait "$RUNPID"; rc=$?
    tail -30 "$RUNLOG"
    kv reason "the guest never printed an address (run rc=$rc)"
    kv RESULT broken
    exit 3
fi
kv guest_ip "$GUESTIP"

# ---- the sampler, on the peer ---------------------------------------------
#
# One ssh, one loop, one line per probe, streamed back as it goes so a run
# that is killed still has everything up to that point.  The deadline is
# absolute and lives on the PEER, so the loop ends by itself if this script
# or the link goes away rather than leaving a probe running on somebody
# else's machine.
END=$(( $(date +%s) + TIMEOUT ))
# NO -n.  The loop below IS this ssh's standard input, and -n points stdin at
# /dev/null: the first run of this harness reported "the peer produced no
# probes at all" for exactly that reason, having started a remote shell with
# nothing to read.
ssh -o BatchMode=yes "$PEER" \
    "GUEST=$GUESTIP END=$END INTERVAL=$INTERVAL bash -s" > "$SAMPLES" 2>&1 <<'SAMPLER' &
while [ "$(date +%s)" -lt "$END" ]; do
    now=$(date +%s)
    state=$(ip neigh show "$GUEST" 2>/dev/null | head -1 |
            awk '{ print $NF }')
    err=$(timeout 3 bash -c "exec 3<>/dev/tcp/$GUEST/80" 2>&1); rc=$?
    case "$rc" in
        0)   tcp=open ;;
        124) tcp=timeout ;;
        *)   case "$err" in
                 *refused*) tcp=refused ;;
                 *)         tcp=unreachable ;;
             esac ;;
    esac
    printf 't=%s neigh=%s tcp=%s\n' "$now" "${state:-none}" "$tcp"
    sleep "$INTERVAL"
done
SAMPLER
SSHPID=$!

# ---- when the handshake started and when it stopped -----------------------

fetch_banner_at() { grep -q '^===== SYS:fetch ' "$REPORT" 2>/dev/null; }
fetch_rc_line()   { sed -n '/^===== SYS:fetch /,$p' "$REPORT" 2>/dev/null |
                    sed -n 's/^----- rc \([0-9-]*\), \([0-9]*\) ms.*/\1 \2/p' |
                    head -1; }

T_FETCH=0
while kill -0 "$RUNPID" 2>/dev/null; do
    fetch_banner_at && { T_FETCH=$(date +%s); break; }
    sleep 1
done

FETCH_RC=""; FETCH_MS=""
while kill -0 "$RUNPID" 2>/dev/null; do
    read -r FETCH_RC FETCH_MS <<<"$(fetch_rc_line)"
    [ -n "$FETCH_RC" ] && break
    sleep 2
done
T_DONE=$(date +%s)

wait "$RUNPID"; RUN_RC=$?
sleep "$INTERVAL"
kill "$SSHPID" 2>/dev/null; wait "$SSHPID" 2>/dev/null
SSHPID=""

read -r FETCH_RC FETCH_MS <<<"$(fetch_rc_line)"

echo "---- what the guest printed ----"
sed -n '/^===== SYS:fetch /,$p' "$REPORT" 2>/dev/null | head -20

# ---- what the peer saw -----------------------------------------------------

if [ ! -s "$SAMPLES" ]; then
    tail -20 "$RUNLOG"
    kv reason "the peer produced no probes at all"
    kv RESULT broken
    exit 3
fi

# The baseline: probes taken before the fetch command started.  If the guest
# was not reachable then, nothing after it means anything.
read -r B_TOTAL B_OK < <(awk -v t="$T_FETCH" '
    { split($1, a, "="); split($3, c, "=")
      if (a[2] < t) { n++; if (c[2] == "open" || c[2] == "refused") ok++ } }
    END { print n + 0, ok + 0 }' "$SAMPLES")
kv baseline_probes "$B_TOTAL"
kv baseline_answered "$B_OK"

if [ "$B_TOTAL" -lt 3 ]; then
    kv reason "only $B_TOTAL probes before the handshake started; nothing to\
 calibrate against"
    kv RESULT broken
    exit 3
fi
if [ "$B_OK" != "$B_TOTAL" ]; then
    grep -v 'tcp=open\|tcp=refused' "$SAMPLES" | head -5
    kv reason "the guest was already unreachable before the handshake\
 ($B_OK of $B_TOTAL); this is the segment or the peer, not the crypto"
    kv RESULT refused
    exit 2
fi

# The measurement: probes taken while the handshake was running.
# The gap is measured between ANSWERS, on the clock: the last answer before
# the handshake started is where the first gap is measured from, and the end
# of the handshake closes the last one, so a machine that goes silent and
# stays silent cannot score a small gap by having no probe after it.
read -r TOTAL OK GAP < <(awk -v a="$T_FETCH" -v b="$T_DONE" '
    { split($1, ts, "="); split($3, c, "=")
      t = ts[2] + 0
      answered = (c[2] == "open" || c[2] == "refused")
      if (t < a) { if (answered) last = t; next }
      if (t > b) next
      n++
      if (!answered) next
      ok++
      if (last > 0 && t - last > worst) worst = t - last
      last = t
    }
    END { if (last > 0 && b - last > worst) worst = b - last
          print n + 0, ok + 0, worst + 0 }' "$SAMPLES")

STATES=$(awk -v a="$T_FETCH" -v b="$T_DONE" '
    { split($1, ts, "="); split($2, s, "=")
      if (ts[2] < a || ts[2] > b) next
      c[s[2]]++ }
    END { for (k in c) printf "%s:%d ", k, c[k] }' "$SAMPLES")

kv probes "$TOTAL"
kv answered "$OK"
kv neigh_states "\"${STATES:-none}\""
kv max_gap_s "$GAP"
kv fetch_rc "${FETCH_RC:-none}"
kv fetch_ms "${FETCH_MS:-none}"
kv run_rc "$RUN_RC"

if [ "$TOTAL" -lt 8 ]; then
    kv reason "only $TOTAL probes during the handshake; a fetch that took\
 ${FETCH_MS:-no} ms is not the workload this measures"
    kv RESULT broken
    exit 3
fi

PCT=$(awk -v o="$OK" -v n="$TOTAL" 'BEGIN { printf "%.0f", 100 * o / n }')
kv answered_pct "$PCT"

failed=0
if [ "$PCT" -lt "$FLOOR" ]; then
    kv fail "only ${PCT}% of probes were answered while the machine was doing\
 a handshake, floor is ${FLOOR}%"
    failed=1
fi
if [ "$GAP" -gt "$MAXGAP" ]; then
    kv fail "the machine answered nothing for ${GAP}s in one stretch, ceiling\
 is ${MAXGAP}s; that is the shape of the defect this gate is against"
    failed=1
fi
if grep -q 'neigh=FAILED' "$SAMPLES"; then
    kv neigh_failed yes
    kv fail "the peer's neighbour entry reached FAILED, so the address could\
 not be resolved at all"
    failed=1
else
    kv neigh_failed no
fi

if [ "$failed" != 0 ]; then
    echo "---- the probes that went unanswered ----"
    grep -v 'tcp=open\|tcp=refused' "$SAMPLES" | head -20
    kv RESULT fail
    exit 1
fi

kv RESULT pass
exit 0
