#!/usr/bin/env bash
#
# A path narrower than the link: what the guest sends through a 1,400-byte hop.
#
#   tests/tools/run-pmtu.sh -B IFACE -R ROUTER [-S SERVER] [-m MODEL]
#                           [-t SECONDS] [-b BUILDDIR] [-N BOARD] [-a ADDR]
#                           [-g GATEWAY] [-r TRANSCRIPT]
#
# THE PATH.  Every card this stack drives sits on a 1,500-byte Ethernet, so
# nothing else in the tree ever puts a datagram larger than the link on the
# wire, or receives one that a router had to cut.  Users do: plipbox, PPP,
# WireGuard and every VPN carry less than 1,500, and genet.device 3.x dropped
# full-size frames outright (emu68-genet-driver #4, SMB2 froze until
# MTU=1450).  This harness routes the guest through a Linux namespace pair on
# ROUTER whose middle link is 1,400 bytes -- tools/pmtu-lab.sh builds it;
# ROUTER must be a THIRD machine, since a bridged guest's frames never reach
# the host that emulates it --
# and asks for what only that path can answer:
#
#   ping -s 1372     fits the hop, one datagram end to end
#   ping -s 1472     a full 1,500-byte datagram: the router cuts it in two,
#                    the far end reassembles, and the reply comes back cut
#                    the same way -- receive-side reassembly
#   ping -s 1473.. 65467
#                    larger than the guest's own link: the STACK fragments
#                    on transmit.  Refused with EMSGSIZE until 2026-09-19
#                    ("a raw sender asking for more than the link carries
#                    wants to be told"), which no BSD stack does, and which
#                    left NFS over UDP -- 8 KB reads and writes -- unable to
#                    write at all.
#   iperf -u -l 8000 the same through a UDP socket
#   iperf, fetch     TCP both ways: MSS against the far end's 1,500, the
#                    router fragmenting our DF-clear segments and telling
#                    the far end to shrink its DF-set ones
#
# WHAT IT NEEDS on ROUTER (the emulator host is fine): tools/pmtu-lab.sh up
# as root, an `iperf -s -p 5001', an `iperf -s -u -p 5002' and a `python3 -m
# http.server 8000' serving 20mb.bin, all inside the far namespace.  -S is
# the far end's address as the guest sees it (10.9.2.2 by default).
#
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=420
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE=""
ROUTER=""
SERVER=10.9.2.2
LABNET=10.9.0.0/16
REPLAY=""
ADDRESS="${AMINETXDUO_PMTU_ADDRESS:-192.168.1.241}"
GATEWAY="${AMINETXDUO_PMTU_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0
SECS=3

while getopts "m:t:b:B:R:S:N:a:g:r:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        R) ROUTER="$OPTARG" ;;
        S) SERVER="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        r) REPLAY="$OPTARG" ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac
TOOLS="$BUILDDIR/src/tools"
BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-pmtu}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
RUN_RC=0

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "no such transcript: $REPLAY" >&2; exit 2; }
    REPORT="$REPLAY"
    echo "==> REPLAY of $REPORT: nothing was run, this only checks the checks"
else
    [ -n "$IFACE" ] && [ -n "$ROUTER" ] || {
        echo "-B <iface> and -R <router address> are required: the guest" \
             "must be bridged to reach the router's namespace pair." >&2
        exit 2
    }
    for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
             "$TOOLS/AddNetRoute" "$TOOLS/ping" "$TOOLS/iperf" \
             "$TOOLS/fetch" "$TOOLS/netstat" "$BSD"; do
        [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
    done

    A2065=""
    if [ "$BOARD" = a2065 ]; then
        A2065="${AMINETXDUO_A2065:-}"
        if [ -z "$A2065" ]; then
            for candidate in "$ROOT/build/a2065.device" \
                             "$HOME/amiga-assets/devs/a2065.device"; do
                [ -f "$candidate" ] && { A2065="$candidate"; break; }
            done
        fi
        [ -n "$A2065" ] && [ -f "$A2065" ] || {
            echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
            exit 2
        }
    fi

    STAGE="$ROOT/build/pmtu-stage-$AMINETXDUO_RUN_TAG"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    . "$ROOT/tools/serial-log.sh"
    serial_log_stage_env "$STAGE" 2
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    [ -z "$A2065" ] || cp "$A2065" "$STAGE/devs/a2065.device"
    cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF
    if [ "$BOARD" != a2065 ]; then
        . "$ROOT/tools/sana2-stage.sh"
        if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ]; then
            sana2_select "$BOARD" "$BUILDDIR"
            [ -n "$SANA2_SEL_PATH" ] || {
                echo "-N $BOARD wants $SANA2_SEL_DRIVER and this host has not" \
                     "got it." >&2; exit 2; }
            export AMINETXDUO_SANA2_DRIVER="$SANA2_SEL_PATH"
            export AMINETXDUO_SANA2_DRIVER_NAME="${AMINETXDUO_SANA2_DRIVER_NAME:-$SANA2_SEL_DRIVER}"
            export AMINETXDUO_SANA2_DEVICE="${AMINETXDUO_SANA2_DEVICE:-$SANA2_SEL_DRIVER}"
            [ -z "$SANA2_SEL_CARD" ] ||
                export AMINETXDUO_SANA2_CARD="${AMINETXDUO_SANA2_CARD:-$SANA2_SEL_CARD}"
        fi
        sana2_stage "$BOARD" "$STAGE/devs"
    fi

    cp "$BSD" "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface AddNetRoute ping iperf fetch netstat; do
        cp "$TOOLS/$t" "$STAGE/$t"
    done

    {
        echo "SYS:AddNetInterface eth0"
        echo "SYS:AddNetRoute NETDST=$LABNET VIA=$ROUTER"
        for s in 56 1372 1472 1473 3000 8000 32000 65467; do
            echo "SYS:ping $SERVER -c 3 -s $s"
        done
        echo "SYS:iperf $SERVER -p 5001 -t $SECS"
        echo "SYS:iperf -u $SERVER -p 5002 -t $SECS -l 8000 -b 2000"
        echo "SYS:netstat -s"
        echo "SYS:netstat -i"
        echo "SYS:fetch http://$SERVER:8000/20mb.bin TO NIL:"
    } > "$STAGE/commands.txt"

    rm -f "$REPORT"
    set +e
    echo "==> booting $MODEL under Amiberry, $BOARD bridged on $IFACE," \
         "routing $LABNET via $ROUTER"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/AddNetRoute" \
        "$STAGE/ping" "$STAGE/iperf" "$STAGE/fetch" "$STAGE/netstat" \
        "$STAGE/env"
    RUN_RC=$?
    set -e
