#!/usr/bin/env bash
# DOES AN IPv6-ONLY MACHINE GET ON THE NETWORK THROUGH DHCPv6.
# BRIDGED, OR IT MEASURES NOTHING
#   SLIRP answers nothing on UDP 547 and rewrites source ports, so -B must
#   name a host NIC.  The guest gets its own MAC, distinct from every other
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/v6dhcp}"
MODEL=A1200
CARD=a2065
TIMEOUT=240
SETTLE=40
MAC="${AMINETXDUO_DHCPV6_MAC:-02:41:4d:49:44:36}"
PEER="${AMINETXDUO_DHCPV6_PEER:-turo@playhouse4}"
LEASE=600
ARM=dhcp
RENEW=no

ULA_PREFIX="${AMINETXDUO_DHCPV6_PREFIX:-fd00:aa5:1:}"
PEER_NIC="${AMINETXDUO_DHCPV6_PEER_NIC:-ens18}"
PEER_ULA="${AMINETXDUO_DHCPV6_PEER_ULA:-fd00:aa5:1::1}"

while getopts "B:b:m:N:P:t:w:M:l:a:R" opt; do
    case "$opt" in
        B) BACKEND="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        N) CARD="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        w) SETTLE="$OPTARG" ;;
        M) MAC="$OPTARG" ;;
        l) LEASE="$OPTARG" ;;
        a) ARM="$OPTARG" ;;
        R) RENEW=yes ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

fail_setup() { echo "result=badinvocation reason=$1"; exit 2; }
fail_link()  { echo "result=nolink reason=$1"; exit 4; }

case "$ARM" in
    auto|dhcp) ;;
    *) fail_setup "arm_must_be_auto_or_dhcp" ;;
esac

case "$BACKEND" in
    slirp|slirp_inbound)
        echo "-B must name a host NIC: SLIRP answers nothing on UDP 547." >&2
        fail_setup "slirp" ;;
esac

[ -n "$PEER" ] || fail_setup "no_peer"

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$BUILD/src/tools/AddNetInterface"
RMIF="$BUILD/src/tools/RemoveNetInterface"
SHOW="$BUILD/src/tools/ShowNetStatus"
PING="$BUILD/src/tools/ping"
ONLINE="$BUILD/src/tools/Online"
OFFLINE="$BUILD/src/tools/Offline"
SMOKE="$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$RMIF" "$SHOW" "$PING" "$ONLINE" "$OFFLINE" \
         "$SMOKE"; do
    [ -f "$f" ] || { echo "result=badinvocation reason=nobuild missing=$f"; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || fail_setup "no_kickstart"

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

echo "arm=$ARM card=$CARD driver=$DRIVER guest_mac=$GUEST_MAC backend=$BACKEND peer=$PEER lease=$LEASE"

ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" true 2>/dev/null ||
    fail_link "peer_unreachable"

PEER_SERVER='$HOME/anxd-dhcpv6-server.sh'

# shellcheck disable=SC2016
ssh -o BatchMode=yes "$PEER" 'test -x "$HOME/anxd-dhcpv6-server.sh"' 2>/dev/null ||
    fail_link "peer_has_no_server"

PEER_MAC=$(ssh -o BatchMode=yes "$PEER" \
    "cat /sys/class/net/${PEER_NIC}/address" 2>/dev/null | tr -d '\r' |
    tr 'A-Z' 'a-z')
[ -n "$PEER_MAC" ] || fail_link "peer_mac_unreadable"

SRVLOG="$ROOT/build/dhcpv6-server.log"
rm -f "$SRVLOG"

ssh -o BatchMode=yes -o ServerAliveInterval=15 "$PEER" \
    "$PEER_SERVER --lease $LEASE" > "$SRVLOG" 2>&1 &
SRV_PID=$!

cleanup() {
    kill "$SRV_PID" 2>/dev/null || true
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
        'pkill -x dnsmasq-cap' > /dev/null 2>&1 || true
    [ -z "${CAP_PID:-}" ] || kill "$CAP_PID" 2>/dev/null || true
}
trap cleanup EXIT

srv_up=no
srv_fatal=
for _ in $(seq 1 40); do
    if grep -q '^FATAL:' "$SRVLOG" 2>/dev/null; then
        srv_fatal=$(sed -n 's/^FATAL: *//p' "$SRVLOG" | head -1)
        break
    fi
    if grep -q 'dnsmasq-dhcp.*DHCPv6, IP range' "$SRVLOG" 2>/dev/null; then
        srv_up=yes
        break
    fi
    kill -0 "$SRV_PID" 2>/dev/null || break
    sleep 0.5
done

echo "srv_up=$srv_up srvlog=$SRVLOG"
if [ "$srv_up" != yes ]; then
    [ -z "$srv_fatal" ] || echo "srv_fatal=$srv_fatal"
    sed -n '1,6p' "$SRVLOG" >&2
    fail_link "peer_server_did_not_start"
fi

TAG="dhcpv6-$ARM"
CAP="$ROOT/build/$TAG-wire.txt"
SERIAL="$ROOT/build/amiberry-serial-$TAG.log"
OUT="$ROOT/build/$TAG.out"
rm -f "$CAP" "$CAP.err" "$SERIAL" "$OUT"

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
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$ADDIF"  "$STAGE/AddNetInterface"
cp "$RMIF"   "$STAGE/RemoveNetInterface"
cp "$SHOW"   "$STAGE/ShowNetStatus"
cp "$PING"   "$STAGE/ping"
cp "$ONLINE" "$STAGE/Online"
cp "$OFFLINE" "$STAGE/Offline"

case "$ARM" in
    dhcp) CONF6=DHCP ;;
    auto) CONF6=AUTO ;;
