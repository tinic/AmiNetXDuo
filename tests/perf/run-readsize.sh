#!/usr/bin/env bash
#
# WHAT ONE recv() COSTS, AND WHETHER A BOOT DECIDES IT.
#
#   tests/perf/run-readsize.sh -b BUILDDIR -B IFACE -P PEERSSH
#                              [-l "4096 8192 16384 32768"] [-r reps]
#                              [-n boots] [-N board] [-m model] [-a addr]
#                              [-s seconds] [-p baseport]
#
# Two questions one rig answers, because they need the same run:
#
#  1. The application read size.  The guest reads IPERF_TCP_DEFAULT_LEN, 4096,
#     and every harness leaves it there.  Fetch -- notify to recv() returning --
#     is paid once per recv(), so a 32 KB read pays it an eighth as often.  -l
#     sweeps it.  The peer's WRITE size is held at 4096 throughout: the variable
#     under test is the receiver's, not the sender's segmentation.
#
#  2. Whether a figure belongs to the boot or to the transfer.  Every arm here
#     runs inside ONE boot, so a quantity that is bimodal across boots and
#     constant within one is a property of what the machine allocated at
#     bring-up, not of the transfer.  -r repeats each size, -n repeats the boot.
#
# Arms are INTERLEAVED: rep 1 runs every size in order, then rep 2, and so on.
# Within a boot the arms differ by ~0.6%; between boots by ~3%.
#
# Each arm gets its OWN PORT, so one capture separates cleanly per arm without
# any timestamp arithmetic.
#
# Guest figures (guest_bytes, guest_ms, guest_bits_per_sec) come off the guest's
# own clock and are insulated from host load.  Peer figures (peer_*) are HOST
# WALL CLOCK and are not.  Legs are guest E-Clock.
#
# THE PEER IS A THIRD MACHINE.  Not the emulator host -- a frame it sends to its
# own guest never returns to that NIC's pcap -- and not an LXC container on a
# veth, whose SYN-ACK carries an uncomputed TX-offload checksum.
#
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
SIZES="4096 8192 16384 32768"
REPS=2
BOOTS=1
SECONDS_ARM=10
BASEPORT=5301

while getopts "b:B:P:N:m:a:l:r:n:s:p:" opt; do
    case "$opt" in
        b) BUILDDIR="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEERSSH="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        l) SIZES="$OPTARG" ;;
        r) REPS="$OPTARG" ;;
        n) BOOTS="$OPTARG" ;;
        s) SECONDS_ARM="$OPTARG" ;;
        p) BASEPORT="$OPTARG" ;;
        *) sed -n '3,10p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$BUILDDIR" ] && [ -n "$IFACE" ] && [ -n "$PEERSSH" ] || {
    sed -n '3,10p' "$0" >&2; exit 2; }

BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"
TOOLS="$BUILDDIR/src/tools"
for f in "$BSD" "$TOOLS/AddNetInterface" "$TOOLS/iperf" "$TOOLS/netstat" \
         "$TOOLS/ToolsSmoke"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
done

# shellcheck source=netstatkv.sh
. "$ROOT/tests/perf/netstatkv.sh"

if [ -z "$ADDRESS" ]; then
    # shellcheck source=../../tools/emu-rig-lock.sh
    . "$ROOT/tools/emu-rig-lock.sh"
    rig_claim_address "${AMINETXDUO_RIG_ADDR_PREFIX:-192.168.1}" \
                      "${AMINETXDUO_RIG_ADDR_FIRST:-200}" \
                      "${AMINETXDUO_RIG_ADDR_LAST:-254}" \
                      "readsize in $ROOT" || {
        echo "no free guest address; pass -a <addr> to pin one" >&2; exit 2; }
    ADDRESS="$RIG_ADDRESS"
fi

# The arm plan, interleaved, one line per arm: index size port rep
PLAN="$ROOT/build/readsize-plan-$$"
: > "$PLAN"
IDX=0
R=1
while [ "$R" -le "$REPS" ]; do
    for SZ in $SIZES; do
        IDX=$((IDX + 1))
        echo "$IDX $SZ $((BASEPORT + IDX)) $R" >> "$PLAN"
    done
    R=$((R + 1))
done
ARMS=$IDX

# ToolsSmoke reads at most 96 lines and each arm costs two of them.
[ $((ARMS * 2 + 4)) -le 96 ] || {
    echo "plan_too_long arms=$ARMS -- ToolsSmoke reads 96 commands" >&2
    exit 2; }

echo "library_sha256=$(sha256sum "$BSD" | cut -d' ' -f1)"
echo "cpu=$(sed -n 's/^AMINETXDUO_CPU:STRING=//p' \
    "$BUILDDIR/CMakeCache.txt" 2>/dev/null || echo unknown)" \
     "rxprobe=$(sed -n 's/^AMINETXDUO_RXPROBE:BOOL=//p' \
    "$BUILDDIR/CMakeCache.txt" 2>/dev/null || echo unknown)"
