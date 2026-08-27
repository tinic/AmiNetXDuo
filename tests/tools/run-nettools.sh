#!/usr/bin/env bash
# nc, telnet, traceroute, tftp and whois against a real peer on the segment.
#
#   tests/tools/run-nettools.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                               [-B INTERFACE] [-H user@host] [-A ADDRESS]
#                               [-a GUEST-ADDRESS]
#
# BRIDGED, AND THE SERVERS LIVE ON A PEER.  This ran under SLIRP: every server
# was tests/tools/netpeer.py on the emulator host, reached at 10.0.2.2, and the
# one arm that mattered most -- something OUTSIDE connecting IN to a listener
# on the Amiga -- needed `uae_slirp_redir`, an FS-UAE option that Amiberry does
# not have and that nothing has read since fs-uae left the tree.  So the
# listen/accept path was only ever driven guest-to-guest over 127.0.0.1.
#
# Bridged there is nothing to forward: the guest has an address on the LAN and
# a machine on the LAN dials it.  netpeer.py's own dialer does that, from the
# peer, and logs what came back -- which is the payload in both directions.
#
# The guest's address is STATIC because the dialer has to be told where to
# knock before the guest has booted.  It is checked free on this segment first.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_NETTOOLS_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"
PEER="${AMINETXDUO_NETTOOLS_PEER:-${AMINETXDUO_PEER:-}}"
PEER_ADDR="${AMINETXDUO_NETTOOLS_PEER_ADDR:-}"
GUEST_ADDR="${AMINETXDUO_NETTOOLS_ADDRESS:-192.168.1.247}"
GATEWAY="${AMINETXDUO_NETTOOLS_GATEWAY:-}"
RESOLVER="${AMINETXDUO_NETTOOLS_RESOLVER:-}"

while getopts "m:t:c:b:B:H:A:a:g:s:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        a) GUEST_ADDR="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        s) RESOLVER="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-B interface] [-H user@host] [-A peer-address] [-a guest-address] [-g gateway] [-s resolver]" >&2
           exit 2 ;;
    esac
done

kv() { printf '%s=%s\n' "$1" "$2"; }
refuse() { kv reason "$1"; kv RESULT refused; exit 2; }

case "$IFACE" in
    slirp|slirp_inbound)
        refuse "slirp_forwards_nothing_inward: -B <interface>" ;;
esac

[ -n "$PEER" ] ||
    refuse "no peer: set AMINETXDUO_NETTOOLS_PEER=<user@host> or pass -H; a bridged guest cannot reach the machine running the emulator"

command -v ip >/dev/null 2>&1 || refuse "no ip(8) on this host"

ECHO_PORT="${AMINETXDUO_ECHO_PORT:-7001}"
TELNET_PORT="${AMINETXDUO_TELNET_PORT:-7023}"
NC_INBOUND_PORT="${AMINETXDUO_NC_PORT:-7042}"
TFTP_PORT="${AMINETXDUO_TFTP_PORT:-7069}"
WHOIS_PORT="${AMINETXDUO_WHOIS_PORT:-7043}"

SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
NC="$ROOT/$BUILD/src/tools/nc"
TELNET="$ROOT/$BUILD/src/tools/telnet"
TRACEROUTE="$ROOT/$BUILD/src/tools/traceroute"
TFTP="$ROOT/$BUILD/src/tools/tftp"
WHOIS="$ROOT/$BUILD/src/tools/whois"

for f in "$SMOKE" "$ADDIF" "$BSD" "$NC" "$TELNET" "$TRACEROUTE" \
         "$TFTP" "$WHOIS"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------ the segment ---

HOSTCIDR=$(ip -o -4 addr show dev "$IFACE" 2>/dev/null | awk '{ print $4; exit }')
[ -n "$HOSTCIDR" ] ||
    refuse "no IPv4 address on $IFACE; -B names the host NIC the guest bridges onto"
PREFIXLEN="${HOSTCIDR#*/}"
[ "$PREFIXLEN" = 24 ] ||
    refuse "$IFACE is a /$PREFIXLEN; this harness writes a 255.255.255.0 netmask into the guest"
NETMASK=255.255.255.0

if [ -z "$GATEWAY" ]; then
    GATEWAY=$(ip -o -4 route show default dev "$IFACE" 2>/dev/null |
              awk '{ print $3; exit }')
fi
[ -n "$GATEWAY" ] || refuse "no default gateway on $IFACE; -g names one"

