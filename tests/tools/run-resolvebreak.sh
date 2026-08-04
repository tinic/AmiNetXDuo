#!/usr/bin/env bash
#
# HOW LONG A NAME LOOKUP BLOCKS, AND WHETHER CTRL-C GETS IT BACK.
#
#   tests/tools/run-resolvebreak.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                   [-l LIBRARY] [-d SECONDS] [-A [-B backend]]
#
# WHAT IT IS PROVING
#
#   1. THE CALLER'S TIMEOUT IS THE WHOLE LOOKUP.  bsdsocket.library asks the
#      resolver for thirty seconds.  NetX Duo's DNS client reads that as a
#      per-query wait and spends it NX_DNS_MAX_RETRIES times over every
#      configured server, doubling between rounds, so one gethostbyname()
#      against a single unreachable server was 30 + 60 + 64 seconds and five
#      of them just under thirteen minutes.  Arm 1 times the call.
#
#   2. THE BREAK SIGNAL IS SAMPLED.  SetSocketSignals documents SIGINT as "the
#      signal to send to the process which owns the socket in order to abort a
#      blocking operation".  Every other blocking call here honours it; the
#      resolver did not.  Arm 2 sets it before the call, arm 3 has a child
#      process send it in the middle, and the interval after it was sent is
#      the number that decides whether Ctrl-C feels like it works.
#
# THE ONE EXTERNAL DEPENDENCY, stated rather than hidden: an address that
# nothing answers.  192.0.2.1 is in RFC 5737's TEST-NET-1, reserved for
# documentation, so no real name server can be there, but a network that
# answers it (a captive portal, a router that NXDOMAINs everything) makes the
# measurement meaningless, and the probe fails rather than reporting a number
# from it.
#
# -l runs a different bsdsocket.library than the one in the build directory,
# which is how a before and an after are compared with one probe.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
# Arm 1 is meant to be slow: it is the measurement.  Before the ladder moved
# out of the DNS client it could be thirteen minutes on its own.
TIMEOUT=1200
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RUNNER="${AMINETXDUO_RUNNER:-fsuae}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"
LIBRARY=""
DELAY=5

while getopts "m:t:b:l:d:AB:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        l) LIBRARY="$OPTARG" ;;
        d) DELAY="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-l library] [-d seconds] [-A [-B backend]]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
PROBE="$ROOT/$BUILD/tests/tools/ResolveBreak"
BSD="${LIBRARY:-$ROOT/$BUILD/src/bsdsocket/bsdsocket.library}"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-$ROOT/build/a2065.device}"
[ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

BLACKHOLE="${AMINETXDUO_RB_BLACKHOLE:-192.0.2.1}"
NAME="${AMINETXDUO_RB_NAME:-probe.invalid}"

# ------------------------------------------------------------- staging ---

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

# ------------------------------------------------------------------ run ---

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL under Amiberry, a2065 on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/ResolveBreak"
else
    HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/ResolveBreak"
fi
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
