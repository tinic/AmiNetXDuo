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
# The allocation phase spends a documented ten-second minimum waiting for a
# DHCP server that cannot answer, twice over, so this is not the usual 240.
TIMEOUT=400
BUILD="${AMINETXDUO_BUILD:-build/cm}"
# FS-UAE needs an X server; on a headless Linux box it dies in GLAD before the
# guest boots, so -A picks Amiberry, which runs genuinely headless.
RUNNER="${AMINETXDUO_RUNNER:-fsuae}"
BOARD=a2065
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"

# -D stages the interface configured down, which is the only way to test that
# STATE=down is honoured: it is read once at startup and there is no way to ask
# for it afterwards.  The assertion is at the bottom.
STATE_DOWN=0

while getopts "m:t:b:AN:B:D" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        D) STATE_DOWN=1 ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-A [-N board] [-B backend]] [-D]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PROBE="$ROOT/$BUILD/tests/tools/IfProbe"
STATPROBE="$ROOT/$BUILD/tests/tools/StatProbe"
AAMPROBE="$ROOT/$BUILD/tests/tools/AamProbe"
MONPROBE="$ROOT/$BUILD/tests/tools/MonProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$PROBE" "$STATPROBE" "$AAMPROBE" "$MONPROBE" "$BSD"; do
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