if [ -z "$RESOLVER" ]; then
    RESOLVER=$(awk '/^nameserver/ && $2 !~ /:/ { print $2; exit }' /etc/resolv.conf)
fi
[ -n "$RESOLVER" ] || refuse "this host has no IPv4 name server; -s names one"
case "$RESOLVER" in
    127.*|0.0.0.0)
        refuse "$RESOLVER is on this host's loopback and a bridged guest cannot reach it; -s names one on the segment" ;;
esac

if [ -z "$PEER_ADDR" ]; then
    PEER_ADDR=$(ssh -o ConnectTimeout=10 "$PEER" \
                    "ip -o -4 addr show scope global |
                     awk '{split(\$4,a,\"/\"); print a[1]}'" 2>/dev/null |
                head -1)
fi
[ -n "$PEER_ADDR" ] || refuse "$PEER did not report an address of its own; pass -A"
ip -o route get "$PEER_ADDR" 2>/dev/null | grep -q "dev $IFACE " ||
    refuse "$PEER_ADDR is not on $IFACE's segment"

# THE GUEST'S ADDRESS HAS TO BE FREE.  A static address that is already in use
# poisons two machines' neighbour caches and what fails is an assertion
# somewhere else; this is the same reasoning as the MAC derivation in
# tools/amiberry-run.sh.
[ "$GUEST_ADDR" != "${HOSTCIDR%/*}" ] || refuse "$GUEST_ADDR is this host's own address"
[ "$GUEST_ADDR" != "$PEER_ADDR" ]    || refuse "$GUEST_ADDR is the peer's address"
if ping -c 2 -W 1 "$GUEST_ADDR" >/dev/null 2>&1; then
    refuse "$GUEST_ADDR answers ICMP already, so something else holds it; -a names a free address"
fi
kv guest_address "$GUEST_ADDR"
kv peer          "$PEER"
kv peer_address  "$PEER_ADDR"
kv iface         "$IFACE"
kv gateway       "$GATEWAY"
kv resolver      "$RESOLVER"

# --------------------------------------------------------------- staging ----

STAGE="$ROOT/build/nettools-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$ADDIF"  "$STAGE/AddNetInterface"
cp "$NC"     "$STAGE/nc"
cp "$TELNET" "$STAGE/telnet"
cp "$TRACEROUTE" "$STAGE/traceroute"
cp "$TFTP"       "$STAGE/tftp"
cp "$WHOIS"      "$STAGE/whois"

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$GUEST_ADDR
NETMASK=$NETMASK
GATEWAY=$GATEWAY
CONFIGURE6=OFF
EOF

# The segment's own resolver: `nc no.such.host.invalid` and the two whois
# lines that go to the real registries need one, and a STATIC interface takes
# no name server from a lease.
cat > "$STAGE/devs/Internet/name_resolution" <<EOF
nameserver $RESOLVER
hostname anxnettools
EOF

printf 'GET / HTTP/1.0\r\n\r\n' > "$STAGE/request.txt"
printf 'hello from the amiga\n' > "$STAGE/greeting.txt"

cat > "$STAGE/telnetin.txt" <<'EOF'
amiga
quit
EOF

if [ -n "${AMINETXDUO_NETTOOLS_COMMANDS:-}" ]; then
    cp "$AMINETXDUO_NETTOOLS_COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $AMINETXDUO_NETTOOLS_COMMANDS"
