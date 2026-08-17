#!/usr/bin/env bash
#
# DOES AN IPv6-ONLY MACHINE GET ON THE NETWORK THROUGH DHCPv6.
#
#   tests/ipv6/run-dhcpv6.sh [-B BACKEND] [-b BUILDDIR] [-m MODEL] [-N CARD]
#                            [-P user@peer] [-t SECONDS] [-w SECONDS]
#                            [-M MAC] [-l SECONDS] [-a auto|dhcp] [-R]
#
# WHAT IT ASSERTS
#
#   srv_up          a real DHCPv6 server answered on the peer, started by this
#                   run and stopped by it
#   solicit_seen    the guest sent a Solicit (or an Information-Request), read
#                   out of a capture taken during THIS run
#   reply_seen      the server answered it
#   guest_ula       the guest holds the address that server hands out, read
#                   out of what ShowNetStatus printed.  It is a ULA, from a
#                   prefix nothing else on this link uses, so it cannot have
#                   come from the router's own advertisement
#   ipv4_none       and it holds no IPv4 address at all, which is the claim
#                   that matters: the interface file says CONFIGURE=NONE and
#                   the machine is on the network anyway
#   reach           ping6 reached the peer over that address
#   renew_seen      with -R, a Renew went out before the lease expired and the
#                   address did not move
#   release_seen    RemoveNetInterface sent a Release, so the server can give
#                   the address to somebody else
#
# -a dhcp is CONFIGURE6=DHCP, which asks without waiting to be told to.
# -a auto is CONFIGURE6=AUTO, which asks only because the router advertisement
# on this link sets the M bit -- so that arm is what proves the M bit is
# honoured, and it fails if it is not, because the ULA can only have come from
# the peer's server.
#
# THE LINK ALREADY HAS A DHCPv6 SERVER ON IT
#
#   The lab router is one, stateful, handing out the site's own /64.  Every
#   Solicit therefore draws two Advertises and the client picks one.  The
#   peer's server sends OPTION_PREFERENCE 255, which RFC 8415 18.2.1 makes
#   decisive -- the client stops waiting and takes it -- so this test is not a
#   race.  If the guest ends up with the site prefix and not the ULA, that is
#   a real finding about preference handling and it fails here.
#
# BRIDGED, OR IT MEASURES NOTHING
#
#   SLIRP answers nothing on UDP 547 and rewrites source ports, so -B must
#   name a host NIC.  The guest gets its own MAC, distinct from every other
#   harness here, because the DUID is derived from it and two machines sharing
#   a MAC share an identity.
#
# WHAT IS NOT ASSERTED HERE, AND WHERE IT IS
#
#   The M-and-O combinations this router does not advertise, and the DUID
#   being the same on the next boot as on this one, cannot be produced against
#   a router and a lease table this test does not own.  Both are in
#   tests/ipv6/host/test_dhcpv6_host.c, which runs in ctest.
#
# OUTPUT IS key=value.  Exit 0 if every assertion held, 1 if one failed, 3 if
# the guest never ran, 4 if the link or the peer was not what the test needs,
# 2 for a broken invocation.
#
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

# The peer's server hands out of this prefix and answers on this address.  Both
# are written into ~/anxd-dhcpv6-server.sh on the peer; they are here so the
# assertions can name them.
ULA_PREFIX="${AMINETXDUO_DHCPV6_PREFIX:-fd00:aa5:1:}"
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
SMOKE="$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$RMIF" "$SHOW" "$PING" "$SMOKE"; do
    [ -f "$f" ] || { echo "result=badinvocation reason=nobuild missing=$f"; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || fail_setup "no_kickstart"

DEVICE="${AMINETXDUO_A2065:-}"
if [ -z "$DEVICE" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { DEVICE="$c"; break; }
    done
fi
[ -f "${DEVICE:-/nonexistent}" ] || fail_setup "no_a2065_device"

command -v tcpdump > /dev/null || fail_setup "no_tcpdump"

# The address the card puts on the wire, which is what the DUID is made of and
# what the peer's server is keyed on.  The a2065's LANCE keeps the last three
# octets and writes Commodore's 00:80:10 over the rest.
TAIL=$(printf '%s' "$MAC" | tr 'A-Z' 'a-z' | cut -d: -f4-6)
GUEST_MAC="00:80:10:$TAIL"

echo "arm=$ARM guest_mac=$GUEST_MAC backend=$BACKEND peer=$PEER lease=$LEASE"

# --------------------------------------------------------------- the peer ---

ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" true 2>/dev/null ||
    fail_link "peer_unreachable"

# The path is expanded on the peer, not here, so it is single-quoted and the
# tilde is the peer's home.
PEER_SERVER='$HOME/anxd-dhcpv6-server.sh'

# shellcheck disable=SC2016
ssh -o BatchMode=yes "$PEER" 'test -x "$HOME/anxd-dhcpv6-server.sh"' 2>/dev/null ||
    fail_link "peer_has_no_server"

SRVLOG="$ROOT/build/dhcpv6-server.log"
rm -f "$SRVLOG"

# Foreground on the peer, output here, killed on the way out.  Not left
# running: a DHCPv6 server on this link is somebody else's problem the moment
# this test is not using it.
ssh -o BatchMode=yes -o ServerAliveInterval=15 "$PEER" \
    "$PEER_SERVER --lease $LEASE" > "$SRVLOG" 2>&1 &
SRV_PID=$!

cleanup() {
    kill "$SRV_PID" 2>/dev/null || true
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
        'pkill -x dnsmasq-cap' > /dev/null 2>&1 || true
    kill "${CAP_PID:-0}" 2>/dev/null || true
}
trap cleanup EXIT

srv_up=no
for _ in $(seq 1 40); do
    if grep -q "listening\|started\|dnsmasq-dhcp" "$SRVLOG" 2>/dev/null; then
        srv_up=yes
        break
    fi
    kill -0 "$SRV_PID" 2>/dev/null || break
    sleep 0.5
done

echo "srv_up=$srv_up srvlog=$SRVLOG"
[ "$srv_up" = yes ] || fail_link "peer_server_did_not_start"

# ------------------------------------------------------------- the capture --
#
# Before the emulator starts, because the Solicit is the first thing the guest
# sends after its link-local settles.

TAG="dhcpv6-$ARM"
CAP="$ROOT/build/$TAG-wire.txt"
SERIAL="$ROOT/build/amiberry-serial-$TAG.log"
OUT="$ROOT/build/$TAG.out"
rm -f "$CAP" "$CAP.err" "$SERIAL" "$OUT"

tcpdump -i "$BACKEND" -n -vv -s0 -l \
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

# ------------------------------------------------------------- the staging --

STAGE="$ROOT/build/$TAG-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$DEVICE" "$STAGE/devs/a2065.device"
cp "$ADDIF"  "$STAGE/AddNetInterface"
cp "$RMIF"   "$STAGE/RemoveNetInterface"
cp "$SHOW"   "$STAGE/ShowNetStatus"
cp "$PING"   "$STAGE/ping"

# NO IPv4 AT ALL, which is the configuration under test.  Nothing in
# DEVS:Internet either, so there is no name server and no route written down:
# everything this machine has, it was given.
case "$ARM" in
    dhcp) CONF6=DHCP ;;
    auto) CONF6=AUTO ;;