esac

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$DRIVER
UNIT=0
CONFIGURE=NONE
CONFIGURE6=$CONF6
EOF

AMINETXDUO_SANA2_DRIVER="$DEVICE" \
AMINETXDUO_SANA2_DRIVER_NAME="$DRIVER" \
AMINETXDUO_SANA2_DEVICE="$DRIVER" \
AMINETXDUO_SANA2_CARD="$ANXCARD" \
    sana2_stage "$CARD" "$STAGE/devs"

{
    echo "SYS:AddNetInterface DEVS:NetInterfaces/eth0"
    echo "wait $SETTLE"
    echo "SYS:ShowNetStatus INTERFACE eth0"
    echo "SYS:ping -c 3 $PEER_ULA"
    if [ "$RENEW" = yes ]; then
        echo "wait $(( LEASE / 2 + 20 ))"
        echo "SYS:ShowNetStatus INTERFACE eth0"
    fi
    echo "SYS:Offline eth0"
    echo "wait 5"
    echo "SYS:Online eth0"
    echo "wait $SETTLE"
    echo "SYS:ShowNetStatus INTERFACE eth0"
    echo "SYS:ping -c 3 $PEER_ULA"
    echo "SYS:RemoveNetInterface eth0"
    echo "wait 5"
} > "$STAGE/commands.txt"

set +e
AMINETXDUO_RUN_TAG="$TAG" AMINETXDUO_AMIBERRY_MAC="$MAC" \
    "$ROOT/tools/amiberry-run.sh" \
    -N "$CARD" -B "$BACKEND" -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/RemoveNetInterface" "$STAGE/ShowNetStatus" "$STAGE/ping" \
    "$STAGE/commands.txt" \
    > "$OUT" 2>&1
run_rc=$?
set -e

kill "$CAP_PID" 2>/dev/null || true
wait "$CAP_PID" 2>/dev/null || true
CAP_PID=

if [ ! -s "$SERIAL" ] && [ ! -s "$OUT" ]; then
    echo "result=noguest run_rc=$run_rc"
    exit 3
fi

GUEST_LL=$(sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
    | sed -n 's|.*address6  *\(fe80::[0-9A-Fa-f:]*\)/.*|\1|p' | head -1)

seen() {
    if [ -z "$GUEST_LL" ]; then
        grep -qi "dhcp6 $1" "$CAP" 2>/dev/null && echo yes || echo no
        return
    fi
    { grep -i "dhcp6 $1" "$CAP" 2>/dev/null || true; } |
        grep -qF "$GUEST_LL" && echo yes || echo no
}

solicit_seen=$(seen "solicit")
advertise_seen=$(seen "advertise")
request_seen=$(seen "request")
reply_seen=$(seen "reply")
renew_seen=$(seen "renew")
release_seen=$(seen "release")
inforeq_seen=$(seen "inf-req")

echo "solicit_seen=$solicit_seen advertise_seen=$advertise_seen"
echo "request_seen=$request_seen reply_seen=$reply_seen"
echo "renew_seen=$renew_seen release_seen=$release_seen"
echo "inforeq_seen=$inforeq_seen run_rc=$run_rc guest_ll=${GUEST_LL:-none}"
echo "capture=$CAP serial=$SERIAL out=$OUT"

peer_linklocal() {
    local m u
    m=$(printf '%s' "$1" | tr -d ':')
    u=$(printf '%02x' $(( 0x${m:0:2} ^ 0x02 )))
    printf 'fe80::%s%s:%sff:fe%s:%s%s' \
        "$u" "${m:2:2}" "${m:4:2}" "${m:6:2}" "${m:8:2}" "${m:10:2}"
}

PEER_LL=$(ssh -o BatchMode=yes "$PEER" \
    "ip -6 -o addr show dev ${PEER_NIC} scope link" 2>/dev/null |
    sed -n 's/.*inet6 \([0-9A-Fa-f:]*\)\/.*/\1/p' | head -1 |
    tr 'A-Z' 'a-z')
[ -n "$PEER_LL" ] || PEER_LL=$(peer_linklocal "$PEER_MAC")

