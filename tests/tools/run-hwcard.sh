#!/usr/bin/env bash
#
# THE CARD IN THE MACHINE ON THE DESK.
#
#   tests/tools/run-hwcard.sh [-A TARGET] [-p PORT] [-P PEERHOST|local]
#                             [-C CARD]
#                             [-a ARMS] [-b BUILDDIR] [-t SECONDS]
#                             [-D DRAWER] [-w SECONDS] [-r RESTART] [-Q]
#
# WHY THIS EXISTS
#
# `xsurf500` and `3c589` in src/netdev/netdev_cards.c are the two rows no
# emulator can run.  The 3c589 is the one this lab has -- a 3Com EtherLink III
# in the PCMCIA slot of the A1200 at `amiga-1200.local` -- and until this
# harness it was covered by somebody remembering to try it.  Everything below
# had been done by hand at least once and asserted by nothing.
#
# The machine is not an emulator, so tools/amiberry-run.sh cannot drive it.
# tools/hwrun.sh does: it pulls the staged binaries onto the machine over its
# own httpd and runs them through /shell.  Read that file first.
#
# THE ARMS, and the question each one answers
#
#   identity   is the interface running on the 3c589 at all?  A run against a
#              machine that fell back to another card proves nothing about
#              this one, and nothing used to notice.
#   payload    do the BYTES arrive, both directions?  Frame counts agree
#              across a placement bug; a position-dependent pattern hashed at
#              both ends does not.  This is the same proof
#              tests/tools/run-payverify.sh makes, on the card that harness
#              cannot reach.
#   overruns   how many frames does the receive path lose under load?  The
#              3c589 holds one packet at the head of a FIFO with 4 KB behind
#              it, and src/netdev/el3.c releases that head before the call up
#              into the stack rather than after.  The C mock says the
#              difference is 1 overrun to 0.  This is where the wire number
#              comes from.
#   shutdown   does the machine survive NetShutdown?  The report that opened
#              the row was a guru on this card.  A guru is not a message from
#              here: it is a machine that stops answering ICMP and never
#              comes back, and both halves are reported.
#
# ABSENCE IS NOT A FAULT.  The A1200 is usually off.  RESULT=skip and exit 3.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

TARGET="${AMINETXDUO_HW_TARGET:-amiga-1200.local}"
PORT="${AMINETXDUO_HW_PORT:-80}"
PEERHOST="${AMINETXDUO_HW_PEER:-}"
CARD=3c589
ARMS=identity,payload,overruns,shutdown
BUILD="${AMINETXDUO_BUILD:-build/cm}"
CMDTIME=600
DRAWER="${AMINETXDUO_HW_DRAWER:-RAM:hwrun}"
RETURNWAIT=240
RESTART="${AMINETXDUO_HW_RESTART:-}"
QUICK=no
IFACE="${AMINETXDUO_HW_IFACE:-eth0}"

usage() { sed -n '3,9p' "$0" >&2; }

while getopts "A:p:P:C:a:b:t:D:w:r:i:Qh" opt; do
    case "$opt" in
        A) TARGET="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        P) PEERHOST="$OPTARG" ;;
        C) CARD="$OPTARG" ;;
        a) ARMS="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        t) CMDTIME="$OPTARG" ;;
        D) DRAWER="$OPTARG" ;;
        w) RETURNWAIT="$OPTARG" ;;
        r) RESTART="$OPTARG" ;;
        i) IFACE="$OPTARG" ;;
        Q) QUICK=yes ;;
        h) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac
TOOLS="$BUILDDIR/src/tools"

has_arm() { case ",$ARMS," in *",$1,"*) return 0 ;; esac; return 1; }

for a in ${ARMS//,/ }; do
    case "$a" in
        identity|payload|overruns|shutdown) ;;
        *) echo "no such arm: $a (identity, payload, overruns, shutdown)" >&2
           exit 2 ;;
    esac
