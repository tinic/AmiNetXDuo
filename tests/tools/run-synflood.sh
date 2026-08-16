#!/usr/bin/env bash
#
# A SYN FLOOD MUST NOT STOP THE MACHINE ANSWERING.
#
#   tests/tools/run-synflood.sh -P PEERHOST [-B IFACE] [-a ADDR] [-b BUILDDIR]
#                               [-N BOARD] [-m MODEL] [-t SECONDS] [-p PORT]
#                               [-T TAG] [-u]
#
# WHAT IT PROVES
#
#   The guest runs `httpd` on the LAN.  The peer -- a third machine -- floods
#   its listening port with SYNs from thousands of forged addresses that never
#   answer, the attack of RFC 4987.  Throughout the flood a legitimate client
#   on this host completes a real handshake and a GET, and the run reports how
#   long that took and how many SYNs the flood delivered.
#
#   A stack that pins a socket or a packet per SYN stops answering: the flood
#   fills its backlog and its packet pool and the legitimate GET times out.  A
#   stack that holds 76 bytes per half-open connection and answers past its
#   cache with a stateless cookie does not, and the GET goes through while the
#   flood runs at full rate.
#
#   -u ("unprotected") inverts the verdict.  It is the same run against a tree
#   built from the submodule pin BEFORE the defence -- d1358950, the parent of
#   amiga-syn-defence -- and it PASSES when the legitimate GET FAILS under the
#   flood, which is the proof that the defence is what changed the outcome and
#   not something else.  Point -b at that build.  Without -u the defended tree
#   is expected to keep answering.
#
# WHY THE FORGED SOURCES MUST BE A DEAD RANGE
#
#   synflood.py draws every source from 10.99.0.0/16, a range nothing on the
#   LAN owns.  A SYN from a live host would get the guest's SYN-ACK and RST it,
#   which completes the exchange this end and is a different test.  A dead
#   range leaves every SYN-ACK unanswered, which is the half-open connection
#   the defence is measured against.
#
# THE MEASUREMENT IS THE LEGITIMATE CLIENT, NOT THE FLOOD
#
#   handshake_ms is a real curl from this host to the guest's httpd, timed,
#   taken three times before the flood and three times during it.  The verdict
#   is whether the during-flood GETs still return 200, and whether their time
#   is within reach of the quiet ones -- a legitimate client must not see a
#   slower or less reliable handshake, which is the whole point of a cache over
#   a drop.
#
# BRIDGED, AND NOT SLIRP; A STATIC ADDRESS
#
#   -B ens18 puts the guest on the host's LAN with its own MAC, the only way
#   the peer can reach it.  amiberry-run.sh refuses the run if the backend
#   silently fell back to SLIRP.  The address is static and known before boot,
#   because the peer floods it and the client fetches from it.
#
# EXIT CODES
#
#   0  the defended guest kept answering under the flood (or, with -u, the
#      unprotected guest stopped answering, as expected)
#   1  the defended guest stopped answering (or, with -u, the unprotected one
#      did not, so the flood was not strong enough to prove anything)
#   2  the rig could not run: no peer, no capability tooling, no flood
#      delivered, or the quiet baseline never answered
#   3  the guest reached no verdict at all -- it never came up
#
# The a2065.device driver is not ours to ship: AMINETXDUO_A2065=<path>, or a
# copy in build/a2065.device.  The peer needs a python with cap_net_raw; see
# synflood.py.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
PEER_IF="${AMINETXDUO_PEER_IFACE:-ens18}"
PEERHOST="${AMINETXDUO_FITZ_PEER:-}"
UNPROTECTED=no

ADDRESS="${AMINETXDUO_SYNFLOOD_ADDRESS:-192.168.1.238}"
GATEWAY="${AMINETXDUO_SYNFLOOD_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0
PORT="${AMINETXDUO_SYNFLOOD_PORT:-8080}"