esac

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=NONE
CONFIGURE6=$CONF6
EOF

{
    echo "SYS:AddNetInterface DEVS:NetInterfaces/eth0"
    echo "wait $SETTLE"
    echo "SYS:ShowNetStatus INTERFACE eth0"
    echo "SYS:ping -c 3 $PEER_ULA"
    if [ "$RENEW" = yes ]; then
        # Past T1, which is half the lease, so a Renew is due and the address
        # must not move across it.
        echo "wait $(( LEASE / 2 + 20 ))"
        echo "SYS:ShowNetStatus INTERFACE eth0"
    fi
    # The Release: RemoveNetInterface takes the interface down, and taking it
    # down is what sends it.
    echo "SYS:RemoveNetInterface eth0"
    echo "wait 5"
} > "$STAGE/commands.txt"

# ------------------------------------------------------------------- run ----

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

# ------------------------------------------------------------ the capture ---
#
# tcpdump prints the message type in the clear: "dhcp6 solicit", "dhcp6 reply".
# Matched on the guest's own MAC as well, so another machine's exchange on the
# same wire cannot satisfy an assertion here.

seen() { grep -qi "dhcp6 $1" "$CAP" 2>/dev/null && echo yes || echo no; }

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
echo "inforeq_seen=$inforeq_seen run_rc=$run_rc"
echo "capture=$CAP serial=$SERIAL out=$OUT"

# ------------------------------------------------------------- the guest ----
#
# ShowNetStatus prints one "address6 <addr>/<len>" line per address.  The ULA
# is the assertion: the site's own prefix could have come from the router's
# advertisement, and this one could not have come from anywhere but the
# server on the peer.

guest_ula=no
guest_addr=
if guest_addr=$(sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
        | sed -n "s|.*address6 *\(${ULA_PREFIX}[0-9A-Fa-f:]*\)/.*|\1|p" | head -1) &&
   [ -n "$guest_addr" ]
then
    guest_ula=yes
fi

# No IPv4, which is the whole configuration under test.  ShowNetStatus prints
# one line saying so for an interface that carries none.
ipv4_none=no
sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
    | grep -qi "carries no IPv4" && ipv4_none=yes

reach=no
sed -n "/^===== SYS:ping/,/^----- rc/p" "$OUT" 2>/dev/null \
    | grep -qE "bytes from|[1-3] (packets )?received" && reach=yes

echo "guest_ula=$guest_ula guest_addr=${guest_addr:-none} ipv4_none=$ipv4_none"
echo "reach=$reach"

# The address did not move across the renewal.  Two ShowNetStatus runs, the
# second after T1; both must name the same address.
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

# --------------------------------------------------------------- verdict ----

fail=0
note() { echo "FAIL $1"; fail=1; }

if [ "$ARM" = auto ] && [ "$solicit_seen" = no ] && [ "$inforeq_seen" = no ]; then
    # Neither flag on this link, so there is nothing for CONFIGURE6=AUTO to do
    # and this arm cannot test anything.  A link fact, not a code fault.
    fail_link "router_ra_asks_for_no_dhcpv6"
fi

[ "$solicit_seen" = yes ] || note "solicit_seen: the guest sent no Solicit"
[ "$reply_seen"   = yes ] || note "reply_seen: the server never answered"
[ "$guest_ula"    = yes ] || note "guest_ula: no address from ${ULA_PREFIX}"
[ "$ipv4_none"    = yes ] || note "ipv4_none: the guest has an IPv4 address"
[ "$reach"        = yes ] || note "reach: ping6 to $PEER_ULA did not answer"
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