echo "sizes=$SIZES reps=$REPS boots=$BOOTS arms_per_boot=$ARMS"
echo "seconds_per_arm=$SECONDS_ARM board=$BOARD model=$MODEL"
echo "fastmem=${AMINETXDUO_FASTMEM:-unset} z3mem=${AMINETXDUO_Z3MEM:-unset}"
echo "guest_address=$ADDRESS peer=$PEERSSH"

RESULTS="$ROOT/build/readsize-results-$$"
mkdir -p "$RESULTS"
cp "$PLAN" "$RESULTS/plan.txt"

# ------------------------------------------------------------------ the peer --
# One script, run once per boot: it walks the same plan the guest walks, in the
# same order, and retries each arm until the guest's listener for that port is
# up.  Ports are per-arm, so a late connect can never land in the wrong arm.
cat > "$RESULTS/peerdrive.sh" <<'PEEREOF'
#!/bin/sh
# $1 plan file  $2 guest address  $3 seconds  $4 peer script
PLAN="$1"; ADDR="$2"; SECS="$3"; PY="$4"
while read -r idx size port rep; do
    [ -n "$idx" ] || continue
    i=0
    while [ "$i" -lt 40 ]; do
        out=$(python3 "$PY" --port "$port" --seconds "$SECS" \
                  --length 4096 send tcp "$ADDR" 2>&1)
        case $out in
            *peer_bytes=*) echo "armpeer=$idx $out"; break ;;
        esac
        i=$((i + 1))
        sleep 2
    done
    [ "$i" -lt 40 ] || echo "armpeer=$idx peer_bytes=0 peer_failed=1"
done < "$PLAN"
PEEREOF

