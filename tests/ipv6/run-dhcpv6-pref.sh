#!/usr/bin/env bash
# TWO DHCPv6 SERVERS ON ONE LINK, AND THE CLIENT HAS TO PICK BY PREFERENCE.
#
# tests/ipv6/run-dhcpv6.sh could not ask this: dnsmasq cannot send
# OPTION_PREFERENCE, so a two-server link picked by arrival order and the run
# stopped rather than assert on a race.  tests/ipv6/dhcpv6-prefserver.py is a
# server that can.  It answers one Solicit with TWO Advertises -- different
# DUIDs, different addresses, preference 200 and preference 10 -- and this
# harness makes the guest solicit TWICE, with the order of the two Advertises
# reversed between them.  The client must name the preference-200 server both
# times.
#
# BRIDGED, or it measures nothing: the two servers are raw Ethernet frames from
# the peer, and SLIRP carries none of it.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
MODEL=A1200
CARD=a2065
TIMEOUT=300
SETTLE=40
MAC="${AMINETXDUO_DHCPV6_PREF_MAC:-02:41:4d:50:52:36}"
PEER="${AMINETXDUO_DHCPV6_PEER:-turo@playhouse4}"
PEER_NIC="${AMINETXDUO_DHCPV6_PEER_NIC:-ens18}"
HIGH_ADDR="${AMINETXDUO_DHCPV6_HIGH:-fd00:aa5:2::a}"
LOW_ADDR="${AMINETXDUO_DHCPV6_LOW:-fd00:aa5:2::b}"

while getopts "B:b:m:N:P:t:w:M:" opt; do
    case "$opt" in
        B) BACKEND="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        N) CARD="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        w) SETTLE="$OPTARG" ;;
        M) MAC="$OPTARG" ;;
        *) sed -n '2,15p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

fail_setup() { echo "result=badinvocation reason=$1"; exit 2; }
fail_link()  { echo "result=nolink reason=$1"; exit 4; }

case "$BACKEND" in
    slirp|slirp_inbound|none)
        echo "-B must name a host NIC: the two servers are raw frames." >&2
        fail_setup "slirp" ;;
esac

[ -n "$PEER" ] || fail_setup "no_peer"
[ -n "${AMINETXDUO_KICKSTART:-}" ] || fail_setup "no_kickstart"

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$BUILD/src/tools/AddNetInterface"
RMIF="$BUILD/src/tools/RemoveNetInterface"
SHOW="$BUILD/src/tools/ShowNetStatus"
ONLINE="$BUILD/src/tools/Online"
OFFLINE="$BUILD/src/tools/Offline"
SMOKE="$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$RMIF" "$SHOW" "$ONLINE" "$OFFLINE" "$SMOKE"; do
    [ -f "$f" ] || { echo "result=badinvocation reason=nobuild missing=$f"; exit 2; }
done

: "${AMINETXDUO_SANA2_VENDOR=1}"
export AMINETXDUO_SANA2_VENDOR
# shellcheck source=tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"
sana2_select "$CARD" "$BUILD"
DRIVER="$SANA2_SEL_DRIVER"
DEVICE="$SANA2_SEL_PATH"
ANXCARD="$SANA2_SEL_CARD"

if [ -z "$DEVICE" ] && [ "$DRIVER" = a2065.device ]; then
    DEVICE="${AMINETXDUO_A2065:-}"
    if [ -z "$DEVICE" ]; then
        for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
            [ -f "$c" ] && { DEVICE="$c"; break; }
        done
    fi
fi
[ -f "${DEVICE:-/nonexistent}" ] || fail_setup "no_driver_for_$CARD"

command -v tcpdump > /dev/null || fail_setup "no_tcpdump"

case "$CARD" in
    a2065) GUEST_MAC="00:80:10:$(printf '%s' "$MAC" |
                                 tr 'A-Z' 'a-z' | cut -d: -f4-6)" ;;
    *)     GUEST_MAC=$(printf '%s' "$MAC" | tr 'A-Z' 'a-z') ;;
esac

echo "card=$CARD driver=$DRIVER guest_mac=$GUEST_MAC backend=$BACKEND peer=$PEER"
echo "high_address=$HIGH_ADDR low_address=$LOW_ADDR"

ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" true 2>/dev/null ||
    fail_link "peer_unreachable"

# THE SEGMENT HAS TO HAVE IPv6 ON IT BEFORE ANY OF THIS MEANS ANYTHING.  The
# lab's upstream flaps, and an outage that started before the run reads as a
# client that chose wrongly.  Asserted, not noted.
PEER_GLOBAL=$(ssh -o BatchMode=yes "$PEER" \
    "ip -6 -o addr show dev ${PEER_NIC} scope global 2>/dev/null" |
    sed -n 's|.*inet6 \([0-9A-Fa-f:]*\)/.*|\1|p' | head -1)