done

TAG="${AMINETXDUO_RUN_TAG:-hwcard}"
OUT="$ROOT/build/hwcard-$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "arms=$ARMS"
echo "card=$CARD"
echo "iface=$IFACE"

# THE UNIT IS THE CARD'S IDENTITY, and it is a published interface.
# src/netdev/netdev_cards.c: a unit pin is (index + 1) * 100 and rows are
# appended, never inserted, so the number below is derived from that file
# rather than written down twice.
CARD_INDEX=$(awk '
    /^const NetdevCard netdev_cards\[\] =/ { in_t = 1; next }
    in_t && /^};/                          { exit }
    in_t && /^    \{ "/ {
        name = $0
        sub(/^[^"]*"/, "", name)
        sub(/".*$/, "", name)
        print n " " name
        n++
    }' src/netdev/netdev_cards.c | awk -v want="$CARD" '$2 == want { print $1 }')

if [ -z "$CARD_INDEX" ]; then
    echo "RESULT=fail"
    echo "run-hwcard: src/netdev/netdev_cards.c has no row called '$CARD'" >&2
    exit 2
fi
WANT_UNIT=$(( (CARD_INDEX + 1) * 100 ))
echo "card_index=$CARD_INDEX"
echo "card_expected_unit=$WANT_UNIT"

# xsurf500 is in that table and is NOT in this lab.  Saying so here, once, is
# what keeps a run from being read as coverage of it.
if [ "$CARD" = xsurf500 ]; then
    echo "RESULT=skip"
    echo "run-hwcard: there is no X-Surf 500 and no ACA500 on this network." >&2
    echo "  The row in netdev_cards.c is transcribed from" >&2
    echo "  wiki.icomp.de/wiki/X-Surf-500_registers.  No harness can close" >&2
    echo "  it and no emulator models the board." >&2
    exit 3
fi

# ------------------------------------------------------------- staging --

STAGE="$OUT/stage"
mkdir -p "$STAGE"

need() {
    [ -f "$1" ] || { echo "RESULT=fail"
                     echo "missing $1, build the tree first" >&2; exit 2; }
    cp "$1" "$STAGE/"
}
need "$TOOLS/ToolsSmoke"
has_arm payload && need "$TOOLS/paysum"

CMDS="$OUT/commands.txt"
SPEC="$OUT/payspec.txt"
: > "$CMDS"
: > "$SPEC"

# The tools are in C: on an installed machine and at the root of the drive
# tools/amiberry-run.sh builds.  Bare names, so this reads the machine's own
# C: -- an installed machine is what this harness is for.
{
    echo "netstat -i"
    echo "ShowNetStatus"
    echo "netstat -s"
} >> "$CMDS"

# ------------------------------------------------------- the payload arm --

PEERADDR=""
PEER_PID=""
PORT_BASE=$((31000 + ($(printf '%s' "$TAG" | cksum | cut -d' ' -f1) % 300) * 10))
CONN_TOTAL=0

if has_arm payload || has_arm overruns; then
    [ -n "$PEERHOST" ] || {
        echo "RESULT=skip"
        echo "run-hwcard: the payload and overruns arms need a peer the" >&2
        echo "  machine can call.  Give one with -P, or -P local to run it" >&2
        echo "  on this host." >&2
        exit 2; }
    # THE PEER HAS TO EXCHANGE FRAMES WITH THE TARGET IN BOTH DIRECTIONS.
    # That is not every machine on the LAN: an Amiberry host bridged with
    # uaenet_pcap SEES its own guest's frames and cannot send to it, so a
    # peer there accepts nothing and every case reports paysum's own timeout
    # against a machine that is working.  `-P local` runs the peer on this
    # host, which is the right answer whenever the machine driving the run
    # is itself a machine the target can call.
    if [ "$PEERHOST" = local ]; then
        PEERADDR=$(ip -4 -o route get "$(getent ahostsv4 "${TARGET%.local}" \
                       2>/dev/null | awk 'NR==1{print $1}')" 2>/dev/null |
                   sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1)
        [ -n "$PEERADDR" ] || PEERADDR=$(hostname -I 2>/dev/null | awk '{print $1}')
        [ -n "$PEERADDR" ] || {
            echo "RESULT=fail"
            echo "cannot work out this host's own address for -P local" >&2
            exit 2; }
    else
        PEERNAME="${PEERHOST#*@}"
        PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null |
                   awk 'NR==1{print $1}')
        case "$PEERNAME" in
            *[!0-9.]*) [ -n "$PEERADDR" ] || {
                           echo "RESULT=fail"
                           echo "cannot resolve $PEERNAME" >&2; exit 2; } ;;
            *) PEERADDR="${PEERADDR:-$PEERNAME}" ;;
        esac
    fi
    echo "peer=$PEERHOST peer_addr=$PEERADDR"
