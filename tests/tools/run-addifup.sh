#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "AddNetInterface never came back".
#
#   tests/tools/run-addifup.sh [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT PROVES
#
#   AddNetInterface, against a card that works and an interface configured the
#   way the installer writes it, RETURNS, and the interface is online with an
#   address afterwards.
#
# WHY IT EXISTS, given that run-addifleak.sh is already named for this command
#
#   That one drives the FAILURE path on purpose: a card configured down, so
#   bring-up fails on the DHCP wait and the memory it took can be counted back.
#   It is a good test and it says so itself.  What neither it nor anything else
#   in tier 2 did was run the command the way a user does and watch it finish.
#
#   0.17.0 and 0.17.1 shipped with that path deadlocked.  bsd_lib_open() holds
#   sb_Lock across bsd_netstack_bringup(), which blocks until DHCP answers; the
#   first lease arrived on the IP thread, reached bsd_address_changed(), and
#   waited there for the same semaphore.  Neither side moved again.  The line
#   the installer puts in S:User-Startup never returned, so a machine that had
#   just installed AmiNetXDuo stopped part way through its Startup-Sequence.
#
#   Every tier-2 test passed throughout, on both arms, because none of them
#   opens bsdsocket.library against a real SANA-II driver and lets DHCP run.
#   install/test/run-workbench.sh found it, and that needs a Workbench, five
#   ADFs and Commodore's Installer.  This needs a ROM and a driver, so it can
#   run wherever the rest of tier 2 does.
#
# A HANG IS THE FAILURE, AND THE TIMEOUT IS THE ASSERTION
#
#   The guest writes its report only after AddNetInterface returns.  A deadlock
#   therefore produces no report at all, which is what this reads as failure --
#   the same shape the harness gives a crash, so the message below says which
#   it is and how to tell them apart.
#
#   CONFIGURE=STATIC is the discriminator worth knowing: it skips the lease
#   entirely, so a run that hangs on DHCP and comes up static places the fault
#   in the lease path rather than in the driver or the library open.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

TIMEOUT=180
BUILD="${AMINETXDUO_BUILD:-build/ci/default}"

while getopts "t:b:" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$BUILD/src/tools/AddNetInterface"
SHOW="$BUILD/src/tools/ShowNetStatus"
# ToolsSmoke, not tools/smoke/smoke: this is the one that reads
# commands.txt and writes DH0:tools.txt (src/tools/toolssmoke.c).
SMOKE="$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$SHOW" "$SMOKE"; do
    [ -f "$f" ] || { echo "build $BUILD first: no $f" >&2; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

# The A2065 is the card Amiberry emulates that this tree is tested most against,
# and unlike the PCMCIA one it reaches a lease on an A1200 for us and for
# Roadshow both, so a failure here is ours.
A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/addifup-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$ADDIF" "$STAGE/AddNetInterface"
cp "$SHOW"  "$STAGE/ShowNetStatus"

# Exactly what install/Install-AmiNetXDuo writes, DHCP and all.  A test that
# configured it any other way would not be testing what a user has.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
EOF

cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface DEVS:NetInterfaces/eth0
SYS:ShowNetStatus
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-addifup}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting an A1200 with a2065.device, CONFIGURE=DHCP"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -t "$TIMEOUT" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/ShowNetStatus" "$STAGE/commands.txt"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || {
    echo
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    echo
    echo "  AddNetInterface did not return.  The report is written after it" >&2
    echo "  does, so a deadlock leaves nothing at all, which is also what a" >&2
    echo "  crash leaves.  To tell them apart, put CONFIGURE=STATIC and an" >&2
    echo "  ADDRESS in $STAGE/devs/NetInterfaces/eth0 and run again: static" >&2
    echo "  skips the lease, so coming up that way places the fault in the" >&2
    echo "  DHCP path rather than in the driver or the library open." >&2
    echo >&2
    echo "  That is where 0.17.0 and 0.17.1 sat: bsd_lib_open() held sb_Lock" >&2
    echo "  across the bring-up and the lease waited for it on the IP thread." >&2
    exit 1
}

echo
echo "===================== what the guest printed ======================="
cat "$REPORT"
echo "===================================================================="
echo

# ---------------------------------------------------------- the verdict ---

FAILED=0
pass() { echo "  ok: $*"; }
fail() { echo "  FAIL: $*"; FAILED=1; }

if grep -qiE "online|address" "$REPORT"; then
    pass "AddNetInterface returned and the interface reports a state"
else
    fail "AddNetInterface returned but the interface never came up"
fi

if grep -qE "[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" "$REPORT"; then
    pass "an address arrived"
else
    fail "no address in the report, the lease never completed"
fi

echo
if [ "$FAILED" = "0" ]; then
    echo "PASS: AddNetInterface comes back, and the interface is up"
    exit 0
fi
echo "FAIL: see above; the drive is at $HD"
exit 1
