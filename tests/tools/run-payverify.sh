#!/usr/bin/env bash
#
# Payload integrity on a real guest: both ends hash CONTENT, and the run
# passes only when every hash pair agrees.
#
#   tests/tools/run-payverify.sh -B IFACE -P PEERHOST [-N BOARD] [-m MODEL]
#                                [-t SECONDS] [-b BUILDDIR] [-a ADDR]
#                                [-g GATEWAY] [-6 PEERV6] [-F v4|v6|both]
#                                [-S] [-Q]
#
# WHY THIS EXISTS
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=600
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-ne2000_pcmcia}"
IFACE=""
PEERHOST=""
PEERV6="${AMINETXDUO_PAY_PEER6:-}"
FAMILIES=both
STORM=no
LOSS=no
NETEM=""
QUICK=no

ADDRESS="${AMINETXDUO_PAY_ADDRESS:-192.168.1.240}"
GATEWAY="${AMINETXDUO_PAY_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0

while getopts "m:t:b:B:P:a:g:N:6:F:SLE:Q" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEERHOST="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        6) PEERV6="$OPTARG" ;;
        F) FAMILIES="$OPTARG" ;;
        S) STORM=yes ;;
        L) LOSS=yes ;;
        E) NETEM="$OPTARG" ;;
        Q) QUICK=yes ;;
        *) sed -n '3,12p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$IFACE" ] && [ -n "$PEERHOST" ] || {
    echo "-B <iface> and -P <peerhost> are both required: the content" >&2
    echo "proof wants the real frame path, and the receive direction" >&2
    echo "wants a peer a bridged guest can reach." >&2
    exit 2
}

case "$FAMILIES" in v4|v6|both) ;; *)
    echo "-F takes v4, v6 or both, not '$FAMILIES'" >&2; exit 2 ;;
esac

case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac

TOOLS="$BUILDDIR/src/tools"
BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-payverify}"

# A block of 100 ports per tag, clear of run-iperf's 10-port blocks at
# 20000..29000 (those take 2xxxx0..9), of tools/amiberry-run.sh's 12000
# block, and of the ephemeral range on the peers this runs on.
PORT_BASE=$((30000 + ($(printf '%s' "$AMINETXDUO_RUN_TAG" | cksum |
                        cut -d' ' -f1) % 350) * 100))
SEED_BASE=$((100 + PORT_BASE % 1000))

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
STAGE="$ROOT/build/payverify-stage-$AMINETXDUO_RUN_TAG"
PEERLOG="$ROOT/build/payverify-peer-$AMINETXDUO_RUN_TAG"
RUN_RC=0


PEERNAME="${PEERHOST#*@}"
PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
if [ -z "$PEERADDR" ]; then
    case "$PEERNAME" in
        *[!0-9.]*) echo "cannot resolve $PEERNAME for the guest to call" >&2
                   exit 2 ;;
        *) PEERADDR="$PEERNAME" ;;
    esac
fi

if [ "$FAMILIES" != v4 ] && [ -z "$PEERV6" ]; then
    PEERV6=$(ssh -o ConnectTimeout=10 "$PEERHOST" \
        "ip -6 addr show scope global 2>/dev/null" 2>/dev/null |
        awk '/inet6/ && !/temporary|deprecated/ {sub(/\/.*/, "", $2);
             print $2; exit}')
    if [ -z "$PEERV6" ]; then
        echo "==> $PEERHOST has no global IPv6 address; the v6 arms are OFF"
        [ "$FAMILIES" = v6 ] && exit 2
        FAMILIES=v4
    fi
fi

echo "==> peer $PEERHOST: v4 $PEERADDR${PEERV6:+, v6 $PEERV6}"


RX_LENS="1 2 3 4 5 1459 1460 1461 1462 1463 4095 4096 4097 65535 65536 65537 1048573 1048574 1048575 1048576"
TX_LENS="1 3 1460 1461 65537 1048575"
CONC_RX_LEN=262144
CONC_TX_LEN=131072

