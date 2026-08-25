#!/usr/bin/env bash
#
# DOES A ROUTER ADVERTISEMENT'S RESOLVER REACH THE RESOLVER.
#
#   tests/ipv6/run-rdnss.sh [-B BACKEND] [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                           [-n NAME] [-w SECONDS] [-M MAC]
#
# WHAT IT ASSERTS
#
#   ra_rdnss        the advertisement on this link carries an RFC 8106 5.1
#                   option, read out of a capture taken during THIS run
#   ra_dnssl        and an RFC 8106 5.2 option
#   guest_global    the guest formed a global IPv6 address from the same
#                   advertisement
#   ra_default_route  netstat -r shows ::/0 via the router that advertised
#   ns6_origin      the address formed from it is reported as advertised
#   ns6_reported    ShowNetStatus lists the advertised name server
#   short_resolved  a name with no dot in it resolved, which it can only have
#                   done through the advertised search domain
#
# The first two are the LINK's assertions, not the code's: this ISP drops IPv6
# without warning and a router can be reconfigured, and a machine with only a
# link-local address produces results indistinguishable from the defects this
# tests for.  They exit 4, "the link is not what this test needs", so a broken
# lab is never reported as a broken stack.  guest_global is the same kind of
# precondition and exits 4 for the same reason.  Only ns6_reported and
# short_resolved exit 1.
#
# THE GUEST HAS NO DHCP AND NO name_resolution FILE
#
#   A static IPv4 address on a subnet nothing on this link uses, so there is no
#   lease, no option 6 name server and no option 15 or 119 search domain; and
#   nothing in DEVS:Internet, so no NAMESERVER, DOMAIN or SEARCH line either.
#   Every name server and every suffix the guest ends up with came out of the
#   advertisement, which is what makes "a short name resolved" mean what it says.
#
#   An address rather than none at all because AddNetInterface refuses an
#   interface with no IPv4 address ("the interface has no address: there is no
#   ADDRESS line and CONFIGURE does not say DHCP"), so the IPv6-only machine the
#   router describes -- it sets managed+other-stateful, this stack builds no
#   DHCPv6, and RFC 8106 is all that is left -- cannot be configured yet.  The
#   subnet is deliberately one nobody answers on: nothing in this test travels
#   over IPv4, and a real address on the lab LAN would be a second machine
#   claiming one.
#
# BRIDGED, OR IT MEASURES NOTHING
#
#   SLIRP answers a router solicitation itself, with no RDNSS and no DNSSL, so
#   -B must name a host NIC; amiberry-run.sh refuses the run if the backend it
#   got was not the one asked for.  That needs CAP_NET_RAW on the amiberry
#   binary, and the capture needs it on tcpdump.
#
#   The guest gets its own MAC, distinct from every other harness here, because
#   two machines with one MAC on one segment is not a test of anything.
#
# WHAT IS NOT ASSERTED HERE, AND WHERE IT IS
#
#   A zero lifetime, several servers at once, and a fifth beyond the four the
#   list holds cannot be produced against a router this test does not own.  The
#   parsing of all three is in tests/ipv6/host/test_ipv6_ra_host.c and the
#   bookkeeping they drive is in src/config/test/test_config.c
#   (test_ra_nameserver6, test_ra_search_option), both of which run in ctest.
#
# OUTPUT IS key=value.  Exit 0 if every assertion held, 1 if one failed, 3 if
# the guest never ran, 4 if the link did not carry what the test needs, 2 for a
# broken invocation.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"

BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/v6ra}"
MODEL=A1200
TIMEOUT=180
SETTLE=30
NAME=playhouse2
MAC="${AMINETXDUO_RDNSS_MAC:-02:41:4d:49:52:44}"
V4ADDR="${AMINETXDUO_RDNSS_V4:-10.99.99.2}"

while getopts "B:b:m:t:n:w:M:" opt; do
    case "$opt" in
        B) BACKEND="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        n) NAME="$OPTARG" ;;
        w) SETTLE="$OPTARG" ;;
        M) MAC="$OPTARG" ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

case "$BACKEND" in
    slirp|slirp_inbound)
        echo "result=badinvocation reason=slirp" >&2
        echo "-B must name a host NIC: SLIRP answers the solicitation itself." >&2
        exit 2 ;;
esac

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$BUILD/src/tools/AddNetInterface"
SHOW="$BUILD/src/tools/ShowNetStatus"
HOSTC="$BUILD/src/tools/host"
NETSTAT="$BUILD/src/tools/netstat"
SMOKE="$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$SHOW" "$HOSTC" "$NETSTAT" "$SMOKE"; do
    [ -f "$f" ] || { echo "result=badinvocation reason=nobuild missing=$f" >&2; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "result=badinvocation reason=nokickstart" >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -f "${A2065:-/nonexistent}" ] || {
    echo "result=badinvocation reason=noa2065" >&2; exit 2; }

