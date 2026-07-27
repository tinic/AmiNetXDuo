#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR THE ROADSHOW INTERFACE AND STATISTICS APIs.
#
#   tests/tools/run-ifquery.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT IS PROVING
#
#   ObtainInterfaceList(), ReleaseInterfaceList(), QueryInterfaceTagList() and
#   ConfigureInterfaceTagList() are the vectors a monitoring or configuration
#   tool -- Roadie, NetMon, RoadshowControl -- reaches for first, and both of
#   the shapes they traffic in are shapes a compiler cannot check:
#
#     * a 'struct List' whose Nodes carry a name in ln_Name and nothing else.
#       A list of the wrong node type walks fine right up to the first
#       dereference in the CALLER, where it gurus.
#     * IFQ_* tags whose ti_Data is a POINTER to caller storage rather than
#       the value.  Getting that backwards writes a number into a pointer the
#       application still owns.
#
#   Neither can be caught by a build, and neither can be caught by a test that
#   shares a header with the implementation.  So this runs a separate
#   executable, tests/tools/ifprobe.c, which knows only the published NDK
#   header, on a booted machine with a real SANA-II card behind the stack.
#
# WHY THE PROBE POISONS ITS BUFFERS
#
#   Half the published IFQ_* tags have no true value on this stack and are
#   documented in src/bsdsocket/interfaces.c to be LEFT ALONE rather than
#   answered with an invented zero.  IfProbe fills every destination with 0xA5
#   first, so the transcript distinguishes "answered zero" from "not answered"
#   -- a test that pre-zeroed could not tell a deliberate omission from a case
#   that fell through, which is the mistake this whole file exists to prevent.
#
# WHAT THE CONFIGURATION HALF ASSERTS
#
#   ConfigureInterfaceTagList() validates the whole tag list before applying
#   any of it, so that a refused call leaves the interface exactly as it was.
#   The probe sends a legal IFC_NetMask followed by an unsupported IFC_Metric
#   and then reads the mask back: a one-pass implementation refuses the call
#   AND changes the mask, which passes the obvious assertion and fails this
#   one.  Everything the probe changes it puts back, and the address is
#   restored before the interface state is touched, so a failure part-way
#   leaves the machine reachable.
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

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PROBE="$ROOT/$BUILD/tests/tools/IfProbe"
STATPROBE="$ROOT/$BUILD/tests/tools/StatProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$STATPROBE" \
         "$BSD"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
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

STAGE="$ROOT/build/ifquery-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/IfProbe"
cp "$STATPROBE" "$STAGE/StatProbe"

# The probe runs twice, either side of AddNetInterface.  Not to catch an empty
# list -- there is no such state to catch, because OpenLibrary("bsdsocket")
# starts the whole stack, DHCP included, so by the time the FIRST IfProbe can
# ask, eth0 is already up.  It runs twice because obtaining and releasing the
# list has to survive being done again: a block freed twice, or a Node still
# linked into a freed list, shows up on the second pass and nowhere else.
cat > "$STAGE/commands.txt" <<'EOF'
SYS:IfProbe
SYS:AddNetInterface eth0
SYS:IfProbe
SYS:StatProbe
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifquery}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/IfProbe" "$STAGE/StatProbe"
RUN_RC=$?
set -e

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

# ---- one boot (docs/RESEARCH.md 25) --------------------------------------
#
# ToolsSmoke reopens the transcript from the top after a reset, so the FIRST
# command appearing more than once means the machine went down and came back.
# A guru inside the caller is exactly what a wrong node shape produces, so
# this is the assertion that matters most here.
STARTS=$(grep -c "SYS:IfProbe =====" "$REPORT" || true)
if [ "$STARTS" -eq 2 ]; then
    pass "the machine booted once and ran both probes (no reset)"