fi

if has_arm payload; then
    RX_LENS="1 3 1459 1460 1461 65537 1048575"
    TX_LENS="1 1461 65537"
    [ "$QUICK" = no ] || { RX_LENS="1 1461 65537"; TX_LENS="1461"; }

    port=$PORT_BASE
    seed=$((100 + PORT_BASE % 1000))
    case_no=0
    emit() { # dir len conns
        echo "case=$case_no port=$port guest=$1 len=$2 seed=$seed conns=$3" \
            >> "$SPEC"
        local extra=""
        [ "$1" = tx ] && extra=" SEND"
        [ "$3" -gt 1 ] && extra="$extra CONNS $3"
        # paysum is a test tool.  It is in C: on a drive amiberry-run.sh
        # built and it is on no installed machine, so it is run out of the
        # drawer hwrun.sh staged it into.
        echo "$DRAWER/paysum $PEERADDR $port LEN $2 SEED $seed$extra TIMEOUT 240" \
            >> "$CMDS"
        port=$((port + $3)); seed=$((seed + $3)); case_no=$((case_no + 1))
    }
    for l in $RX_LENS; do emit rx "$l" 1; done
    for l in $TX_LENS; do emit tx "$l" 1; done
    emit rx 262144 3
    CONN_TOTAL=$((port - PORT_BASE))
    echo "payload_cases=$case_no payload_conns=$CONN_TOTAL"
    echo "payload_ports=$PORT_BASE..$((port - 1))"
fi

# ------------------------------------------------------ the overruns arm --

if has_arm overruns; then
    # One long receive is the load: the head of the FIFO has to be released
    # and the next frame taken while the stack is still carrying the last
    # one.  4 MB at 10 Mbit is about four seconds of continuous frames.
    OVR_PORT=$((PORT_BASE + 900))
    OVR_LEN=4194304
    [ "$QUICK" = no ] || OVR_LEN=1048576
    OVR_SEED=$((PORT_BASE + 7))
    echo "case=900 port=$OVR_PORT guest=rx len=$OVR_LEN seed=$OVR_SEED conns=1" \
        >> "$SPEC"
    {
        echo "netstat -s"
        echo "$DRAWER/paysum $PEERADDR $OVR_PORT LEN $OVR_LEN SEED $OVR_SEED TIMEOUT 600"
        echo "netstat -s"
        echo "netstat -i"
    } >> "$CMDS"
    CONN_TOTAL=$((CONN_TOTAL + 1))
    echo "overrun_load_bytes=$OVR_LEN"
fi

# ---------------------------------------------------------- the peer --

stop_peer() {
    [ -n "$PEER_PID" ] && kill "$PEER_PID" 2>/dev/null || true
    PEER_PID=""
}
trap stop_peer EXIT INT TERM HUP

