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
#   probeorder does an NE2000 clone that answers only 16-bit cycles come up on
#              a WARM probe?  NOT IN THE DEFAULT SET and not about the 3c589:
#              it is the CNet16 question.  The NE2000 reset port is register
#              31, which is odd, and ne2000_probe_reset() strobes it by
#              READING it -- a cycle such a card does not answer -- so the
#              chip is not reset and the command register readback that
#              follows refuses the card before the word-read probe written
#              for it is reached.  A COLD BOOT HIDES IT: the chip powers up
#              with CR at $21 anyway.  This arm therefore takes the probe
#              record twice, once as booted and once after NetShutdown and
#              AddNetInterface, and the second one is the warm case.  It
#              needs -r for the same reason the shutdown arm does.
#
#                  tests/tools/run-hwcard.sh -C pcmcia -a probeorder \
#                      -A <machine> -r <the command that starts httpd>
#
#              src/netdev/test/test_netdev_ne2000.c proves the driver's half
#              of this on a host.  This is the half only the card can answer.
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
        identity|payload|overruns|shutdown|probeorder) ;;
        *) echo "no such arm: $a (identity, payload, overruns, shutdown," \
                "probeorder)" >&2
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
# include/aminetxduo/anxnet.h:16, read out of the header rather than written
# down a second time here.
UNIT_PIN=$(sed -n "s/^#define ANXNET_UNIT_PIN  *\([0-9][0-9]*\).*/\1/p" \
           include/aminetxduo/anxnet.h | head -1)
UNIT_PIN="${UNIT_PIN:-100}"
WANT_UNIT=$(( (CARD_INDEX + 1) * UNIT_PIN ))
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

# ------------------------------------------------- is there a machine --
#
# ASKED BEFORE ANYTHING IS SET UP.  The A1200 is off more often than it is
# on, and a skip that has already started a peer on another host, claimed a
# block of ports and copied a spec around is a skip that cost something.
# hwrun.sh -n is the same resolver and the same two probes, so the answer
# here cannot differ from the answer phase A would get.
PROBE_RC=0
"$ROOT/tools/hwrun.sh" -A "$TARGET" -p "$PORT" -n > "$OUT/probe.txt" 2>&1 ||
    PROBE_RC=$?
grep -E '^(target_|resolved_by|hw_)' "$OUT/probe.txt" || true
if [ "$PROBE_RC" = 3 ]; then
    echo "RESULT=skip"
    grep -v -E '^(target_|resolved_by|hw_|RESULT)' "$OUT/probe.txt" >&2 || true
    exit 3
fi
[ "$PROBE_RC" = 0 ] || {
    echo "RESULT=fail"
    cat "$OUT/probe.txt" >&2
    exit 1; }

# ------------------------------------------------------------- staging --

STAGE="$OUT/stage"
mkdir -p "$STAGE"

need() {
    [ -f "$1" ] || { echo "RESULT=fail"
                     echo "missing $1, build the tree first" >&2; exit 2; }
    cp "$1" "$STAGE/"
}
need "$TOOLS/ToolsSmoke"
has_arm payload  && need "$TOOLS/paysum"
has_arm identity && need "$TOOLS/CheckNetDevice"

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

# THE UNIT NUMBER DOES NOT NAME A CARD unless it is a pin, so the probe
# record is what identity is read out of.  CheckNetDevice is run FROM THE
# DRAWER: it is a test tool on an installed machine, not a C: command, and
# the C: copy is whatever release the machine has installed.
has_arm identity && echo "$DRAWER/CheckNetDevice" >> "$CMDS"

# The same record, RAW, for the probe-order arm.  RAW because the readable
# form is prose and a harness that greps prose is a harness that stops
# noticing when the prose is reworded.  RAW prints one line per step:
#   NN. code <n> card <n> value $xxxxxxxx
has_arm probeorder && echo "$DRAWER/CheckNetDevice RAW" >> "$CMDS"

# ------------------------------------------------------- the payload arm --