else
cat > "$STAGE/commands.txt" <<EOF
# ---- the templates, through ReadArgs' own "?" -------------------------
SYS:nc ?
SYS:telnet ?
# ---- bring the network up once, and leave it up -----------------------
SYS:AddNetInterface eth0
# ---- nc, as a client --------------------------------------------------
SYS:nc -z $PEER_ADDR $ECHO_PORT -v
SYS:nc -z $PEER_ADDR 1-2 -v -w 5
SYS:nc $PEER_ADDR $ECHO_PORT -v -w 10 -N <DH0:greeting.txt >DH0:nc-echo.txt
# ---- nc, as a SERVER, dialled FROM ANOTHER MACHINE ---------------------
# The arm SLIRP could never drive: netpeer.py's dialer runs on $PEER and
# connects to this listener across the LAN, sends a line and half-closes, then
# reads what the Amiga sends back.  Both directions are on the wire.  The
# dialer retries until the listener appears, so it is started here and
# everything below happens while it waits.
&SYS:nc -l $NC_INBOUND_PORT -v -w 120 -N <DH0:greeting.txt >DH0:nc-inbound.txt
# ---- telnet -----------------------------------------------------------
SYS:telnet $PEER_ADDR $TELNET_PORT -d <DH0:telnetin.txt >DH0:telnet.txt
# ---- and what the failures look like ----------------------------------
SYS:nc $PEER_ADDR 1 -v -w 5
SYS:nc no.such.host.invalid 80
# ---- nc as a server, guest to guest -----------------------------------
# Still worth having: a full conversation, half-close included, that never
# leaves the machine.
&SYS:nc -l 7099 -v -w 10 -N >DH0:nc-loopback.txt
wait 4
SYS:nc 127.0.0.1 7099 -v -w 10 -N <DH0:greeting.txt >DH0:nc-loopclient.txt
wait 4
# ... and over this machine's own Ethernet address rather than 127.0.0.1.
&SYS:nc -l 7098 -v -w 10 >DH0:nc-self.txt
wait 4
SYS:nc $GUEST_ADDR 7098 -v -w 10 <DH0:greeting.txt >DH0:nc-selfclient.txt
# ---- traceroute, tftp and whois ---------------------------------------
SYS:traceroute ?
SYS:tftp ?
SYS:whois ?
# On a real segment the TTL means what it says, which is the difference from
# the SLIRP runs docs/RESEARCH.md 20 is about:
#
#   \$PEER_ADDR    one hop, on this segment, and it answers
#   \$GUEST_ADDR   our own address, one hop by definition
#   8.8.8.8       the real internet, through the real gateway
#   192.0.2.1     TEST-NET-1: routed at the gateway and answered by nobody
SYS:traceroute $PEER_ADDR -m 4 -q 2 -w 3 -n
SYS:traceroute $GUEST_ADDR -m 3 -q 1 -w 3 -n
SYS:traceroute 8.8.8.8 -m 5 -q 1 -w 3 -n -v
SYS:traceroute 192.0.2.1 -m 2 -q 1 -w 3 -n -v
# tftp against netpeer.py's server: a small file, a big one, one that is an
# exact multiple of the block size, which ends with an EMPTY data block --
# one going the other way, and one that is not there.
SYS:tftp $PEER_ADDR PORT $TFTP_PORT GET hello.txt AS DH0:tftp-hello.txt
SYS:tftp $PEER_ADDR PORT $TFTP_PORT GET big.bin AS DH0:tftp-big.bin
SYS:tftp $PEER_ADDR PORT $TFTP_PORT GET exact.bin AS DH0:tftp-exact.bin
SYS:tftp $PEER_ADDR PORT $TFTP_PORT PUT DH0:greeting.txt AS from-amiga.txt
SYS:tftp $PEER_ADDR PORT $TFTP_PORT GET no.such.file
# whois against netpeer.py's, whose canned records cover the three shapes.
# referral.test refers to the server it came from, which is a loop and has to
# be recognised as one; chain.test refers somewhere ELSE, so without FOLLOW
# the line to type next is printed and with it the client goes there, to
# 127.0.0.1, where nothing is listening, so the second hop demonstrates the
# failure being legible.
SYS:whois plain.test SERVER $PEER_ADDR PORT $WHOIS_PORT
SYS:whois referral.test SERVER $PEER_ADDR PORT $WHOIS_PORT FOLLOW
SYS:whois chain.test SERVER $PEER_ADDR PORT $WHOIS_PORT
SYS:whois chain.test SERVER $PEER_ADDR PORT $WHOIS_PORT FOLLOW
# ... and the default server, which is a real registry over the real internet.
# example.com produces NO referral, IANA administers it and answers for it
# directly, so amiga.com is here as well: IANA refers that one to Verisign,
# and Verisign's record carries the indented "Registrar WHOIS Server:" line
# that a matcher anchored at column zero silently misses.  That is the case
# which found the bug, so it is the case that keeps it fixed.
SYS:whois example.com
SYS:whois amiga.com FOLLOW
# ---- give the inbound connection time to have happened ----------------
wait 15
EOF
fi

# ------------------------------------------------------------ the peer ------

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-nettools}"
TAG="$AMINETXDUO_RUN_TAG"

PEERLOG="$ROOT/build/netpeer.log"
: > "$PEERLOG"
PEER_SCRIPT="/tmp/netpeer-$TAG.py"
PEER_LOGFILE="/tmp/netpeer-$TAG.log"
PEER_OUT="/tmp/netpeer-$TAG.out"
KILLPAT="[n]etpeer-$TAG.py"
PEER_PYTHON="${AMINETXDUO_NETTOOLS_PYTHON:-python3}"

