#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "IfProbe takes the machine's network away".
#
#   tests/tools/run-ifreadd.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT WENT WRONG
#
#   RemoveInterface() followed by AddInterfaceTagList() -- the round trip the
#   autodoc describes as the point of the pair -- gave eth0 back with the
#   CLASSFUL mask for its address and no default route.  `netstat -r` went from
#
#       10.0.2.0    *          255.255.255.0  U   eth0
#       default     10.0.2.2   0.0.0.0        UG  eth0
#
#   to
#
#       10.0.0.0    *          255.0.0.0      U   eth0
#
#   so the machine kept its address and lost every destination that was not on
#   its own wire.  Two causes with one shape: AddInterfaceTagList() carries a
#   name, a device, a unit and an MTU and nothing else, and the add path copied
#   that empty configuration over the machine's own -- the only record of the
#   mask that was left, since DEVS:NetInterfaces is read at startup and never
#   again.  ConfigureInterfaceTagList() then guessed the mask from the address
#   class, which is what it is documented to do for an interface that has none.
#   The gateway went with nx_ip_interface_detach(), which clears it when it
#   belonged to the interface being detached, and nothing put it back.
#
# WHY eth0 IS STATIC HERE, AND NOT DHCP
#
#   Because a static interface is the case where nothing on the machine could
#   put the configuration back even in principle.  A DHCP interface has a client
#   that may re-bind and write the address, the mask and the gateway back, so a
#   transcript showing them is not evidence that the stack kept them -- it may
#   be evidence that something else replaced them.  Under STATIC there is no
#   second source, and what the routing table says after the round trip is what
#   the remove-and-add did and nothing else.
#
#   It is also the configuration a user with a fixed address has, and the one
#   where the loss is permanent: DEVS:NetInterfaces is read at startup and never
#   again, so once the add path has overwritten the stored copy there is nothing
#   left to read it back from.
#
# WHY NOT IN run-ifquery.sh, WHICH ALSO RUNS IfProbe
#
#   That harness runs IfProbe as its FIRST command, and this stack cannot get
#   past the `AddNetInterface` that follows the probe -- observed hanging there
#   on more than one build.  That is a separate defect and not this one, which
#   is why nothing is added to that file: an assertion behind a hang is an
#   assertion that never runs.
#
# WHAT IS ASSERTED
#
#   * the routing table before the round trip has the /24 and the default;
#   * IfProbe's own IFQ_NetMask reads back the mask it started with;
#   * the routing table after the round trip has both again.
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

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$TOOLS/NetShutdown" "$PROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
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
# address it would hand out, 10.0.2.2 is the gateway it answers on, and
# tests/tools/ifprobe.c reconfigures with 10.0.2.15 after the re-add -- so the
# probe puts back the address this file asked for and nothing else changes.
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
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/NetShutdown" \
    "$STAGE/IfProbe"
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
    fail "eth0 did not start with the /24 -- nothing to lose, so nothing proved"
    printf '%s\n' "$BEFORE" | sed 's/^/       /' >&2
fi

if printf '%s\n' "$BEFORE" | grep -Eq '^default +10\.0\.2\.2 '; then
    pass "and with the default route through 10.0.2.2"
else
    fail "eth0 started with no default route -- nothing to lose, so nothing proved"
    printf '%s\n' "$BEFORE" | sed 's/^/       /' >&2
fi

# ---- and what it came back with ------------------------------------------
if grep -q "^netmask after the round trip: .* -- the mask it had, correctly" "$REPORT"; then
    pass "IFQ_NetMask reads back the mask it started with"
else
    fail "the re-added interface did not come back with the mask it had"
    grep -n "^netmask after the round trip:" "$REPORT" | sed 's/^/       /' >&2
fi

AFTER=$(routes 2)
if printf '%s\n' "$AFTER" | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.0 '; then
    pass "the attached route is still the /24 afterwards"
else
    fail "the attached route came back as something else -- the classful mask"
    printf '%s\n' "$AFTER" | sed 's/^/       /' >&2
fi

if printf '%s\n' "$AFTER" | grep -Eq '^default +10\.0\.2\.2 '; then
    pass "and the default route survived the remove-and-add"
else
    fail "the default route did not survive the remove-and-add"
    printf '%s\n' "$AFTER" | sed 's/^/       /' >&2
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifreadd: FAILED" >&2
    exit 1
fi

echo "ifreadd: PASSED"
exit 0