echo "peer_global_ipv6=${PEER_GLOBAL:-none}"
[ -n "$PEER_GLOBAL" ] || fail_link "segment_has_no_global_ipv6"

# cap_net_raw, and no root: see the header of dhcpv6-prefserver.py.
PYCAP='$HOME/python3-cap'
# shellcheck disable=SC2016
ssh -o BatchMode=yes "$PEER" 'test -x "$HOME/python3-cap"' 2>/dev/null ||
    fail_link "peer_has_no_python3_cap"

RDIR="/tmp/anxd-dhcpv6-pref"
ssh -o BatchMode=yes "$PEER" "mkdir -p $RDIR" > /dev/null
scp -q -o BatchMode=yes "$ROOT/tests/ipv6/dhcpv6-prefserver.py" \
    "${PEER}:$RDIR/dhcpv6-prefserver.py" || fail_link "peer_copy_failed"

TAG="dhcpv6pref"
SRVLOG="$ROOT/build/$TAG-server.log"
CAP="$ROOT/build/$TAG-wire.txt"
SERIAL="$ROOT/build/amiberry-serial-$TAG.log"
OUT="$ROOT/build/$TAG.out"
rm -f "$SRVLOG" "$CAP" "$CAP.err" "$SERIAL" "$OUT"

# shellcheck disable=SC2029
ssh -o BatchMode=yes -o ServerAliveInterval=15 "$PEER" \
    "$PYCAP $RDIR/dhcpv6-prefserver.py --iface $PEER_NIC --order alternate \
        --high-address $HIGH_ADDR --low-address $LOW_ADDR \
        --client-mac $GUEST_MAC --seconds $((TIMEOUT + 30))" \
    > "$SRVLOG" 2>&1 &
SRV_PID=$!

cleanup() {
    kill "$SRV_PID" 2>/dev/null || true
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
        "pkill -f dhcpv6-prefserver.py" > /dev/null 2>&1 || true
    [ -z "${CAP_PID:-}" ] || kill "$CAP_PID" 2>/dev/null || true
}
trap cleanup EXIT

srv_up=no
for _ in $(seq 1 60); do
    grep -q '^server_ready=yes' "$SRVLOG" 2>/dev/null && { srv_up=yes; break; }
    grep -q '^result=badinvocation' "$SRVLOG" 2>/dev/null && break
    kill -0 "$SRV_PID" 2>/dev/null || break
    sleep 0.5
done

echo "srv_up=$srv_up srvlog=$SRVLOG"
if [ "$srv_up" != yes ]; then
    sed -n '1,8p' "$SRVLOG" >&2
    fail_link "peer_server_did_not_start"
fi

sed -n 's/^\(high_duid\|low_duid\|high_linklocal\|low_linklocal\|allmulti\)=/&/p' \
    "$SRVLOG" | head -8

tcpdump -i "$BACKEND" -n -s0 -l \
        "udp port 546 or udp port 547" > "$CAP" 2>"$CAP.err" &
CAP_PID=$!

for _ in $(seq 1 50); do
    grep -q "listening on" "$CAP.err" 2>/dev/null && break
    sleep 0.2
done
grep -q "listening on" "$CAP.err" 2>/dev/null || {
    sed -n '1,3p' "$CAP.err" >&2
    fail_setup "nocapture"
}

STAGE="$ROOT/build/$TAG-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"     "$STAGE/libs/bsdsocket.library"
cp "$ADDIF"   "$STAGE/AddNetInterface"
cp "$RMIF"    "$STAGE/RemoveNetInterface"
cp "$SHOW"    "$STAGE/ShowNetStatus"
cp "$ONLINE"  "$STAGE/Online"
cp "$OFFLINE" "$STAGE/Offline"

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$DRIVER
UNIT=0
CONFIGURE=NONE
CONFIGURE6=DHCP
EOF

AMINETXDUO_SANA2_DRIVER="$DEVICE" \
AMINETXDUO_SANA2_DRIVER_NAME="$DRIVER" \
AMINETXDUO_SANA2_DEVICE="$DRIVER" \
AMINETXDUO_SANA2_CARD="$ANXCARD" \
    sana2_stage "$CARD" "$STAGE/devs"

# TWO SOLICITS, one per Advertise order.  Offline/Online is what run-dhcpv6.sh
# already uses to make the client acquire again, and it is a shorter round than
# a RemoveNetInterface.
{
    echo "SYS:AddNetInterface DEVS:NetInterfaces/eth0"
    echo "wait $SETTLE"
    echo "SYS:ShowNetStatus INTERFACE eth0"
    echo "SYS:Offline eth0"
    echo "wait 5"
    echo "SYS:Online eth0"
    echo "wait $SETTLE"
    echo "SYS:ShowNetStatus INTERFACE eth0"
    echo "SYS:RemoveNetInterface eth0"
    echo "wait 5"
} > "$STAGE/commands.txt"

