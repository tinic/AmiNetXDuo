#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "the machine cannot see its own share".
#
#   tests/netstack/run-fitzquery.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT WENT WRONG
#
#   Reported from an A3000 with Fitz 1.21: `fitz serve ram: name ramdisk` in
#   one shell and `fitz query` in another, and the machine lists nothing.
#   Every other machine on the LAN lists the share, and throughput on the same
#   box is fine, so this is not the network being down.
#
#   `fitz query` broadcasts "LIST\n" to 255.255.255.255:17710
#   (fitz-common-client.c, fitz_pms_list_udp) and `fitz serve` binds that port
#   on INADDR_ANY.  Ethernet is simplex, a card does not hear its own
#   transmission, so a broadcast has to be copied back in software or the
#   sender never sees it.  NetX Duo's _nx_ip_driver_packet_send() copies one
#   back for a unicast to our own address and for a joined multicast group, and
#   did not for a broadcast; see NX_ENABLE_IP_BROADCAST_LOOPBACK in
#   port/netxduo-amiga/inc/nx_user.h and RESEARCH 45.
#
# WHY THIS RUNS FITZ AND NOT A HARNESS
#
#   tests/netstack/host/test_bcast_loopback_host.c already answers the question
#   at the packet level, deterministically, in a millisecond.  What it cannot
#   answer is whether the datagram reaches bsdsocket.library's recvfrom() and
#   whether the reply, which the server unicasts back to the sender's
#   ephemeral port on our own address, gets home.  This runs the reporter's
#   two commands, on the released Fitz binary, and reads what the second one
#   printed.
#
#   No host peer and no LAN: both halves are in the guest.  SLIRP is there only
#   so the interface has an address to broadcast from.
#
# Needs build/fitz (tests/endurance/fetch-fitz.sh) and an a2065.device, which
# is not ours to ship: point AMINETXDUO_A2065 at one, or drop a copy in
# build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=180
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
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
FITZ="$ROOT/build/fitz/Fitz/fitz"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
done

[ -f "$FITZ" ] || {
    echo "missing $FITZ -- run tests/endurance/fetch-fitz.sh" >&2; exit 2; }

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

STAGE="$ROOT/build/fitzquery-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$FITZ" "$STAGE/fitz"

# The reporter's two commands.  `&` is SYS_Asynch: the server stays resident,
# which is the whole point, it has to be listening when the query goes out.
# `fitz query` with no host is the broadcast form; `fitz query localhost` is
# the TCP form and would pass whether or not the broadcast is looped back, so
# it is here as a control rather than as the assertion.
cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
wait 4
&SYS:fitz serve RAM: name ramdisk
wait 6
SYS:fitz query
wait 2
SYS:fitz query
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-fitzquery}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/fitz"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

# ------------------------------------------------------------ assertions ---

FAILED=0

fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# One boot, or the transcript restarted from the top and means nothing --
# tests/tools/run-livetools.sh explains why this is checked from the banner.
BOOTS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$BOOTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: $BOOTS starts in one run"
elif [ "$BOOTS" -eq 1 ]; then
    pass "the machine booted exactly once"
else
    fail "no start in the transcript -- the run did not get far enough to judge"
fi

if grep -q "no named services found on LAN" "$REPORT"; then
    fail "'fitz query' found nothing -- the broadcast did not come back"
fi

# The share, in the shape `fitz query` prints it: two spaces, the name, the
# port and the server's pid.
if grep -qE "^  ramdisk [0-9]+ [0-9]+" "$REPORT"; then
    pass "'fitz query' listed this machine's own share"
    grep -E "^  ramdisk [0-9]+ [0-9]+" "$REPORT" | sed 's/^/       /'
else
    fail "'fitz query' did not list 'ramdisk'"
fi

# Both queries, because the first one racing the server's bind would look the
# same as a broadcast that is never delivered.
LISTED=$(grep -cE "^  ramdisk [0-9]+ [0-9]+" "$REPORT" || true)
if [ "$LISTED" -ge 2 ]; then
    pass "both queries listed it ($LISTED)"
else
    fail "only $LISTED of 2 queries listed the share"
fi

if grep -q "Roadshow detected" "$REPORT"; then
    fail "Fitz took its Roadshow path -- the result says nothing about ours"
fi

echo
if [ "$FAILED" = "0" ]; then
    echo "run-fitzquery: PASSED"
else
    echo "run-fitzquery: FAILED"
fi
exit "$FAILED"