if [ "$STATE_DOWN" = "1" ]; then
    # Roadshow's default is up, so the line has to be added rather than changed.
    for cfg in "$STAGE"/devs/NetInterfaces/*; do
        [ -f "$cfg" ] || continue
        printf 'STATE=down\n' >> "$cfg"
        echo "==> staged $(basename "$cfg") with STATE=down"
    done
fi
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/netstat" "$STAGE/netstat"
cp "$PROBE" "$STAGE/IfProbe"
cp "$STATPROBE" "$STAGE/StatProbe"
cp "$AAMPROBE" "$STAGE/AamProbe"
cp "$MONPROBE" "$STAGE/MonProbe"

# The probe runs twice, either side of AddNetInterface.  Not to catch an empty
# list -- there is no such state to catch, because OpenLibrary("bsdsocket")
# starts the whole stack, DHCP included, so by the time the FIRST IfProbe can
# ask, eth0 is already up.  It runs twice because obtaining and releasing the
# list has to survive being done again: a block freed twice, or a Node still
# linked into a freed list, shows up on the second pass and nowhere else.
#
# The three IfProbe runs with an argument park the interface in one state and
# do nothing else, so that netstat can be asked about it afterwards.  SM_Down
# and SM_Offline both report IFQ_State == SM_Down; the difference is whether
# the SANA-II device is still on the network, which only netstat prints.
#
# They have to come after AddNetInterface: nothing else in the list keeps
# bsdsocket.library open between commands, and netstat run on its own finds no
# stack to report on.  AddNetInterface stays resident, so the interface is
# still in the state the previous command left it in.
cat > "$STAGE/commands.txt" <<'EOF'
SYS:IfProbe
SYS:AddNetInterface eth0
SYS:IfProbe
SYS:IfProbe DOWN
SYS:netstat -s
SYS:IfProbe OFFLINE
SYS:netstat -s
SYS:IfProbe UP
SYS:StatProbe
SYS:MonProbe
SYS:AamProbe
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifquery}"

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/IfProbe" \
        "$STAGE/StatProbe" "$STAGE/AamProbe" "$STAGE/MonProbe"
else
    HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/IfProbe" \
        "$STAGE/StatProbe" "$STAGE/AamProbe" "$STAGE/MonProbe"
fi
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
    # INCONCLUSIVE, not failed, and it stops here.
    #
    # Every assertion below reads $REPORT, so a guest that stopped early fails
    # all of them at once and the output reads as sixty defects in the code
    # under test. It is not: it is one run that did not happen. That has cost a
    # four-run bisect of a change that turned out to be innocent -- the same
    # binary passed on the next attempt and three times after it.
    #
    # Seen once in about nine runs of this harness, cause unknown, so it is
    # worth telling apart rather than explaining away.
    echo
    echo "INCONCLUSIVE: the guest ran $STARTS of 2 IfProbe invocations." >&2
    echo "  It stopped early. Nothing below was tested, so nothing below is" >&2
    echo "  reported. Run it again; if it repeats, the guest is the subject." >&2
    echo
    echo "ifquery: INCONCLUSIVE" >&2
    exit 2
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

if live_only; then
    if grep -Eq "IFQ_Address +10\.0\.2\.[0-9]+ \(len 16 family 2\)" "$REPORT"; then
        pass "IFQ_Address is a well-formed sockaddr_in holding the leased address"
    else
        fail "IFQ_Address is not a well-formed sockaddr_in"
    fi
fi

if live_only; then
    if grep -Eq "IFQ_NetMask +255\." "$REPORT"; then
        pass "IFQ_NetMask is the interface's mask"
    else
        fail "IFQ_NetMask did not come back"
    fi
fi

# SM_Up is 3 and SM_Down is 2; the autodoc restricts this tag to those two.
if live_only; then
    if grep -Eq "IFQ_State +3$" "$REPORT"; then
        pass "IFQ_State is SM_Up on a live interface"
    else
        fail "IFQ_State is not SM_Up"
    fi
fi

# The lease came from SLIRP's DHCP server, so the bind type is IFABT_Dynamic.
if live_only; then
    if grep -Eq "IFQ_AddressBindType +2$" "$REPORT"; then
        pass "IFQ_AddressBindType is IFABT_Dynamic for a DHCP lease"
    else
        fail "IFQ_AddressBindType is not IFABT_Dynamic"
    fi
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

if live_only; then
    if grep -q "config: mask after the refusal: .* -- unchanged, correctly" "$REPORT"; then
        pass "and nothing in that list was applied: validate first, then act"
    else
        fail "the refused list changed the netmask -- the call is not atomic"
    fi
fi

# What made IFC_Metric unsupported is that IFQ_Metric would answer something
# else.  Zero is what it answers, so zero is not a change to refuse.
if grep -q "config: metric 0: .* -- accepted, correctly" "$REPORT"; then
    pass "IFC_Metric 0 names what IFQ_Metric reports, and is accepted"
else
    fail "IFC_Metric 0 was refused even though IFQ_Metric answers 0"
fi

# The other half of the same policy. A BOOL tag set FALSE asks for nothing, so
# it cannot be a change this stack failed to make; refusing it failed the whole
# configuration for a tool that merely spelled out a default.
if grep -q "config: BOOL tags at FALSE: .* -- accepted, correctly" "$REPORT"; then
    pass "IFC_AssociatedRoute and IFC_SetDebugMode at FALSE are accepted"
else
    fail "a BOOL configure tag set FALSE was refused"
fi

if grep -q "config: IFC_AssociatedRoute TRUE: .* -- refused, correctly" "$REPORT"; then
    pass "and TRUE is still refused -- nothing here tears a route down"
else
    fail "IFC_AssociatedRoute TRUE was accepted although nothing acts on it"
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

if live_only; then
    if grep -Eq "config: address -> 10\.0\.2\.200: rc 0, IFQ_Address now 10\.0\.2\.200" "$REPORT"; then
        pass "IFC_Address moved the interface address"
    else
        fail "IFC_Address did not move the interface address"
    fi
fi

if live_only; then
    if grep -Eq "config: address restored: rc 0, IFQ_Address now 10\.0\.2\.[0-9]+" "$REPORT"; then
        pass "and IFC_Address with IFC_NetMask put it back in one call"
    else
        fail "the address was not restored"
    fi
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

# IFA_PacketFilterMode PFM_Everything asks for promiscuous capture. That is
# the refusable kind: a caller told it succeeded would read a busy wire as a
# quiet one. IFA_NumReadRequests used to be refused here and is now accepted --
# see the next assertion and the tag policy in src/bsdsocket/interfaces.c.
if grep -q "^add with an unsupported tag: .* -- refused, correctly" "$REPORT"; then
    pass "AddInterfaceTagList refuses a tag that would change what is seen"
else
    fail "AddInterfaceTagList accepted IFA_PacketFilterMode PFM_Everything"
fi

# And the tuning tags a Roadshow caller passes as a matter of course --
# IFA_NumReadRequests, IFA_CopyMode, IFA_PacketFilterMode PFM_Local -- are
# accepted. None is implemented and none changes anything this API or the wire
# can show; refusing one refused the interface itself, which is the likeliest
# reason a third-party tool worked on Roadshow and not here.
if grep -q "^add eth0 (a2065.device unit 0): rc 0 .* -- added, correctly" "$REPORT"; then
    pass "AddInterfaceTagList accepts the advisory tuning tags and adds"
else
    fail "AddInterfaceTagList refused a tuning tag and did not re-add"
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
if live_only; then
    if grep -Eq "^NETSTATUS_ip: rc 96 total [1-9][0-9]* localout [1-9][0-9]* " "$REPORT"; then
        pass "ipstat carries the running stack's packet counts"
    else
        fail "ipstat came back zeroed or the wrong size"
    fi
fi

if live_only; then
    if grep -Eq "^NETSTATUS_udp: rc 36 ipackets [1-9][0-9]* opackets [1-9][0-9]* " "$REPORT"; then
        pass "udpstat carries the DHCP exchange this machine actually had"
    else
        fail "udpstat came back zeroed or the wrong size"
    fi
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

# ---- the address allocation message --------------------------------------
#
# BeginInterfaceConfig() returns VOID.  Everything it has to say it says by
# filling in aam_Result and replying the message, so an ENOSYS stub for it was
# not a refusal but a HANG: it returned -1 in a register the caller cannot see
# and never replied the message the caller was already waiting on.  "replied"
# on each line below is the assertion that removes that.

for case in "no result ptr:CAAME_Invalid_result_ptr" \
            "version 99:CAAME_Invalid_version" \
            "protocol 77:CAAME_Invalid_protocol" \
            "an empty name:CAAME_Invalid_interface_name" \
            "an unknown interface:CAAME_Interface_not_found" \
            "a 1-character client id:CAAME_Client_identifier_too_short" \
            "a 299-character client id:CAAME_Client_identifier_too_long"; do
    what=${case%%:*}
    code=${case##*:}
    if grep -q "^create \(with \)\?\(for \)\?$what: .* -- correctly" "$REPORT"; then
        pass "CreateAddrAllocMessageA returns $code"
    else
        fail "CreateAddrAllocMessageA got $what wrong"
    fi
done

if grep -q "^create with every buffer: 0, message allocated" "$REPORT"; then
    pass "CreateAddrAllocMessageA built a message with every buffer asked for"
else
    fail "CreateAddrAllocMessageA could not build a full message"
fi

if grep -q "^timeout asked 3, got 10 -- extended, correctly" "$REPORT"; then
    pass "a timeout below the documented minimum is extended, not refused"
else
    fail "the 10-second minimum timeout was not applied"
fi

if grep -q "^reply port set, mn_Length .* -- correctly" "$REPORT"; then
    pass "the message is a well-formed struct Message, mn_Length and all"
else
    fail "the message was not initialised for ReplyMsg()"
fi

if grep -q "^client id .* -- duplicated, correctly" "$REPORT"; then
    pass "the client identifier was duplicated into the message"
else
    fail "the client identifier was not duplicated"
fi

# Two of the buffers are arrays of ULONG, and an m68k handed a misaligned one
# takes an address error rather than a wrong answer.
if grep -q "^buffers: all present, aligned and distinct -- correctly" "$REPORT"; then
    pass "every buffer is present, longword-aligned and distinct"
else
    fail "the carved buffers overlap, are misaligned or are missing"
fi

if grep -q "^buffers zeroed: yes -- correctly" "$REPORT"; then
    pass "and zeroed, so a caller reading them after the reply sees no rubbish"
else
    fail "the carved buffers were not zeroed"
fi

if grep -q "^unicast 1 -- honoured at version 2, correctly" "$REPORT"; then
    pass "CAAMTA_RequestUnicast is honoured at AAM_VERSION 2"
else
    fail "CAAMTA_RequestUnicast was not stored"
fi

# THE ONE THAT MATTERS.  Not the result code -- the message coming back.
for case in "on an addressed interface" "on an unknown interface" \
            "with a bad version"; do
    if grep -q "^begin $case: .* replied -- correctly" "$REPORT"; then
        pass "BeginInterfaceConfig replied the message: $case"
    else
        fail "BeginInterfaceConfig did not reply the message: $case"
    fi
done

if grep -q "^AbortInterfaceConfig(NULL): returned" "$REPORT"; then
    pass "AbortInterfaceConfig is safe with nothing in flight and with NULL"
else
    fail "AbortInterfaceConfig did not return"
fi

# ---- a real DHCP allocation ----------------------------------------------
#
# The interface this run is riding on already has an address, so the probe
# removes it and adds it back -- which is what AddInterfaceTagList leaves you
# with, an interface with no address at all -- and then asks
# BeginInterfaceConfig for one.  SLIRP runs a DHCP server, so this is a real
# DISCOVER/OFFER/REQUEST/ACK on the wire.

if grep -q "^live: begin returned with the message still out -- asynchronous, correctly" "$REPORT"; then
    pass "BeginInterfaceConfig returned before the allocation finished"
else
    fail "BeginInterfaceConfig blocked its caller -- it is documented asynchronous"
fi

if grep -q "^live: replied after .* result 0 -- AAMR_Success, correctly" "$REPORT"; then
    pass "and the message came back with AAMR_Success"
else
    fail "the allocation did not succeed"
fi

# The address is the assertion.  A server chose it; nothing in this stack
# could have invented 10.0.2.15 with the interface freshly added and empty.
if grep -Eq "^live: address 10\.0\.2\.[0-9]+ mask 255\.255\.255\.0 server 10\.0\.2\.2$" "$REPORT"; then
    pass "the address, mask and server address came from SLIRP's DHCP server"
else
    fail "the lease does not carry the server's own numbers"
fi

# aam_RouterTable is filled from DHCP option 3, which the server only sends
# because the client asked for it in its parameter request list.
if grep -q "^live: router\[0\] 10.0.2.2 -- the server offered one" "$REPORT"; then
    pass "aam_RouterTable carries the router the server offered"
else
    fail "no router came back -- the parameter request list is not being sent"
fi

if grep -Eq "^live: lease expires day [1-9][0-9]+ " "$REPORT"; then
    pass "aam_LeaseExpires holds a real date rather than the zero that means infinite"
else
    fail "the lease expiry DateStamp was not filled in"
fi

# And it really configured the interface, rather than only reporting a number.
if grep -q "^begin a second time: result 4, replied -- correctly" "$REPORT"; then
    pass "a second allocation on the now-addressed interface is AAMR_AddressKnown"
else
    fail "the allocation did not put the address on the interface"
fi

# ---- the two paths a working DHCP server hides ---------------------------
#
# AAMR_Timeout and AAMR_Aborted cannot be reached while SLIRP is answering in
# four tenths of a second: neither the deadline nor the abort window ever
# opens.  The probe takes the interface DOWN first, so DISCOVER goes nowhere
# and the worker runs to its own deadline -- the only path that proves the
# deadline exists, and the only one that releases a lease never granted.

if grep -q "^slow: still running after a second: yes -- correctly" "$REPORT"; then
    pass "an allocation with no server to answer is still running a second in"
else
    fail "the allocation did not stay in flight"
fi

# ---- and only one of them at a time --------------------------------------
#
# "AAMR_Busy -- Address allocation is already in progress for this interface."
# bsd_aam_launch() claims bsd_aam_jobs[index] under Forbid() and refuses a
# launch that finds it taken.  Nothing one process does can reach that guard:
# BeginInterfaceConfig() is asynchronous, but the caller holding the job is the
# one that would ask again, so the second request has to come from an unrelated
# program with a base of its own.  The probe forks one, during the window above
# in which a worker is certainly running.
#
# THE RESULT CODE ALONE WOULD NOT CATCH IT.  AAMR_Busy is answered twice over:
# at the door by that guard, and -- if the guard is gone and the worker starts
# anyway -- by netstack_interface_dhcp_start() refusing a second DHCP client on
# one interface, which bsd_aam_worker() also reports as AAMR_Busy.  Measured:
# with the guard deleted the second caller is STILL told 11.  What changes is
# when.  A refusal comes back inside BeginInterfaceConfig(); the other answer
# costs a CreateNewProc() and a worker's first DHCP call, and arrived ~10 ticks
# later.  So the probe reports whether the message beat the call's return, and
# that is what is asserted here.
#
# NEGATIVE CONTROL, measured: delete the `if (bsd_aam_jobs[index] != NULL)`
# branch from src/bsdsocket/addralloc.c and this line reads "REPLIED LATE, so a
# worker was started".  The collateral is worth knowing too -- the second
# launch overwrites bsd_aam_jobs[index], so the FIRST caller's
# AbortInterfaceConfig() can no longer find its own job and the three `slow:`
# assertions below fail with it.
if live_only; then
    if grep -q "^busy: .* -- refused at the door with AAMR_Busy, correctly" "$REPORT"; then
        pass "a second process asking for an interface already being configured is refused AAMR_Busy"
    elif grep -q "^busy: .* -- NOT TESTED" "$REPORT"; then
        fail "the second process never got as far as asking -- AAMR_Busy is UNTESTED"
    else
        fail "the second request got a worker of its own -- bsd_aam_jobs[] did not refuse it at the door, and the first caller's job is no longer the one in the table"
    fi
fi

if grep -q "^slow: abort replied after .* -- AAMR_Aborted, correctly" "$REPORT"; then
    pass "AbortInterfaceConfig stopped one in flight, replied AAMR_Aborted"
else
    fail "AbortInterfaceConfig did not abort an allocation in flight"
fi

if grep -q "^slow: timeout replied after .* -- AAMR_Timeout, correctly" "$REPORT"; then
    pass "and one left alone gives up with AAMR_Timeout"
else
    fail "the allocation never timed out"
fi

# A worker that gave up early would report AAMR_Timeout too, and would be
# wrong.  The number of ticks is what says the deadline is the caller's.
if grep -q "^slow: waited at least -- correctly the 10-second floor" "$REPORT"; then
    pass "it waited at least the documented ten-second minimum"
else
    fail "the allocation gave up before the ten-second floor"
fi

# A library that could not tell its own messages from a caller's would free a
# stack frame here, and the machine would not survive the next allocation.
if grep -q "^DeleteAddrAllocMessage on a stack message: .* -- refused, correctly" "$REPORT"; then
    pass "DeleteAddrAllocMessage refuses a message it did not allocate"
else
    fail "DeleteAddrAllocMessage tried to free a message it did not allocate"
fi

# ---- SM_Down is not SM_Offline -------------------------------------------
#
# "SM_Down -- the stack will no longer attempt to transmit messages through
# this interface.  However, the underlying SANA-II device driver may still be
# connected to the network", against "SM_Offline -- same as SM_Down, but also
# sends an S2_OFFLINE command".  The document separates them so that a unit
# shared with Envoy or ACS keeps working when this stack lets go of it.
#
# Both report IFQ_State == SM_Down, so the difference is invisible to the
# published API.  netstat prints the device's own state, which is why it runs
# after each of the two IfProbe state commands.
if grep -Eq "^state DOWN: rc 0 .* IFQ_State now 2$" "$REPORT"; then
    pass "SM_Down stopped the stack transmitting -- IFQ_State reports SM_Down"
else
    fail "IFC_State SM_Down did not report SM_Down"
fi

# netstat prints "eth0 (online)" / "eth0 (offline)" from the SANA-II shim's own
# flag.  The first of the two must be online, the second offline.
NETSTAT_STATES=$( { grep -Eo "^eth0 \((online|offline)\)$" "$REPORT" || true; } | tr '\n' ' ')
if [ "$NETSTAT_STATES" = "eth0 (online) eth0 (offline) " ]; then
    pass "SM_Down left the SANA-II device on the network, SM_Offline took it off"
else
    fail "the device state after SM_Down/SM_Offline was: $NETSTAT_STATES"
fi

if grep -Eq "^state UP: rc 0 .* IFQ_State now 3$" "$REPORT"; then
    pass "and SM_Up brought it back"
else
    fail "SM_Up did not bring the interface back after SM_Offline"
fi

# ---- the monitoring hooks ------------------------------------------------
#
# The denying half is the half with consequences: a hook that returns an errno
# must make bind() or connect() fail with exactly that errno, before the stack
# has done anything.

# A client asks whether the API is there before it calls any of it, so a FALSE
# here means the hooks below are never reached by a conforming caller.
if grep -q "^SBTC_HAVE_MONITORING_API: .* -- TRUE, correctly" "$REPORT"; then
    pass "SBTC_HAVE_MONITORING_API answers TRUE for the three types that work"
else
    fail "SBTC_HAVE_MONITORING_API answers FALSE although hooks install"
fi

# SocketBaseTagList() stops at the first tag it refuses, so a SET of a tunable
# to the value it already holds must not cost the caller the rest of its list.
if grep -q "^set IP_DEFAULT_TTL to its own value.* -- accepted and the next tag was serviced, correctly" "$REPORT"; then
    pass "writing a tunable back at its current value is not a change"
else
    fail "a no-op SET was refused and discarded the rest of the tag list"
fi

if grep -q "^turn IP forwarding on: .* -- refused at tag 1, correctly" "$REPORT"; then
    pass "and a real change to something unimplemented is still refused"
else
    fail "SBTC_IP_FORWARDING was set although this stack does not forward"
fi

for case in "a NULL hook:EFAULT" "type 99:EINVAL"; do
    what=${case%%:*}
    code=${case##*:}
    if grep -q "^add $what: .* -- $code, correctly" "$REPORT"; then
        pass "AddNetMonitorHookTagList returns $code for $what"
    else
        fail "AddNetMonitorHookTagList got $what wrong"
    fi
done

if grep -q "^add MHT_Packet: .* -- refused rather than silently ignored, correctly" "$REPORT"; then
    pass "an in-stack type nothing dispatches is refused, not silently ignored"
else
    fail "MHT_Packet was accepted although nothing dispatches it"
fi

if grep -q "^add the same hook twice: .* -- refused, correctly" "$REPORT"; then
    pass "one Hook cannot be installed twice -- removal takes no type"
else
    fail "the same Hook was accepted into two lists"
fi

if grep -q "^bind with an allowing hook: .* -- allowed and seen, correctly" "$REPORT"; then
    pass "a hook that returns 0 sees the call and lets it through"
else
    fail "an allowing hook was not consulted, or blocked the call"
fi

# The register convention: A2 was poisoned before the call and must come back
# NULL.  A wrong guess hands the message in the wrong register entirely.
if grep -q "^reserved was NULL -- correctly, hook was ours -- correctly" "$REPORT"; then
    pass "the hook was entered with A0=Hook, A2=NULL, A1=message"
else
    fail "the hook register convention is wrong"
fi

if grep -q "^message is the published shape: yes -- correctly" "$REPORT"; then
    pass "bmm_Size, bmm_Socket and bmm_Name are what bind() was given"
else
    fail "the monitor message does not match what was passed to bind()"
fi

if grep -q "^bind with a denying hook: .* -- denied with the hook.s errno, correctly" "$REPORT"; then
    pass "a hook that returns an errno fails bind() with exactly that errno"
else
    fail "a denying hook did not fail bind() with its own errno"
fi

if grep -q "^two hooks on one type: .* -- both installed, correctly" "$REPORT"; then
    pass "more than one hook can be installed for one task"
else
    fail "a second hook on one type was refused"
fi

if grep -q "^first allows, second denies: .* -- one hook cannot overrule another, correctly" "$REPORT"; then
    pass "a hook that allows cannot overrule one that denies"
else
    fail "an allowing hook overruled a denying one"
fi

if grep -q "^first denies: .* -- the walk stopped, correctly" "$REPORT"; then
    pass "and the walk stops at the first refusal"
else
    fail "hooks after the first refusal were still consulted"
fi

if grep -q "^connect with a denying hook: .* -- denied before the connect, correctly" "$REPORT"; then
    pass "MHT_Connect denies connect() before the stack has done anything"
else
    fail "MHT_Connect did not deny the connect"
fi

# MHT_Send, and the per-call message shape the autodoc specifies.  These
# compile fine while being wrong, which is why they are asserted rather than
# commented.
if grep -q "^send denied: .* -- denied before the send, correctly" "$REPORT"; then
    pass "MHT_Send fails send() with the hook's errno, before anything is sent"
else
    fail "MHT_Send did not deny the send"
fi

if grep -q "^send shape: .* -- correctly" "$REPORT"; then
    pass "send(): smm_Buffer and smm_Len are the caller's, To and Msg both NULL"
else
    fail "send() built the wrong SendMonitorMessage"
fi

if grep -q "^sendto shape: to ours msg NULL -- correctly" "$REPORT"; then
    pass "sendto(): smm_To is the caller's address, smm_Msg NULL"
else
    fail "sendto() built the wrong SendMonitorMessage"
fi

if grep -q "^sendmsg shape: to NULL msg ours .* -- correctly" "$REPORT"; then
    pass "sendmsg(): smm_Msg is the caller's msghdr, smm_To NULL, smm_Len total"
else
    fail "sendmsg() built the wrong SendMonitorMessage"
fi

if grep -q "^never both set: yes -- correctly" "$REPORT"; then
    pass "smm_To and smm_Msg are never both set, across all three calls"
else
    fail "smm_To and smm_Msg were both set"
fi

if grep -q "^send allowed: .* -- sent in full, correctly" "$REPORT"; then
    pass "and a send the hook allows goes through untouched"
else
    fail "an allowed send did not complete"
fi

if grep -q "^after removal: .* -- no longer consulted, correctly" "$REPORT"; then
    pass "a removed hook stops being consulted"
else
    fail "a removed hook was still called"
fi

if grep -q "^RemoveNetMonitorHook(NULL) and twice: returned" "$REPORT"; then
    pass "RemoveNetMonitorHook is safe with NULL and with a hook already out"
else
    fail "RemoveNetMonitorHook did not survive NULL or a double removal"
fi

# STATE=down, when it was staged.  SM_Down is 2 and SM_Up is 3
# (libraries/bsdsocket.h).  The first IfProbe runs before anything has called
# Online, so the state it reports is the one the config asked for -- and with
# the default config that same reading is SM_Up, which is what makes this an
# assertion rather than a restatement of the file.
if [ "$STATE_DOWN" = "1" ]; then
    if grep -q "^  IFQ_State *2$" "$REPORT"; then
        pass "STATE=down was honoured: the interface came up configured down"
    else
        fail "STATE=down was ignored: $(grep -m1 '^  IFQ_State' "$REPORT")"
    fi
else
    if grep -q "^  IFQ_State *3$" "$REPORT"; then
        pass "the default config leaves the interface up"
    else
        fail "the default config did not leave the interface up"
    fi
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifquery: FAILED" >&2
    exit 1
fi

echo "ifquery: PASSED"
exit 0
