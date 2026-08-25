#!/usr/bin/env bash
#
# IS THE MACHINE STILL ON THE NETWORK WHILE IT IS DOING A TLS HANDSHAKE?
#
#   tests/tls/run-reachability.sh [-P user@peer] [-B BACKEND] [-b BUILDDIR]
#                                 [-m MODEL] [-u URL] [-i SECONDS]
#                                 [-x PERCENT] [-g SECONDS] [-t SECONDS]
#                                 [-k MHZ]
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
END=$(( $(date +%s) + TIMEOUT ))
# NO -n.  The loop below IS this ssh's standard input, and -n points stdin at
# /dev/null: the first run of this harness reported "the peer produced no
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
