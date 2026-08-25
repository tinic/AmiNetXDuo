#!/usr/bin/env bash
# THE REGRESSION TEST FOR THE IPv4 ROUTING TABLE.
#   bridged only: under SLIRP the frames touch no NIC and there is nothing to
#   read at all.  -B names the NIC and the string `slirp` is refused outright.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "routes_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

PROBE="$ROOT/$BUILD/tests/tools/RouteProbe"
RTPROBE="$ROOT/$BUILD/tests/tools/RtProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$PROBE" "$RTPROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

STAGE="$ROOT/build/routes-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface netstat; do
    cp "$TOOLS/$t" "$STAGE/$t"
done
cp "$PROBE" "$STAGE/RouteProbe"
cp "$RTPROBE" "$STAGE/RtProbe"

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:netstat -r
SYS:RouteProbe
SYS:netstat -r
SYS:RtProbe
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-routes}"
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_ROUTES_MAC:-02:41:4d:49:00:c7}"

WIRE=""
WIRE_PID=""
if command -v tcpdump >/dev/null 2>&1; then
    WIRE="$STAGE/wire.pcap"
    tcpdump -i "$IFACE" -n -s0 -U -w "$WIRE" arp >/dev/null 2>&1 &
    WIRE_PID=$!
    sleep 2
    if ! kill -0 "$WIRE_PID" 2>/dev/null; then
        echo "==> tcpdump could not capture on $IFACE, the wire check will skip" >&2
        WIRE=""; WIRE_PID=""
    else
        echo "==> capturing ARP on $IFACE into $WIRE (pid $WIRE_PID)"
    fi
fi

set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/RouteProbe" \
    "$STAGE/RtProbe"
RUN_RC=$?
set -e

if [ -n "$WIRE_PID" ]; then
    kill "$WIRE_PID" 2>/dev/null || true
    wait "$WIRE_PID" 2>/dev/null || true
fi

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
UNRUN=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
skip() { echo "  --: $*"; UNRUN=$((UNRUN + 1)); }

RP_HOP=$(sed -n 's/^route next hop: \([0-9.]*\)$/\1/p' "$REPORT" | head -1)
HOP_A=$(sed -n 's/^next hops: \([0-9.]*\) then [0-9.]*$/\1/p' "$REPORT" | head -1)
HOP_B=$(sed -n 's/^next hops: [0-9.]* then \([0-9.]*\)$/\1/p' "$REPORT" | head -1)

