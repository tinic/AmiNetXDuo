#!/usr/bin/env bash
#
# THE FIRST RPC EXCHANGE, ON A REAL WIRE.
#
#   tests/tools/run-rpcprobe.sh -P peer [-B iface] [-b build] [-N board]
#                               [-m model] [-t seconds] [-a addr] [-g gw]
#                               [-p port]
#
# bifat reported against 0.26.5 that ch_nfsmount fails with `RPC: Port mapper
# failure - Unable to receive` on this stack while the same setup works on
# AmiTCP4, AmiTCP_NG and Roadshow.  Nothing in this tree spoke RPC, portmap or
# NFS, so nothing could have caught it and nothing could reproduce it.
#
# This runs RpcProbe (tests/tools/rpcprobe.c) against tests/tools/rpcpeer.py.
# NO rpcbind AND NO NFS SERVER ARE INSTALLED ANYWHERE: the question is whether
# a portmap reply reaches a bound UDP socket on the guest, and a 90-line
# responder answers that without touching the peer's configuration.
#
# FOUR ARMS, AND THE CONTROL IS THE POINT.  `ephem` binds port 0 the way the
# resolver does; `resv` walks down from 1023 the way bindresvport() does for
# RPC; `conn` does that and then connect()s, which is the other shape an RPC
# client takes; `sig` repeats that with a SIGNAL MASK handed to WaitSelect(),
# which is what AmiTCP's net.lib does and what a probe written from the BSD
# side never passes.  Each arm exchanges TWICE on one socket, because an RPC
# client retries on the socket it already has.  All four passing is a real
# answer -- it says the mount is not failing here.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

PEERHOST="${AMINETXDUO_FITZ_PEER:-}"
PEER_IF="${AMINETXDUO_RPCPROBE_IFACE:-ens18}"
BUILD="build/cm"
BOARD="a2065"
MODEL="A1200"
TIMEOUT=180
ADDRESS="${AMINETXDUO_RPCPROBE_ADDRESS:-192.168.1.239}"
GATEWAY="${AMINETXDUO_RPCPROBE_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0
PORT="${AMINETXDUO_RPCPROBE_PORT:-11111}"

while getopts "P:B:b:N:m:t:a:g:p:h" opt; do
    case "$opt" in
        P) PEERHOST="$OPTARG" ;;
        B) PEER_IF="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        h) sed -n '3,8p' "$0"; exit 0 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$PEERHOST" ] || {
    echo "-P is required: the responder must run on a THIRD machine, not on" >&2
    echo "the emulator host (docs/RESEARCH.md 63)." >&2
    exit 2; }

