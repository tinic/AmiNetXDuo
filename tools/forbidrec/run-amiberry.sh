#!/usr/bin/env bash
#
# forbidrec under Amiberry: the self-test on a quiet machine, then the
# self-test and a recording while the stack moves traffic.
#
#   tools/forbidrec/run-amiberry.sh [-b BUILDDIR] [-m MODEL] [-t SECONDS] [-3]
#
# -3 boots the A1200's OS 3.2 Kickstart ($AMINETXDUO_KICKSTART_A1200_32)
# instead of 3.1.  The guest is an A2065 on SLIRP; the peer is
# tests/tools/iperfpeer.py on this host.  The staging is run-iperf.sh's.
#
# Prints the guest's forbidrec lines, then verdict=pass|fail.  Exit 0 pass,
# 1 fail, 2 setup.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=400
BUILD="${AMINETXDUO_BUILD:-build/cm}"
OS32=""

while getopts "b:m:t:3" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        3) OS32=1 ;;
        *) sed -n '3,14p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac
TOOLS="$BUILDDIR/src/tools"
BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-forbidrec${OS32:+32}}"
PORT_BASE=$((20000 + ($(printf '%s' "$AMINETXDUO_RUN_TAG" | cksum |
                        cut -d' ' -f1) % 900) * 10))
PORT_TCP=$((PORT_BASE + 1))
PORT_UDP=$((PORT_BASE + 2))

if [ -n "$OS32" ]; then
    [ -f "${AMINETXDUO_KICKSTART_A1200_32:-}" ] || {
        echo "-3 needs AMINETXDUO_KICKSTART_A1200_32" >&2; exit 2; }
    export AMINETXDUO_KICKSTART_A1200="$AMINETXDUO_KICKSTART_A1200_32"
fi

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/iperf" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done
A2065="${AMINETXDUO_A2065:-$HOME/amiga-assets/devs/a2065.device}"
[ -f "$A2065" ] || { echo "no a2065.device; set AMINETXDUO_A2065" >&2; exit 2; }

# The recorder is the tree's own build of it, -mregparm=3 and LTO included:
# what passes here is the binary that goes to the machine.
OUTDIR="$BUILDDIR/tools/forbidrec"
[ -f "$OUTDIR/forbidrec" ] || {
    echo "missing $OUTDIR/forbidrec, build the forbidrec target first" >&2; exit 2; }

# ---------------------------------------------------------------- staging ---
STAGE="$ROOT/build/forbidrec-stage-$AMINETXDUO_RUN_TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$TOOLS/iperf" "$OUTDIR/forbidrec" "$STAGE/"

{
    echo "SYS:forbidrec SELFTEST SECS=2"
    echo "SYS:AddNetInterface eth0"
    echo "&SYS:iperf 10.0.2.2 -p $PORT_TCP -t 40 >DH0:iperf-tcp.txt"
    echo "&SYS:iperf -u 10.0.2.2 -p $PORT_UDP -t 40 -b 2000 >DH0:iperf-udp.txt"
    echo "SYS:forbidrec SELFTEST SECS=30"
    echo "SYS:forbidrec SECS=3 NOSAMPLER"
} > "$STAGE/commands.txt"

# ------------------------------------------------------------------- peer ---
PEERLOG="$ROOT/build/forbidrec-peer-$AMINETXDUO_RUN_TAG"
mkdir -p "$PEERLOG"
python3 "$ROOT/tests/tools/iperfpeer.py" serve tcp --port "$PORT_TCP" \
    --seconds $((TIMEOUT + 30)) > "$PEERLOG/tcp.out" 2>&1 &
P1=$!
python3 "$ROOT/tests/tools/iperfpeer.py" serve udp --port "$PORT_UDP" \
    --seconds $((TIMEOUT + 30)) > "$PEERLOG/udp.out" 2>&1 &
P2=$!
trap 'kill $P1 $P2 2>/dev/null || true' EXIT INT TERM HUP

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B slirp -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/iperf" "$STAGE/forbidrec" \
    > "$ROOT/build/forbidrec-$AMINETXDUO_RUN_TAG.log" 2>&1
RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "run_rc=$RC verdict=fail reason=no_transcript"; exit 1; }
grep -E '^(selftest|hist|result|patched|clock_probe|sampler|counters|max|hold|sample|unmatched) ' \
     "$REPORT" || true
grep -E '^result=|^selftest |^----- rc' "$REPORT" || true

# Three runs, each ending with result= and an unpatch that restored exec;
# both self-tests pass; the machine lived to run the third one.
n_res=$(grep -c '^result=.* unpatch=restored inflight=0 ' "$REPORT" || true)
n_st=$(grep -c ' selftest=pass$' "$REPORT" || true)
if [ "$n_res" = 3 ] && [ "$n_st" = 2 ]; then
    echo "run_rc=$RC results=$n_res selftests_passed=$n_st verdict=pass"
    exit 0
fi
echo "run_rc=$RC results=$n_res selftests_passed=$n_st verdict=fail"
exit 1
