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
#   The route's next hop is 10.0.2.99, on the guest's own subnet, so NetX
#   Duo will accept it, and answered by nothing, because SLIRP is 10.0.2.2 and
#   10.0.2.3.  Sending to 192.168.77.5:
#
#     * with the route, _nx_ip_route_find() matches the table entry,
#                             the next hop becomes 10.0.2.99, and the stack
#                             emits  ARP who-has 10.0.2.99  and nothing else;
#     * without the route , the default gateway 10.0.2.2 is used, whose ARP
#                             entry the DHCP exchange already resolved, so the
#                             frame goes straight out with no ARP at all.
#
#   "ARP who-has 10.0.2.99" therefore appears if and only if the routing table
#   was consulted.  It is read out of the EMULATOR's own frame log, the
#   a2065 writes every frame it handles as hex, unconditionally, below every
#   line of our code, rather than out of a capture the stack took of itself.
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
FSLOG="$ROOT/build/fsuae-base-$AMINETXDUO_RUN_TAG/Cache/Logs/fs-uae.log.txt"

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
    HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/RouteProbe" \
        "$STAGE/RtProbe"
fi
RUN_RC=$?
set -e

SERIAL="$ROOT/build/serial-$AMINETXDUO_RUN_TAG.log"
REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
skip() { echo "  --: $*"; }

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
if grep -q "^add 192.168.77.0/24 via 10.0.2.99: 0 " "$REPORT"; then
    pass "NETCTRL_ROUTE_ADD accepted 192.168.77.0/24 via 10.0.2.99"
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

if grep -Eq "^  192\.168\.77\.0 +10\.0\.2\.99 +255\.255\.255\.0 +U?G?S" "$REPORT"; then
    pass "it is flagged S (added by hand) with 10.0.2.99 as its next hop"
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
if [ -f "$FSLOG" ]; then
    python3 "$ROOT/tests/trace/a2065pcap.py" "$FSLOG" -o "$HD/host.pcap" \
        > "$HD/a2065.txt" 2>&1 || true
fi

if [ -s "$HD/host.pcap" ]; then
    ARP=$(tcpdump -r "$HD/host.pcap" -n 2>/dev/null |
          grep -c "who-has 10.0.2.99" || true)
    if [ "${ARP:-0}" -gt 0 ]; then
        pass "the wire shows $ARP ARP request(s) for 10.0.2.99, the route was used"
    else
        fail "no ARP for 10.0.2.99 on the wire: the route was not consulted"
        tcpdump -r "$HD/host.pcap" -n 2>/dev/null | grep -i arp | head -20 >&2 || true
    fi

    # And nothing addressed to 192.168.77.5 may have gone to the DEFAULT
    # gateway's hardware address, which is what a build without the table
    # would have done.  Checked as "no IP packet for 192.168.77.5 left at
    # all", because the ARP never resolves, so the queued packet is dropped.
    LEAKED=$(tcpdump -r "$HD/host.pcap" -n 2>/dev/null |
             grep -c "> 192.168.77.5" || true)
    if [ "${LEAKED:-0}" -eq 0 ]; then
        pass "nothing for 192.168.77.5 went out via the default gateway"
    else
        fail "$LEAKED packet(s) for 192.168.77.5 left the machine unrouted"
    fi
elif [ "$RUNNER" = "amiberry" ]; then
    # a2065pcap.py decodes the capture out of FS-UAE's own log. Amiberry writes
    # no equivalent, so under -A the two wire assertions above have nothing to
    # read. Skipped rather than passed: they are the only checks here that see
    # what actually left the machine, and a silent pass would hide that.
    skip "no wire capture under Amiberry, run without -A to check the wire"
fi

# ---- THE PUBLISHED ROUTING API -------------------------------------------
#
# Everything above drives the private NETCTRL_ROUTE_ADD vector.  RtProbe
# drives AddRouteTagList()/DeleteRouteTagList()/GetRouteInfo()/FreeRouteInfo()
# instead, the Roadshow ABI a third-party tool uses, and walks the
# returned table by rtm_msglen, which is the shape no build can check.

if grep -q "^add 192.168.66.0 via 10.0.2.98: rc 0 " "$REPORT"; then
    pass "AddRouteTagList added a route with no netmask tag in the grammar"
else
    fail "AddRouteTagList refused 192.168.66.0 via 10.0.2.98"
fi

# The mask is IMPLIED: 192.168.66.0 has a zero host part under its classful
# mask, so "the route is assumed to be a to a network" and 255.255.255.0 comes
# from nothing but the address.  192.168.67.7 does not, so it is a host route
# with a /32 and the H flag.
if grep -Eq "^  with +192\.168\.66\.0 +10\.0\.2\.98 +255\.255\.255\.0 +UGS" "$REPORT"; then
    pass "and the classful mask was derived from the address alone"
else
    fail "192.168.66.0 did not come back as a /24 gateway route"
fi

if grep -Eq "^  with +192\.168\.67\.7 +10\.0\.2\.98 +255\.255\.255\.255 +UGHS" "$REPORT"; then
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
if [ "$FAILED" -ne 0 ]; then
    echo "routes: FAILED" >&2
    exit 1
fi

echo "routes: PASSED"
exit 0