if [ -s "$SPEC" ]; then
    # THE PEER MUST OUTLIVE THE GUEST'S WHOLE WINDOW, staging included.
    # CMDTIME is what ONE command on the machine is allowed; the peer has to
    # stand through staging, every case, and the report read after them.  At
    # CMDTIME + 60 it died while the machine was still working and the
    # remaining cases spent paysum's own TIMEOUT retrying a SYN into nothing
    # -- which reads as a slow machine, not as a peer that went home.
    PEER_LIFE=$((CMDTIME + 360))
    if [ "$PEERHOST" = local ]; then
        timeout $((PEER_LIFE + 60)) python3 "$ROOT/tests/tools/paypeer.py" \
            matrix --spec "$SPEC" --lifetime "$PEER_LIFE" \
            > "$OUT/peer.out" 2> "$OUT/peer.err" &
        PEER_PID=$!
    else
        REMOTE_PY="/tmp/paypeer-$TAG.py"
        REMOTE_SPEC="/tmp/payspec-$TAG.txt"
        scp -q "$ROOT/tests/tools/paypeer.py" "$PEERHOST:$REMOTE_PY"
        scp -q "$SPEC" "$PEERHOST:$REMOTE_SPEC"
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "pkill -f '[p]aypeer-$TAG' 2>/dev/null; exit 0" || true
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "timeout $((PEER_LIFE + 60)) python3 $REMOTE_PY matrix \
                 --spec $REMOTE_SPEC --lifetime $PEER_LIFE" \
            > "$OUT/peer.out" 2> "$OUT/peer.err" &
        PEER_PID=$!
    fi
    sleep 2
    kill -0 "$PEER_PID" 2>/dev/null || {
        echo "RESULT=fail"
        echo "the peer died before the run started:" >&2
        cat "$OUT/peer.err" >&2
        exit 1; }
fi

# ---------------------------------------------------------------- run --

echo "==> phase A on $TARGET: what the card is, and what it carries"
RC_A=0
"$ROOT/tools/hwrun.sh" -A "$TARGET" -p "$PORT" -t "$CMDTIME" -D "$DRAWER" \
    -o "$OUT/phaseA" "$CMDS" "$STAGE"/* > "$OUT/phaseA.txt" 2>&1 || RC_A=$?
sed 's/^/  /' "$OUT/phaseA.txt"

if [ "$RC_A" = 3 ]; then
    echo "RESULT=skip"
    exit 3
fi

REPORT="$OUT/phaseA/tools.txt"
[ -f "$REPORT" ] || { echo "RESULT=fail"
                      echo "run-hwcard: no report came back" >&2; exit 1; }

stop_peer

echo
echo "===================== what the machine printed ====================="
cat "$REPORT"
echo "========================== what the peer saw ======================="
cat "$OUT/peer.out" 2>/dev/null || true
echo "==================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# ------------------------------------------------------ identity --

if has_arm identity; then
    DEVLINE=$(grep -E "^Interface $IFACE \(" "$REPORT" | head -1 || true)
    if [ -z "$DEVLINE" ]; then
        echo "card_device=none"
        fail "ShowNetStatus lists no interface called $IFACE, so nothing" \
             "below is about $CARD"
    else
        DEV=$(printf '%s' "$DEVLINE" | sed -E 's/.*\((.*) unit ([0-9]+)\).*/\1/')
        UNIT=$(printf '%s' "$DEVLINE" | sed -E 's/.*unit ([0-9]+)\).*/\1/')
        echo "card_device=$DEV"
        echo "card_unit=$UNIT"
        if [ "$UNIT" = "$WANT_UNIT" ]; then
            echo "card_match=yes"
            pass "$IFACE is on $DEV unit $UNIT, which is $CARD's row"
        else
            echo "card_match=no"
            fail "$IFACE is on $DEV unit $UNIT and $CARD's unit is" \
                 "$WANT_UNIT -- this machine is running another card and" \
                 "every number below belongs to that one"
        fi
    fi

    LINK=$(grep -E "^  state " "$REPORT" | head -1 || true)
    case "$LINK" in
        *"link up"*) pass "the link is up" ;;
        "")          fail "no state line, the interface did not report" ;;
        *)           fail "the link is not up:$LINK" ;;
    esac