STARTS=$(grep -c "SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted $STARTS times"
else
    fail "the run did not get as far as bringing the interface up"
fi

if grep -q "^static routing: compiled in" "$REPORT"; then
    pass "NX_ENABLE_IP_STATIC_ROUTING is in the running stack"
else
    fail "the running stack reports no routing table"
fi

if grep -Eq "^add 192\\.168\\.77\\.0/24 via [0-9.]+: 0 " "$REPORT"; then
    pass "NETCTRL_ROUTE_ADD accepted 192.168.77.0/24 via $RP_HOP"
else
    fail "NETCTRL_ROUTE_ADD did not accept the route"
fi

SHOWN=$(grep -c "^  192\.168\.77\.0 " "$REPORT" || true)
if [ "$SHOWN" -eq 1 ]; then
    pass "the route is in the 'with' listing and in neither of the other two"
else
    fail "the route appears $SHOWN times in the three listings, expected 1"
fi

if [ -n "$RP_HOP" ] &&
   grep -Eq "^  192\.168\.77\.0 +${RP_HOP//./\\.} +255\.255\.255\.0 +U?G?S" "$REPORT"; then
    pass "it is flagged S (added by hand) with $RP_HOP as its next hop"
else
    fail "the route is not reported with the S flag and the right next hop"
fi

if grep -q "8.8.8.8: .*, refused, correctly" "$REPORT"; then
    pass "a next hop on no local subnet was refused"
else
    fail "8.8.8.8 was accepted as a next hop, or the call did not run"
fi

if grep -q "never added): .*, refused, correctly" "$REPORT"; then
    pass "deleting a route that is not in a non-empty table failed"
else
    fail "deleting an absent route reported success"
fi

if grep -q "^delete 192.168.77.0/24: 0 " "$REPORT"; then
    pass "NETCTRL_ROUTE_DELETE removed the route"
else
    fail "NETCTRL_ROUTE_DELETE did not remove the route"
fi

if grep -q "^Routing table" "$REPORT" && grep -q "127.0.0.0" "$REPORT"; then
    pass "netstat -r printed the table from NETSTATUS_ROUTES"
else
    fail "netstat -r printed no routing table"
fi

PCAP=""
[ -s "${WIRE:-}" ] && PCAP="$WIRE"
[ -z "$PCAP" ] && [ -s "$HD/host.pcap" ] && PCAP="$HD/host.pcap"

if [ -n "$PCAP" ] && [ -n "$RP_HOP" ]; then
    ARP=$(tcpdump -r "$PCAP" -n 2>/dev/null |
          grep -c "who-has $RP_HOP" || true)
    if [ "${ARP:-0}" -gt 0 ]; then
        pass "the wire shows $ARP ARP request(s) for $RP_HOP, the route was used"
    else
        fail "no ARP for $RP_HOP on the wire: the route was not consulted"
        tcpdump -r "$PCAP" -n 2>/dev/null | grep -i arp | head -20 >&2 || true
    fi

    LEAKED=$(tcpdump -r "$PCAP" -n 2>/dev/null |
             grep -c "> 192.168.77.5" || true)
    if [ "${LEAKED:-0}" -eq 0 ]; then
        pass "nothing for 192.168.77.5 went out via the default gateway"
    else
        fail "$LEAKED packet(s) for 192.168.77.5 left the machine unrouted"
    fi
else
    skip "no wire to read: this host has no tcpdump that can open $IFACE, where the
       frames are on a real NIC and this script captures them"
fi

if [ -n "$WIRE" ] && [ -s "$WIRE" ] && [ -n "${HOP_A:-}" ] && [ -n "${HOP_B:-}" ]; then
    tcpdump -r "$WIRE" -n 2>/dev/null > "$STAGE/wire.txt" || true
    A_FIRST=$(grep -n "who-has $HOP_A" "$STAGE/wire.txt" | head -1 | cut -d: -f1)
    B_FIRST=$(grep -n "who-has $HOP_B" "$STAGE/wire.txt" | head -1 | cut -d: -f1)

    if [ -n "$A_FIRST" ]; then
        pass "the wire shows ARP for $HOP_A, the route's first next hop"
    else
        fail "no ARP for $HOP_A: the route was not consulted before the change"
    fi

    if [ -n "$B_FIRST" ]; then
        pass "and ARP for $HOP_B, the next hop the change installed"
    else
        fail "no ARP for $HOP_B: the packets did not follow the changed route"
    fi

    if [ -n "$A_FIRST" ] && [ -n "$B_FIRST" ] && [ "$B_FIRST" -gt "$A_FIRST" ]; then
        pass "in that order: $HOP_A first, $HOP_B only after the change"
    else
        fail "the two next hops were not ARPed in the order the changes happened"
        grep -E "who-has ($HOP_A|$HOP_B)" "$STAGE/wire.txt" | head -20 >&2 || true
    fi
elif [ -n "$WIRE" ]; then
    skip "the capture on $IFACE is empty, so nothing was read off the wire"
else
    skip "no capture on $IFACE (tcpdump could not open it): the change was checked
       against the table only, not against what left the card.  Run with
       -A -B <iface> on a bridged host for the wire half"
fi

if grep -Eq "^add 192\.168\.66\.0 via [0-9.]+: rc 0 " "$REPORT"; then
    pass "AddRouteTagList added a route with no netmask tag in the grammar"
else
    fail "AddRouteTagList refused 192.168.66.0"
fi

if grep -Eq "^  with +192\.168\.66\.0 +[0-9.]+ +255\.255\.255\.0 +UGS" "$REPORT"; then
    pass "and the classful mask was derived from the address alone"
else
    fail "192.168.66.0 did not come back as a /24 gateway route"
fi

if grep -Eq "^  with +192\.168\.67\.7 +[0-9.]+ +255\.255\.255\.255 +UGHS" "$REPORT"; then
    pass "a destination with a host part became a host route, flagged H"
else
    fail "192.168.67.7 did not come back as a /32 host route"
fi

if grep -Eq "^  before .* v3 if " "$REPORT"; then
    pass "every entry reports rtm_version 3, as the autodoc specifies"
else
    fail "the table entries do not carry rtm_version 3"
fi

if grep -Eq "^  before .* mtu 1500$" "$REPORT"; then
    pass "the interface route carries its MTU in rtm_rmx"
else
    fail "no entry carries an MTU"
fi

if grep -q "^routes static-only: 2 entries" "$REPORT"; then
    pass "the flags filter returned exactly the two RTF_STATIC entries"
else
    fail "GetRouteInfo(AF_INET, RTF_STATIC) did not return exactly two entries"
fi

if grep -Eq "^change 192\.168\.66\.0 to via [0-9.]+: rc 0 " "$REPORT"; then
    pass "ChangeRouteTagList changed the route's next hop"
else
    fail "ChangeRouteTagList did not change 192.168.66.0"
fi

if [ -n "$HOP_B" ] && grep -Eq "^  changed +192\.168\.66\.0 +${HOP_B//./\\.} +255\.255\.255\.0 +UGS" "$REPORT"; then
    pass "the table reads back 192.168.66.0 via $HOP_B, the new next hop"
else
    fail "the table does not report 192.168.66.0 via the new next hop"
fi

if [ -n "$HOP_A" ] && grep -Eq "^  changed +192\.168\.66\.0 +${HOP_A//./\\.} " "$REPORT"; then
    fail "the old next hop $HOP_A is still in the table after the change"
else
    pass "and the old next hop is gone from it"
fi

if [ "$(grep -c '^  changed ' "$REPORT")" -eq 2 ]; then
    pass "the change left two static entries, not one and not three"
else
    fail "the static table has $(grep -c '^  changed ' "$REPORT") entries after the change, expected 2"
fi

for case in "a route never added" "with no gateway" "dest+default together" \
            "to an unreachable next hop"; do
    if grep -q "^change $case: .*, refused, correctly" "$REPORT"; then
        pass "ChangeRouteTagList refused: $case"
    else
        fail "ChangeRouteTagList accepted: $case"
    fi
done

if [ -n "$HOP_B" ] && grep -Eq "^  survived +192\.168\.66\.0 +${HOP_B//./\\.} " "$REPORT"; then
    pass "and a refused change left the route exactly as it was"
else
    fail "a refused change disturbed the route"
fi

if grep -Eq "^change the default gateway to [0-9.]+: rc 0 " "$REPORT"; then
    pass "the default gateway can be changed"
else
    fail "ChangeRouteTagList refused the default gateway"
fi

if grep -q "^default gateway after the change: .*, unmoved, correctly" "$REPORT"; then
    pass "and naming the installed one left it where it was"
else
    fail "the default gateway moved when it was changed to its own address"
fi

for case in "dest+default together" "dest with no gateway" \
            "via an unreachable next hop"; do
    if grep -q "^add $case: .*, refused, correctly" "$REPORT"; then
        pass "AddRouteTagList refused: $case"
    else
        fail "AddRouteTagList accepted: $case"
    fi
done

if grep -q "^GetRouteInfo(AF_INET6): NULL .*, refused, correctly" "$REPORT"; then
    pass "GetRouteInfo refuses an address family it has no table for"
else
    fail "GetRouteInfo(AF_INET6) returned a table"
fi

if grep -q "^delete a route never added: .*, refused, correctly" "$REPORT"; then
    pass "deleting an absent route fails while the table is not empty"
else
    fail "deleting an absent route reported success"
fi

if grep -q "^delete the default gateway by the wrong address: .*, refused, correctly" "$REPORT"; then
    pass "deleting the default gateway by the wrong address is refused"
else
    fail "the default gateway was deleted by an address that is not the one installed"
fi

if grep -q "^default gateway after that: .*, still there, correctly" "$REPORT"; then
    pass "and the real default gateway survived it"
else
    fail "the default gateway went away"
fi

if grep -q "^counts: .*, two added and two removed, correctly" "$REPORT"; then
    pass "the table came back to exactly what it was"
else
    fail "the routes added through the published API were not all removed"
fi

if grep -q "^FreeRouteInfo(NULL): returned" "$REPORT"; then
    pass "FreeRouteInfo(NULL) did nothing, as documented"
else
    fail "FreeRouteInfo(NULL) did not return"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "routes: FAILED" >&2
    exit 1
fi

if [ "$UNRUN" -ne 0 ]; then
    echo "routes: SKIPPED, $UNRUN assertion group(s) did not run" >&2
    echo "  The wire is what this test is for: nothing else here can tell a" >&2
    echo "  stack that consulted the routing table from one that sent to the" >&2
    echo "  default gateway and printed a table it never used." >&2
    exit 77
fi

echo "routes: PASSED"
exit 0
