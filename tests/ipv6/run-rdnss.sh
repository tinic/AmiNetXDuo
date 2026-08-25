#!/usr/bin/env bash
#
# DOES A ROUTER ADVERTISEMENT'S RESOLVER REACH THE RESOLVER.
#
#   tests/ipv6/run-rdnss.sh [-B BACKEND] [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                           [-n NAME] [-w SECONDS] [-M MAC]
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


STAGE="$ROOT/build/rdnss-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$ADDIF" "$STAGE/AddNetInterface"
cp "$SHOW"  "$STAGE/ShowNetStatus"
cp "$HOSTC" "$STAGE/host"
cp "$NETSTAT" "$STAGE/netstat"

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


ra_count=$(grep -c "router advertisement" "$CAP" || true)
ra_rdnss=$(sed -n 's/.*rdnss option (25).*addr: \([0-9A-Fa-f:]*\).*/\1/p' "$CAP" \
           | head -1)
ra_dnssl=$(sed -n 's/.*dnssl option (31).*domain(s): \([^ ,]*\)\.$/\1/p' "$CAP" \
           | head -1)
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

# No trailing-anchor on any of these: the guest's lines end CR LF.
ns6_reported=no
sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
    | grep -qiE "^(Name servers: +| +)${ra_rdnss}[[:space:]]*$" &&
    ns6_reported=yes

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

ns6_origin=no
sed -n '/^===== SYS:ShowNetStatus/,/^----- rc/p' "$OUT" 2>/dev/null \
    | grep -E "^  address6 +${ra_prefix}[0-9A-Fa-f:]+/[0-9]+ \(advertised\)" \
    | grep -q . && ns6_origin=yes

echo "ns6_origin=$ns6_origin"

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
