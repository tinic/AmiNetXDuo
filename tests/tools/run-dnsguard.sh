#!/usr/bin/env bash
# THE REGRESSION TEST FOR TWO PROPERTIES OF THE DNS CLIENT.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
ADDRESS="${AMINETXDUO_DNSGUARD_ADDRESS:-192.168.1.246}"
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
BOARD=a2065
IFDEVICE="${AMINETXDUO_IFDEVICE:-a2065.device}"
ZONE="${AMINETXDUO_DNSGUARD_ZONE:-dnsguard.test}"
PEER="${AMINETXDUO_DNSGUARD_PEER:-}"
PEER_ADDR="${AMINETXDUO_DNSGUARD_PEER_ADDR:-}"
REALNAME="${AMINETXDUO_DNSGUARD_REALNAME:-www.github.com}"

while getopts "m:t:b:B:a:g:N:H:A:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        *) echo "usage: $0 -H user@host [-A addr] [-m model] [-t seconds] [-b builddir] [-B backend] [-a address] [-g gateway] [-N board]" >&2; exit 2 ;;
    esac
done

[ -n "$PEER" ] || {
    echo "set AMINETXDUO_DNSGUARD_PEER=<user@host> or pass -H: the name server" >&2
    echo "cannot live on the machine running the emulator, see the header" >&2
    exit 2; }

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/host" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

case "$BACKEND" in
    slirp|slirp_inbound)
        echo "!! $BACKEND cannot reach a name server on this host; -B <interface>" >&2
        exit 2 ;;
esac

if [ -z "$PEER_ADDR" ]; then
    PEER_ADDR=$(ssh "$PEER" "ip -o -4 addr show scope global |
                             awk '{split(\$4,a,\"/\"); print a[1]}'" |
                head -1)
fi
[ -n "$PEER_ADDR" ] || {
    echo "$PEER did not report an address of its own; pass -A <addr>" >&2
    exit 2; }
SERVER="$PEER_ADDR"

PYTHON="${AMINETXDUO_DNS_PYTHON:-\$HOME/bin/python3-dns}"

TAG="${AMINETXDUO_RUN_TAG:-dnsguard}"
export AMINETXDUO_RUN_TAG="$TAG"
HD="$ROOT/build/amiberry-testhd-$TAG"
SRVLOG="$ROOT/build/dnsguard-$TAG-server.log"

export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:49:0d:47}"


PEER_LOG="/tmp/dnsguard-$TAG-peer.log"
KILLPAT="[d]nsfake.py --zone $ZONE"

OFFLOAD=$(ssh "$PEER" "/sbin/ethtool -k \$(ip -o -4 route show to default |
                       awk '{print \$5}') 2>/dev/null |
                       awk '/^tx-checksumming/{print \$2}'" || true)
if [ "$OFFLOAD" != "off" ]; then
    echo "!! the peer's TX checksum offload is '$OFFLOAD', not 'off'." >&2
    echo "!! On the peer: sudo ethtool -K <iface> tx off gso off tso off" >&2
    exit 2
fi

UPSTREAM=$(ssh "$PEER" "awk '/^nameserver/ && \$2 !~ /:/ {print \$2; exit}' /etc/resolv.conf")
[ -n "$UPSTREAM" ] || {
    echo "$PEER has no IPv4 name server of its own to forward to" >&2; exit 2; }

REAL_CNAME=$(ssh "$PEER" "dig +short CNAME $REALNAME @$UPSTREAM 2>/dev/null | head -1" || true)
[ -n "$REAL_CNAME" ] || {
    echo "!! $PEER sees no CNAME for $REALNAME, so case 4 would test nothing." >&2
    echo "!! Set AMINETXDUO_DNSGUARD_REALNAME to a name that is still an alias." >&2
    exit 2; }

ssh "$PEER" "pkill -f \"$KILLPAT\" || true" >/dev/null 2>&1 || true
scp -q "$ROOT/tests/tools/dnsfake.py" "$PEER:/tmp/dnsfake.py" || {
    echo "could not stage the server on $PEER" >&2; exit 2; }

ssh "$PEER" "nohup $PYTHON /tmp/dnsfake.py --zone $ZONE --bind $SERVER \
             --forward $UPSTREAM \
             < /dev/null > $PEER_LOG 2>&1 & sleep 1; head -1 $PEER_LOG" \
    > /dev/null 2>&1 || true

cleanup() { ssh "$PEER" "pkill -f \"$KILLPAT\" || true" >/dev/null 2>&1 || true; }
trap cleanup EXIT

READY=no
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if ssh "$PEER" "grep -q '^READY ' $PEER_LOG" 2>/dev/null; then
        READY=yes
        break
    fi
    sleep 1
done

[ "$READY" = yes ] || {
    echo "!! the fake name server did not come up on $PEER:" >&2
    ssh "$PEER" "cat $PEER_LOG" 2>/dev/null | sed 's/^/!! /' >&2
    echo "!! port 53 is privileged; see the header of this script" >&2
    exit 2; }

