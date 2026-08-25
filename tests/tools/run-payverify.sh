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
#   Every transfer gate before this one compared byte counts.  The receive
#   path computes its checksum in the same pass that places the bytes
#   (single-copy claim, fused drain-and-sum), so the one failure a byte count
#   cannot see -- bytes placed at the wrong offset, a word duplicated or
#   skipped, a tail mishandled, and the whole thing then CERTIFIED by a sum
#   taken over the wrongly placed bytes -- passes TCP, passes iperf, and
#   corrupts the payload without a symptom.  Content is the only witness.
#
#   So: src/tools/paysum.c on the guest and tests/tools/paypeer.py on the
#   peer each derive the same position-dependent pattern from (seed, offset)
#   and each hash what actually crossed on their own side.  This script
#   writes both ends' case tables from one matrix, runs them, and joins the
#   two reports port by port.  A placement bug changes the bytes, the bytes
#   change a CRC on exactly one side, and the join fails with the case named.
#
# THE MATRIX
#
#   Both directions.  Sizes sweep every tail residue (len mod 4 = 0..3, the
#   class the three-byte-tail bug lived in) at several magnitudes, the MSS
#   edges (1460 +/- and the doubles), and 1 byte to a megabyte.  One
#   concurrency case per direction runs three simultaneous sockets carrying
#   three different seeds through one select() loop, so a chunk leaked
#   across connections lands in a stream whose pattern disagrees with it
#   everywhere.  -F both appends the same matrix again over IPv6, which is
#   an independent CONTROL, not just coverage: the v4 receive verifier
#   consumes the fused sum, the v6 path never does (the stack walks the
#   delivered buffer itself), so v4-bad-with-v6-clean is the signature of
#   self-certified placement corruption, and a v6 drop-count climbing during
#   clean v4 hashes is the same bug seen from the other side.
#
#   -S runs the whole matrix under an mDNS storm from the peer host
#   (/tmp/mdnsstorm.py, unicast -- multicast never reaches a pcap-bridged
#   guest), which is the arm where the responder's bursts interleave with
#   the claim lane's drains.
#
# WHAT IT NEEDS
#
#   Bridged, always: -B and -P are required.  A SLIRP transfer terminates
#   TCP inside the emulator's NAT and the frames the guest drains are not
#   the frames the peer sent, which makes its content proof worth less; and
#   the guest-as-receiver direction wants a peer that can be reached from a
#   THIRD machine anyway (docs/RESEARCH.md 63).  The claim lane itself only
#   exists on ne2000_pcmcia (DP8390) among the emulated boards; a2065 runs
#   the classic CopyToBuff lane and is the cross-lane control: same matrix,
#   same hashes expected, different placement code.
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
QUICK=no

ADDRESS="${AMINETXDUO_PAY_ADDRESS:-192.168.1.240}"
GATEWAY="${AMINETXDUO_PAY_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0

while getopts "m:t:b:B:P:a:g:N:6:F:SLQ" opt; do
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

# ---------------------------------------------------------------- the peer ---

PEERNAME="${PEERHOST#*@}"
PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
if [ -z "$PEERADDR" ]; then
    case "$PEERNAME" in
        *[!0-9.]*) echo "cannot resolve $PEERNAME for the guest to call" >&2
                   exit 2 ;;
        *) PEERADDR="$PEERNAME" ;;
    esac
fi

# The guest calls the peer's global v6 address for the v6 arms.  Asked for,
# or read off the peer itself; without one the v6 arms are skipped and the
# verdict says so rather than quietly passing half the matrix.
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

# --------------------------------------------------------------- the matrix ---
#
# One table writes both ends.  Each row: direction (the GUEST's), length,
# connections.  Ports and seeds are handed out sequentially, so every
# CONNECTION in the run has a unique port and a unique seed, and the join at
# the end is port -> (guest line, peer line) with nothing shared to agree by
# accident.
#
# The receive rows are the campaign: every tail residue at four magnitudes,
# the MSS edges, one byte to a megabyte.  The transmit rows cover the same
# residues once; the sending side has no fused sum, but a transmit placement
# bug corrupts the wire the same way.

RX_LENS="1 2 3 4 5 1459 1460 1461 1462 1463 4095 4096 4097 65535 65536 65537 1048573 1048574 1048575 1048576"
TX_LENS="1 3 1460 1461 65537 1048575"
CONC_RX_LEN=262144
CONC_TX_LEN=131072