elif [ "$STARTS" -gt 2 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run did not reach both IfProbe invocations"
fi

# ---- the list ------------------------------------------------------------
LISTED=$(grep -c "^ObtainInterfaceList: 1 interface(s)" "$REPORT" || true)
if [ "$LISTED" -eq 2 ]; then
    pass "one interface listed, both times, obtained and released twice"
else
    fail "expected two listings of one interface, got $LISTED"
fi

if grep -q "^interface 1: eth0" "$REPORT"; then
    pass "ln_Name carries the name from DEVS:NetInterfaces"
else
    fail "the node's ln_Name is not the configured interface name"
fi

# ---- the query -----------------------------------------------------------
if grep -q "^query eth0: rc 0 " "$REPORT"; then
    pass "QueryInterfaceTagList(eth0) returned 0"
else
    fail "QueryInterfaceTagList(eth0) did not return 0"
fi

if grep -Eq "IFQ_DeviceName +a2065\.device" "$REPORT"; then
    pass "IFQ_DeviceName returned a pointer to the SANA-II device name"
else
    fail "IFQ_DeviceName did not return the device name"
fi

if grep -Eq "IFQ_HardwareAddressSize +48$" "$REPORT"; then
    pass "IFQ_HardwareAddressSize is 48 -- bits, from S2_DEVICEQUERY"
else
    fail "IFQ_HardwareAddressSize is not 48 bits"
fi

# The seventh byte must still be poison: "a maximum of 16 bytes will be
# copied", and six is what an Ethernet address is.  A shim that copied a fixed
# sixteen would scribble ten bytes past what the caller reserved.
if grep -Eqi "IFQ_HardwareAddress +[0-9a-f:]+ \(7th byte a5\)" "$REPORT"; then
    pass "IFQ_HardwareAddress wrote six bytes and not one more"
else
    fail "IFQ_HardwareAddress wrote past the six bytes of an Ethernet address"
fi

if grep -Eq "IFQ_Address +10\.0\.2\.[0-9]+ \(len 16 family 2\)" "$REPORT"; then
    pass "IFQ_Address is a well-formed sockaddr_in holding the leased address"
else
    fail "IFQ_Address is not a well-formed sockaddr_in"
fi

if grep -Eq "IFQ_NetMask +255\." "$REPORT"; then
    pass "IFQ_NetMask is the interface's mask"
else
    fail "IFQ_NetMask did not come back"
fi

# SM_Up is 3 and SM_Down is 2; the autodoc restricts this tag to those two.
if grep -Eq "IFQ_State +3$" "$REPORT"; then
    pass "IFQ_State is SM_Up on a live interface"
else
    fail "IFQ_State is not SM_Up"
fi

# The lease came from SLIRP's DHCP server, so the bind type is IFABT_Dynamic.
if grep -Eq "IFQ_AddressBindType +2$" "$REPORT"; then
    pass "IFQ_AddressBindType is IFABT_Dynamic for a DHCP lease"
else
    fail "IFQ_AddressBindType is not IFABT_Dynamic"
fi

if grep -Eq "IFQ_MTU +1500$" "$REPORT"; then
    pass "IFQ_MTU is the driver's 1500"
else
    fail "IFQ_MTU is not 1500"
fi

# ---- what is deliberately NOT answered -----------------------------------
#
# These two are documented in interfaces.c as having no true value here.  The
# assertion is that they were left alone rather than filled with a zero a
# monitor would render as a measurement.
if grep -q "IFQ_GetBytesIn           unanswered" "$REPORT"; then
    pass "IFQ_GetBytesIn was left alone, as documented"
else
    fail "IFQ_GetBytesIn wrote something -- there are no byte counters"
fi

if grep -q "IFQ_LastStart            unanswered" "$REPORT"; then
    pass "IFQ_LastStart was left alone, as documented"
else
    fail "IFQ_LastStart wrote something -- nothing records it"
fi

# ---- the negative and boundary cases -------------------------------------
if grep -q "^query nosuchif: .* -- refused, correctly" "$REPORT"; then
    pass "an unknown interface name fails rather than returning 0"
else
    fail "querying an unknown interface did not fail"
fi

if grep -q "with no tags: rc 0 -- accepted, correctly" "$REPORT"; then
    pass "an empty tag list is 'does this interface exist?', and succeeds"
else
    fail "an empty tag list was refused"
fi

if grep -q "^ReleaseInterfaceList(NULL): returned" "$REPORT"; then
    pass "ReleaseInterfaceList(NULL) did nothing, as documented"
else
    fail "ReleaseInterfaceList(NULL) did not return"
fi

# ---- ConfigureInterfaceTagList -------------------------------------------
#
# The atomicity assertion is the one worth having. The refused list had a
# legal IFC_NetMask in front of the unsupported IFC_Metric, so a one-pass
# implementation would refuse the call AND leave 255.255.0.0 on the
# interface -- passing the "refused" check and failing this one.
if grep -q "config: mask+metric: .* -- refused, correctly" "$REPORT"; then
    pass "a tag list containing an unsupported tag is refused"
else
    fail "IFC_Metric was accepted, or the call did not run"
fi

if grep -q "config: mask after the refusal: .* -- unchanged, correctly" "$REPORT"; then
    pass "and nothing in that list was applied: validate first, then act"
else
    fail "the refused list changed the netmask -- the call is not atomic"
fi

if grep -q "config: bad address: .* -- refused, correctly" "$REPORT"; then
    pass "an address string that is neither dotted-quad nor a host is refused"
else
    fail "a malformed IFC_Address was accepted"
fi

if grep -q "config: IFC_LimitMTU 576: rc 0, IFQ_MTU now 576" "$REPORT"; then
    pass "IFC_LimitMTU lowered the MTU and IFQ_MTU reports it"
else
    fail "IFC_LimitMTU did not lower the MTU"
fi

# "you can request that a smaller size is used" -- so more than the hardware
# can carry is the hardware's own number, not an error.
if grep -q "config: IFC_LimitMTU 9000: rc 0, IFQ_MTU now 1500" "$REPORT"; then
    pass "a request above the hardware MTU is clamped to 1500, not refused"
else
    fail "IFC_LimitMTU 9000 was not clamped to the driver's 1500"
fi

if grep -Eq "config: address -> 10\.0\.2\.200: rc 0, IFQ_Address now 10\.0\.2\.200" "$REPORT"; then
    pass "IFC_Address moved the interface address"
else
    fail "IFC_Address did not move the interface address"
fi

if grep -Eq "config: address restored: rc 0, IFQ_Address now 10\.0\.2\.[0-9]+" "$REPORT"; then
    pass "and IFC_Address with IFC_NetMask put it back in one call"
else
    fail "the address was not restored"
fi

if grep -q "config: SM_Down: .* -- down, correctly" "$REPORT"; then
    pass "IFC_State SM_Down took the interface down"
else
    fail "IFC_State SM_Down did not take the interface down"
fi

if grep -q "config: SM_Online: .* -- up, correctly" "$REPORT"; then
    pass "IFC_State SM_Online brought it back, and IFQ_State reports SM_Up"
else
    fail "IFC_State SM_Online did not bring the interface back"
fi

if grep -q "config: IFC_State 99: .* -- refused, correctly" "$REPORT"; then
    pass "an IFC_State value the API never defined is refused"
else
    fail "IFC_State 99 was accepted"
fi

if grep -q "config: nosuchif: .* -- refused, correctly" "$REPORT"; then
    pass "configuring an interface that does not exist is refused"
else
    fail "ConfigureInterfaceTagList accepted an unknown interface"
fi

# ---- AddInterfaceTagList / RemoveInterface --------------------------------
#
# "It tries to release all the resources associated with a networking
# interface, thus permitting it to be added again with new parameters" -- so
# removing and re-adding IS the documented use, and doing exactly that is the
# only way to find out whether the SANA-II device was really closed and really
# reopened.  The machine this test runs on has one card, and the round trip
# happens on the interface the run is riding on.

if grep -q "^remove nosuchif: .* -- refused, correctly" "$REPORT"; then
    pass "RemoveInterface refuses a name that is not there"
else
    fail "RemoveInterface accepted an unknown interface"
fi

# TRUE for success, 0 for failure -- the opposite of every other call in the
# API, and the NDK header types it LONG where the autodoc says BOOL.
if grep -q "^remove eth0: rc 1 .* -- removed, correctly" "$REPORT"; then
    pass "RemoveInterface returned TRUE, not 0-for-success"
else
    fail "RemoveInterface did not return TRUE"
fi

if grep -q "^after remove: 0 interface(s), eth0 is gone -- correctly" "$REPORT"; then
    pass "the interface left the list"
else
    fail "the interface is still listed after RemoveInterface"
fi

if grep -q "^query the removed eth0: .* -- refused, correctly" "$REPORT"; then
    pass "and nothing can be queried about it any more"
else
    fail "QueryInterfaceTagList still answers for a removed interface"
fi

if grep -q "^add with an unsupported tag: .* -- refused, correctly" "$REPORT"; then
    pass "AddInterfaceTagList refuses a tag it cannot honour"
else
    fail "AddInterfaceTagList accepted IFA_NumReadRequests"
fi

if grep -q "^add eth0 (a2065.device unit 0): rc 0 .* -- added, correctly" "$REPORT"; then
    pass "AddInterfaceTagList put it back"
else
    fail "AddInterfaceTagList did not re-add the interface"
fi

if grep -q "^after add: 1 interface(s), eth0 is there -- correctly" "$REPORT"; then
    pass "and the refused add above left nothing half-created"
else
    fail "the interface count is wrong after the re-add"
fi

if grep -q "^add eth0 twice: .* -- refused, correctly" "$REPORT"; then
    pass "a second interface of the same name is refused"
else
    fail "two interfaces were allowed to share a name"
fi

# THE EVIDENCE.  The hardware address is read from the card by S2_DEVICEQUERY
# at open time, so a re-added interface reporting the same MAC went all the
# way down to the device and back.  Zeroes, or a stale value out of memory
# that was never freed, would both show here.
if grep -q "^hardware address after the round trip: .* -- the device was reopened, correctly" "$REPORT"; then
    pass "the SANA-II device was really closed and really reopened"
else
    fail "the re-added interface did not report the card's own address"
fi

if grep -q "^state after: 3, .* -- up again, correctly" "$REPORT"; then
    pass "and it configures and comes up like any other interface"
else
    fail "the re-added interface would not come up"
fi

# ---- GetNetworkStatistics ------------------------------------------------
#
# Three things a build cannot check: that the return value is a BYTE COUNT
# rather than zero-or-an-entry-count, that the numbers are the running
# stack's, and that pcd_tcp_state is 4.4BSD's enumeration rather than NetX
# Duo's -- the two agree up to CLOSE_WAIT and diverge after it.

for case in "version 0" "type 99" "NETSTATUS_mb"; do
    if grep -q "^$case: .* -- refused, correctly" "$REPORT"; then
        pass "GetNetworkStatistics refused: $case"
    else
        fail "GetNetworkStatistics accepted: $case"
    fi
done

if grep -q "^NETSTATUS_ip size: .* -- sizeof(struct ipstat), correctly" "$REPORT"; then
    pass "a NULL destination returns the size a complete copy would need"
else
    fail "GetNetworkStatistics(NULL) did not return sizeof(struct ipstat)"
fi

# This machine leased its address by DHCP, which is UDP over IP, so a stack
# that is really counting cannot report zero for either of these.  A stub
# returning a zeroed struct of the right size passes every structural check
# and fails here.
if grep -Eq "^NETSTATUS_ip: rc 96 total [1-9][0-9]* localout [1-9][0-9]* " "$REPORT"; then
    pass "ipstat carries the running stack's packet counts"
else
    fail "ipstat came back zeroed or the wrong size"
fi

if grep -Eq "^NETSTATUS_udp: rc 36 ipackets [1-9][0-9]* opackets [1-9][0-9]* " "$REPORT"; then
    pass "udpstat carries the DHCP exchange this machine actually had"
else
    fail "udpstat came back zeroed or the wrong size"
fi

# Every copy is bounded.  The guard bytes past the end are the assertion that
# matters: "size" is the caller's limit, and a call that copied the whole
# struct regardless would corrupt a buffer sized against an older layout.
GUARDS=$(grep -c "guard intact" "$REPORT" || true)
OVERRUNS=$(grep -c "OVERRUN" "$REPORT" || true)
if [ "$OVERRUNS" -eq 0 ] && [ "$GUARDS" -ge 6 ]; then
    pass "no copy wrote past its bound ($GUARDS guards checked)"
else
    fail "$OVERRUNS copy/copies overran the caller's buffer"
fi

if grep -q "^NETSTATUS_ip into 8 bytes: rc 8 " "$REPORT"; then
    pass "a caller asking for 8 bytes gets 8 and is told so"
else
    fail "a partial request did not return the partial count"
fi

if grep -q "^tcp sockets after listen: .* -- one more connection, correctly" "$REPORT"; then
    pass "NETSTATUS_tcp_sockets grew by exactly one entry after listen()"
else
    fail "the socket table did not grow by one entry"
fi

# TCPS_LISTEN is 1 and NX_TCP_LISTEN_STATE is 2.  A stack passing NetX Duo's
# value straight through reports 2, and every monitor shows a listener as a
# connection in SYN_SENT.
if grep -q "^listener state: 1 -- TCPS_LISTEN, correctly" "$REPORT"; then
    pass "pcd_tcp_state is 4.4BSD's enumeration, not NetX Duo's"
else
    fail "pcd_tcp_state did not come back as TCPS_LISTEN"
fi

if grep -q "^tcp table into one entry: .* -- one entry, correctly" "$REPORT"; then
    pass "a buffer that holds one entry gets one entry and no more"
else
    fail "the socket table ignored the caller's size limit"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifquery: FAILED" >&2
    exit 1
fi

echo "ifquery: PASSED"
exit 0
