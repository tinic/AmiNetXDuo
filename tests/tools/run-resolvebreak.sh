#!/usr/bin/env bash
# HOW LONG A NAME LOOKUP BLOCKS, AND WHETHER CTRL-C GETS IT BACK.
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=1200
BUILD="${AMINETXDUO_BUILD:-build/cm}"
# BRIDGED, AND ONLY BRIDGED.  This used to carry two branches, `-A` picking
# between them: one passed -B and one did not, so the default was
# AMINETXDUO_RUNNER=fsuae falling through to amiberry-run.sh with no backend,
# which is SLIRP.  Nothing else differed between them.  The measurement does
# not need SLIRP -- the blackhole is 192.0.2.1 and the name is probe.invalid,
# neither of which anything on a real segment answers -- and a run over the
# emulator's own TCP/IP is not a measurement of this stack blocking.
IFACE="${AMINETXDUO_RESOLVEBREAK_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"
LIBRARY=""
DELAY=5

while getopts "m:t:b:l:d:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        l) LIBRARY="$OPTARG" ;;
        d) DELAY="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-l library] [-d seconds] [-B interface]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
PROBE="$ROOT/$BUILD/tests/tools/ResolveBreak"
BSD="${LIBRARY:-$ROOT/$BUILD/src/bsdsocket/bsdsocket.library}"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-$ROOT/build/a2065.device}"
[ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

BLACKHOLE="${AMINETXDUO_RB_BLACKHOLE:-192.0.2.1}"
NAME="${AMINETXDUO_RB_NAME:-probe.invalid}"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-resolvebreak}"

STAGE="$ROOT/build/resolvebreak-stage-$AMINETXDUO_RUN_TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/ResolveBreak"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:ResolveBreak $BLACKHOLE $NAME $DELAY
EOF

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

set +e
echo "==> booting $MODEL under Amiberry, a2065 bridged on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/ResolveBreak"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="

grep -q "^0 failure" "$REPORT" || {
    echo "FAIL: the probe reported failures" >&2
    exit 1
}

echo "PASS"