fi

if [ ! -f "$REPORT" ]; then
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    [ "$RUN_RC" = 124 ] &&
        echo "       rc 124 is the ${TIMEOUT}s timeout: the machine never" \
             "got as far as writing one." >&2
    exit 1
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# ToolsSmoke writes one block per command: "===== SYS:<command> =====", the
# output, then "----- rc N, ... -----".
block() {
    awk -v want="$1" '
        /^===== /    { on = (index($0, want) > 0); next }
        /^----- rc / { on = 0; next }
        on { print }
    ' "$REPORT"
}

if block "AddNetRoute" | grep -q "now go through"; then
    pass "the lab route is in"
else
    fail "AddNetRoute did not take the lab route"
fi

# 32000 and 65467 are reported, not required: a reply that size is 23 or 45
# back-to-back frames at wire speed, and the A2065 ring holds 16 (an X-Surf
# 13 KB, about 9).  Some go missing in the card, the datagram never completes,
# and that is the card's ceiling, not the stack's -- 8000, which is what NFS
# over UDP moves, has to cross every time.  Measured 2026-09-19: 32000 passed
# one run in two, 65467 neither, "64 dropped on receipt" beside them.
for s in 56 1372 1472 1473 3000 8000 32000 65467; do
    out=$(block "ping $SERVER -c 3 -s $s")
    if printf '%s\n' "$out" | grep -q "3 received, 0% packet loss"; then
        case "$s" in
            56|1372) pass "ping -s $s: one datagram, fits the hop" ;;
            1472)    pass "ping -s $s: full frame, cut by the router both ways" ;;
            *)       pass "ping -s $s: fragmented by the stack on transmit" ;;
        esac
    elif printf '%s\n' "$out" | grep -q "error 40"; then
        fail "ping -s $s: refused with EMSGSIZE, the stack did not fragment"
    elif [ "$s" -ge 32000 ] && printf '%s\n' "$out" | grep -q "packets transmitted"; then
        echo "note: ping -s $s: $(printf '%s\n' "$out" | grep -m1 'packets transmitted') -- the card's ring, see above"
    else
        fail "ping -s $s: $(printf '%s\n' "$out" | grep -m1 -E 'packets transmitted|cannot|error' || echo 'no verdict line')"
    fi
done

tcp=$(block "iperf $SERVER -p 5001" | sed -n 's/.*bits_per_sec=\([0-9]*\).*/\1/p' | head -1)
if [ -n "$tcp" ] && [ "$tcp" -gt 0 ]; then
    pass "TCP through the hop: $tcp bit/s"
else
    fail "TCP through the hop moved nothing"
fi

udp=$(block "iperf -u $SERVER -p 5002" | sed -n 's/.*bits_per_sec=\([0-9]*\).*/\1/p' | head -1)
if [ -n "$udp" ] && [ "$udp" -gt 0 ]; then
    pass "UDP with 8000-byte datagrams through the hop: $udp bit/s sent"
else
    fail "UDP with 8000-byte datagrams sent nothing: $(block "iperf -u $SERVER -p 5002" | grep -m1 -i 'error\|cannot' || echo 'no verdict line')"
fi

if block "fetch http://$SERVER:8000/20mb.bin" | grep -q "20971520 bytes"; then
    pass "fetch brought the whole 20 MB through the hop"
else
    fail "fetch did not bring 20 MB: $(block "fetch http://$SERVER:8000/20mb.bin" | tail -1)"
fi

frags=$(block "netstat -s" | sed -n 's/^[[:space:]]*\([0-9][0-9]*\) fragments sent, \([0-9][0-9]*\) received.*/\1 \2/p' | head -1)
sent=${frags%% *}; recv=${frags##* }
if [ -n "$frags" ] && [ "${sent:-0}" -gt 0 ] && [ "${recv:-0}" -gt 0 ]; then
    pass "netstat counts fragments both ways: $sent sent, $recv received"
else
    fail "netstat fragment counters: '${frags:-absent}'"
fi

echo
if [ "$FAILED" = 0 ]; then
    echo "pmtu=PASS"
    exit 0
fi
echo "pmtu=FAIL"
exit 1