set +e
AMINETXDUO_RUN_TAG="$TAG" AMINETXDUO_AMIBERRY_MAC="$MAC" \
    "$ROOT/tools/amiberry-run.sh" \
    -N "$CARD" -B "$BACKEND" -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/RemoveNetInterface" "$STAGE/ShowNetStatus" "$STAGE/Online" \
    "$STAGE/Offline" "$STAGE/commands.txt" \
    > "$OUT" 2>&1
run_rc=$?
set -e

sleep 2
kill "$CAP_PID" 2>/dev/null || true
wait "$CAP_PID" 2>/dev/null || true
CAP_PID=

kill "$SRV_PID" 2>/dev/null || true
ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
    "pkill -f dhcpv6-prefserver.py" > /dev/null 2>&1 || true

echo "run_rc=$run_rc capture=$CAP serial=$SERIAL out=$OUT"

echo
echo "===================== what the peer answered ====================="
cat "$SRVLOG"
echo "=================================================================="
echo

solicits=$(grep -c '^solicit_seen=' "$SRVLOG" || true)
order1=$(sed -n 's/^advertise_order=//p' "$SRVLOG" | sed -n 1p)
order2=$(sed -n 's/^advertise_order=//p' "$SRVLOG" | sed -n 2p)
picks=$(sed -n 's/^request_picked=//p' "$SRVLOG" | tr '\n' ',' | sed 's/,$//')
pick1=$(sed -n 's/^request_picked=//p' "$SRVLOG" | sed -n 1p)
pick2=$(sed -n 's/^request_picked=//p' "$SRVLOG" | sed -n 2p)
replies=$(sed -n 's/^reply_sent=//p' "$SRVLOG" | tr '\n' ',' | sed 's/,$//')

echo "solicits=$solicits"
echo "advertise_order_1=${order1:-none} advertise_order_2=${order2:-none}"
echo "request_picked=${picks:-none}"
echo "reply_sent=${replies:-none}"

guest_high=no
guest_low=no
sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null |
    grep -qF "$HIGH_ADDR" && guest_high=yes
sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null |
    grep -qF "$LOW_ADDR" && guest_low=yes

high_reports=$(sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null |
    grep -cF "$HIGH_ADDR" || true)

echo "guest_took_high=$guest_high guest_took_low=$guest_low"
echo "guest_high_reports=$high_reports"

# WHO ELSE ANSWERED.  This link carries the site router's DHCPv6 server as
# well, which is the whole reason tests/ipv6/run-dhcpv6.sh had to stop rather
# than assert; naming the advertisers is what turns "the right server won" into
# a statement about a contested link.
from_port_547() {
    sed -n "s/^[0-9:.]* IP6 \([0-9A-Fa-f:]*\)\.547 > .*dhcp6 $1.*/\1/p" \
        "$CAP" 2>/dev/null
}
HIGH_LL=$(sed -n 's/^high_linklocal=//p' "$SRVLOG" | head -1)
advertiser_list=$(from_port_547 advertise | sort -u | tr '\n' ',' | sed 's/,$//')
advertisers=$(printf '%s\n' "$advertiser_list" | tr ',' '\n' | grep -c . || true)
reply_sources=$(from_port_547 reply | sort -u | tr '\n' ',' | sed 's/,$//')

echo "advertisers=$advertisers advertiser_list=${advertiser_list:-none}"
echo "high_linklocal=${HIGH_LL:-none} reply_sources=${reply_sources:-none}"

fail=0
note() { echo "FAIL $1"; fail=1; }

[ "$solicits" -ge 2 ] ||
    note "solicits: the guest soliciting twice is what makes this two arms, \
saw $solicits"
[ -n "$order1" ] && [ -n "$order2" ] && [ "$order1" != "$order2" ] ||
    note "advertise_order: the two arms were not run both ways round"
[ "$pick1" = high ] ||
    note "request_picked_1: arm 1 (order $order1) named ${pick1:-nothing}"
[ "$pick2" = high ] ||
    note "request_picked_2: arm 2 (order $order2) named ${pick2:-nothing}"
[ "$guest_high" = yes ] ||
    note "guest_took_high: no $HIGH_ADDR on the interface"
[ "$guest_low" = no ] ||
    note "guest_took_low: $LOW_ADDR is on the interface, so the \
preference-10 server was used"
[ "$advertisers" -ge 2 ] ||
    note "advertisers: only $advertisers server answered, so nothing was \
chosen between"
printf '%s\n' "$reply_sources" | tr ',' '\n' | grep -qx "${HIGH_LL:-none}" ||
    note "reply_sources: no Reply came from the preference-200 server"
[ "$high_reports" -ge 2 ] ||
    note "guest_high_reports: $HIGH_ADDR was reported $high_reports times, \
one per arm is what is expected"

if [ "$fail" -ne 0 ]; then
    echo "result=fail"
    exit 1
fi

echo "result=pass"
exit 0