case "$BUILD" in /*) ;; *) BUILD="$ROOT/${BUILD#./}" ;; esac
TOOLS="$BUILD/src/tools"
PROBE="$BUILD/tests/tools/RpcProbe"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-rpcprobe}"
TAG="$AMINETXDUO_RUN_TAG"
HD="$ROOT/build/amiberry-testhd-$TAG"
REPORT="$HD/tools.txt"
OUT="$ROOT/build/rpcprobe-$TAG"

# --------------------------------------------------------------- preflight ---

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD"; do
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

peer_sh() { ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" "$@"; }

peer_sh "command -v python3 >/dev/null" || {
    echo "$PEERHOST has no python3, which is what answers the call" >&2
    exit 2; }

# THE ADDRESS HAS TO BE FREE.  Same lesson as run-cardsweep.sh: an occupied
# address loses duplicate-address detection and the arm times out saying
# nothing useful.  One of OUR guests (02:41:4d:49:*) is never a conflict.
if command -v ip > /dev/null 2>&1; then
    ip neigh del "$ADDRESS" dev "$PEER_IF" > /dev/null 2>&1 || true
    ping -c2 -W2 "$ADDRESS" > /dev/null 2>&1 || true
    sleep 2
    conflict=$(ip neigh show "$ADDRESS" 2>/dev/null \
               | grep -vi "02:41:4d:49" \
               | grep -oE "lladdr [0-9a-f:]+" | head -1 || true)
    [ -z "$conflict" ] || {
        echo "$ADDRESS is taken ($conflict); pass -a with a free one" >&2
        exit 2; }
fi

# ----------------------------------------------------------------- staging ---

STAGE="$ROOT/build/rpcprobe-stage-$TAG"
rm -rf "$STAGE" "$OUT"
mkdir -p "$STAGE/libs" "$OUT"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

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

cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/RpcProbe"

PROBECMD="SYS:RpcProbe $PEERADDR $PORT"
{
    echo "SYS:AddNetInterface eth0"
    echo "$PROBECMD"
} > "$STAGE/commands.txt"

# -------------------------------------------------------------- the peer -----

RTMP="/tmp/rpcpeer-$TAG"
PEER_PID=""

cleanup() {
    [ -z "$PEER_PID" ] || kill "$PEER_PID" 2>/dev/null || true
    peer_sh "pkill -f '[r]pcpeer-$TAG' >/dev/null 2>&1; exit 0" || true
}
trap cleanup EXIT INT TERM HUP

peer_sh "rm -f $RTMP-*; exit 0"
scp -q "$ROOT/tests/tools/rpcpeer.py" "$PEERHOST:$RTMP.py" || {
    echo "cannot copy the responder to $PEERHOST" >&2; exit 2; }

PEER_LIFE=$((TIMEOUT + 60))
ssh -o BatchMode=yes -o ConnectTimeout=10 -n "$PEERHOST" \
    "timeout $((PEER_LIFE + 30)) python3 $RTMP.py --port $PORT \
     --seconds $PEER_LIFE" > "$OUT/peer.out" 2> "$OUT/peer.err" &
PEER_PID=$!

sleep 2
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "the responder died before the run started:" >&2
    cat "$OUT/peer.err" >&2; exit 2; }

# --------------------------------------------------------------------- run ---

echo "==> booting $MODEL, $BOARD bridged on $PEER_IF, guest static at $ADDRESS"
echo "==> responder on $PEERHOST udp/$PORT, guest asks it for NFS v3"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$PEER_IF" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
    "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/RpcProbe"
RUN_RC=$?
set -e

sleep 2
cleanup
trap - EXIT INT TERM HUP

# ----------------------------------------------------------- what happened ---

STATUS=fail
IFACE_RC=none
EPHEM=none
RESV=none
CONN=none
SIG=none
VERDICT=none

if [ -f "$REPORT" ]; then
    cp "$REPORT" "$OUT/tools.txt"
    echo
    echo "===================== what the guest printed ======================"
    cat "$REPORT"
    echo "==================================================================="

    IFACE_RC=$(awk '
        index($0, "===== SYS:AddNetInterface eth0 =====") == 1 { on = 1; next }
        on && /^----- rc / { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
    ' "$REPORT")
    IFACE_RC="${IFACE_RC:-none}"

    EPHEM=$(awk -F= '/^ephem_RESULT=/ { print $2; exit }' "$REPORT")
    RESV=$(awk  -F= '/^resv_RESULT=/  { print $2; exit }' "$REPORT")
    CONN=$(awk  -F= '/^conn_RESULT=/  { print $2; exit }' "$REPORT")
    SIG=$(awk   -F= '/^sig_RESULT=/   { print $2; exit }' "$REPORT")
    VERDICT=$(awk -F= '/^verdict=/ { sub(/^verdict=/, ""); print; exit }' \
              "$REPORT")
fi

# `|| true`, not `|| echo 0`: grep -c PRINTS 0 and EXITS 1 on no match, so
# `|| echo 0` makes the variable the two-line string "0\n0" and the printf
# below breaks across lines.  tools/emurun.sh:85 and
# install/test/run-workbench.sh:729 already say this.
PEER_CALLS=$(grep -c '^peer_call ' "$OUT/peer.out" 2>/dev/null || true)
PEER_REPLIES=$(grep -c '^peer_reply ' "$OUT/peer.out" 2>/dev/null || true)
PEER_CALLS=${PEER_CALLS:-0}
PEER_REPLIES=${PEER_REPLIES:-0}

# THE RESPONDER'S OWN COUNT IS WHAT SEPARATES "we never sent" FROM "the reply
# never came back".  Without it a failing arm cannot tell those apart, and they
# are different defects in different halves of the stack.
[ "${EPHEM:-none}" = PASS ] && [ "${RESV:-none}" = PASS ] &&
    [ "${CONN:-none}" = PASS ] && [ "${SIG:-none}" = PASS ] && STATUS=pass

printf 'rpcprobe: status=%s run_rc=%s iface_rc=%s ephem=%s resv=%s conn=%s sig=%s peer_calls=%s peer_replies=%s addr=%s port=%s out=%s\n' \
       "$STATUS" "$RUN_RC" "$IFACE_RC" "${EPHEM:-none}" "${RESV:-none}" \
       "${CONN:-none}" "${SIG:-none}" "$PEER_CALLS" "$PEER_REPLIES" \
       "$ADDRESS" "$PORT" "$OUT"
[ -n "$VERDICT" ] && [ "$VERDICT" != none ] && printf 'rpcprobe: %s\n' "$VERDICT"

[ "$STATUS" = pass ] || exit 1
exit 0
