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

# THE ONE COMBINATION THIS HARNESS CANNOT MEASURE.  Its whole subject is how
# much memory the machine has, and an A1200 with a PCMCIA card loses the card
# above 4 MB of Fast RAM -- which is why tools/emu-board.sh caps that board at
# 4.  Asked for both, the run boots, brings no interface up, and then spends
# three minutes with the peer retrying: a defect that reads as a slow harness.
# Refused with the reason instead.
case "$BOARD" in
    ne2000_pcmcia)
        if [ "${AMINETXDUO_FASTMEM:-4}" -gt 4 ] 2>/dev/null; then
            echo "poolshare: ne2000_pcmcia loses the card above 4 MB of Fast" >&2
            echo "  RAM, and AMINETXDUO_FASTMEM=$AMINETXDUO_FASTMEM asks for" >&2
            echo "  more.  Measure memory on a Zorro board -- -N a2065 is what" >&2
            echo "  every figure this harness has produced was taken on." >&2
            exit 2
        fi ;;
esac

echo "library_sha256=$(sha256sum "$BSD" | cut -d' ' -f1)"
# -b may point at a per-CPU build, so the codegen the number belongs to is
# recorded next to it; without this a peer_bytes= line is unattributable.
echo "cpu=$(sed -n 's/^AMINETXDUO_CPU:STRING=//p' \
    "$BUILDDIR/CMakeCache.txt" 2>/dev/null || echo unknown)" \
     "arch=$(sed -n 's/^AMIGA_ARCH_FLAGS:STRING=//p' \
    "$BUILDDIR/CMakeCache.txt" 2>/dev/null || echo unknown)"
echo "divisors=$DIVISORS readsizes=$READSIZES" \
     "fastmem=${AMINETXDUO_FASTMEM:-unset} z3mem=${AMINETXDUO_Z3MEM:-unset}"
echo "model=$MODEL board=$BOARD seconds=$SECONDS_ARM" \
     "exclusive=${AMINETXDUO_RIG_EXCLUSIVE-yes}" \
     "hostload=$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null | tr ' ' ',')"
echo "guest_address=$ADDRESS"

# Read once, before the loop exports its own: the base name of every arm.
RUNTAG="${AMINETXDUO_RUN_TAG:-poolshare$$}"

RESULTS="$ROOT/build/poolshare-results-$$"
mkdir -p "$RESULTS"