# The flood: how hard, and from where.  2000 SYNs a second is far past the 512
# entries the cache holds, so the cookie path is exercised, and well within
# what a2065 bridged carries.  The source range must be dead; see the header.
FLOOD_PPS="${AMINETXDUO_SYNFLOOD_PPS:-2000}"
FLOOD_NET="${AMINETXDUO_SYNFLOOD_NET:-10.99.0.0/16}"
FLOOD_SECS="${AMINETXDUO_SYNFLOOD_SECS:-40}"

# A python on the peer carrying cap_net_raw, the tc-cap / tcpdump-cap pattern.
PYCAP="${AMINETXDUO_PEER_PYCAP:-\$HOME/python3-cap}"

while getopts "P:B:a:g:b:N:m:t:p:T:uh" opt; do
    case "$opt" in
        P) PEERHOST="$OPTARG" ;;
        B) PEER_IF="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        T) AMINETXDUO_RUN_TAG="$OPTARG" ;;
        u) UNPROTECTED=yes ;;
        h) sed -n '3,7p' "$0"; exit 0 ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$PEERHOST" ] || {
    echo "-P is required: the peer must be a THIRD machine.  A frame the" >&2
    echo "emulator host sends to its own bridged guest never comes back to" >&2
    echo "that NIC's pcap (docs/RESEARCH.md 63)." >&2
    exit 2; }