from_port_547() {
    sed -n "s/^[0-9:.]* IP6 \([0-9A-Fa-f:]*\)\.547 > .*dhcp6 $1.*/\1/p" \
        "$CAP" 2>/dev/null
}

advertiser_list=$(from_port_547 advertise | sort -u | tr '\n' ',' |
    sed 's/,$//')
advertisers=$(printf '%s\n' "$advertiser_list" | tr ',' '\n' | grep -c . ||
    true)

chosen_server=$(from_port_547 reply | head -1)

echo "peer_mac=$PEER_MAC peer_linklocal=$PEER_LL"
echo "advertisers=$advertisers advertiser_list=${advertiser_list:-none}"
echo "chosen_server=${chosen_server:-none}"

if [ "$solicit_seen" = no ] && [ "$inforeq_seen" = no ]; then
    if [ "$ARM" = auto ]; then
        fail_link "router_ra_asks_for_no_dhcpv6"
    fi
    echo "FAIL solicit_seen: the guest sent no Solicit"
    echo "result=fail"
    exit 1
fi

if [ -n "$chosen_server" ] && [ "$chosen_server" != "$PEER_LL" ]; then
    echo "another_server_won=yes"
    echo "NOT A FAILURE.  This link carries a second DHCPv6 server -- the site"
    echo "router -- and the guest took its offer instead of the peer's.  Which"
    echo "one wins is decided by OPTION_PREFERENCE, and dnsmasq cannot send it"
    echo "(see the header of $0).  Every assertion after this point would be"
    echo "about $chosen_server's exchange rather than the one under test, so"
    echo "the run stops here and exits 4.  Re-run it; it is a race and the"
    echo "other outcome is common.  DO NOT make this exit 1 -- the stack did"
    echo "nothing wrong, and the missing piece is a server that can send the"
    echo "preference option."
    fail_link "another_dhcpv6_server_answered_first"
fi

guest_ula=no
guest_addr=
if guest_addr=$(sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
        | sed -n "s|.*address6 *\(${ULA_PREFIX}[0-9A-Fa-f:]*\)/.*|\1|p" | head -1) &&
   [ -n "$guest_addr" ]
then
    guest_ula=yes
fi

ipv4_none=no
sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
    | grep -qi "carries no IPv4" && ipv4_none=yes

reach=no
sed -n "/^===== SYS:ping/,/^----- rc/p" "$OUT" 2>/dev/null \
    | grep -qE "bytes from|[1-3] (packets )?received" && reach=yes

ula_reports=$(sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
    | grep -c "address6  *${ULA_PREFIX}" || true)
resumed_ula=no
[ "$ula_reports" -ge 2 ] && resumed_ula=yes

resumed_reach=no
awk '
    /^===== SYS:ping/ { block = ""; inside = 1 }
    inside { block = block $0 "\n" }
    inside && /^----- rc/ { last = block; inside = 0 }
    END { printf "%s", last }
' "$OUT" 2>/dev/null | grep -qE "bytes from|[1-3] (packets )?received" &&
    resumed_reach=yes

echo "guest_ula=$guest_ula guest_addr=${guest_addr:-none} ipv4_none=$ipv4_none"
echo "reach=$reach resumed_ula=$resumed_ula resumed_reach=$resumed_reach"

addr_stable=skipped
if [ "$RENEW" = yes ]; then
    second=$(sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
        | sed -n "s|.*address6 *\(${ULA_PREFIX}[0-9A-Fa-f:]*\)/.*|\1|p" | tail -1)
    if [ -n "$second" ] && [ "$second" = "$guest_addr" ]; then
        addr_stable=yes
    else
        addr_stable=no
    fi
    echo "addr_stable=$addr_stable second_addr=${second:-none}"
fi

fail=0
note() { echo "FAIL $1"; fail=1; }

[ "$reply_seen"    = yes ] || note "reply_seen: the server never answered"
[ "$chosen_server" = "$PEER_LL" ] || note "chosen_server: no Reply from $PEER_LL"
[ "$guest_ula"    = yes ] || note "guest_ula: no address from ${ULA_PREFIX}"
[ "$ipv4_none"    = yes ] || note "ipv4_none: the guest has an IPv4 address"
[ "$reach"        = yes ] || note "reach: ping6 to $PEER_ULA did not answer"
[ "$resumed_ula"  = yes ] || note "resumed_ula: Online acquired no new address"
[ "$resumed_reach" = yes ] || note "resumed_reach: ping6 failed after Online"
[ "$release_seen" = yes ] || note "release_seen: no Release when the interface went down"

if [ "$RENEW" = yes ]; then
    [ "$renew_seen"  = yes ] || note "renew_seen: no Renew before T2"
    [ "$addr_stable" = yes ] || note "addr_stable: the address moved across the renewal"
fi

if [ "$fail" -ne 0 ]; then
    echo "result=fail"
    exit 1
fi

echo "result=pass"
exit 0
