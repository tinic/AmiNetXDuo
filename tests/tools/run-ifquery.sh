#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR THE ROADSHOW INTERFACE QUERY API.
#
#   tests/tools/run-ifquery.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT IS PROVING
#
#   ObtainInterfaceList(), ReleaseInterfaceList() and QueryInterfaceTagList()
#   are the three vectors a monitoring tool -- Roadie, NetMon,
#   RoadshowControl -- reaches for first, and both of the shapes they traffic
#   in are shapes a compiler cannot check:
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

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD"; do
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
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifquery}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/IfProbe"
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

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifquery: FAILED" >&2
    exit 1
fi

echo "ifquery: PASSED"
exit 0
