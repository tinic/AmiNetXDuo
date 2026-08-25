#!/usr/bin/env bash
#
# ONE LOST ARP MUST NOT FAIL A TCP ARM.
#
#   tests/tools/run-arpretry.sh -P PEERHOST [-B IFACE] [-a ADDR] [-b BUILDDIR]
#                               [-N BOARD] [-m MODEL] [-t SECONDS] [-T TAG]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
PEER_IF="${AMINETXDUO_PEER_IFACE:-ens18}"
PEERHOST="${AMINETXDUO_FITZ_PEER:-}"

ADDRESS="${AMINETXDUO_ARPRETRY_ADDRESS:-192.168.1.239}"
GATEWAY="${AMINETXDUO_ARPRETRY_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0
PORT_TCP="${AMINETXDUO_ARPRETRY_PORT:-7431}"

# The sweep's arm, unchanged: 3 s of transfer, so 5 s of connect.
SECS=3

TC="${AMINETXDUO_PEER_TC:-\$HOME/tc-cap}"
TCPDUMP="${AMINETXDUO_PEER_TCPDUMP:-\$HOME/tcpdump-cap}"

HOLD="${AMINETXDUO_ARPRETRY_HOLD:-0.3}"

while getopts "P:B:a:g:b:N:m:t:T:h" opt; do
    case "$opt" in
        P) PEERHOST="$OPTARG" ;;
        B) PEER_IF="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) AMINETXDUO_RUN_TAG="$OPTARG" ;;
        h) sed -n '3,6p' "$0"; exit 0 ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
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

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-arpretry}"
TAG="$AMINETXDUO_RUN_TAG"
HD="$ROOT/build/amiberry-testhd-$TAG"
REPORT="$HD/tools.txt"
OUT="$ROOT/build/arpretry-$TAG"

# --------------------------------------------------------------- preflight ---

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/iperf" "$BSD"; do
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
    echo "$PEERHOST has no python3, which is what runs the peer" >&2; exit 2; }
peer_sh "test -x $TC && test -x $TCPDUMP" || {
    echo "$PEERHOST is missing $TC or $TCPDUMP.  Both need capabilities:" >&2
    echo "  cp /usr/sbin/tc ~/tc-cap && sudo setcap cap_net_admin+ep ~/tc-cap" >&2
    echo "  cp /usr/bin/tcpdump ~/tcpdump-cap && sudo setcap cap_net_admin,cap_net_raw+ep ~/tcpdump-cap" >&2
    exit 2; }

peer_sh "$TC qdisc show dev $PEER_IF" | grep -q '^qdisc prio 1: root' && {
    echo "$PEERHOST:$PEER_IF already has a prio root qdisc.  Something else" >&2
    echo "is shaping this interface -- lossgate, or another arpretry run." >&2
    exit 2; }

ip_hex() { local IFS=.; set -- $1; printf '0x%02x%02x%02x%02x' "$1" "$2" "$3" "$4"; }
ip_hi()  { local IFS=.; set -- $1; printf '0x%02x%02x' "$1" "$2"; }
ip_lo()  { local IFS=.; set -- $1; printf '0x%02x%02x' "$3" "$4"; }
GHEX=$(ip_hex "$ADDRESS")
PHI=$(ip_hi "$PEERADDR"); PLO=$(ip_lo "$PEERADDR")

# ----------------------------------------------------------------- staging ---

STAGE="$ROOT/build/arpretry-stage-$TAG"
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

cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/iperf"           "$STAGE/iperf"

TCPCMD="SYS:iperf $PEERADDR -p $PORT_TCP -t $SECS"
{
    echo "SYS:AddNetInterface eth0"
    echo "$TCPCMD"
} > "$STAGE/commands.txt"

# -------------------------------------------------------------- peer state ---

RTMP="/tmp/arpretry-$TAG"
PEER_PID=""
WATCH_PID=""
CAP_PID=""

drop_off() { peer_sh "$TC qdisc del dev $PEER_IF root >/dev/null 2>&1; exit 0" || true; }

cleanup() {
    local p
    for p in $PEER_PID $WATCH_PID $CAP_PID; do kill "$p" 2>/dev/null || true; done
    drop_off
    peer_sh "pkill -f '[i]perfpeer-$TAG' >/dev/null 2>&1; exit 0" || true
}
trap cleanup EXIT INT TERM HUP

peer_sh "rm -f $RTMP-*; exit 0"

scp -q "$ROOT/tests/tools/iperfpeer.py" "$PEERHOST:$RTMP-iperfpeer-$TAG.py" || {
    echo "cannot copy the peer to $PEERHOST" >&2; exit 2; }

PEER_LIFE=$((TIMEOUT + 120))
ssh -o ConnectTimeout=10 -n "$PEERHOST" \
    "timeout $((PEER_LIFE + 30)) python3 $RTMP-iperfpeer-$TAG.py serve tcp \
     --port $PORT_TCP --seconds $PEER_LIFE --idle 8" \
    > "$OUT/peer.out" 2> "$OUT/peer.err" &
PEER_PID=$!

ssh -o ConnectTimeout=10 -n "$PEERHOST" \
    "timeout $((TIMEOUT + 60)) $TCPDUMP -i $PEER_IF -n -tt -l arp \
     > $RTMP-arp.txt 2> $RTMP-arp.err; exit 0" &
CAP_PID=$!

sleep 2
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "the iperf peer died before the run started:" >&2
    cat "$OUT/peer.err" >&2; exit 2; }

# --------------------------------------------------------------- the drop ---