command -v tcpdump >/dev/null || {
    echo "result=badinvocation reason=notcpdump" >&2; exit 2; }

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/rdnss-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$ADDIF" "$STAGE/AddNetInterface"
cp "$SHOW"  "$STAGE/ShowNetStatus"
cp "$HOSTC" "$STAGE/host"
cp "$NETSTAT" "$STAGE/netstat"

# No DHCP and no DEVS:Internet: see the note at the top.  CONFIGURE6 is spelled
# out although AUTO is also the default, because this test is about that path
# and a default is a bad thing to leave implicit here.
cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
ADDRESS=$V4ADDR
NETMASK=255.255.255.0
CONFIGURE6=AUTO
EOF

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface DEVS:NetInterfaces/eth0
wait $SETTLE
SYS:ShowNetStatus
SYS:netstat -r
SYS:host $NAME
EOF

# ------------------------------------------------------------- capture ---
#
# In the test, not from the router's good behaviour: the guest's solicitation
# is what pulls the advertisement, so the capture has to be listening before
# the emulator starts and has to outlive the solicitation retries.

TAG="rdnss"
CAP="$ROOT/build/rdnss-ra.txt"
SERIAL=$(serial_log_path "$TAG")
OUT="$ROOT/build/$TAG.out"
rm -f "$CAP" "$SERIAL" "$OUT"

tcpdump -i "$BACKEND" -n -vv -s0 -l \
        "icmp6 and ip6[40] == 134" > "$CAP" 2>"$CAP.err" &
CAP_PID=$!
trap 'kill '"$CAP_PID"' 2>/dev/null || true' EXIT

# tcpdump prints its "listening on" line once the socket is open.
for _ in $(seq 1 50); do
    grep -q "listening on" "$CAP.err" 2>/dev/null && break
    sleep 0.2
done
grep -q "listening on" "$CAP.err" 2>/dev/null || {
    echo "result=badinvocation reason=nocapture"
    sed -n '1,3p' "$CAP.err" >&2
    exit 2
}

# ------------------------------------------------------------------ run ---

set +e
AMINETXDUO_RUN_TAG="$TAG" AMINETXDUO_AMIBERRY_MAC="$MAC" \
    "$ROOT/tools/amiberry-run.sh" \
    -N a2065 -B "$BACKEND" -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/ShowNetStatus" "$STAGE/host" "$STAGE/netstat" \
    "$STAGE/commands.txt" \
    > "$OUT" 2>&1
run_rc=$?
set -e

kill "$CAP_PID" 2>/dev/null || true
wait "$CAP_PID" 2>/dev/null || true
trap - EXIT

if [ ! -s "$SERIAL" ] && [ ! -s "$OUT" ]; then
    echo "result=noguest run_rc=$run_rc"
    exit 3
fi

# --------------------------------------------------------- the capture ---

ra_count=$(grep -c "router advertisement" "$CAP" || true)
ra_rdnss=$(sed -n 's/.*rdnss option (25).*addr: \([0-9A-Fa-f:]*\).*/\1/p' "$CAP" \
           | head -1)
ra_dnssl=$(sed -n 's/.*dnssl option (31).*domain(s): \([^ ,]*\)\.$/\1/p' "$CAP" \
           | head -1)
# "2607:f598:e1a8:4c00::/64" -> "2607:f598:e1a8:4c00:", the stem every address
# formed from this advertisement begins with.
ra_prefix=$(sed -n 's|.*prefix info option (3).*: \([0-9A-Fa-f:]*\)::/64.*|\1:|p' \
            "$CAP" | head -1)

# The advertisement's own source: the link-local address that must end up in
# the guest's default router table.  RFC 4861 4.2 requires it be link-local.
ra_router=$(sed -n 's/.*[^0-9A-Fa-f:]\(fe80::[0-9A-Fa-f:]*\) > .*router advertisement.*/\1/p' \
            "$CAP" | head -1)

echo "ra_count=$ra_count ra_rdnss=${ra_rdnss:-none} ra_dnssl=${ra_dnssl:-none}"
echo "ra_prefix=${ra_prefix:-none} ra_router=${ra_router:-none}"

if [ "$ra_count" -eq 0 ]; then
    echo "result=nolink reason=no_ra_during_run run_rc=$run_rc"
    exit 4
fi
if [ -z "$ra_rdnss" ]; then
    echo "result=nolink reason=ra_carries_no_rdnss run_rc=$run_rc"
    exit 4
fi
if [ -z "$ra_dnssl" ]; then
    echo "result=nolink reason=ra_carries_no_dnssl run_rc=$run_rc"
    exit 4