# -Q is the smoke form: residues and edges once, no megabyte class.
if [ "$QUICK" = yes ]; then
    RX_LENS="1 3 331 1460 1461 65537"
    TX_LENS="3 1461"
    CONC_RX_LEN=65536
    CONC_TX_LEN=65536
fi

# -L narrows the matrix instead of running the full sweep under fire: with
# the ring being overrun on purpose every transfer crawls (the first attempt
# ran the full matrix and timed out 14 cases in), and the megabyte class
# proves nothing about retransmission that 256 KB does not.  What matters
# here is bytes ARRIVING TWICE: enough length for many drops, every tail
# residue once, both directions.
if [ "$LOSS" = yes ]; then
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

# emit_case dir len conns family
emit_case() {
    local dir="$1" len="$2" conns="$3" fam="$4" host flag=""
    if [ "$fam" = v6 ]; then host="$PEERV6"; flag=" -6"; else host="$PEERADDR"; fi
    echo "case=$case_no port=$port guest=$dir len=$len seed=$seed conns=$conns" \
        >> "$SPEC"
    local extra=""
    [ "$dir" = tx ] && extra=" SEND"
    [ "$conns" -gt 1 ] && extra="$extra CONNS $conns"
    # The idle bound is generous on purpose: a shared emulator host can
    # stall a transfer into deep RTO backoff for minutes without a byte
    # being wrong, and a bail that fires inside one recovery reads as a
    # truncated case.  Content decides this tier; time only bounds it.
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
    # The claim counters, read after each family's cases so the numbers
    # bracket what just ran.
    echo "SYS:netstat -s" >> "$CMDS"
done

CONN_TOTAL=$((port - PORT_BASE))
echo "==> $case_no cases, $CONN_TOTAL connections, ports $PORT_BASE..$((port - 1)), seeds from $SEED_BASE"

# ----------------------------------------------------------------- staging ---

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

# ------------------------------------------------------------- peer + storm ---

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

STORM_PID=""
stop_peers() {
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

# -L: the loss arm, without the tc netem this rig cannot have (~/tc-cap on
# the peer is staged but not setcap'd).  Bursty UDP at the guest overruns
# the emulated NIC's receive ring mid-drain, which is REAL loss in the exact
# layer under test: frames vanish between the wire and the claim, TCP
# retransmits, and the retransmission overlap path -- partial re-delivery
# into a stream whose earlier bytes are already placed -- runs for real.
# The hashes must still match; the overrun counter in the guest's
# netstat -s is the witness that loss actually happened.
LOSS_PID=""
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
    # The retransmission witness.  The guest's overrun counter only sees the
    # NIC ring; under Amiberry the drop happens in the host's bridge queue
    # and the ring never fills, so the loss is real and the guest counter
    # stays 0.  The peer's kernel knows: ss -ti keeps a per-socket lifetime
    # retransmission count, sampled here every two seconds for every socket
    # talking to the guest.
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

# --------------------------------------------------------------------- run ---

set +e
echo "==> booting $MODEL under Amiberry, $BOARD bridged on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$CMDS" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/paysum" "$STAGE/netstat"
RUN_RC=$?
set -e

# The peer's report is written as connections finish; give the last one a
# moment, then take the log as it stands.
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

# ----------------------------------------------------------------- verdict ---
#
# The join: every connection appears once in the guest's transcript and once
# in the peer's log, keyed by port.  bytes must agree, crc32 must agree, and
# neither side may have found a first_bad.  Counted, so a line that never
# appeared fails rather than not being checked.

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

GUEST_LINES="$STAGE/guest-lines.txt"
PEER_LINES="$STAGE/peer-lines.txt"
grep '^paysum dir='   "$REPORT"           > "$GUEST_LINES" || true
grep '^paypeer case=' "$PEERLOG/peer.out" > "$PEER_LINES"  || true

# Case-folded: the guest's RawDoFmt prints hex in upper case, python in
# lower, and 2FF815EF and 2ff815ef are the same number.
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

# The claimed lane must have CARRIED the frames it is being credited with.
# DP8390 boards claim; the a2065 classic lane must show zero claims, which
# is what makes it the control.
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

# The loss arm's witness: the run only means what it claims if frames were
# actually dropped under it.  Overruns live in the guest's own interface
# stats; said out loud either way.
if [ "$LOSS" = yes ]; then
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

echo
if [ "$FAILED" = 0 ]; then
    echo "PASS: $CHECKED/$CONN_TOTAL connections content-verified on $BOARD" \
         "($FAMILIES, storm=$STORM, loss=$LOSS)"
    exit 0
fi
echo "FAIL: content verification did not hold; the lines above name the cases"
exit 1
