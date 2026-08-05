#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR OUTBOUND SOURCE SELECTION.
#
#   tests/tools/run-srcsel.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                             [-A [-N board] [-B backend]]
#
# WHAT IT IS PROVING
#
#   bind() names a local address and the send direction has to honour it.
#   NetX Duo binds a socket to a port only, so the library maps the bound
#   address to the index nxd_udp_socket_source_send() takes for a datagram and
#   nxd_tcp_client_socket_source_connect() takes for a connect.
#
#   The assertion is on the source address the RECEIVER saw, not on the send
#   returning len, the send returned len before any of this existed, with
#   whatever source NetX picked.  The TCP arm asserts the address the
#   ACCEPTING end reports, for the same reason.
#
#   It also holds the refusal.  A destination the bound address cannot reach
#   is ENETUNREACH, for a datagram, which used to be dropped inside
#   _nx_ip_packet_send() with the send already reported successful, and for a
#   connect(), before the SYN.
#
# WHAT IT DOES NOT PROVE.  The case the source connect exists for is two
# interfaces, source on one, route out of the other, and the guest here
# has one.  What is measured is the single-interface half: that the bound
# address really is the source on loopback and on the interface, and that the
# refusal fires where the bound address has no route at all.
#
# The guest's own address is SLIRP's first lease, 10.0.2.15, and the peer is
# its gateway.  On another backend set AMINETXDUO_SRC_SELF / _PEER.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=420
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
PROBE="$ROOT/$BUILD/tests/tools/SrcProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

SELF="${AMINETXDUO_SRC_SELF:-10.0.2.15}"
PEER="${AMINETXDUO_SRC_PEER:-10.0.2.2}"

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/srcsel-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/SrcProbe"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:SrcProbe $SELF $PEER
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-srcsel}"

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/SrcProbe"
else
    HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/SrcProbe"
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

# The probe running twice means the machine reset, which a wrong source index
# into nx_ip_interface[] is one way to produce.
STARTS=$(grep -c "SYS:SrcProbe" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once and ran the probe (no reset)"
else
    fail "the probe line appears $STARTS times, the machine reset"
fi

SUMMARY=$(grep "^SrcProbe: .* checks, " "$REPORT" | head -1 || true)
if [ -z "$SUMMARY" ]; then
    fail "the probe never reached its summary line"
else
    echo "  $SUMMARY"
    case "$SUMMARY" in
        *"0 failures") pass "every check passed" ;;
        *) fail "$SUMMARY" ; grep "^  FAIL " "$REPORT" >&2 || true ;;
    esac
fi

# Named individually so a silently skipped arm cannot pass by absence.
for want in \
    "a datagram bound to the interface address has it as source" \
    "a datagram bound to 127.0.0.1 has it as source" \
    "an unbound datagram still leaves by the route" \
    "sending to loopback from the interface address is ENETUNREACH" \
    "connect to 127.0.0.1 from 127.0.0.1" \
    "connect to 127.0.0.1 from the interface address is ENETUNREACH" \
    "connect to the interface address from the interface address" \
    "the accepted peer address is the bound source" \
    "an unbound connect still leaves by the route" \
    "bind to a foreign address is EADDRNOTAVAIL"
do
    if grep -Fq "  ok   $want" "$REPORT"; then
        pass "$want"
    else
        fail "missing or failed: $want"
    fi
done

echo
if [ "$FAILED" -ne 0 ]; then
    echo "srcsel: FAILED" >&2
    exit 1
fi

echo "srcsel: PASSED"
exit 0
