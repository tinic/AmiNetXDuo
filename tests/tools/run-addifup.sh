#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "AddNetInterface never came back".
#
#   tests/tools/run-addifup.sh [-t SECONDS] [-b BUILDDIR]
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
SMOKE="$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$SHOW" "$SMOKE"; do
    [ -f "$f" ] || { echo "build $BUILD first: no $f" >&2; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

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

cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
EOF

cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface DEVS:NetInterfaces/eth0
SYS:ShowNetStatus
SYS:ShowNetStatus INTERFACES
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

. "$ROOT/tests/tools/addifup-verdict.sh"

if addifup_verdict "$REPORT" eth0; then
    echo
    echo "PASS: AddNetInterface comes back, and the interface is up"
    exit 0
fi
echo
echo "FAIL: see above; the drive is at $HD"
exit 1