PEERADDR=""
PEER_PID=""
# A BLOCK OF 100 PER TAG, the size tests/tools/run-payverify.sh uses and for
# the same reason: the payload arm's full list is 27 cases and the overruns
# arm sits at +90, so a block of 10 put one tag's load port inside the next
# tag's cases.  31000..61000, clear of run-payverify's 30000 block and of
# run-iperf's 20000s.
PORT_BASE=$((31000 + ($(printf '%s' "$TAG" | cksum | cut -d' ' -f1) % 300) * 100))
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
    # tests/tools/run-payverify.sh's own lengths, because this is that
    # harness's proof on the card it cannot reach and a shorter list would
    # be a weaker claim wearing the same name.  -Q is for a rig check.
    RX_LENS="1 2 3 4 5 1459 1460 1461 1462 1463 4095 4096 4097 65535 65536 65537 1048573 1048574 1048575 1048576"
    TX_LENS="1 3 1460 1461 65537 1048575"
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
    # WHAT TURNS THIS INTO AN EFFECT.  One run gives the count for the
    # binary the machine is running.  The change under test is el3.c
    # releasing the receive FIFO's head packet before the call up into the
    # stack rather than after, and its effect is the DIFFERENCE between two
    # runs of this arm with the two libraries installed, at the same load
    # and on the same card.  overruns_delta and loss_ppm are printed so the
    # comparison is arithmetic and not a reading of two transcripts.
    #
    # One long receive is the load: the head of the FIFO has to be released
    # and the next frame taken while the stack is still carrying the last
    # one.  4 MB at 10 Mbit is about four seconds of continuous frames.
    OVR_PORT=$((PORT_BASE + 90))
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
    # And the process it was managing, wherever it is.  Killing timeout(1)
    # is not always killing the peer, and a peer left holding its ports is
    # the next run's diagnosis rather than this one's.
    if [ "$PEERHOST" = local ]; then
        pkill -f "[p]aypeer-$TAG" 2>/dev/null || true
    elif [ -n "$PEERHOST" ]; then
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "pkill -f '[p]aypeer-$TAG' 2>/dev/null; exit 0" \
            > /dev/null 2>&1 || true
    fi
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
    # A COPY UNDER THE TAG'S OWN NAME, in both modes.  The peer holds its
    # whole port block for PEER_LIFE, which is minutes, and a run that ended
    # early leaves one standing; the next run of the same tag then reports
    # "the peer died before the run started" with eight Address already in
    # use behind it and nothing about the machine.  The name is what lets
    # this kill its own predecessor and nobody else's.
    PEER_PY="/tmp/paypeer-$TAG.py"
    PEER_SPEC="/tmp/payspec-$TAG.txt"
    if [ "$PEERHOST" = local ]; then
        cp "$ROOT/tests/tools/paypeer.py" "$PEER_PY"
        cp "$SPEC" "$PEER_SPEC"
        pkill -f "[p]aypeer-$TAG" 2>/dev/null || true
        timeout $((PEER_LIFE + 60)) python3 "$PEER_PY" \
            matrix --spec "$PEER_SPEC" --lifetime "$PEER_LIFE" \
            > "$OUT/peer.out" 2> "$OUT/peer.err" &
        PEER_PID=$!
    else
        scp -q "$ROOT/tests/tools/paypeer.py" "$PEERHOST:$PEER_PY"
        scp -q "$SPEC" "$PEERHOST:$PEER_SPEC"
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "pkill -f '[p]aypeer-$TAG' 2>/dev/null; exit 0" || true
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "timeout $((PEER_LIFE + 60)) python3 $PEER_PY matrix \
                 --spec $PEER_SPEC --lifetime $PEER_LIFE" \
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

        # A UNIT NUMBER IS NOT A CARD NAME.  include/aminetxduo/anxnet.h:16
        # and src/netdev/netdev_device.c:1507: below ANXNET_UNIT_PIN the unit
        # is a POSITION IN THE PROBE ORDER, and only at or above it does the
        # number name a card type.  The installer writes UNIT=0 into
        # DEVS:NetInterfaces/<name>, so an ordinary machine answers on unit 0,
        # and reading that as "some other card" failed this arm on the one
        # machine the harness exists for -- with 29 of 29 payload cases
        # passing underneath it.
        #
        # A pin is believed as a pin.  A probe-order unit is resolved against
        # the driver's own probe record, which is where a card's identity
        # actually lives, and CheckNetDevice prints it.
        if [ "$UNIT" = "$WANT_UNIT" ]; then
            echo "card_unit_kind=pin"
            echo "card_match=yes"
            pass "$IFACE is on $DEV unit $UNIT, which is $CARD's row"
        elif [ "$UNIT" -ge "$UNIT_PIN" ] 2>/dev/null; then
            echo "card_unit_kind=pin"
            echo "card_match=no"
            fail "$IFACE is on $DEV unit $UNIT, which pins another card:" \
                 "$CARD's pin is $WANT_UNIT.  Every number below belongs" \
                 "to whatever that unit names"
        else
            echo "card_unit_kind=probe_order"
            PROBED=$(awk -v want="CARD \"$CARD\"" '
                $0 == want        { in_s = 1; next }
                in_s && /^CARD "/ { exit }
                in_s              { print }' "$REPORT")
            PROBE_UNIT=$(printf "%s\n" "$PROBED" |
                         sed -n "s/.*This card is unit \([0-9][0-9]*\) of.*/\1/p" |
                         head -1)
            if printf "%s\n" "$PROBED" | grep -qx "  ATTACHED."; then
                echo "card_probe_attached=yes"
            else
                echo "card_probe_attached=no"
            fi
            echo "card_probe_unit=${PROBE_UNIT:-none}"
            if [ -z "$PROBE_UNIT" ]; then
                echo "card_match=unknown"
                fail "unit $UNIT is a probe-order unit, so it names no card," \
                     "and the probe record holds no attached $CARD." \
                     "Nothing here establishes which card $IFACE is on"
            elif [ "$PROBE_UNIT" = "$UNIT" ]; then
                echo "card_match=yes"
                pass "$IFACE is on $DEV unit $UNIT, and the probe record" \
                     "gives that unit to $CARD"
            else
                echo "card_match=no"
                fail "$IFACE is on $DEV unit $UNIT and the probe record" \
                     "gives $CARD unit $PROBE_UNIT -- this machine is" \
                     "running another card and every number below belongs" \
                     "to that one"
            fi
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

# ----------------------------------------------------- probeorder --
#
# The COLD half, read out of phase A.  Every value is reported whatever it
# says: which of the two readings the command register passed on is the whole
# question, and a run that only printed a verdict would throw it away.

raw_last() {
    sed -n "s/^ *[0-9]*\. code $2 card [-0-9]* value \\\$\([0-9a-fA-F]*\)\$/\1/p" \
        "$1" | tail -1
}
raw_count() {
    sed -n "s/^ *[0-9]*\. code $2 card [-0-9]* value \\\$[0-9a-fA-F]*\$/x/p" \
        "$1" | wc -l | tr -d ' '
}

# include/aminetxduo/anxdiag.h.  Named here because a number in a grep is a
# number nobody can check.
D_ATTACH_OK=13
D_ATTACH_FAIL=14
D_GETODD=18
D_CR_READ=20
D_ODD_PLAIN=26
D_ODD_WORD=27
D_CR_RETRY=76

probe_report() {          # <file> <tag>
    local f="$1" tag="$2"

    echo "probe_${tag}_steps=$(grep -cE '^ *[0-9]+\. code ' "$f" || true)"
    echo "probe_${tag}_attached=$(raw_count "$f" "$D_ATTACH_OK")"
    echo "probe_${tag}_refused=$(raw_count "$f" "$D_ATTACH_FAIL")"
    echo "probe_${tag}_why=$(raw_last "$f" "$D_ATTACH_FAIL")"
    echo "probe_${tag}_cr_reads=$(raw_count "$f" "$D_CR_READ")"
    echo "probe_${tag}_cr_last=$(raw_last "$f" "$D_CR_READ")"
    echo "probe_${tag}_cr_retry=$(raw_count "$f" "$D_CR_RETRY")"
    echo "probe_${tag}_getodd=$(raw_last "$f" "$D_GETODD")"
    echo "probe_${tag}_odd_plain=$(raw_count "$f" "$D_ODD_PLAIN")"
    echo "probe_${tag}_odd_word=$(raw_count "$f" "$D_ODD_WORD")"
}

if has_arm probeorder; then
    [ -n "$RESTART" ] || {
        echo "probeorder_arm=unwired"
        fail "the probeorder arm needs -r for the same reason the shutdown
       arm does: the warm probe is taken after NetShutdown, which stops the
       httpd the report comes back through"
    }
    if grep -qE '^ *[0-9]+\. code ' "$REPORT"; then
        probe_report "$REPORT" cold
        pass "the cold probe record came back"
    else
        echo "probe_cold_steps=0"
        fail "CheckNetDevice RAW printed no steps in phase A, so there is no
       probe record to compare the warm one against"
    fi
fi

if has_arm shutdown; then
    [ -n "$RESTART" ] || {
        echo "shutdown_arm=unwired"
        fail "the shutdown arm needs the command that starts httpd on this
       machine (-r), because NetShutdown stops it and the report is read
       back through it.  Without one the machine would be left headless"
    }
fi

if { has_arm shutdown || has_arm probeorder; } && [ -n "$RESTART" ]; then
    CMDS_B="$OUT/commands-shutdown.txt"
    {
        echo "netstat -i"
        echo "NetShutdown"
        echo "AddNetInterface $IFACE"
        echo "wait 20"
        echo "netstat -i"
        # THE WARM PROBE.  NetShutdown let the device go and AddNetInterface
        # claimed the slot again, so this record is a second claim in the same
        # power cycle -- which is the condition a cold boot hides.
        has_arm probeorder && echo "CheckNetDevice RAW"
        echo "&$RESTART"
    } > "$CMDS_B"

    echo "==> phase B on $TARGET: NetShutdown, and whether it comes back"
    RC_B=0
    "$ROOT/tools/hwrun.sh" -A "$TARGET" -p "$PORT" -d -t 120 -w "$RETURNWAIT" \
        -D "$DRAWER" -o "$OUT/phaseB" "$CMDS_B" "$STAGE/ToolsSmoke" \
        > "$OUT/phaseB.txt" 2>&1 || RC_B=$?
    sed 's/^/  /' "$OUT/phaseB.txt"

    if has_arm probeorder; then
        BREPORT="$OUT/phaseB/tools.txt"
        if [ -f "$BREPORT" ] && grep -qE '^ *[0-9]+\. code ' "$BREPORT"; then
            probe_report "$BREPORT" warm
            if [ "$(raw_count "$BREPORT" "$D_ATTACH_OK")" != 0 ]; then
                pass "the card attached on the WARM claim, which is the claim
      that used to refuse it"
            else
                fail "the card did NOT attach on the warm claim.  probe_warm_why
      names the reason: 1 is the command register, which is the defect this
      arm exists for and means the retry did not recover it"
            fi
        else
            echo "probe_warm_steps=0"
            fail "no warm probe record came back, so the arm answered nothing"
        fi
    fi

    RET_ICMP=$(sed -n 's/^return_icmp=//p' "$OUT/phaseB.txt" | tail -1)
    RET_HTTP=$(sed -n 's/^return_http=//p' "$OUT/phaseB.txt" | tail -1)

    if ! has_arm shutdown; then
        : # the phase ran only for the warm probe; the return is not asserted
    else
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
fi

echo
if [ "$FAILED" != 0 ]; then
    echo "RESULT=fail"
    exit 1
fi
echo "RESULT=pass"
exit 0
