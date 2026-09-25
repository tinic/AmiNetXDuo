#!/usr/bin/env bash
# THE #38 FOREIGN-MULTICAST RUN.
#
#   tests/tools/run-mcastshare.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                 [-N BOARD] [-B IFACE] [-n NAME] [-H HOST]
#
# One bridged boot of a guest whose built-in mDNS responder holds UDP 5353
# (MDNS=YES), plus a co-bound BSD listener (McastRecv) that sets SO_REUSEPORT
# and binds the same port.  A process OUTSIDE the guest then puts one mDNS
# query for the guest's own name on 224.0.0.251:5353.  Two independent pieces
# of evidence say the same foreign multicast was cloned to both sharers:
#
#   - McastRecv's transcript (a "mcastrecv: recv ... from <host>" line) says
#     the co-bound BSD listener received it;
#   - mcastquery.py's "REPLY from <guest>" line says the built-in responder
#     received it and answered.
#
# BRIDGED ONLY, NEVER SLIRP: a foreign multicast has to cross the wire to be
# foreign.  -B names the host NIC to bridge onto, and `slirp` is refused.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
NAME="${AMINETXDUO_MCAST_NAME:-amigatest}"
QHOST="${AMINETXDUO_MCAST_QUERY_HOST:-}"

while getopts "m:t:b:N:B:n:H:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        n) NAME="$OPTARG" ;;
        H) QHOST="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B iface] [-n name] [-H host]" >&2
           exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "mcastshare_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
RECV="$ROOT/$BUILD/tests/tools/McastRecv"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/host" \
         "$RECV" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# The stack is anxnet.device (NetX Duo), not a vendor SANA-II driver: it
# drives the a2065 LANCE directly and is where both the built-in mDNS
# responder and the UDP fan-out under test live.  Resolve the build dir for
# sana2_select(), which locates anxnet.device inside it.
case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/mcastshare-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"

# AMI_LOG_INFO on the serial port, which is the tier the bring-up marks are
# at; a boot that never moved a byte still records what it configured.
. "$ROOT/tools/serial-log.sh"
serial_log_stage_env "$STAGE" 2

cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"

# Stage anxnet.device and point eth0 at it, exactly as run-iperf.sh does for
# a bridged boot.  The default eth0 keeps CONFIGURE=DHCP, so the guest leases
# a real LAN address and the host's foreign query reaches a real mDNS
# responder.
. "$ROOT/tools/sana2-stage.sh"
sana2_select a2065 "$BUILDDIR"
if [ -z "$SANA2_SEL_PATH" ]; then
    echo "anxnet.device not built or not found; build the tree first" >&2
    exit 2
fi
export AMINETXDUO_SANA2_DRIVER="$SANA2_SEL_PATH"
export AMINETXDUO_SANA2_DRIVER_NAME="${AMINETXDUO_SANA2_DRIVER_NAME:-$SANA2_SEL_DRIVER}"
export AMINETXDUO_SANA2_DEVICE="${AMINETXDUO_SANA2_DEVICE:-$SANA2_SEL_DRIVER}"
[ -z "$SANA2_SEL_CARD" ] ||
    export AMINETXDUO_SANA2_CARD="${AMINETXDUO_SANA2_CARD:-$SANA2_SEL_CARD}"
sana2_stage a2065 "$STAGE/devs"

cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/host"            "$STAGE/host"
cp "$RECV"                  "$STAGE/McastRecv"

# The built-in mDNS responder binds 5353 (opting into sharing) when the
# interface comes up with MDNS=YES.  Appended AFTER sana2_stage, which
# rewrites the DEVICE= line and may add CARD=.
echo "MDNS=YES" >> "$STAGE/devs/NetInterfaces/eth0"
echo "hostname $NAME.home.lan" >> "$STAGE/devs/Internet/name_resolution"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 3
SYS:host $NAME.local
wait 1
SYS:McastRecv
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-mcastshare}"
SERIAL="$ROOT/build/amiberry-serial-${AMINETXDUO_RUN_TAG:-mcastshare}.log"
QUERYLOG="$ROOT/build/mcastquery-${AMINETXDUO_RUN_TAG:-mcastshare}.log"

# The serial log holds the previous run's MCASTSHARE-READY until amiberry-run.sh
# truncates it (it does, but only after we have already started polling for a
# fresh marker below).  Remove it first so a stale marker from an earlier boot
# can never make us fire the foreign query before this boot's McastRecv is
# actually listening.
rm -f "$SERIAL" "$SERIAL.stamped" "$QUERYLOG"

# -------------------------------------------------------------- booting ---

echo "==> booting $MODEL with the A2065 bridged on $IFACE"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/host" "$STAGE/McastRecv" &
RUN_PID=$!
set -e

# Wait for McastRecv's serial marker, then send the foreign multicast while it
# is listening.  The marker goes to the serial log (RawPutChar), which the
# harness streams in real time; Printf output goes to DH0:stdout.txt and is
# only read after the run.
READY=0
for _ in $(seq 1 "$TIMEOUT"); do
    if grep -aq "MCASTSHARE-READY" "$SERIAL" 2>/dev/null; then
        READY=1
        break
    fi
    if ! kill -0 "$RUN_PID" 2>/dev/null; then
        break
    fi
    sleep 1
done

if [ "$READY" = 1 ]; then
    if [ -n "$QHOST" ]; then
        # A foreign multicast has to egress ANOTHER host's NIC.  This host's
        # own outbound frames are not captured by amiberry's pcap socket, so a
        # query sent here never crosses the wire and never reaches the guest.
        # Send the script to $QHOST and run it there; it egresses that host's
        # default route, which IS foreign to amiberry's capture.
        scp -q "$ROOT/tests/tools/mcastquery.py" "$QHOST:/tmp/mcastquery.py" || true
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$QHOST" \
            "python3 /tmp/mcastquery.py $NAME.local --repeat 3 --wait 4" \
            > "$QUERYLOG" 2>&1 || true
    else
        python3 "$ROOT/tests/tools/mcastquery.py" "$NAME.local" \
            --iface "$IFACE" --repeat 3 --wait 4 > "$QUERYLOG" 2>&1 || true
    fi
else
    echo "mcastshare: never saw MCASTSHARE-READY on the serial log" >&2
fi

wait "$RUN_PID"
RUN_RC=$?

REPORT="$ROOT/build/amiberry-testhd-${AMINETXDUO_RUN_TAG:-mcastshare}/tools.txt"
[ -f "$REPORT" ] || {
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    exit 1
}

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo
echo "===================== what the host's query saw ===================="
[ -f "$QUERYLOG" ] && cat "$QUERYLOG"
echo "====================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# 1. the co-bound BSD listener received a foreign multicast.
if grep -q "mcastrecv: recv .* bytes from " "$REPORT"; then
    pass "the co-bound BSD 5353 listener received a datagram"
    grep "mcastrecv: recv" "$REPORT" | sed 's/^/       /'
else
    fail "the co-bound BSD 5353 listener received nothing"
fi

# 2. the built-in mDNS responder answered the foreign query.
if [ -f "$QUERYLOG" ] && grep -q "REPLY from " "$QUERYLOG"; then
    pass "the built-in mDNS responder answered the foreign query"
    grep "REPLY from" "$QUERYLOG" | sed 's/^/       /'
else
    fail "the built-in mDNS responder did not answer"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "mcastshare: FAILED" >&2
    exit 1
fi

echo "mcastshare: PASSED"
exit 0