for DIV in $DIVISORS; do
  for RSZ in $READSIZES; do
    ARM="$DIV-$RSZ"
    # ONE NAME PER RUN, not one per arm.  The tag names the guest's drive, its
    # serial log and its emulator configuration, and two runs of this harness
    # with the same divisor and read size shared all three: the second one's
    # guest wrote the first one's transcript, and the arm that lost the race
    # exited fifteen seconds in with an empty stdout.  Both were then
    # unattributable.  A caller that has already named its run keeps that
    # name; anything else is separated by the pid.
    TAG="$RUNTAG-$ARM"
    STAGE="$ROOT/build/poolshare-stage-$TAG"
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
    # anxnet.device serves every card it covers out of one binary and is told
    # which by CARD=.  Without the line it opens nothing and the run reads as
    # "unit 0 did not answer" -- which is what the harness's OWN default board
    # did, so it could not bring an interface up at all.  A vendor driver has
    # no CARD= and gets none.  tools/sana2-stage.sh writes the same line for
    # every harness that goes through sana2_stage.
    [ -z "$SANA2_SEL_CARD" ] ||
        echo "CARD=$SANA2_SEL_CARD" >> "$STAGE/devs/NetInterfaces/eth0"

    {
        echo "echo >ENV:ANXDPOOLDIV $DIV"
        # The arithmetic this harness is about is printed at AMI_LOG_INFO and
        # nowhere else: "N bytes free / D, pool = P x 1568", and the TCP
        # window budget derived from P.  Without the dial the run reports a
        # pool size with no way to check the sum that produced it.  Both lines
        # are said once, at startup; nothing here is paid per packet.
        echo "echo >ENV:ANXDLOGLEVEL 2"
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
    ARM_T0=$(date +%s)
    ARM_LOAD0=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null)
    # Exclusive by default, because a throughput arm that shares the host NIC
    # and the host CPUs is measuring the other run as much as this one.  An
    # override and not a hardcode, so a busy lab can still take the figure it
    # can get: AMINETXDUO_RIG_EXCLUSIVE= (empty) runs alongside, and the
    # exclusive= line in the header says which of the two produced the number.
    AMINETXDUO_RIG_EXCLUSIVE="${AMINETXDUO_RIG_EXCLUSIVE-poolshare arm $ARM}" \
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t $((SECONDS_ARM + 180)) \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/iperf" \
        "$STAGE/netstat" > "$RESULTS/arm-$ARM.run" 2>&1
    RUN_RC=$?

    # An emulator that refused to start has no guest for the peer to reach,
    # and the peer would go on knocking for three minutes before saying so.
    # That is the second way this harness spent its time looking slow instead
    # of saying what was wrong; the first is the Fast RAM refusal above.
    [ "$RUN_RC" = 0 ] || kill "$PEER_PID" 2>/dev/null
    wait "$PEER_PID"
    kill "$TCPDUMP_PID" 2>/dev/null
    wait "$TCPDUMP_PID" 2>/dev/null

    HD="$ROOT/build/amiberry-testhd-$TAG"
    cp "$HD/tools.txt" "$RESULTS/arm-$ARM.guest" 2>/dev/null
    cp "$ROOT/build/amiberry-serial-$TAG.log" "$RESULTS/arm-$ARM.serial" \
       2>/dev/null

    echo "==== arm divisor=$DIV readsize=$RSZ (emulator rc=$RUN_RC) ===="
    # Per arm and not per run: on a host with other guests on it these move
    # between one arm and the next, and a zero-window count with no load
    # beside it cannot be told from one that is about the stack.
    echo "host: wall_s=$(( $(date +%s) - ARM_T0 ))" \
         "load_start=$ARM_LOAD0 load_end=$(cut -d' ' -f1 /proc/loadavg)"
    grep -E "peer_bytes=|never accepted" "$PEERLOG" | tail -1
    grep -E "packets free|fewest|pool empty|window" \
        "$RESULTS/arm-$ARM.guest" 2>/dev/null | head -4
    grep -aE "bytes free|window budget" "$RESULTS/arm-$ARM.serial" \
        2>/dev/null | sed 's/^/pool: /' | head -2

    # The three legs a frame pays on its way out and the one the socket pays
    # to reach them, and the one the application pays to collect what came in.
    # Only a probe build keeps any of them (-DAMINETXDUO_RXPROBE=ON); a
    # shipping build prints "not instrumented" and these lines are absent.
    # Quoted TOGETHER, because xmit alone says nothing: the question is how
    # the transmit half divides, and each leg is only large or small next to
    # the other three.
    sed -n 's/^[[:space:]]*\(fetch,\|xmit,\|reap,\|stuff,\|post,\)/leg \1/p' \
        "$RESULTS/arm-$ARM.guest" 2>/dev/null
    if [ -s "$PCAP" ]; then
        # BY DIRECTION.  Counting both ends together files the peer's own
        # advertisements under the guest, and the zero windows this harness
        # exists to find are the GUEST's.  peer_zero_windows is printed rather
        # than dropped: a peer that shuts its own window has stopped being a
        # sender, and every guest-side number in the arm is then about that.
        Z=$(tcpdump -n -r "$PCAP" "src host $ADDRESS" 2>/dev/null |
            grep -c "win 0,")
        ZP=$(tcpdump -n -r "$PCAP" "dst host $ADDRESS" 2>/dev/null |
             grep -c "win 0,")
        W=$(tcpdump -n -r "$PCAP" "src host $ADDRESS" 2>/dev/null |
            grep -oE "win [0-9]+" | sort -t' ' -k2 -n | tail -1)
        SEGS=$(tcpdump -n -r "$PCAP" "dst host $ADDRESS" 2>/dev/null | wc -l)
        echo "wire: zero_windows=$Z peer_zero_windows=$ZP" \
             "guest_max_advertised=$W inbound_segments=$SEGS"
    fi
  done
done

echo
echo "results kept in $RESULTS"
echo "A useful result shows the WHOLE chain: more pool, fewer zero windows,"
echo "higher peer_bytes.  One number moving alone is not a result."