ssh "$PEER" "pkill -f \"$KILLPAT\" || true" >/dev/null 2>&1 || true
scp -q "$ROOT/tests/tools/netpeer.py" "$PEER:$PEER_SCRIPT" ||
    refuse "could not stage netpeer.py on $PEER"

cleanup_peer() {
    ssh "$PEER" "pkill -f \"$KILLPAT\" || true" >/dev/null 2>&1 || true
}
trap cleanup_peer EXIT INT TERM HUP

ssh "$PEER" "nohup $PEER_PYTHON $PEER_SCRIPT \
             --echo-port $ECHO_PORT --telnet-port $TELNET_PORT \
             --tftp-port $TFTP_PORT --whois-port $WHOIS_PORT \
             --advertise $PEER_ADDR \
             --dial $GUEST_ADDR:$NC_INBOUND_PORT --dial-for $TIMEOUT \
             --log $PEER_LOGFILE --seconds $((TIMEOUT + 600)) \
             < /dev/null > $PEER_OUT 2>&1 & sleep 2" >/dev/null 2>&1 || true

if ! ssh "$PEER" "pgrep -f \"$KILLPAT\" >/dev/null" 2>/dev/null; then
    ssh "$PEER" "cat $PEER_OUT" 2>/dev/null | sed 's/^/!! /' >&2
    refuse "netpeer.py did not start on $PEER"
fi
echo "==> netpeer.py on $PEER ($PEER_ADDR): echo $ECHO_PORT," \
     "telnet $TELNET_PORT, tftp $TFTP_PORT, whois $WHOIS_PORT," \
     "dialling $GUEST_ADDR:$NC_INBOUND_PORT"

# ------------------------------------------------------------- the guest ----

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "${CPUARG[@]}" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/nc" "$STAGE/telnet" \
    "$STAGE/traceroute" "$STAGE/tftp" "$STAGE/whois" \
    "$STAGE/AddNetInterface" "$STAGE/commands.txt" \
    "$STAGE/request.txt" "$STAGE/greeting.txt" "$STAGE/telnetin.txt"
RC=$?
set -e

ssh "$PEER" "cat $PEER_LOGFILE" > "$PEERLOG" 2>/dev/null || : > "$PEERLOG"
cleanup_peer
trap - EXIT INT TERM HUP

echo
echo "================ what the peer's servers saw ================"
cat "$PEERLOG" 2>/dev/null || true

echo
echo "---- the verdict ----"
# shellcheck source=tests/tools/nettools-verdict.sh
. "$ROOT/tests/tools/nettools-verdict.sh"

HD="$ROOT/build/amiberry-testhd-$TAG"

printf 'run_rc=%s\n' "$RC"
if [ "$RC" != 0 ]; then
    printf 'reason=%s\n' "the guest did not come back (124 is the timeout)"
    printf 'RESULT=broken\n'
    exit 3
fi

if [ -n "${AMINETXDUO_NETTOOLS_COMMANDS:-}" ]; then
    printf 'reason=%s\n' "custom_command_list"
    printf 'RESULT=skip\n'
    exit 77
fi

# THE INBOUND ARM, FROM THE PEER'S SIDE.  nettools_verdict reads the guest's
# copy of what arrived; this reads the dialer's copy of what came back, which
# is the direction no guest-side file can evidence.
INBOUND_BACK=fail
if grep -q 'dial .*connected to ' "$PEERLOG" 2>/dev/null; then
    if grep -q "dial .*the Amiga sent back .*hello from the amiga" "$PEERLOG"; then
        INBOUND_BACK=ok
    else
        INBOUND_BACK=connected_but_silent
    fi
else
    INBOUND_BACK=never_connected
fi
kv nettools_nc_inbound_reply "$INBOUND_BACK"

if nettools_verdict "$HD/tools.txt" "$HD" "$PEERLOG" "$PEER_ADDR" yes &&
   [ "$INBOUND_BACK" = ok ]; then
    printf 'RESULT=pass\n'
    exit 0
fi
[ "$INBOUND_BACK" = ok ] ||
    kv nettools_fail "the_peer_did_not_get_the_guests_reply_${INBOUND_BACK}"
printf 'RESULT=fail\n'
exit 1
