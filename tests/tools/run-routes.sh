#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR THE IPv4 ROUTING TABLE.
#
#   tests/tools/run-routes.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT IS PROVING
#
#   NX_ENABLE_IP_STATIC_ROUTING is a `#define` in port/netxduo-amiga/inc/
#   nx_user.h, and a `#define` that changes no packet is not a feature.  So
#   this test does not stop at "AddNetRoute printed something": it makes the
#   guest send to an address that ONLY a static route can reach, and then
#   looks for the consequence on the wire.
#
#   The consequence is chosen so that it cannot be produced any other way.
#   The next hops are .249, .250 and .251 of the guest's own subnet, derived at
#   run time because a fixed address is only on-subnet on the network it was
#   written for, and NetX Duo refuses one that is not.  Sending to 192.168.77.5:
#
#     * with the route, _nx_ip_route_find() matches the table entry,
#                             the next hop becomes .249, and the stack
#                             emits  ARP who-has <.249>  and nothing else;
#     * without the route , the default gateway is used, whose ARP entry the
#                             DHCP exchange already resolved, so the frame goes
#                             straight out with no ARP at all.
#
#   That ARP request therefore appears if and only if the routing table was
#   consulted.  It is read off the host NIC, which needs a bridged run: -A -B
#   <iface>.  Under SLIRP the frames touch no NIC and there is nothing to read,
#   which is why the wire assertions skip there rather than pass.
#
# WHAT ELSE IT ASSERTS
#
#   * NETSTATUS_ROUTES reports the entry, with the S flag that marks it as one
#     somebody added rather than one derived from an interface, and stops
#     reporting it after the delete.
#   * a next hop that is NOT on any of the machine's own subnets is refused
#     rather than stored, and so is deleting a route that is not there.  That
#     is NetX Duo's rule, and "the call returned success" is not evidence that
#     anything was checked.
#   * netstat -r prints the same table, through the same renderer
#     ShowNetStatus uses, so the two commands cannot disagree about it.
#   * ChangeRouteTagList moves a route's next hop, and the next packet for that
#     destination goes to the NEW one.  Same shape of evidence as above and for
#     the same reason: "the table reads back differently" is what a vector that
#     wrote to the wrong table would also produce.  This half needs a host NIC
#     to capture on, so it runs under -A -B <iface> and skips otherwise.
#
# The route is added through NETCTRL_ROUTE_ADD by tests/tools/routeprobe.c
# rather than by AddNetRoute: what is under test is the stack, and a test that
# went through a command's ReadArgs template would fail whenever the template
# changed.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
# FS-UAE needs an X server; on a headless Linux box it dies in GLAD before the
# guest boots, so -A picks Amiberry, which runs genuinely headless.
RUNNER="${AMINETXDUO_RUNNER:-fsuae}"
BOARD=a2065
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"

while getopts "m:t:b:AN:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-A [-N board] [-B backend]]" >&2; exit 2 ;;
    esac
done

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

# ------------------------------------------------------------- staging ---

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

# The order is the experiment.  RouteProbe prints the table before, with and
# after the route, so the transcript shows it changing rather than only its
# final state; netstat -r is run either side to prove the shipped command sees
# the same table the library reports.
cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:netstat -r
SYS:RouteProbe
SYS:netstat -r
SYS:RtProbe
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-routes}"

# ---- the wire, when there is one to watch --------------------------------
#
# Amiberry writes no frame log, so the a2065pcap.py route the FS-UAE-era
# assertions below took does not exist here.  Bridged onto a host NIC there is
# something better: the frames really are on that NIC, so tcpdump on the host
# sees exactly what left the card.  Captured for the whole run and read after
# it; ARP only, which is all the assertions look at and keeps the file small.
WIRE=""
WIRE_PID=""
if [ "$RUNNER" = "amiberry" ] && [ "$IFACE" != "slirp" ] &&
   command -v tcpdump >/dev/null 2>&1; then
    WIRE="$STAGE/wire.pcap"
    tcpdump -i "$IFACE" -n -s0 -U -w "$WIRE" arp >/dev/null 2>&1 &
    WIRE_PID=$!
    # tcpdump opens its socket asynchronously; without this the guest's first
    # frames are missed and the capture reads like a stack that sent nothing.
    sleep 2
    if ! kill -0 "$WIRE_PID" 2>/dev/null; then
        echo "==> tcpdump could not capture on $IFACE, the wire check will skip" >&2
        WIRE=""; WIRE_PID=""
    else
        echo "==> capturing ARP on $IFACE into $WIRE (pid $WIRE_PID)"
    fi