if [ "$QUICK" = yes ]; then
    RX_LENS="1 3 331 1460 1461 65537"
    TX_LENS="3 1461"
    CONC_RX_LEN=65536
    CONC_TX_LEN=65536
fi

if [ "$LOSS" = yes ] || [ -n "$NETEM" ]; then
    RX_LENS="1461 65534 65535 65536 65537 262144"
    TX_LENS="65537"
    CONC_RX_LEN=65536
    CONC_TX_LEN=65536
fi

SPEC="$STAGE/payspec.txt"
CMDS="$STAGE/commands.txt"

rm -rf "$STAGE"
mkdir -p "$STAGE/libs"

port=$PORT_BASE
seed=$SEED_BASE
case_no=0

emit_case() {
    local dir="$1" len="$2" conns="$3" fam="$4" host flag=""
    if [ "$fam" = v6 ]; then host="$PEERV6"; flag=" -6"; else host="$PEERADDR"; fi
    echo "case=$case_no port=$port guest=$dir len=$len seed=$seed conns=$conns" \
        >> "$SPEC"
    local extra=""
    [ "$dir" = tx ] && extra=" SEND"
    [ "$conns" -gt 1 ] && extra="$extra CONNS $conns"
    echo "SYS:paysum$flag $host $port LEN $len SEED $seed$extra TIMEOUT 240" \
        >> "$CMDS"
    port=$((port + conns))
    seed=$((seed + conns))
    case_no=$((case_no + 1))
}

: > "$SPEC"
{
    echo "SYS:AddNetInterface eth0"
} > "$CMDS"

FAMS=""
[ "$FAMILIES" != v6 ] && FAMS="v4"
[ "$FAMILIES" != v4 ] && FAMS="$FAMS v6"

for fam in $FAMS; do
    for l in $RX_LENS; do emit_case rx "$l" 1 "$fam"; done
    for l in $TX_LENS; do emit_case tx "$l" 1 "$fam"; done
    emit_case rx "$CONC_RX_LEN" 3 "$fam"
    emit_case tx "$CONC_TX_LEN" 3 "$fam"
    echo "SYS:netstat -s" >> "$CMDS"
done

CONN_TOTAL=$((port - PORT_BASE))
echo "==> $case_no cases, $CONN_TOTAL connections, ports $PORT_BASE..$((port - 1)), seeds from $SEED_BASE"


for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/paysum" \
         "$TOOLS/netstat" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ]; then
    sana2_select "$BOARD" "$BUILDDIR"
    if [ -z "$SANA2_SEL_PATH" ]; then
        echo "-N $BOARD wants $SANA2_SEL_DRIVER and this host has not got" \
             "it. Build the tree, or set AMINETXDUO_SANA2_DRIVER=<path>." >&2
        exit 2
    fi
    export AMINETXDUO_SANA2_DRIVER="$SANA2_SEL_PATH"
    export AMINETXDUO_SANA2_DRIVER_NAME="${AMINETXDUO_SANA2_DRIVER_NAME:-$SANA2_SEL_DRIVER}"
    export AMINETXDUO_SANA2_DEVICE="${AMINETXDUO_SANA2_DEVICE:-$SANA2_SEL_DRIVER}"
    [ -z "$SANA2_SEL_CARD" ] ||
        export AMINETXDUO_SANA2_CARD="${AMINETXDUO_SANA2_CARD:-$SANA2_SEL_CARD}"
fi
sana2_stage "$BOARD" "$STAGE/devs"
echo "driver_board=$BOARD driver_device=$SANA2_DEVICE driver_card=${SANA2_CARD:-none}"

cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/paysum"          "$STAGE/paysum"
cp "$TOOLS/netstat"         "$STAGE/netstat"


rm -rf "$PEERLOG"
mkdir -p "$PEERLOG"

PEER_PY="$ROOT/tests/tools/paypeer.py"
REMOTE_PY="/tmp/paypeer-$AMINETXDUO_RUN_TAG.py"
REMOTE_SPEC="/tmp/payspec-$AMINETXDUO_RUN_TAG.txt"
scp -q "$PEER_PY" "$PEERHOST:$REMOTE_PY"
scp -q "$SPEC"    "$PEERHOST:$REMOTE_SPEC"