case "$BUILD" in /*) ;; *) BUILD="$ROOT/${BUILD#./}" ;; esac
TOOLS="$BUILD/src/tools"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-synflood}"
TAG="$AMINETXDUO_RUN_TAG"
HD="$ROOT/build/amiberry-testhd-$TAG"
OUT="$ROOT/build/synflood-$TAG"

# --------------------------------------------------------------- preflight ---

for f in "$TOOLS/httpd" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

PEERNAME="${PEERHOST#*@}"
PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
[ -n "$PEERADDR" ] || case "$PEERNAME" in
    *[!0-9.]*) echo "cannot resolve $PEERNAME" >&2; exit 2 ;;
    *) PEERADDR="$PEERNAME" ;;
esac

peer_sh() { ssh -o ConnectTimeout=10 -n "$PEERHOST" "$@"; }

peer_sh "command -v python3 >/dev/null" || {
    echo "$PEERHOST has no python3, which is what runs the flood" >&2; exit 2; }
peer_sh "test -x $PYCAP" || {
    echo "$PEERHOST is missing $PYCAP.  The flooder needs a raw socket:" >&2
    echo "  cp \"\$(command -v python3)\" ~/python3-cap" >&2
    echo "  sudo setcap cap_net_raw+ep ~/python3-cap" >&2
    exit 2; }

# ----------------------------------------------------------------- staging ---

STAGE="$ROOT/build/synflood-stage-$TAG"
rm -rf "$STAGE" "$OUT"
mkdir -p "$STAGE/libs" "$STAGE/Public" "$OUT"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

cp "$BSD" "$STAGE/libs/bsdsocket.library"
echo "served by an Amiga under a SYN flood" > "$STAGE/Public/readme.txt"
echo "<html><body>ok</body></html>" > "$STAGE/Public/index.html"

# ---------------------------------------------------------------- peer prep ---

RTMP="/tmp/synflood-$TAG"
FLOOD_PID=""

flood_off() {
    peer_sh "pkill -f '[s]ynflood-$TAG' >/dev/null 2>&1; exit 0" || true
}

cleanup() {
    [ -n "$FLOOD_PID" ] && kill "$FLOOD_PID" 2>/dev/null || true
    flood_off
    peer_sh "rm -f $RTMP-*; exit 0" || true
}
trap cleanup EXIT INT TERM HUP

peer_sh "rm -f $RTMP-*; exit 0"
scp -q "$ROOT/tests/tools/synflood.py" "$PEERHOST:$RTMP-synflood-$TAG.py" || {
    echo "cannot copy the flooder to $PEERHOST" >&2; exit 2; }

# --------------------------------------------------------------------- boot ---

echo "==> booting $MODEL, $BOARD bridged on $PEER_IF, httpd static at $ADDRESS:$PORT"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$PEER_IF" -m "$MODEL" \
    -t "$TIMEOUT" -a "DH0:Public $PORT" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" &
RUNNER=$!
set -e

# --------------------------------------------------- wait for the quiet server ---

geturl() {
    curl -s -m 6 -o /dev/null -w '%{http_code}' \
        "http://$ADDRESS:$PORT/readme.txt" 2>/dev/null || echo 000
}

# A timed GET, in milliseconds, or `fail`.
timed_get() {
    local start end code
    start=$(date +%s%N)
    code=$(geturl)
    end=$(date +%s%N)
    if [ "$code" = 200 ]; then
        echo $(( (end - start) / 1000000 ))
    else
        echo fail
    fi
}

BASE_MS=none
ANSWERED=no
for _ in $(seq 1 $((TIMEOUT / 2))); do
    sleep 2
    kill -0 "$RUNNER" 2>/dev/null || break
    if [ "$(geturl)" = 200 ]; then
        ANSWERED=yes
        break
    fi
done

if [ "$ANSWERED" != yes ]; then
    echo "the guest never answered a quiet GET.  Nothing was measured: this" >&2
    echo "is a boot that did not come up, not a flood that took it down." >&2
    kill "$RUNNER" 2>/dev/null || true
    wait "$RUNNER" 2>/dev/null || true
    printf 'synflood: status=no_verdict quiet_ms=none flood_ms=none flood_sent=0 board=%s log=%s\n' \
           "$BOARD" "$OUT"
    exit 3
fi

# Three quiet handshakes, the baseline the flood is judged against.
QUIET_OK=0
QUIET_SUM=0
for _ in 1 2 3; do
    ms=$(timed_get)
    echo "    quiet GET: $ms" | tee -a "$OUT/quiet.txt"
    if [ "$ms" != fail ]; then
        QUIET_OK=$((QUIET_OK + 1))
        QUIET_SUM=$((QUIET_SUM + ms))
    fi
    sleep 1
done
[ "$QUIET_OK" -ge 2 ] || {
    echo "the quiet baseline was not reliable ($QUIET_OK/3).  A flood result" >&2
    echo "would mean nothing against it." >&2
    kill "$RUNNER" 2>/dev/null || true
    wait "$RUNNER" 2>/dev/null || true
    printf 'synflood: status=skip_baseline quiet_ms=none flood_ms=none flood_sent=0 board=%s log=%s\n' \
           "$BOARD" "$OUT"
    exit 2; }
BASE_MS=$((QUIET_SUM / QUIET_OK))

# ------------------------------------------------------------------- flood ---

echo "==> $PEERHOST flooding $ADDRESS:$PORT at ${FLOOD_PPS} pps from $FLOOD_NET for ${FLOOD_SECS}s"
ssh -o ConnectTimeout=10 -n "$PEERHOST" \
    "cp $RTMP-synflood-$TAG.py $RTMP-synflood-$TAG-run.py
     timeout $((FLOOD_SECS + 20)) $PYCAP $RTMP-synflood-$TAG-run.py \
        $ADDRESS --port $PORT --seconds $FLOOD_SECS --pps $FLOOD_PPS \
        --source-net $FLOOD_NET --report $RTMP-report.txt \
        > $RTMP-flood.out 2> $RTMP-flood.err; exit 0" &
FLOOD_PID=$!

# Let the flood get going before the client tries, so the measurement is taken
# with the cache under pressure and not before it.
sleep 5

FLOOD_OK=0
FLOOD_SUM=0
FLOOD_TRIES=0
for _ in 1 2 3; do
    ms=$(timed_get)
    echo "    flooded GET: $ms" | tee -a "$OUT/flood.txt"
    FLOOD_TRIES=$((FLOOD_TRIES + 1))
    if [ "$ms" != fail ]; then
        FLOOD_OK=$((FLOOD_OK + 1))
        FLOOD_SUM=$((FLOOD_SUM + ms))
    fi
    sleep 2
done

wait "$FLOOD_PID" 2>/dev/null || true
FLOOD_PID=""
scp -q "$PEERHOST:$RTMP-report.txt" "$OUT/flood-report.txt" 2>/dev/null || : > "$OUT/flood-report.txt"
scp -q "$PEERHOST:$RTMP-flood.err"  "$OUT/flood.err"        2>/dev/null || true

FLOOD_SENT=$(awk -F'sent=' '/sent=/{split($2,a," "); print a[1]; exit}' \
                 "$OUT/flood-report.txt" 2>/dev/null || echo 0)
FLOOD_SENT="${FLOOD_SENT:-0}"

FLOOD_MS=none
[ "$FLOOD_OK" -gt 0 ] && FLOOD_MS=$((FLOOD_SUM / FLOOD_OK))

kill "$RUNNER" 2>/dev/null || true
wait "$RUNNER" 2>/dev/null || true
cleanup
trap - EXIT INT TERM HUP

# ----------------------------------------------------------- what happened ---

echo
echo "===================== the guest under the flood ==================="
if [ -f "$HD/stdout.txt" ]; then
    tail -40 "$HD/stdout.txt"
else
    echo "(the guest wrote no stdout.txt)"
fi
echo "==================================================================="
echo

# The flood has to have actually delivered, or nothing was tested.  A few
# thousand is a flood; a handful is a broken raw socket on the peer.
if [ "$FLOOD_SENT" -lt 1000 ]; then
    STATUS=skip_flood
    RC=2
elif [ "$UNPROTECTED" = yes ]; then
    # The inverted arm: the point is that the OLD tree stops answering.  A run
    # that keeps answering means the flood was too weak to prove the defence
    # is what changed the outcome.
    if [ "$FLOOD_OK" -eq 0 ]; then
        STATUS=unprotected_fell_over
        RC=0
    else
        STATUS=unprotected_survived
        RC=1
    fi
else
    # The defended arm: every flooded GET has to return 200, and its time has
    # to be within reach of the quiet one -- a legitimate client must not see a
    # slower handshake under attack.  A generous ceiling, because the point is
    # "still works", not a throughput figure, but not so generous that a
    # near-timeout passes: four times the quiet time plus a fixed floor.
    CEIL=$(( BASE_MS * 4 + 1000 ))
    if [ "$FLOOD_OK" -eq "$FLOOD_TRIES" ] && [ "$FLOOD_MS" != none ] &&
       [ "$FLOOD_MS" -le "$CEIL" ]; then
        STATUS=answered_under_flood
        RC=0
    else
        STATUS=stopped_answering
        RC=1
    fi
fi

printf 'synflood: status=%s protected=%s quiet_ms=%s flood_ms=%s flood_ok=%s/%s flood_sent=%s pps=%s board=%s log=%s\n' \
       "$STATUS" "$([ "$UNPROTECTED" = yes ] && echo no || echo yes)" \
       "$BASE_MS" "$FLOOD_MS" "$FLOOD_OK" "$FLOOD_TRIES" \
       "$FLOOD_SENT" "$FLOOD_PPS" "$BOARD" "$OUT"

case "$STATUS" in
    skip_flood)
        echo "the flood delivered only $FLOOD_SENT SYNs.  Nothing was tested;" >&2
        echo "check that $PYCAP on $PEERHOST carries cap_net_raw." >&2 ;;
    stopped_answering)
        echo "the guest stopped answering under the flood: $FLOOD_OK of" >&2
        echo "$FLOOD_TRIES GETs completed, at $FLOOD_MS ms against a quiet" >&2
        echo "$BASE_MS ms.  This is the defect the SYN cache exists to fix." >&2 ;;
    unprotected_survived)
        echo "the unprotected tree kept answering ($FLOOD_OK/$FLOOD_TRIES)." >&2
        echo "The flood was not strong enough to prove the defence is what" >&2
        echo "changes the outcome.  Raise --pps or lengthen the run." >&2 ;;
esac

exit "$RC"