fi
if [ -z "$ra_router" ]; then
    echo "result=nolink reason=ra_source_unreadable run_rc=$run_rc"
    exit 4
fi

# ----------------------------------------------------------- the guest ---
#
# A global address is a precondition, not an assertion: without one the guest
# has no source for a DNS query and every result below is the same shape as the
# defects being tested for.

# An address of the advertised prefix that is not the advertised name server,
# read out of what the guest itself printed. ami_netstack_mark("ip6-global") on
# the serial port says the same thing and is the second reading, but a build
# without AMINETXDUO_LOG has no serial port to say it on.

# Whether there is a serial log to read at all, once, so the three fields it
# feeds can say `notchecked` rather than `no`.  `no` and `not looked at` are
# different answers and were reported with the same word.
serial_have=no
if serial_log_have "$SERIAL" "$BUILD" \
                   "what the stack absorbed off the advertisement" 2>/dev/null
then
    serial_have=yes
fi

guest_global=no
if [ -n "$ra_prefix" ] &&
   grep -oiE "${ra_prefix}[0-9A-Fa-f:]+" "$OUT" 2>/dev/null \
       | grep -viF "$ra_rdnss" | grep -q .
then
    guest_global=yes
fi
if [ "$serial_have" = yes ] &&
   grep -qE "netstack: mark ip6-global " "$SERIAL"; then
    guest_global=yes
fi

echo "guest_global=$guest_global run_rc=$run_rc serial=$SERIAL out=$OUT"

if [ "$guest_global" = no ]; then
    echo "result=nolink reason=guest_has_no_global_address"
    exit 4
fi

# ShowNetStatus and host both print to the guest's stdout, which the harness
# copies into $OUT -- ALONG WITH the serial log, so the address has to be found
# in the report itself and not merely somewhere in the file. ShowNetStatus
# writes "Name servers:   <address>" and then one indented address per line.
#
# No trailing-anchor on any of these: the guest's lines end CR LF.
ns6_reported=no
sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
    | grep -qiE "^(Name servers: +| +)${ra_rdnss}[[:space:]]*$" &&
    ns6_reported=yes

# Both of these are AMI_INFO lines, so they exist only in a build with
# AMINETXDUO_LOG.  With the log compiled out the serial file is 0 bytes and
# these two greps answered `no` -- the same word the real negative uses -- on
# every run.  `notchecked` is the honest third value.
ns6_absorbed=notchecked
dnssl_absorbed=notchecked
if [ "$serial_have" = yes ]; then
    ns6_absorbed=no
    grep -qiE "netstack: advertised name server ${ra_rdnss}" "$SERIAL" &&
        ns6_absorbed=yes

    dnssl_absorbed=no
    grep -qiF "advertised search list" "$SERIAL" && dnssl_absorbed=yes
fi

# The only route from "playhouse2" to an address is the advertised suffix:
# there is no hosts file, no DOMAIN and no SEARCH on this guest.
short_resolved=no
sed -n "/^===== SYS:host $NAME/,/^----- rc/p" "$OUT" 2>/dev/null \
    | grep -qE "^$NAME (has address|is) " && short_resolved=yes

echo "ns6_reported=$ns6_reported ns6_absorbed=$ns6_absorbed"
echo "dnssl_absorbed=$dnssl_absorbed short_resolved=$short_resolved"

# An address formed from an advertisement must be reported as one: ShowNetStatus
# marks it (advertised), which is what tells it apart from a static address.
ns6_origin=no
sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
    | grep -E "^  address6 +${ra_prefix}[0-9A-Fa-f:]+/[0-9]+ \(advertised\)" \
    | grep -q . && ns6_origin=yes

echo "ns6_origin=$ns6_origin"

# The advertisement's sender must be in the default router table, and netstat
# prints that table as `::/0` with the next hop beside it.
ra_default_route=no
sed -n '/^===== SYS:netstat -r/,/^----- rc/p' "$OUT" 2>/dev/null \
    | grep -E '(^|[[:space:]])::/0[[:space:]]' | grep -qiF "$ra_router" &&
    ra_default_route=yes

echo "ra_default_route=$ra_default_route ra_router=${ra_router:-none}"

fail=0
[ "$ra_default_route" = yes ] || { echo "FAIL ra_default_route: netstat -r shows no ::/0 via $ra_router"; fail=1; }
[ "$ns6_origin" = yes ] || { echo "FAIL ns6_origin: ShowNetStatus does not mark the advertised address (advertised)"; fail=1; }
[ "$ns6_reported"   = yes ] || { echo "FAIL ns6_reported: ShowNetStatus does not list $ra_rdnss"; fail=1; }
[ "$short_resolved" = yes ] || { echo "FAIL short_resolved: '$NAME' did not resolve through $ra_dnssl"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "result=fail"
    exit 1
fi

echo "result=pass"
exit 0