ssh -o ConnectTimeout=10 "$PEERHOST" \
    "pkill -f '[p]aypeer-$AMINETXDUO_RUN_TAG' 2>/dev/null; exit 0" || true

PEER_LIFE=$((TIMEOUT + 60))
ssh -o ConnectTimeout=10 "$PEERHOST" \
    "timeout $((PEER_LIFE + 60)) python3 $REMOTE_PY matrix \
        --spec $REMOTE_SPEC --lifetime $PEER_LIFE" \
    > "$PEERLOG/peer.out" 2> "$PEERLOG/peer.err" &
PEER_PID=$!

NETEM_ON=no
NETEM_IFACE=""
GUEST6=""

netem_apply() {
    NETEM_IFACE=$(ssh -o ConnectTimeout=10 "$PEERHOST" \
        "ip -o route get $ADDRESS 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p'")
    [ -n "$NETEM_IFACE" ] || {
        echo "cannot work out which interface $PEERHOST reaches $ADDRESS on" >&2
        exit 2; }

    GUEST6=$(ssh -o ConnectTimeout=10 "$PEERHOST" \
        "ip -6 neigh show nud all 2>/dev/null" |
        awk '/^2/ && !/router/ {print $1; exit}')

    if [ "$FAMILIES" != v4 ] && [ -z "$GUEST6" ]; then
        echo "no guest IPv6 address on $PEERHOST's neighbour table: the v6" >&2
        echo "arm would run unshaped.  Run a v6 arm without -E first, or" >&2
        echo "pass -F v4." >&2
        exit 2
    fi

    ssh -o ConnectTimeout=10 "$PEERHOST" "
        ~/tc-cap qdisc del dev $NETEM_IFACE root 2>/dev/null
        ~/tc-cap qdisc add dev $NETEM_IFACE root handle 1: prio &&
        ~/tc-cap qdisc add dev $NETEM_IFACE parent 1:3 handle 30: netem $NETEM &&
        ~/tc-cap filter add dev $NETEM_IFACE protocol ip parent 1: prio 1 u32 \
            match ip dst $ADDRESS/32 flowid 1:3" || {
        echo "cannot put netem on $PEERHOST:$NETEM_IFACE" >&2; exit 2; }

    if [ -n "$GUEST6" ]; then
        ssh -o ConnectTimeout=10 "$PEERHOST" "
            ~/tc-cap filter add dev $NETEM_IFACE protocol ipv6 parent 1: \
                prio 2 u32 match ip6 dst $GUEST6/128 flowid 1:3" || {
            echo "cannot add the v6 netem filter" >&2; NETEM_ON=yes; exit 2; }
    fi

    NETEM_ON=yes
    echo "==> netem on $PEERHOST:$NETEM_IFACE toward the guest only: $NETEM"
    echo "    (v4 $ADDRESS${GUEST6:+, v6 $GUEST6})"
}

netem_remove() {
    [ "$NETEM_ON" = yes ] || return 0
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "~/tc-cap qdisc del dev $NETEM_IFACE root 2>/dev/null; exit 0" \
        >/dev/null 2>&1 || true
    NETEM_ON=no
}

STORM_PID=""
stop_peers() {
    netem_remove
    [ -n "$PEER_PID" ]  && kill "$PEER_PID"  2>/dev/null || true
    [ -n "$STORM_PID" ] && kill "$STORM_PID" 2>/dev/null || true
    [ -n "${LOSS_PID:-}" ] && kill "$LOSS_PID" 2>/dev/null || true
    [ -n "${SS_PID:-}" ]   && kill "$SS_PID"   2>/dev/null || true
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "pkill -f '[m]dnsstorm' 2>/dev/null; exit 0" >/dev/null 2>&1 || true
}
trap stop_peers EXIT INT TERM HUP