fi

# ------------------------------------------------------- payload --

if has_arm payload; then
    grep '^paysum dir='   "$REPORT"       > "$OUT/guest-lines.txt" || true
    grep '^paypeer case=' "$OUT/peer.out" > "$OUT/peer-lines.txt"  || true

    gv() { grep -o "$2=[^ ]*" <<< "$1" | head -1 | cut -d= -f2 |
           tr '[:upper:]' '[:lower:]'; }

    CHECKED=0
    MISMATCH=0
    while read -r spec; do
        s_case=$(gv "$spec" case)
        [ "$s_case" = 900 ] && continue      # the overruns arm's own case
        s_port=$(gv "$spec" port); s_dir=$(gv "$spec" guest)
        s_len=$(gv "$spec" len);   s_conns=$(gv "$spec" conns)
        for k in $(seq 0 $((s_conns - 1))); do
            p=$((s_port + k))
            g=$(grep " port=$p " "$OUT/guest-lines.txt" | head -1)
            r=$(grep " port=$p " "$OUT/peer-lines.txt"  | head -1)
            what="case $s_case $s_dir len $s_len port $p"
            if [ -z "$g" ]; then fail "$what: no line from the machine"
                                 MISMATCH=$((MISMATCH + 1)); continue; fi
            if [ -z "$r" ]; then fail "$what: no line from the peer"
                                 MISMATCH=$((MISMATCH + 1)); continue; fi
            g_b=$(gv "$g" bytes); r_b=$(gv "$r" bytes)
            g_c=$(gv "$g" crc32); r_c=$(gv "$r" crc32)
            g_x=$(gv "$g" first_bad); r_x=$(gv "$r" first_bad)
            if [ "$g_b" != "$s_len" ] || [ "$r_b" != "$s_len" ]; then
                fail "$what: bytes machine=$g_b peer=$r_b want=$s_len"
                MISMATCH=$((MISMATCH + 1))
            elif [ "$g_c" != "$r_c" ]; then
                fail "$what: CRC MISMATCH machine=$g_c peer=$r_c" \
                     "(first_bad machine=$g_x peer=$r_x) -- the bytes on" \
                     "the wire are not the bytes that were sent"
                MISMATCH=$((MISMATCH + 1))
            elif [ "$g_x" != "-1" ] || [ "$r_x" != "-1" ]; then
                fail "$what: pattern divergence with matching CRCs"
                MISMATCH=$((MISMATCH + 1))
            else
                pass "$what crc $g_c"
            fi
            CHECKED=$((CHECKED + 1))
        done
    done < "$SPEC"
    echo "payload_checked=$CHECKED"
    echo "payload_mismatch=$MISMATCH"
fi

# ------------------------------------------------------ overruns --

