#!/usr/bin/env bash
#
# The packet-pool memory-share A/B of docs/PHYSICAL_RX_A1200.md.
#
#   tests/perf/run-poolshare.sh -b BUILDDIR -B IFACE -P PEERSSH
#                               [-N BOARD] [-m MODEL] [-a ADDRESS]
#                               [-d "16 8"] [-s SECONDS]
#
# WHAT IT MEASURES
#
#   Whether the low-memory receive window is the throughput.  The library under
#   test reads ENV:ANXDPOOLDIV at stack start, so every arm boots the SAME
#   binary and differs by one environment variable: the divisor over free
#   memory that sizes the packet pool.  16 is the shipped sixteenth; 8 is the
#   doc's prescribed eighth.  The guest receives TCP from a third machine at
#   whatever rate that machine chooses, which is the one direction the pool
#   can starve.
#
#   Run it with AMINETXDUO_FASTMEM=0 to model the machine this is about: a
#   2 MB chip-RAM A1200.  With fast memory present the pool sizes far from the
#   clamp and both arms should agree, which makes that configuration the
#   negative control, not the experiment.
#
# WHAT IT RECORDS, PER ARM
#
#   the guest's own pool accounting (netstat -s: total, fewest ever free,
#     found-empty count) after the transfer;
#   the peer's byte count and rate, which is what the sender's kernel saw
#     accepted, not what anything here printed;
#   the wire's zero-window count and window ceiling, from a pcap taken on the
#     emulator host's own interface, which carries every guest frame.
#
#   The doc asks for the whole causal chain: more pool storage, fewer or no
#   zero windows, less sender limitation, higher throughput.  One number
#   moving alone is not a result.
#
# WHY THE PEER IS A THIRD MACHINE
#
#   A bridged guest is unreachable from the emulator host (docs/RESEARCH.md
#   63), so the sender must be elsewhere; -P names it as an ssh destination
#   that has this repository's tests/tools/iperfpeer.py on PATH-reachable
#   python3.  The pcap, by contrast, MUST be taken on the emulator host: its
#   interface is the guest's port, so it sees every frame the guest sends and
#   receives, switched LAN or not.
#
# SPDX-License-Identifier: MIT

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

BUILDDIR=
IFACE=
PEERSSH=
BOARD=ne2000_pcmcia
MODEL=A1200
ADDRESS=192.168.1.240
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
DIVISORS="16 8"
SECONDS_ARM=20
PORT=5201

while getopts "b:B:P:N:m:a:d:s:" opt; do
    case "$opt" in
        b) BUILDDIR="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEERSSH="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        d) DIVISORS="$OPTARG" ;;
        s) SECONDS_ARM="$OPTARG" ;;
        *) echo "usage: $0 -b BUILDDIR -B IFACE -P PEERSSH [-N board]" \
                "[-m model] [-a addr] [-d \"16 8\"] [-s seconds]" >&2; exit 2 ;;
    esac
done

[ -n "$BUILDDIR" ] && [ -n "$IFACE" ] && [ -n "$PEERSSH" ] || {
    echo "usage: $0 -b BUILDDIR -B IFACE -P PEERSSH ..." >&2; exit 2; }

BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"
TOOLS="$BUILDDIR/src/tools"
for f in "$BSD" "$TOOLS/AddNetInterface" "$TOOLS/iperf" "$TOOLS/netstat"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
done

# The whole point is one binary for every arm; print what it is so the record
# survives the build directory.
echo "library_sha256=$(sha256sum "$BSD" | cut -d' ' -f1)"
echo "divisors=$DIVISORS fastmem=${AMINETXDUO_FASTMEM:-unset}"

RESULTS="$ROOT/build/poolshare-results-$$"
mkdir -p "$RESULTS"

for DIV in $DIVISORS; do
    TAG="poolshare$DIV"
    STAGE="$ROOT/build/poolshare-stage-$DIV"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$BSD" "$STAGE/libs/bsdsocket.library"
    cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
    cp "$TOOLS/iperf"           "$STAGE/iperf"
    cp "$TOOLS/netstat"         "$STAGE/netstat"

    . "$ROOT/tools/sana2-stage.sh"
    sana2_select "$BOARD" "$BUILDDIR"
    [ -n "$SANA2_SEL_PATH" ] || { echo "no driver for $BOARD" >&2; exit 2; }
    cp "$SANA2_SEL_PATH" "$STAGE/devs/$SANA2_SEL_DRIVER"

    cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=$SANA2_SEL_DRIVER
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF

    # The divisor is set explicitly in EVERY arm, the shipped 16 included, so
    # both arms take the identical code path through the reader.
    {
        echo "echo >ENV:ANXDPOOLDIV $DIV"
        echo "SYS:AddNetInterface eth0"
        echo "SYS:iperf -s -p $PORT -t $((SECONDS_ARM + 40))"
        echo "SYS:netstat -s"
    } > "$STAGE/commands.txt"

    PCAP="$RESULTS/arm-$DIV.pcap"
    timeout $((SECONDS_ARM + 220)) tcpdump -i "$IFACE" -s 128 -w "$PCAP" \
        "host $ADDRESS and tcp port $PORT" >/dev/null 2>&1 &
    TCPDUMP_PID=$!

    # The sender retries until the guest listens, then sends for the window.
    # Its last line is the peer_bytes= record the comparison uses.  The peer
    # runs this repository's own iperfpeer.py, copied fresh so the two ends
    # cannot be different versions of the wire format.
    PEERLOG="$RESULTS/arm-$DIV.peer"
    scp -q "$ROOT/tests/tools/iperfpeer.py" "$PEERSSH:/tmp/iperfpeer.$$.py"
    ssh -o BatchMode=yes "$PEERSSH" "
        for i in \$(seq 1 60); do
            out=\$(python3 /tmp/iperfpeer.$$.py --port $PORT \
                     --seconds $SECONDS_ARM --length 4096 send tcp $ADDRESS)
            case \$out in *peer_bytes=*) echo \"\$out\"; exit 0 ;; esac
            sleep 3
        done
        echo 'peer: the guest never accepted'; exit 1" > "$PEERLOG" 2>&1 &
    PEER_PID=$!

    export AMINETXDUO_RUN_TAG="$TAG"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t $((SECONDS_ARM + 180)) \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/iperf" \
        "$STAGE/netstat" > "$RESULTS/arm-$DIV.run" 2>&1
    RUN_RC=$?

    wait "$PEER_PID"
    kill "$TCPDUMP_PID" 2>/dev/null
    wait "$TCPDUMP_PID" 2>/dev/null

    HD="$ROOT/build/amiberry-testhd-$TAG"
    cp "$HD/tools.txt" "$RESULTS/arm-$DIV.guest" 2>/dev/null

    echo "==== arm $DIV (emulator rc=$RUN_RC) ===="
    grep -E "peer_bytes=|never accepted" "$PEERLOG" | tail -1
    grep -E "packets free|fewest|pool empty|window" \
        "$RESULTS/arm-$DIV.guest" 2>/dev/null | head -4
    if [ -s "$PCAP" ]; then
        Z=$(tcpdump -n -r "$PCAP" 2>/dev/null | grep -c "win 0,")
        W=$(tcpdump -n -r "$PCAP" 2>/dev/null |
            grep -oE "win [0-9]+" | sort -t' ' -k2 -n | tail -1)
        echo "wire: zero_windows=$Z max_advertised=$W"
    fi
done

echo
echo "results kept in $RESULTS"
echo "A useful result shows the WHOLE chain: more pool, fewer zero windows,"
echo "higher peer_bytes.  One number moving alone is not a result."