fi

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/RouteProbe" \
        "$STAGE/RtProbe"
else
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/RouteProbe" \
        "$STAGE/RtProbe"
fi
RUN_RC=$?
set -e

# By pid, never by name: other runs on this host have tcpdumps of their own.
if [ -n "$WIRE_PID" ]; then
    kill "$WIRE_PID" 2>/dev/null || true
    wait "$WIRE_PID" 2>/dev/null || true
fi

SERIAL="$ROOT/build/serial-$AMINETXDUO_RUN_TAG.log"
REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
# An assertion that did not run is counted, not just printed.  See the verdict
# at the end: it is what stops this exiting 0 with its own subject untested.
UNRUN=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
skip() { echo "  --: $*"; UNRUN=$((UNRUN + 1)); }

# The two next hops RtProbe derived, .250 and .251 of whatever subnet the
# machine is on.  Read here rather than beside the assertions that use them:
# the wire check below needs them too and comes first.
RP_HOP=$(sed -n 's/^route next hop: \([0-9.]*\)$/\1/p' "$REPORT" | head -1)
HOP_A=$(sed -n 's/^next hops: \([0-9.]*\) then [0-9.]*$/\1/p' "$REPORT" | head -1)
HOP_B=$(sed -n 's/^next hops: [0-9.]* then \([0-9.]*\)$/\1/p' "$REPORT" | head -1)