if has_arm overruns; then
    # netstat -s prints "bad data N overruns N".  Three of them were asked
    # for; the first is the floor and the last is after the load.
    mapfile -t OVR < <(grep -oE "overruns[[:space:]]+[0-9]+" "$REPORT" |
                       grep -oE "[0-9]+")
    # netstat -i: Name Mtu Address Link Ipkts Ierrs Opkts Oerrs.  Ipkts is
    # the FIFTH field; the sixth is Ierrs, and a loss rate computed against
    # the error count is not a loss rate.
    mapfile -t IPKT < <(grep -E "^$IFACE " "$REPORT" |
                        awk '{ print $5 }')
    if [ "${#OVR[@]}" -lt 2 ]; then
        fail "netstat -s printed fewer than two overrun figures, so nothing" \
             "measured what the load cost"
    else
        BEFORE="${OVR[$(( ${#OVR[@]} - 2 ))]}"
        AFTER="${OVR[$(( ${#OVR[@]} - 1 ))]}"
        echo "overruns_before=$BEFORE"
        echo "overruns_after=$AFTER"
        echo "overruns_delta=$((AFTER - BEFORE))"
        if [ "${#IPKT[@]}" -ge 2 ]; then
            RXA="${IPKT[$(( ${#IPKT[@]} - 1 ))]}"
            RXB="${IPKT[0]}"
            echo "rx_packets_first=$RXB"
            echo "rx_packets_last=$RXA"
            if [ "$RXA" -gt "$RXB" ]; then
                D=$((RXA - RXB))
                echo "rx_packets_delta=$D"
                echo "loss_ppm=$(( (AFTER - BEFORE) * 1000000 / D ))"
            fi
        fi
        pass "the load ran and the counters were read either side of it"
    fi

    UR=$(grep -oE "Transmit FIFO underruns[^0-9]*[0-9]+" "$REPORT" |
         grep -oE "[0-9]+$" | tail -1 || true)
    [ -z "$UR" ] || echo "tx_underruns=$UR"
fi

# ------------------------------------------------------ shutdown --

if has_arm shutdown; then
    [ -n "$RESTART" ] || {
        echo "shutdown_arm=unwired"
        fail "the shutdown arm needs the command that starts httpd on this
       machine (-r), because NetShutdown stops it and the report is read
       back through it.  Without one the machine would be left headless"
    }
fi

if has_arm shutdown && [ -n "$RESTART" ]; then
    CMDS_B="$OUT/commands-shutdown.txt"
    {
        echo "netstat -i"
        echo "NetShutdown"
        echo "AddNetInterface $IFACE"
        echo "wait 20"
        echo "netstat -i"
        echo "&$RESTART"
    } > "$CMDS_B"

    echo "==> phase B on $TARGET: NetShutdown, and whether it comes back"
    RC_B=0
    "$ROOT/tools/hwrun.sh" -A "$TARGET" -p "$PORT" -d -t 120 -w "$RETURNWAIT" \
        -D "$DRAWER" -o "$OUT/phaseB" "$CMDS_B" "$STAGE/ToolsSmoke" \
        > "$OUT/phaseB.txt" 2>&1 || RC_B=$?
    sed 's/^/  /' "$OUT/phaseB.txt"

    RET_ICMP=$(sed -n 's/^return_icmp=//p' "$OUT/phaseB.txt" | tail -1)
    RET_HTTP=$(sed -n 's/^return_http=//p' "$OUT/phaseB.txt" | tail -1)
    echo "shutdown_return_icmp=${RET_ICMP:-unknown}"
    echo "shutdown_return_http=${RET_HTTP:-unknown}"

    if [ "$RC_B" = 0 ]; then
        echo "shutdown_returned=yes"
        pass "the machine came back after NetShutdown and served its report"
        sed -n '/===== NetShutdown/,$p' "$OUT/phaseB/tools.txt" 2>/dev/null |
            sed 's/^/    /' || true
    elif [ "${RET_ICMP:-no}" = no ]; then
        echo "shutdown_returned=no"
        echo "shutdown_verdict=machine_stopped"
        fail "the machine answered ICMP before NetShutdown and answers
       none $RETURNWAIT s after it.  It did not come back.  THIS IS THE
       REPORT THE BACKLOG ROW IS ABOUT and it has now reproduced on
       $CARD: read the screen"
    else
        echo "shutdown_returned=no"
        echo "shutdown_verdict=network_did_not_return"
        fail "the machine is alive -- it answers ICMP -- and did not serve
       HTTP again within $RETURNWAIT s.  That is not a guru: the stack or
       httpd did not come back up"
    fi
fi

echo
if [ "$FAILED" != 0 ]; then
    echo "RESULT=fail"
    exit 1
fi
echo "RESULT=pass"
exit 0