echo "==> $PEERHOST:$PEER_IF: dropping ARP to $ADDRESS and ARP requests from $PEERADDR"
peer_sh "$TC qdisc add dev $PEER_IF root handle 1: prio bands 3 && \
         $TC qdisc add dev $PEER_IF parent 1:3 handle 30: netem loss 100% && \
         $TC filter add dev $PEER_IF protocol arp parent 1: prio 1 u32 \
             match u32 $GHEX 0xffffffff at 24 flowid 1:3 && \
         $TC filter add dev $PEER_IF protocol arp parent 1: prio 2 u32 \
             match u16 0x0001 0xffff at 6 \
             match u16 $PHI 0xffff at 14 match u16 $PLO 0xffff at 16 flowid 1:3" || {
    echo "could not install the drop on $PEERHOST" >&2; exit 2; }

ssh -o ConnectTimeout=10 -n "$PEERHOST" \
    "timeout $((TIMEOUT + 30)) $TCPDUMP -i $PEER_IF -n -c 1 \
        'arp[6:2] = 1 and src host $ADDRESS and dst host $PEERADDR' \
        > $RTMP-trigger.txt 2>&1
     sleep $HOLD
     $TC -s qdisc show dev $PEER_IF > $RTMP-tcstat.txt 2>&1
     $TC qdisc del dev $PEER_IF root >/dev/null 2>&1
     exit 0" &
WATCH_PID=$!

# --------------------------------------------------------------------- run ---

echo "==> booting $MODEL, $BOARD bridged on $PEER_IF, guest static at $ADDRESS"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$PEER_IF" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
    "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/iperf"
RUN_RC=$?
set -e

sleep 2
drop_off
kill "$CAP_PID" 2>/dev/null || true
peer_sh "sleep 1; exit 0" || true
scp -q "$PEERHOST:$RTMP-arp.txt"    "$OUT/arp.txt"    2>/dev/null || : > "$OUT/arp.txt"
scp -q "$PEERHOST:$RTMP-tcstat.txt" "$OUT/tcstat.txt" 2>/dev/null || : > "$OUT/tcstat.txt"
cleanup
trap - EXIT INT TERM HUP

# ----------------------------------------------------------- what happened ---

STATUS=fail
CONNECT_RC=none
ARP_REQS=0
FIRST_RETRY_MS=none
DROPPED=0

if [ -f "$REPORT" ]; then
    echo
    echo "===================== what the guest printed ======================"
    cat "$REPORT"
    echo "==================================================================="
    CONNECT_RC=$(awk -v banner="$TCPCMD" '
        index($0, "===== " banner " =====") == 1 { on = 1; next }
        on && /^----- rc / { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
    ' "$REPORT")
    CONNECT_RC="${CONNECT_RC:-none}"
fi

read -r ARP_REQS FIRST_RETRY_MS <<EOF
$(awk -v peer="$PEERADDR" -v guest="$ADDRESS" '
    $0 ~ ("Request who-has " peer " tell " guest) {
        n++
        if (n == 1) t1 = $1
        else if (n == 2) d = ($1 - t1) * 1000
    }
    END { printf "%d %s\n", n, (n >= 2) ? sprintf("%.0f", d) : "none" }
' "$OUT/arp.txt" 2>/dev/null || echo "0 none")
EOF

DROPPED=$(awk '/qdisc netem/ { on = 1; next }
               on && /dropped [0-9]+/ { sub(/.*dropped /, ""); sub(/,.*/, ""); print; exit }
              ' "$OUT/tcstat.txt" 2>/dev/null || echo 0)
DROPPED="${DROPPED:-0}"

echo
echo "========================= the ARP on the wire ====================="
grep -E "Request who-has $PEERADDR tell $ADDRESS|Reply $PEERADDR is-at" \
     "$OUT/arp.txt" 2>/dev/null | head -20 || echo "(nothing captured)"
echo "==================================================================="
echo

if [ ! -f "$REPORT" ]; then
    STATUS=no_verdict
    RC=3
elif [ "$ARP_REQS" = 0 ]; then
    STATUS=skip_setup
    RC=2
elif [ "$DROPPED" = 0 ]; then
    STATUS=skip_setup
    RC=2
elif [ "$CONNECT_RC" = 0 ] && [ "$ARP_REQS" -ge 2 ] &&
     [ "$FIRST_RETRY_MS" != none ] && [ "$FIRST_RETRY_MS" -le 5000 ]; then
    STATUS=pass
    RC=0
else
    STATUS=fail
    RC=1
fi

printf 'arpretry: status=%s connect_rc=%s arp_reqs=%s first_retry_ms=%s netem_dropped=%s run_rc=%s board=%s log=%s\n' \
       "$STATUS" "$CONNECT_RC" "$ARP_REQS" "$FIRST_RETRY_MS" "$DROPPED" \
       "$RUN_RC" "$BOARD" "$OUT"

case "$STATUS" in
    no_verdict)
        echo "the guest wrote no $REPORT (run rc=$RUN_RC).  Nothing was" >&2
        echo "measured: this is a boot that did not happen, not a lost ARP" >&2
        echo "that was not recovered from." >&2 ;;
    skip_setup)
        echo "the drill measured nothing.  arp_reqs=0 means the guest never" >&2
        echo "asked for $PEERADDR -- it had the mapping already, so no" >&2
        echo "retransmit was tested.  netem_dropped=0 means the reply was" >&2
        echo "never lost, so there was nothing to retransmit for." >&2 ;;
    fail)
        echo "the first ARP was lost and the connect did not recover inside" >&2
        echo "the ${SECS}s+2s the application waits.  arp_reqs=$ARP_REQS," >&2
        echo "first_retry_ms=$FIRST_RETRY_MS." >&2 ;;
esac

exit "$RC"