# ---- one boot, as ever (docs/RESEARCH.md 25) ------------------------------
#
# The serial log is the usual boot counter, but FS-UAE writes nothing to it on
# this host, so the transcript is used instead: ToolsSmoke reopens DH0:tools.txt
# from the top after a reset, so the FIRST command appearing twice is a reboot.
# A crash that resets the machine is a different and much worse defect than a
# command that blocks, and a test that cannot tell them apart sends the next
# person looking in the wrong place.
STARTS=$(grep -c "SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted $STARTS times"
else
    fail "the run did not get as far as bringing the interface up"
fi

# ---- the table is compiled in -------------------------------------------
if grep -q "^static routing: compiled in" "$REPORT"; then
    pass "NX_ENABLE_IP_STATIC_ROUTING is in the running stack"
else
    fail "the running stack reports no routing table"
fi

# ---- the entry went in, and came back out --------------------------------
if grep -Eq "^add 192\\.168\\.77\\.0/24 via [0-9.]+: 0 " "$REPORT"; then
    pass "NETCTRL_ROUTE_ADD accepted 192.168.77.0/24 via $RP_HOP"
else
    fail "NETCTRL_ROUTE_ADD did not accept the route"
fi

# Reported exactly once, in the "with" listing and in neither of the others.
# Counting is the assertion: a single "is it there" grep could not tell a
# table that gained a route from one that always had it.
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

# ---- the negative cases --------------------------------------------------
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

# ---- and the shipped command shows the same table ------------------------
if grep -q "^Routing table" "$REPORT" && grep -q "127.0.0.0" "$REPORT"; then
    pass "netstat -r printed the table from NETSTATUS_ROUTES"
else
    fail "netstat -r printed no routing table"
fi

# ---- THE WIRE ------------------------------------------------------------
#
# The whole point.  The emulator's own frame log, converted, must contain an
# ARP request for the route's next hop, an address nothing in this test ever
# named to the stack except through AddNetRoute.
# $WIRE is the capture this script took on the host NIC, which exists on a
# bridged run; $HD/host.pcap is a2065pcap.py's decode of FS-UAE's own frame
# log, which exists on no runner that is left.  Either is the wire.
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

    # And nothing addressed to 192.168.77.5 may have gone to the DEFAULT
    # gateway's hardware address, which is what a build without the table
    # would have done.  Checked as "no IP packet for 192.168.77.5 left at
    # all", because the ARP never resolves, so the queued packet is dropped.
    LEAKED=$(tcpdump -r "$PCAP" -n 2>/dev/null |
             grep -c "> 192.168.77.5" || true)
    if [ "${LEAKED:-0}" -eq 0 ]; then
        pass "nothing for 192.168.77.5 went out via the default gateway"
    else
        fail "$LEAKED packet(s) for 192.168.77.5 left the machine unrouted"
    fi
else
    # a2065pcap.py decoded the capture out of FS-UAE's own log, and FS-UAE is
    # gone: Amiberry writes no equivalent, on either branch.  Bridged onto a
    # host NIC there is a real capture instead, which is why this arm is now
    # reached only on SLIRP, where the frames never touch a NIC at all.
    #
    # And skipped is not zero.  "ARP for the next hop appears if and only if
    # the routing table was consulted" is the whole claim of this file; every
    # other assertion here reads what a command PRINTED, which a build with no
    # routing table at all can print correctly.  The verdict at the end exits
    # 77 for it.
    skip "no wire to read: run with -A -B <iface> on a bridged host, where the
       frames are on a real NIC and this script captures them"
fi

# ---- THE WIRE AGAIN, FOR THE CHANGE --------------------------------------
#
# The claim ChangeRouteTagList has to earn: not that the table reads back
# differently, but that the PACKETS go somewhere else afterwards.  RtProbe
# sends to 192.168.66.5 once before the change and once after, and 192.168.66.5
# can only leave this machine by the route it just added, so the stack ARPs for
# whichever next hop that route names.  Two requests, for two different
# addresses, in that order, and nothing else could have produced them.
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

    # FIRST against FIRST, not last against first.  ARP is retried while a
    # packet waits on it, so the datagram sent before the change goes on asking
    # for $HOP_A for as long as it is queued, which is after the change and is
    # correct: that packet was routed when the old next hop was the one.  What
    # cannot happen unless the change took is a request for $HOP_B at all, and
    # what orders the two is which poke produced the first of each.
    if [ -n "$A_FIRST" ] && [ -n "$B_FIRST" ] && [ "$B_FIRST" -gt "$A_FIRST" ]; then
        pass "in that order: $HOP_A first, $HOP_B only after the change"
    else
        fail "the two next hops were not ARPed in the order the changes happened"
        grep -E "who-has ($HOP_A|$HOP_B)" "$STAGE/wire.txt" | head -20 >&2 || true
    fi
elif [ -n "$WIRE" ]; then
    skip "the capture on $IFACE is empty, so nothing was read off the wire"
else
    skip "no host NIC to capture on (SLIRP is not one): the change was checked
       against the table only, not against what left the card.  Run with
       -A -B <iface> on a bridged host for the wire half"
fi

# ---- THE PUBLISHED ROUTING API -------------------------------------------
#
# Everything above drives the private NETCTRL_ROUTE_ADD vector.  RtProbe
# drives AddRouteTagList()/DeleteRouteTagList()/GetRouteInfo()/FreeRouteInfo()
# instead, the Roadshow ABI a third-party tool uses, and walks the
# returned table by rtm_msglen, which is the shape no build can check.

# The next hop is not named here: RtProbe derives it from the machine's own
# subnet, because NetX Duo refuses one that is on no interface and this file
# should not decide what network the emulator is bridged onto.
if grep -Eq "^add 192\.168\.66\.0 via [0-9.]+: rc 0 " "$REPORT"; then
    pass "AddRouteTagList added a route with no netmask tag in the grammar"
else
    fail "AddRouteTagList refused 192.168.66.0"
fi

# The mask is IMPLIED: 192.168.66.0 has a zero host part under its classful
# mask, so "the route is assumed to be a to a network" and 255.255.255.0 comes
# from nothing but the address.  192.168.67.7 does not, so it is a host route
# with a /32 and the H flag.
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

# GetRouteInfo's own shape: version 3 on every entry, and the MTU carried in
# rtm_rmx for the interface routes that have one.
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

# ---- ChangeRouteTagList ---------------------------------------------------
#
# The vector with no autodoc page (src/bsdsocket/routing.c).  Three claims:
# the change reaches the table, the entry keeps its mask and its count, and a
# change of a route that is not there is refused rather than installing one.
if grep -Eq "^change 192\.168\.66\.0 to via [0-9.]+: rc 0 " "$REPORT"; then
    pass "ChangeRouteTagList changed the route's next hop"
else
    fail "ChangeRouteTagList did not change 192.168.66.0"
fi

# The "changed" listing must carry the second next hop and not the first: a
# read-back is the only thing that separates a vector that returned 0 from one
# that did something.
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

# The refused change must not have taken the route with it.
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

# RTA_DefaultGateway names the gateway the route was installed with, so a
# delete that names a different one has named no entry.  Clearing regardless
# would take the machine's real default gateway away and report success.
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

# The whole of it: delete has to derive the same prefix length add did, from
# the same string and no mask, or the entries could never be found again.
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
# Failures first.  A run with both a failure and an unrun assertion is a
# failure: the skip is the less urgent of the two facts and reporting it first
# sends somebody to look at the rig instead of at the defect.
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