# --------------------------------------------------------------------- boots --
BOOT=1
while [ "$BOOT" -le "$BOOTS" ]; do
    TAG="readsize-b$BOOT-$$"
    STAGE="$ROOT/build/readsize-stage-$BOOT-$$"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$BSD" "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface iperf netstat; do cp "$TOOLS/$t" "$STAGE/$t"; done

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
    # anxnet.device covers a family of cards out of one binary and is told
    # which by CARD=.  Without the line it opens nothing, and the run reads as
    # "anxnet.device unit 0 did not answer" -- on THE DEFAULT BOARD here, whose
    # sana2_select() answer is anxnet.device with CARD=pcmcia, so this harness
    # could not bring an interface up as shipped.  tools/sana2-stage.sh writes
    # the same line for everything that goes through sana2_stage; a vendor
    # driver has no CARD= and gets none.
    [ -z "$SANA2_SEL_CARD" ] ||
        echo "CARD=$SANA2_SEL_CARD" >> "$STAGE/devs/NetInterfaces/eth0"

    {
        echo "SYS:AddNetInterface eth0"
        echo "SYS:netstat -s"                 # block 1: the baseline
        while read -r idx size port rep; do
            [ -n "$idx" ] || continue
            echo "SYS:iperf -s -p $port -t $((SECONDS_ARM + 25)) -l $size"
            echo "SYS:netstat -s"
        done < "$PLAN"
    } > "$STAGE/commands.txt"

    PCAP="$RESULTS/boot-$BOOT.pcap"
    BUDGET=$(( (SECONDS_ARM + 30) * ARMS + 200 ))
    timeout "$BUDGET" tcpdump -i "$IFACE" -s 128 -w "$PCAP" \
        "host $ADDRESS and tcp" >/dev/null 2>&1 &
    TCPDUMP_PID=$!

    PEERLOG="$RESULTS/boot-$BOOT.peer"
    scp -q "$ROOT/tests/tools/iperfpeer.py" "$PEERSSH:/tmp/iperfpeer.$$.py"
    scp -q "$PLAN"                          "$PEERSSH:/tmp/readsize.$$.plan"
    scp -q "$RESULTS/peerdrive.sh"          "$PEERSSH:/tmp/peerdrive.$$.sh"
    ssh -o BatchMode=yes "$PEERSSH" \
        "sh /tmp/peerdrive.$$.sh /tmp/readsize.$$.plan $ADDRESS \
             $SECONDS_ARM /tmp/iperfpeer.$$.py" > "$PEERLOG" 2>&1 &
    PEER_PID=$!

    # Host load at the start of the boot: peer_* is wall clock and is only
    # comparable across arms that ran under the same load.
    echo "boot=$BOOT host_load=$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null |
        tr ' ' ',' || echo unknown)"

    export AMINETXDUO_RUN_TAG="$TAG"
    # Exclusive by default, because peer_* is host wall clock and an arm that
    # shares the host CPUs is measuring the other run as much as this one.  An
    # override rather than a hardcode: a lab running eight guests refuses this
    # claim outright, and a figure taken alongside is worth more than none.
    AMINETXDUO_RIG_EXCLUSIVE="${AMINETXDUO_RIG_EXCLUSIVE-readsize boot $BOOT}" \
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$BUDGET" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/iperf" \
        "$STAGE/netstat" > "$RESULTS/boot-$BOOT.run" 2>&1
    RUN_RC=$?

    # An emulator that refused to start has no guest for the peer to reach,
    # and the peer would spend the whole plan knocking before saying so.
    [ "$RUN_RC" = 0 ] || kill "$PEER_PID" 2>/dev/null
    wait "$PEER_PID"
    kill "$TCPDUMP_PID" 2>/dev/null
    wait "$TCPDUMP_PID" 2>/dev/null

    GUEST="$RESULTS/boot-$BOOT.guest"
    cp "$ROOT/build/amiberry-testhd-$TAG/tools.txt" "$GUEST" 2>/dev/null

    echo "boot=$BOOT emulator_rc=$RUN_RC tag=$TAG"
    if [ ! -s "$GUEST" ]; then
        echo "boot=$BOOT guest_report=missing"
        BOOT=$((BOOT + 1))
        continue
    fi

    # The netstat blocks, in order.  Block 1 is the baseline; block n+1 closes
    # arm n.  Counters are cumulative, so an arm's own figures are a difference.
    awk '/^===== SYS:netstat -s =====/ { n++; f = 1; next }
         /^===== /                     { f = 0 }
         f { print > (out "/ns-" n ".txt") }' out="$RESULTS" \
        "$GUEST"
    netstat_kv "$RESULTS/ns-1.txt" > "$RESULTS/kv-boot$BOOT-0.txt" 2>/dev/null

    while read -r idx size port rep; do
        [ -n "$idx" ] || continue
        PREV="$RESULTS/kv-boot$BOOT-$((idx - 1)).txt"
        CUR="$RESULTS/kv-boot$BOOT-$idx.txt"
        netstat_kv "$RESULTS/ns-$((idx + 1)).txt" > "$CUR" 2>/dev/null

        GLINE=$(awk -v p="-p $port " '
            /^===== SYS:iperf / { f = (index($0, p) > 0); next }
            /^===== /           { f = 0 }
            f && /^dir=/        { print; exit }' "$GUEST")

        PLINE=$(sed -n "s/^armpeer=$idx //p" "$PEERLOG" | tail -1)

        if [ -s "$PCAP" ]; then
            Z=$(tcpdump -n -r "$PCAP" "src host $ADDRESS and tcp port $port" \
                    2>/dev/null | grep -c "win 0,")
            W=$(tcpdump -n -r "$PCAP" "src host $ADDRESS and tcp port $port" \
                    2>/dev/null | grep -oE "win [0-9]+" |
                awk '{ if ($2+0 > m) m = $2+0 } END { print m+0 }')
            S=$(tcpdump -n -r "$PCAP" "src host $ADDRESS and tcp port $port" \
                    2>/dev/null | grep -c .)
        else
            Z=; W=; S=
        fi

        # Deltas.  A leg's mean is printed as an integer, so its sum is
        # reconstructed as mean*count; over thousands of samples the rounding
        # is far below the difference an arm is looking for.
        DELTA=$(awk -F= '
            FNR == NR { a[$1] = $2; next }
            {
                b[$1] = $2
                if ($1 ~ /_samples$/) keys[$1] = 1
            }
            END {
                for (k in keys) {
                    leg = k; sub("_samples$", "", leg)
                    dn = b[k] - a[k]
                    if (dn <= 0) continue
                    ds = b[leg "_mean_us"] * b[k] - a[leg "_mean_us"] * a[k]
                    printf "d_%s_samples=%d d_%s_mean_us=%d ", \
                           leg, dn, leg, ds / dn
                }
                printf "d_rx_direct=%d d_rx_fallback=%d ", \
                       b["rx_direct"] - a["rx_direct"], \
                       b["rx_fallback"] - a["rx_fallback"]
                printf "d_pool_empty=%d pool_low=%s pool_total=%s ", \
                       b["pool_empty"] - a["pool_empty"], \
                       b["pool_low"], b["pool_total"]
                printf "mem_largest=%s probe=%s", b["mem_largest"], b["probe"]
            }' "$PREV" "$CUR")

        echo "arm=$idx boot=$BOOT rep=$rep read_bytes=$size port=$port" \
             "${GLINE:-dir=none bytes=0 ms=0 bits_per_sec=0}" \
             "${PLINE:-peer_bytes=0 peer_missing=1}" \
             "zero_windows=${Z:-na} max_advertised=${W:-na}" \
             "guest_frames=${S:-na}" "$DELTA"
    done < "$PLAN"

    BOOT=$((BOOT + 1))
done

rm -f "$PLAN"
echo "results_dir=$RESULTS"
