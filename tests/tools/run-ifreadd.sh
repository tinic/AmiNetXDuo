#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "AddInterfaceTagList() gives back an interface that
# is bare, and ConfigureInterfaceTagList() addresses it".
#
#   tests/tools/run-ifreadd.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                              [-A [-N board] [-B backend]]
#
# WHAT THE PUBLISHED API SAYS
#
#   AddInterfaceTagList(), "Make a new interface available for network
#   access ... Each such device must be assigned a unique interface name and
#   refer to a SANA-II device name and unit number."  Its whole tag set is
#   device-facing: IFA_IPType, IFA_ARPType, IFA_Num*Requests,
#   IFA_PacketFilterMode, IFA_PointToPoint, IFA_Multicast, IFA_DownGoesOffline,
#   IFA_ReportOffline.  There is no address, netmask, gateway or broadcast tag.
#
#   ConfigureInterfaceTagList(), "for changing the configuration of an
#   interface previously added with AddInterfaceTagList(), such as setting
#   interface addresses, status and routing metrics", IFC_Address,
#   IFC_NetMask, IFC_DestinationAddress, IFC_BroadcastAddress.
#
#   So the add creates a bare interface and the configure addresses it.  An
#   interface that comes back from a re-add with no address is the contract
#   being kept, not a fault.
#
# WHAT THE ORIGINAL FAULT WAS, AND STILL IS
#
#   The routing table went from
#
#       10.0.2.0    *          255.255.255.0  U   eth0
#       default     10.0.2.2   0.0.0.0        UG  eth0
#
#   to
#
#       10.0.0.0    *          255.0.0.0      U   eth0
#
#   A SILENTLY WRONG CLASSFUL /8 where the machine had a /24.  That is the
#   thing worth a regression test, and it is asserted here against the right
#   expectation: IfProbe hands ConfigureInterfaceTagList() the address AND the
#   mask, and the mask that comes out has to be the one it asked for rather
#   than the class of the address.  A stack that guesses anyway reads /8 in
#   `netstat -r` and in IFQ_NetMask, and both are checked.
#
#   The default route is a separate matter and is NOT put back by either
#   vector: nx_ip_interface_detach() takes it with the interface it belonged
#   to, and neither AddInterfaceTagList() nor ConfigureInterfaceTagList() has a
#   tag for a gateway.  AddRouteTagList() is where a caller puts one back.  Its
#   absence after the round trip is asserted too, so that anything reinstating
#   it inside the library vector shows up here.
#
# WHY eth0 IS STATIC HERE, AND NOT DHCP
#
#   Because a static interface has no second source.  A DHCP interface has a
#   client that may re-bind and write an address, a mask and a gateway back, so
#   a transcript showing them proves nothing about what the remove-and-add
#   did.  Under STATIC, what the routing table says afterwards is the round
#   trip and IfProbe's own configure, and nothing else.
#
# WHY NOT IN run-ifquery.sh, WHICH ALSO RUNS IfProbe
#
#   That harness runs IfProbe as its FIRST command, and this stack cannot get
#   past the `AddNetInterface` that follows the probe, observed hanging there
#   on more than one build.  That is a separate defect and not this one, which
#   is why nothing is added to that file: an assertion behind a hang is an
#   assertion that never runs.
#
# WHAT IS ASSERTED
#
#   * the routing table before the round trip has the /24 and the default,
#     so there is something to lose;
#   * the freshly re-added interface carries NO address and NO mask;
#   * IFQ_NetMask after the configure is the mask that was asked for;
#   * the routing table afterwards has the /24 back and no default route.
#
#   The mask is asserted from both instruments because neither sees both
#   halves: no published IFQ_ tag reaches the routing table, and netstat cannot
#   say what the interface thinks its mask is.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
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
PROBE="$ROOT/$BUILD/tests/tools/IfProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$TOOLS/NetShutdown" "$PROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/ifreadd-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# SLIRP's own numbers, taken statically rather than leased: 10.0.2.15 is the
# address it would hand out and 10.0.2.2 is the gateway it answers on.
# tests/tools/ifprobe.c reconfigures with 10.0.2.15 and the mask the interface
# left with, so the address and the mask below are the pair it asks for back.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/netstat"         "$STAGE/netstat"
cp "$TOOLS/NetShutdown"     "$STAGE/NetShutdown"
cp "$PROBE"                 "$STAGE/IfProbe"

cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:netstat -r
SYS:IfProbe
SYS:netstat -r
SYS:NetShutdown
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifreadd}"

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/NetShutdown" \
        "$STAGE/IfProbe"
else
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/NetShutdown" \
        "$STAGE/IfProbe"
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

# The routing table printed by the Nth `netstat -r` in the transcript, one line
# per route.  Both tables are read the same way so the before and the after
# cannot be compared by different rules.
routes() {
    awk -v want="$1" '
        /^===== SYS:netstat -r =====/ { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { exit }
        on { print }
    ' "$REPORT"
}

# ---- one boot (docs/RESEARCH.md 25) --------------------------------------
#
# ToolsSmoke reopens the transcript from the top after a reset, so the first
# command appearing twice is a reboot and not a hang.
STARTS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run did not get as far as bringing eth0 up"
fi

# ---- what it had ---------------------------------------------------------
#
# Asserted rather than assumed: if SLIRP ever stops handing out a /24 and a
# gateway, the two assertions below would pass on a machine that never had
# either, and this file would be testing nothing.
BEFORE=$(routes 1)
if printf '%s\n' "$BEFORE" | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.0 '; then
    pass "eth0 started on 10.0.2.0/24"
else
    fail "eth0 did not start with the /24, nothing to lose, so nothing proved"
    printf '%s\n' "$BEFORE" | sed 's/^/       /' >&2
fi

if printf '%s\n' "$BEFORE" | grep -Eq '^default +10\.0\.2\.2 '; then
    pass "and with the default route through 10.0.2.2"
else
    fail "eth0 started with no default route, nothing to lose, so nothing proved"
    printf '%s\n' "$BEFORE" | sed 's/^/       /' >&2
fi

# ---- bare, as the published API says -------------------------------------
#
# AddInterfaceTagList() has no address tag, so the interface it hands back must
# be carrying nothing.  A library vector that put the old addressing back would
# read as still addressed here, and would also stop BeginInterfaceConfig()
# from ever running a DHCP allocation on a re-added interface, which is how it
# was found.
if grep -q "^bare after add: address 0\.0\.0\.0 netmask 0\.0\.0\.0, bare, correctly" "$REPORT"; then
    pass "the re-added interface came back with no address and no mask"
else
    fail "the re-added interface came back already addressed"
    grep -n "^bare after add:" "$REPORT" | sed 's/^/       /' >&2
fi

# ---- and the configure addressed it --------------------------------------
#
# IfProbe passes IFC_Address AND IFC_NetMask.  The mask that comes back has to
# be the one it asked for: the classful fallback is for a caller that supplied
# an address alone, and using it here is the /8 this file exists for.
if grep -q "^netmask after the round trip: .*, the mask it had, correctly" "$REPORT"; then
    pass "IFQ_NetMask is the mask ConfigureInterfaceTagList() was given"
else
    fail "the configure did not honour IFC_NetMask, the classful guess again"
    grep -n "^netmask after the round trip:" "$REPORT" | sed 's/^/       /' >&2
fi

AFTER=$(routes 2)
if printf '%s\n' "$AFTER" | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.0 '; then
    pass "and the attached route is the /24 the configure asked for"
else
    fail "the attached route came back as something else, the classful mask"
    printf '%s\n' "$AFTER" | sed 's/^/       /' >&2
fi

# The gateway went with nx_ip_interface_detach() and neither vector has a tag
# for one, so it stays gone until a caller runs AddRouteTagList().  Asserted
# rather than ignored: putting it back inside the add is what was wrong before.
if printf '%s\n' "$AFTER" | grep -Eq '^default +'; then
    fail "a default route reappeared on its own, neither vector may install one"
    printf '%s\n' "$AFTER" | sed 's/^/       /' >&2
else
    pass "the default route is gone, as it must be, AddRouteTagList() puts it back"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifreadd: FAILED" >&2
    exit 1
fi

echo "ifreadd: PASSED"
exit 0