if [ "$STORM" = yes ]; then
    if ssh -o ConnectTimeout=10 "$PEERHOST" "test -f /tmp/mdnsstorm.py"; then
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "timeout $PEER_LIFE python3 /tmp/mdnsstorm.py $ADDRESS" \
            > "$PEERLOG/storm.out" 2> "$PEERLOG/storm.err" &
        STORM_PID=$!
        echo "==> mDNS storm running from $PEERHOST at $ADDRESS for the whole run"
    else
        echo "==> -S asked for a storm but $PEERHOST has no /tmp/mdnsstorm.py; STORM ARM OFF"
        STORM=missing
    fi
fi

LOSS_PID=""
[ -z "$NETEM" ] || netem_apply

if [ -n "$NETEM" ] && [ "$LOSS" != yes ]; then
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "timeout $PEER_LIFE sh -c 'while :; do ss -tino 2>/dev/null | grep -A1 \"$ADDRESS\\|${GUEST6:-no-v6-address}\" | grep -o \"retrans:[0-9]*/[0-9]*\"; sleep 2; done'" \
        > "$PEERLOG/ss.log" 2>/dev/null &
    SS_PID=$!
fi

if [ "$LOSS" = yes ]; then
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "timeout $PEER_LIFE python3 -c '
import socket, time, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
pkt = bytes(1400)
end = time.time() + $PEER_LIFE
n = 0
while time.time() < end:
    for _ in range(40):
        s.sendto(pkt, (\"$ADDRESS\", 9))
        n += 1
    time.sleep(0.4)
print(\"blast sent\", n, file=sys.stderr)
'" > "$PEERLOG/blast.out" 2> "$PEERLOG/blast.err" &
    LOSS_PID=$!
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "timeout $PEER_LIFE sh -c 'while :; do ss -tino 2>/dev/null | grep -A1 \"$ADDRESS\" | grep -o \"retrans:[0-9]*/[0-9]*\"; sleep 2; done'" \
        > "$PEERLOG/ss.log" 2>/dev/null &
    SS_PID=$!
    echo "==> loss arm: UDP burst blast running from $PEERHOST at $ADDRESS"
fi

sleep 1
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "the peer died before the run started:" >&2
    cat "$PEERLOG/peer.err" >&2
    exit 2
}


set +e
echo "==> booting $MODEL under Amiberry, $BOARD bridged on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$CMDS" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/paysum" "$STAGE/netstat"
RUN_RC=$?
set -e

wait "$PEER_PID" 2>/dev/null || true
PEER_PID=""
stop_peers

if [ ! -f "$REPORT" ]; then
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    exit 1
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "========================== what the peer saw ========================"
cat "$PEERLOG/peer.out" 2>/dev/null
sed 's/^/       /' "$PEERLOG/peer.err" 2>/dev/null || true
echo "====================================================================="
echo


FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

GUEST_LINES="$STAGE/guest-lines.txt"
PEER_LINES="$STAGE/peer-lines.txt"
grep '^paysum dir='   "$REPORT"           > "$GUEST_LINES" || true
grep '^paypeer case=' "$PEERLOG/peer.out" > "$PEER_LINES"  || true

gv() { grep -o "$2=[^ ]*" <<< "$1" | head -1 | cut -d= -f2 |
       tr '[:upper:]' '[:lower:]'; }

CHECKED=0
while read -r spec_line; do
    s_port=$(gv "$spec_line" port)
    s_dir=$(gv "$spec_line" guest)
    s_len=$(gv "$spec_line" len)
    s_conns=$(gv "$spec_line" conns)
    s_case=$(gv "$spec_line" case)
    for k in $(seq 0 $((s_conns - 1))); do
        p=$((s_port + k))
        g=$(grep " port=$p " "$GUEST_LINES" | head -1)
        r=$(grep " port=$p " "$PEER_LINES"  | head -1)
        what="case $s_case $s_dir len $s_len port $p"
        if [ -z "$g" ]; then fail "$what: no guest line"; continue; fi
        if [ -z "$r" ]; then fail "$what: no peer line";  continue; fi
        g_bytes=$(gv "$g" bytes); r_bytes=$(gv "$r" bytes)
        g_crc=$(gv "$g" crc32);   r_crc=$(gv "$r" crc32)
        g_bad=$(gv "$g" first_bad); r_bad=$(gv "$r" first_bad)
        if [ "$g_bytes" != "$s_len" ] || [ "$r_bytes" != "$s_len" ]; then
            fail "$what: bytes guest=$g_bytes peer=$r_bytes want=$s_len"
        elif [ "$g_crc" != "$r_crc" ]; then
            fail "$what: CRC MISMATCH guest=$g_crc peer=$r_crc" \
                 "(guest first_bad=$g_bad peer first_bad=$r_bad," \
                 "seed=$(gv "$g" seed)) -- content diverged, this is the" \
                 "injection the tier exists to catch"
        elif [ "$g_bad" != "-1" ] || [ "$r_bad" != "-1" ]; then
            fail "$what: pattern divergence guest=$g_bad peer=$r_bad with" \
                 "matching CRCs (both ends corrupted alike)"
        else
            pass "$what crc $g_crc"
        fi
        CHECKED=$((CHECKED + 1))
    done
done < "$SPEC"

[ "$CHECKED" = "$CONN_TOTAL" ] ||
    fail "checked $CHECKED of $CONN_TOTAL connections"

DIRECT=$(grep -o 'direct fills[[:space:]]*[0-9]*' "$REPORT" |
         awk '{n=$3} END {print n+0}')
if [ "$BOARD" = a2065 ]; then
    if [ "${DIRECT:-0}" = 0 ]; then
        pass "classic lane: 0 direct fills, the control is a control"
    else
        fail "a2065 shows $DIRECT direct fills; the control arm is claiming"
    fi
else
    if [ "${DIRECT:-0}" -gt 0 ]; then
        pass "claim lane carried the frames: $DIRECT direct fills"
    else
        fail "no direct fills on $BOARD: the claim lane never engaged and" \
             "this run proved the wrong path"
    fi
fi

if [ "$LOSS" = yes ] || [ -n "$NETEM" ]; then
    OVR=$(grep -io 'overrun[s]*[[:space:]]*[0-9]*' "$REPORT" |
          grep -o '[0-9]*' | sort -n | tail -1)
    RETR=$(grep -o 'retrans:[0-9]*/[0-9]*' "$PEERLOG/ss.log" 2>/dev/null |
           cut -d/ -f2 | sort -n | tail -1)
    if [ "${OVR:-0}" -gt 0 ] || [ "${RETR:-0}" -gt 0 ]; then
        pass "loss arm: witnessed (guest overruns ${OVR:-0}, peer-side" \
             "retransmissions up to ${RETR:-0} on one socket), so the" \
             "hashes above held across real drops and re-delivery"
    else
        echo "NOTE: loss arm ran but neither witness fired (overruns 0, no"
        echo "      retransmissions sampled).  The hashes still held, but do"
        echo "      not credit this run with loss coverage."
    fi
fi

if [ -n "$NETEM" ]; then
    netem_remove
    LEFT=$(ssh -o ConnectTimeout=10 "$PEERHOST" \
        "~/tc-cap qdisc show dev $NETEM_IFACE 2>/dev/null | grep -c netem; exit 0")
    LEFT=${LEFT:-unknown}
    if [ "$LEFT" = 0 ]; then
        pass "netem removed from $PEERHOST:$NETEM_IFACE, verified clean"
    else
        fail "netem may still be on $PEERHOST:$NETEM_IFACE ($LEFT netem" \
             "qdiscs still there) -- remove it before trusting any later" \
             "measurement from that machine"
    fi
fi

echo
if [ "$FAILED" = 0 ]; then
    echo "PASS: $CHECKED/$CONN_TOTAL connections content-verified on $BOARD" \
         "($FAMILIES, storm=$STORM, loss=$LOSS${NETEM:+, netem: $NETEM})"
    exit 0
fi
echo "FAIL: content verification did not hold; the lines above name the cases"
exit 1