echo "==> fake name server on $PEER ($SERVER:53), zone $ZONE"
echo "==> everything else forwarded to $UPSTREAM; $REALNAME is CNAME $REAL_CNAME"


STAGE="$ROOT/build/dnsguard-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface host; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$IFDEVICE
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

cat > "$STAGE/devs/Internet/name_resolution" <<EOF
nameserver $SERVER
EOF

{
    echo "SYS:AddNetInterface eth0"
    echo "SYS:host -4 plain.$ZONE"
    echo "SYS:host -4 alias.$ZONE"
    echo "SYS:host -4 evil.$ZONE"
    echo "SYS:host -4 $REALNAME"
    echo "SYS:host -4 nx.$ZONE"
    echo "SYS:host -4 nx.$ZONE"
    echo "SYS:host -4 nx.$ZONE"
    echo "SYS:host 10.0.0.1"
    echo "SYS:host 10.0.0.9"
    echo "SYS:host 10.0.0.9"
    echo "SYS:host 10.0.0.9"
} > "$STAGE/commands.txt"


echo "==> booting $MODEL, $BOARD bridged on $BACKEND, guest $ADDRESS"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/host"
RUN_RC=$?
set -e

ssh "$PEER" "cat $PEER_LOG" > "$SRVLOG" 2>/dev/null || : > "$SRVLOG"
cleanup
trap - EXIT

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || {
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    echo "dnsguard=fail reason=no-report"
    exit 1; }

echo
echo "==================== what the commands printed ===================="
cat "$REPORT"
echo "==================== what the server was asked ===================="
grep '^Q ' "$SRVLOG" || echo "(nothing)"
echo "==================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

queries() { grep -c "^Q $1 " "$SRVLOG" 2>/dev/null || true; }


PLAIN_Q=$(queries "plain.$ZONE")
if grep -q "^plain.$ZONE has address 10.0.0.1$" "$REPORT"; then
    pass "plain.$ZONE resolved to 10.0.0.1 ($PLAIN_Q queries)"
    CONTROL=ok
else
    fail "plain.$ZONE did not resolve; the guest never reached the server"
    CONTROL=fail
fi


if grep -q "^alias.$ZONE has address 10.0.0.2$" "$REPORT"; then
    pass "alias.$ZONE followed the CNAME to 10.0.0.2"
    CNAME=ok
else
    fail "alias.$ZONE did not resolve; the owner-name check landed without the chain"
    CNAME=fail
fi


if grep -q "^$REALNAME has address " "$REPORT"; then
    pass "$REALNAME resolved through its real CNAME ($REAL_CNAME)"
    REAL=ok
else
    fail "$REALNAME did not resolve; a real CNAME chain is not being followed"
    REAL=fail
fi


if grep -q "^evil.$ZONE has address" "$REPORT"; then
    fail "evil.$ZONE resolved: a record owned by attacker.example was accepted"
    BAILIWICK=fail
else
    pass "evil.$ZONE was refused; the answer's owner name was checked"
    BAILIWICK=ok
fi


NX_Q=$(queries "nx.$ZONE")

# The first of the two lines host prints for a name it could not resolve;
# tool_explain_resolve() adds a second, so an unanchored count reads double.
NX_FAILS=$(grep -c "^host: cannot resolve \"nx.$ZONE\"$" "$REPORT" || true)
if [ "$NX_FAILS" -eq 3 ]; then
    pass "nx.$ZONE failed all three times"
else
    fail "nx.$ZONE reported failure $NX_FAILS times, expected 3"
fi

if [ "$NX_Q" -eq 1 ]; then
    pass "nx.$ZONE was asked for exactly ONCE in three lookups"
    NEGCACHE=ok
else
    fail "nx.$ZONE was asked for $NX_Q times in three lookups, expected 1"
    NEGCACHE=fail
fi


if grep -q "^10.0.0.1 is plain.$ZONE$" "$REPORT"; then
    pass "10.0.0.1 reversed to plain.$ZONE"
    REVERSE=ok
else
    fail "10.0.0.1 did not reverse; the PTR path answered nothing"
    REVERSE=fail
fi

RNX_Q=$(queries "9.0.0.10.in-addr.arpa")
if [ "$RNX_Q" -eq 1 ]; then
    pass "10.0.0.9 was asked for exactly ONCE in three reverse lookups"
    RNEGCACHE=ok
else
    fail "10.0.0.9 was asked for $RNX_Q times in three reverse lookups, expected 1"
    RNEGCACHE=fail
fi

echo
echo "control=$CONTROL cname=$CNAME realcname=$REAL bailiwick=$BAILIWICK negcache=$NEGCACHE revnegcache=$RNEGCACHE reverse=$REVERSE nxqueries=$NX_Q revnxqueries=$RNX_Q plainqueries=$PLAIN_Q"

if [ "$FAILED" -ne 0 ]; then
    echo "dnsguard=fail"
    exit 1
fi

echo "dnsguard=pass"
exit 0
