#!/usr/bin/env bash
# The packet-pool memory-share A/B.
# SPDX-License-Identifier: MIT

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

BUILDDIR=
IFACE=
PEERSSH=
BOARD=ne2000_pcmcia
MODEL=A1200
ADDRESS=
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
DIVISORS="16 8"
# The application read size, in bytes, one arm per value.  Fetch (notify to
# recv()) is paid per call and not per byte, so the read size decides how
# OFTEN it is paid: 8192 pays it an eighth as often as 1024 over the same
# bytes.  Nothing varied it until this existed, and the leg was read as if
# the size of the read did not enter into it.
READSIZES="4096"
SECONDS_ARM=20
PORT=5201

while getopts "b:B:P:N:m:a:d:r:s:" opt; do
    case "$opt" in
        b) BUILDDIR="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEERSSH="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        d) DIVISORS="$OPTARG" ;;
        r) READSIZES="$OPTARG" ;;
        s) SECONDS_ARM="$OPTARG" ;;
        *) echo "usage: $0 -b BUILDDIR -B IFACE -P PEERSSH [-N board]" \
                "[-m model] [-a addr] [-d \"16 8\"] [-r \"4096 8192\"]" \
                "[-s seconds]" >&2; exit 2 ;;
    esac
done

[ -n "$BUILDDIR" ] && [ -n "$IFACE" ] && [ -n "$PEERSSH" ] || {
    echo "usage: $0 -b BUILDDIR -B IFACE -P PEERSSH ..." >&2; exit 2; }

BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"
TOOLS="$BUILDDIR/src/tools"
for f in "$BSD" "$TOOLS/AddNetInterface" "$TOOLS/iperf" "$TOOLS/netstat"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
done

if [ -z "$ADDRESS" ]; then
    # shellcheck source=../../tools/emu-rig-lock.sh
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_claim_address "${AMINETXDUO_RIG_ADDR_PREFIX:-192.168.1}" \
                      "${AMINETXDUO_RIG_ADDR_FIRST:-200}" \
                      "${AMINETXDUO_RIG_ADDR_LAST:-254}" \
                      "poolshare in $ROOT" || {
        echo "no free guest address; pass -a <addr> to pin one" >&2; exit 2; }
    ADDRESS="$RIG_ADDRESS"
fi

echo "library_sha256=$(sha256sum "$BSD" | cut -d' ' -f1)"
# -b may point at a per-CPU build, so the codegen the number belongs to is
# recorded next to it; without this a peer_bytes= line is unattributable.
echo "cpu=$(sed -n 's/^AMINETXDUO_CPU:STRING=//p' \
    "$BUILDDIR/CMakeCache.txt" 2>/dev/null || echo unknown)" \
     "arch=$(sed -n 's/^AMIGA_ARCH_FLAGS:STRING=//p' \
    "$BUILDDIR/CMakeCache.txt" 2>/dev/null || echo unknown)"
echo "divisors=$DIVISORS readsizes=$READSIZES" \
     "fastmem=${AMINETXDUO_FASTMEM:-unset} z3mem=${AMINETXDUO_Z3MEM:-unset}"
echo "guest_address=$ADDRESS"

RESULTS="$ROOT/build/poolshare-results-$$"
mkdir -p "$RESULTS"

for DIV in $DIVISORS; do
  for RSZ in $READSIZES; do
    ARM="$DIV-$RSZ"
    TAG="poolshare$ARM"
    STAGE="$ROOT/build/poolshare-stage-$ARM"
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

    {
        echo "echo >ENV:ANXDPOOLDIV $DIV"
        echo "SYS:AddNetInterface eth0"
        echo "SYS:iperf -s -p $PORT -t $((SECONDS_ARM + 40)) -l $RSZ"
        echo "SYS:netstat -s"
    } > "$STAGE/commands.txt"

    PCAP="$RESULTS/arm-$ARM.pcap"
    timeout $((SECONDS_ARM + 220)) tcpdump -i "$IFACE" -s 128 -w "$PCAP" \
        "host $ADDRESS and tcp port $PORT" >/dev/null 2>&1 &
    TCPDUMP_PID=$!

    PEERLOG="$RESULTS/arm-$ARM.peer"
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
    AMINETXDUO_RIG_EXCLUSIVE="poolshare arm $ARM" \
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t $((SECONDS_ARM + 180)) \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/iperf" \
        "$STAGE/netstat" > "$RESULTS/arm-$ARM.run" 2>&1
    RUN_RC=$?

    wait "$PEER_PID"
    kill "$TCPDUMP_PID" 2>/dev/null
    wait "$TCPDUMP_PID" 2>/dev/null

    HD="$ROOT/build/amiberry-testhd-$TAG"
    cp "$HD/tools.txt" "$RESULTS/arm-$ARM.guest" 2>/dev/null

    echo "==== arm divisor=$DIV readsize=$RSZ (emulator rc=$RUN_RC) ===="
    grep -E "peer_bytes=|never accepted" "$PEERLOG" | tail -1
    grep -E "packets free|fewest|pool empty|window" \
        "$RESULTS/arm-$ARM.guest" 2>/dev/null | head -4
    if [ -s "$PCAP" ]; then
        Z=$(tcpdump -n -r "$PCAP" 2>/dev/null | grep -c "win 0,")
        W=$(tcpdump -n -r "$PCAP" 2>/dev/null |
            grep -oE "win [0-9]+" | sort -t' ' -k2 -n | tail -1)
        echo "wire: zero_windows=$Z max_advertised=$W"
    fi
  done
done

echo
echo "results kept in $RESULTS"
echo "A useful result shows the WHOLE chain: more pool, fewer zero windows,"
echo "higher peer_bytes.  One number moving alone is not a result."
