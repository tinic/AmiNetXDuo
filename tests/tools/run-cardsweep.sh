#!/usr/bin/env bash
#
# Every network card this project supports, booted once each.
#
#   tests/tools/run-cardsweep.sh [-P PEERHOST] [-B IFACE] [-b BUILDDIR]
#                                [-t SECONDS] [-c CARD[,CARD...]] [-l]
#
# WHAT IT PROVES, PER CARD
#
#   The interface comes ONLINE and bytes move in BOTH DIRECTIONS, counted by a
#   machine that is not this one.  Each card is one boot of
#   tests/tools/run-iperf.sh with -N <board> -B <iface> -P <peer>, which puts
#   the guest's own byte counts beside the peer's recv() totals in all four
#   directions; this script re-reads those same two artefacts and gates on the
#   two that matter -- guest tx == peer received, guest rx == peer sent, both
#   non-zero.
#
#   OFF-BOX IS NOT A PREFERENCE.  A frame the emulator's host sends to its own
#   bridged guest never comes back to that NIC's pcap, so a peer on this
#   machine is unreachable from the guest while every other machine on the LAN
#   reaches it (tests/tools/run-iperf.sh:119, tests/perf/run-fitzbench.sh:49).
#   -P must name a third machine, and the guest-as-server arms -- the only
#   proof that anything can be delivered TO the card -- exist only with it.
#
# WHY A SWEEP AND NOT NINE SCRIPTS
#
#   Until this existed, one card was booted by CI (install/test/run-workbench.sh,
#   the a2065) and the other eight had only ever been run by hand.  x-surf-100
#   reached a user broken.  The list of cards this project claims is
#   tools/amiberry-run.sh:226-241 and docs/ReadMe:36-45; the list it can prove
#   is whatever this prints.
#
# EVERY PER-CARD SETTING BELOW IS TRACEABLE
#
#   board -> driver           tools/sana2-stage.sh, sana2_select().  OUR
#                             driver where it covers the card -- anxnet.device,
#                             pinned with CARD= -- and the vendor driver
#                             everywhere else.  AMINETXDUO_SANA2_VENDOR=1
#                             forces the vendor driver for every card, which is
#                             the A/B side of tests/tools/mcastjoin.c.
#   where the driver lives    tools/sana2-stage.sh:43-48 (~/amiga-assets/devs)
#   Zorro II boards           tools/amiberry-run.sh:230-233
#   ne2000_pcmcia             pcmcia=true + inserted=true and so a Gayle
#                             machine (tools/amiberry-run.sh:236-238), a 68020
#                             or better (tools/amiberry-run.sh:196-224), and
#                             4 MB of Fast RAM because 8 MB collides with the
#                             PCMCIA windows (tools/amiberry-run.sh:315-323,
#                             applied by that script, not by this one)
#   xsurf100z3                BOARD_AUTOCONFIG_Z3 (expansion.cpp:6476), which
#                             maps only with cs_z3autoconfig AND a 32-bit
#                             address space (expansion.cpp:565).  An A1200 is
#                             address_space_24 = true (cfgfile.cpp:9289) and
#                             sets neither, so the board is never seen; A3000
#                             sets both (cfgfile.cpp:9489, :10421).  That is
#                             why one card here boots a different machine.
#
# ONE GUEST AT A TIME, AND NEVER THE DEMO'S
#
#   Each card gets its own AMINETXDUO_RUN_TAG, its own MAC and its own address,
#   because tools/amiberry-run.sh:245-257 derives the drive, the serial log and
#   the serial PORT from the tag, and a shared tag means one run deleting
#   another's drive.  The cards still run in sequence, under a lock: this host
#   has 8 cores and the demo instance is one of them.  NOTHING here pkills
#   anything.
#
#   The addresses are static and outside what the lab's DHCP hands out, for the
#   reason tests/tools/run-iperf.sh:66-69 gives: the peer has to call IN, so the
#   guest's address must be known before it boots.
#
# COST, MEASURED
#
#   See the wall_s column.  A card that comes up costs about two and a half
#   minutes; a card that does not costs its timeout.  The whole sweep is a
#   nightly, not a per-push job.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
PEERHOST="${AMINETXDUO_CARDSWEEP_PEER:-}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TIMEOUT=300
ONLY=""
LIST=0

# TEN PORTS PER CARD ON THE PEER, AND NOT ONE SET SHARED.
#
# tests/tools/run-iperf.sh clears leftover peers with a pkill scoped to its OWN
# run tag, which is right -- a broad pattern takes out other people's runs on a
# shared machine.  The consequence is that consecutive tagged runs cannot free
# each other, and its peers outlive their run by design (timeout $TIMEOUT + 150)
# while a card here takes about thirty seconds.  Cards two onward then met
# "[Errno 98] Address already in use" and a guest whose peer never answered.
#
# run-iperf.sh derives its own block from its tag now, which would cover this,
# but the explicit base stays: it is contiguous and printable, so a sweep's
# ports can be read off one line rather than hashed per card.
#
# Distinct ports rather than a wider kill: nothing has to die on time.
#
# PORTBASE ALONE IS NOT ENOUGH to run two sweeps on one LAN, and this comment
# used to say it was. Ports are the only thing it moves; the run tag, the guest
# address and the guest MAC all come from the card name, so two sweeps of one
# card still collide on all three. SWEEP_ID below is what actually separates
# them, and the address is offset with it.
PORTBASE="${AMINETXDUO_CARDSWEEP_PORTBASE:-7400}"

# Who this sweep is. Defaults to the checkout's directory name, which is what
# distinguishes two agents on one host; override for anything else that wants
# to run concurrently. Sanitised because it reaches a run tag, a file name and
# a pkill pattern.
SWEEP_ID="${AMINETXDUO_CARDSWEEP_ID:-$(basename "$ROOT")}"
SWEEP_ID=$(printf '%s' "$SWEEP_ID" | tr -c 'A-Za-z0-9' '-' | cut -c1-16)

# The last octet of the guest address and of the MAC move with the sweep, so
# two sweeps of one card are two different machines on the wire rather than two
# machines claiming one address. A collision there produces an RST from the
# wrong guest, which reads as "connection refused" and looks like a stack fault.
SWEEP_SLOT=$(( $(printf '%s' "$SWEEP_ID" | cksum | cut -d' ' -f1) % 6 ))

while getopts "P:B:b:t:c:p:l" opt; do
    case "$opt" in
        P) PEERHOST="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) ONLY="$OPTARG" ;;
        p) PORTBASE="$OPTARG" ;;
        l) LIST=1 ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac

# ------------------------------------------------------------- the cards ----
#
# The table is tests/tools/cards.sh, shared with tests/tools/run-cardsweep6.sh.
# One table: a card added for one sweep is a card the other sweep boots too.
# CARDS, UNTESTABLE and cards_rows() come from there.

# shellcheck source=tests/tools/cards.sh
. "$ROOT/tests/tools/cards.sh"

. "$ROOT/tools/sana2-stage.sh"

# ----------------------------------------------------------------- listing --

card_rows() { cards_rows "$ONLY"; }

if [ "$LIST" = 1 ]; then
    echo "cards this sweep boots:"
    card_rows | while read -r board model addr mac; do
        sana2_select "$board" "$BUILDDIR"
        printf '  card=%s board=%s model=%s driver=%s driver_source=%s anxcard=%s address=%s mac_tail=%s driver_present=%s\n' \
               "$board" "$board" "$model" "$SANA2_SEL_DRIVER" \
               "$SANA2_SEL_SOURCE" "${SANA2_SEL_CARD:-none}" "$addr" "$mac" \
               "$( [ -n "$SANA2_SEL_PATH" ] && echo yes || echo no)"
    done
    echo "drivers with no board to run them on:"
    printf '%s\n' "$UNTESTABLE" | while read -r drv reason; do
        [ -n "$drv" ] || continue
        printf '  driver=%s status=untestable reason="%s"\n' "$drv" "$reason"
    done
    exit 0
fi

[ -n "$PEERHOST" ] || {
    echo "-P is not optional: a bridged guest cannot be reached from the" >&2
    echo "machine running the emulator, so the byte counts have to be taken" >&2
    echo "on a third machine.  -P <user@host>, or AMINETXDUO_CARDSWEEP_PEER." >&2
    exit 2
}

# ---------------------------------------------------------------- the lock --
#
# tests/tools/run-iperf.sh stages into one directory per tag but its peer logs
# and its stage are shared, and this host runs a demo instance nobody wants
# restarted.  One sweep at a time, and it says so rather than interleaving.
mkdir -p "$ROOT/build"
LOCK="$ROOT/build/cardsweep.lock"
exec 9>"$LOCK"
flock -n 9 || {
    echo "another cardsweep holds $LOCK; not starting a second one" >&2
    exit 2
}

# ----------------------------------------------------------------- reading --
#
# Both halves of every assertion come out of files, not out of the other
# script's prose: the guest's own transcript, and the peer's.

# rc of the nth block of a ToolsSmoke transcript.
block_rc() { # transcript banner
    awk -v banner="$2" '
        index($0, "===== " banner " =====") == 1 { on = 1; next }
        on && /^----- rc / { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
    ' "$1"
}

# Every bytes= the guest reported for one direction, added up.  run-iperf.sh
# runs the guest-as-client direction twice, once bounded by seconds and once by
# size, and both report dir=tcp-tx; the total is what left the card, and it is
# the number the peer's total is compared against.
guest_bytes() { # transcript dir
    awk -v want="$2" '
        $0 ~ ("dir=" want "([^-a-z]|$)") {
            for (i = 1; i <= NF; i++)
                if ($i ~ /^bytes=/) { sub(/^bytes=/, "", $i); t += $i }
        }
        END { print t + 0 }
    ' "$1"
}

# The same total from the other end of the wire.  Several logs, because the two
# guest-as-client arms are served by two peers on two ports.
peer_bytes() { # peerlogdir name...
    _dir=$1; shift
    _t=0
    for _n in "$@"; do
        [ -f "$_dir/$_n.out" ] || continue
        _v=$(awk '{ for (i = 1; i <= NF; i++)
                        if ($i ~ /^peer_bytes=/) { sub(/^peer_bytes=/, "", $i); v = $i } }
                  END { print v + 0 }' "$_dir/$_n.out")
        _t=$((_t + _v))
    done
    echo "$_t"
}

# -------------------------------------------------------------------- run ---

RESULTS="$ROOT/build/cardsweep-results.txt"
: > "$RESULTS"
LOGDIR="$ROOT/build/cardsweep-logs"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

SWEEP_START=$(date +%s)
NPASS=0; NFAIL=0; NSKIP=0; NCARRIED=0; IDX=0

echo "==> peer $PEERHOST, bridge $IFACE, build $BUILD, ${TIMEOUT}s per card," \
     "peer ports from $PORTBASE"

# The card list arrives on fd 3, not on stdin: everything in the loop body is
# free to have a stdin of its own, and nothing in it can eat the list.
while read -r -u 3 board model addr mac; do
    [ -n "$board" ] || continue

    # Move this sweep's guest off the card's shared address and MAC. The table
    # gives one of each per card, which is right for one sweep and wrong for
    # two: two guests answering for 192.168.1.241 means an RST from whichever
    # one did not open the socket, and that reads as "connection refused" from
    # a stack that is working. SWEEP_SLOT is 0 for a lone sweep, so the
    # ordinary case keeps the table's own addresses.
    # DOWNWARD, not upward: the table's addresses are .241-.249, and adding
    # even one slot's worth puts the last octet past 255. Ten per slot keeps
    # six slots of nine cards inside .191-.249 with no overlap between slots
    # and none between cards. (Checked: .261 is what the first version of this
    # produced, and every arm would have failed to configure.)
    if [ "$SWEEP_SLOT" -ne 0 ]; then
        addr="${addr%.*}.$(( ${addr##*.} - SWEEP_SLOT * 10 ))"
        mac="${mac%:*}:$(printf '%02x' $(( 0x${mac##*:} + SWEEP_SLOT * 32 )))"
    fi

    sana2_select "$board" "$BUILDDIR"
    drv=$SANA2_SEL_DRIVER
    drvpath=$SANA2_SEL_PATH
    anxcard=$SANA2_SEL_CARD

    # A driver that is not on this machine is a SKIP with its own status, and
    # it is decided here rather than by booting: tools/sana2-stage.sh warns and
    # carries on, which would spend a boot and a timeout to report "the network
    # did not start".
    if [ -z "$drvpath" ]; then
        if [ "$SANA2_SEL_SOURCE" = anxnet ]; then
            reason="no anxnet.device in $BUILD; build the tree, or set AMINETXDUO_ANXNET"
        else
            reason="no $drv in the driver store; set AMINETXDUO_SANA2_STORE"
        fi
        printf 'card=%s board=%s model=%s driver=%s driver_source=%s anxcard=%s status=skip_no_driver wall_s=0 reason="%s"\n' \
               "$board" "$board" "$model" "$drv" "$SANA2_SEL_SOURCE" \
               "${anxcard:-none}" "$reason" | tee -a "$RESULTS"
        NSKIP=$((NSKIP + 1))
        continue
    fi

    # THE TAG CARRIES THE SWEEP'S IDENTITY, NOT JUST THE CARD'S.
    #
    # It used to be "cardsweep-$board" and nothing else, and -p PORTBASE was
    # believed to be enough to run two sweeps on one LAN. It is not: the tag,
    # the guest address and the guest MAC all derive from the card name alone,
    # so two sweeps of the same card share all three, and run-iperf.sh's
    # peer-side `pkill -f '[i]perfpeer-<tag>'` then kills the OTHER sweep's
    # peers mid-transfer.
    #
    # What that looks like is not noise, it is a meaningless verdict: every arm
    # reports peer_tx_bytes=0 and "the guest can send and cannot be sent to",
    # the peer logs "connection refused" from whichever guest answered for the
    # shared address, and the guest's own transfer is truncated -- 978,944
    # bytes in 1,624 ms where a clean run gets ~2.1 MB in ~3.5 s. The rate
    # survives, so nothing looks obviously wrong, and a 2% difference cannot be
    # resolved through it at all. Three agents measured this way for an hour.
    #
    # AMINETXDUO_CARDSWEEP_ID defaults to the checkout's own name, which is
    # what actually distinguishes two sweeps on this host.
    tag="cardsweep-$SWEEP_ID-$board"
    t0=$(date +%s)
    base=$((PORTBASE + IDX * 10))
    IDX=$((IDX + 1))

    echo
    echo "===================== $board ($drv${anxcard:+ CARD=$anxcard}, $model, $addr, ports $base+) ====================="

    # < /dev/null is load-bearing.  run-iperf.sh starts its peers as
    # backgrounded ssh, and an ssh with a readable stdin reads it: given this
    # loop's card list on stdin it swallows the remaining cards, so the sweep
    # ran ONE card and reported itself complete, and whichever peer won the
    # race came up without a listening socket.  Both symptoms, one cause.
    # DELETE THE PREVIOUS TRANSCRIPT AND PEER LOG FIRST.
    #
    # Everything this sweep reports is read back out of these two files. If an
    # arm fails to start -- and one whose flock is still held by a leaked fd 9
    # exits 2 before booting anything -- the files from the PREVIOUS arm are
    # still sitting there, and the reader below picks them up and reports them
    # as this card's result. It produced nine identical "measurements"
    # (bytes=1830912 ms=3532) from one real run before anyone noticed, which is
    # worse than any failure: a sweep that fabricates numbers is not a sweep.
    #
    # Cheap, unconditional, and it makes a missing file mean "this arm produced
    # nothing", which is what the reader below already assumes.
    rm -f "$ROOT/build/amiberry-testhd-$tag/tools.txt" \
          "$ROOT/build/iperf-peers-$tag" 2>/dev/null || true

    set +e
    env AMINETXDUO_RUN_TAG="$tag" \
        AMINETXDUO_AMIBERRY_MAC="02:41:4d:49:$mac" \
        AMINETXDUO_SANA2_DRIVER="$drvpath" \
        AMINETXDUO_SANA2_DRIVER_NAME="$drv" \
        AMINETXDUO_SANA2_DEVICE="$drv" \
        AMINETXDUO_SANA2_CARD="$anxcard" \
        AMINETXDUO_IPERF_PORT_TCP="$((base + 1))" \
        AMINETXDUO_IPERF_PORT_UDP="$((base + 2))" \
        AMINETXDUO_IPERF_PORT_SIZE="$((base + 3))" \
        AMINETXDUO_IPERF_PORT_SRVTCP="$((base + 4))" \
        AMINETXDUO_IPERF_PORT_SRVUDP="$((base + 5))" \
        AMINETXDUO_IPERF_PORT_DEAD="$((base + 9))" \
        "$ROOT/tests/tools/run-iperf.sh" \
            -N "$board" -B "$IFACE" -P "$PEERHOST" -m "$model" \
            -a "$addr" -b "$BUILD" -t "$TIMEOUT" \
        > "$LOGDIR/$board.log" 2>&1 < /dev/null
    rc=$?
    set -e

    t1=$(date +%s)
    wall=$((t1 - t0))

    # This card's own peers, and nothing else's.  The bracket is what stops
    # pkill matching the shell that carries the pattern, and the tag is what
    # stops it matching another agent's run on a shared machine -- the same
    # two precautions tests/tools/run-iperf.sh:258-263 takes.  Best effort:
    # the ports are already unique, so this only keeps nine idle peers from
    # sitting on the peer's memory for the rest of the sweep.
    ssh -o ConnectTimeout=10 -n "$PEERHOST" \
        "pkill -f '[i]perfpeer-$tag' 2>/dev/null; exit 0" >/dev/null 2>&1 || true

    tail -40 "$LOGDIR/$board.log"

    report="$ROOT/build/amiberry-testhd-$tag/tools.txt"
    peers="$ROOT/build/iperf-peers-$tag"

    iface_rc=""
    tx=0; rx=0; utx=0
    if [ -f "$report" ]; then
        iface_rc=$(block_rc "$report" "SYS:AddNetInterface eth0")
        tx=$(guest_bytes "$report" tcp-tx)
        rx=$(guest_bytes "$report" tcp-rx)
        utx=$(guest_bytes "$report" udp-tx)
    fi
    # tcp and size are the two ports the guest-as-client arms send to; srvtcp
    # is the peer that calls IN, which is the only proof anything can be
    # delivered to this card.
    peer_rx=$(peer_bytes "$peers" tcp size)
    peer_tx=$(peer_bytes "$peers" srvtcp)
    peer_urx=$(peer_bytes "$peers" udp)

    # This script's own gate, on numbers it read itself.  run-iperf.sh asserts
    # more than this and its rc carries all of it; what is added here is that
    # BOTH directions are non-zero and both agree with the machine off the box,
    # which is the one claim a card has to support to count as covered.
    # Does the CARD's own claim hold: online, and bytes both ways, each total
    # agreeing with the machine off the box.
    carried=no
    if [ "${iface_rc:-1}" = 0 ] && [ "$tx" -gt 0 ] && [ "$rx" -gt 0 ] &&
       [ "$tx" = "$peer_rx" ] && [ "$rx" = "$peer_tx" ]; then
        carried=yes
    fi

    status=fail
    why=""
    if [ "$rc" = 0 ] && [ "$carried" = yes ]; then
        status=pass
    elif [ "$carried" = yes ]; then
        # The card carried traffic in both directions and the arm still failed.
        # Its own status, because "the card does not work" and "something in
        # this arm does not work on this card" are different claims and the
        # second one has been read as the first.  Both are failures; only this
        # one has byte counts behind it.
        status=fail_assert
        why=" reason=\"bytes moved both ways and agreed off-box; another assertion in the arm failed -- read the log\""
    elif [ "$rc" = 2 ]; then
        # run-iperf.sh spends 2 on "this rig cannot run this arm" -- a binary
        # that was not built, a peer that cannot be reached, a peer process
        # that died before the boot.  That is not a verdict on the card, and
        # recording it as one would blame the card for the lab.  It is not a
        # pass either: the card was not measured.
        status=skip_setup
        why=" reason=\"the rig refused the arm before the card was measured; see the log\""
    elif [ "$rc" = 124 ]; then
        why=" reason=\"the guest never finished; ${TIMEOUT}s timeout\""
    elif [ ! -f "$report" ]; then
        why=" reason=\"the guest wrote no transcript at all\""
    elif [ "${iface_rc:-1}" != 0 ]; then
        why=" reason=\"the interface did not come online\""
    elif [ "$peer_rx" = 0 ] && [ "$peer_tx" = 0 ]; then
        why=" reason=\"online, and not one byte reached the peer either way\""
    elif [ "$peer_tx" = 0 ]; then
        why=" reason=\"the guest can send and cannot be sent to\""
    elif [ "$peer_rx" = 0 ]; then
        why=" reason=\"the guest can be sent to and cannot send\""
    else
        why=" reason=\"counts disagree with the peer, or an assertion in run-iperf.sh failed\""
    fi

    case "$status" in
        pass)        NPASS=$((NPASS + 1)) ;;
        skip_setup)  NSKIP=$((NSKIP + 1)) ;;
        fail_assert) NFAIL=$((NFAIL + 1)); NCARRIED=$((NCARRIED + 1)) ;;
        *)           NFAIL=$((NFAIL + 1)) ;;
    esac

    printf 'card=%s board=%s model=%s driver=%s driver_source=%s anxcard=%s status=%s rc=%s iface_rc=%s tx_bytes=%s peer_rx_bytes=%s rx_bytes=%s peer_tx_bytes=%s udp_tx_bytes=%s peer_udp_rx_bytes=%s wall_s=%s log=%s%s\n' \
           "$board" "$board" "$model" "$drv" "$SANA2_SEL_SOURCE" \
           "${anxcard:-none}" "$status" "$rc" "${iface_rc:-none}" \
           "$tx" "$peer_rx" "$rx" "$peer_tx" "$utx" "$peer_urx" "$wall" \
           "$LOGDIR/$board.log" "$why" | tee -a "$RESULTS"
done 3<<EOF
$(card_rows)
EOF

# ---------------------------------------------------------------- verdict ---

WALL=$(( $(date +%s) - SWEEP_START ))

echo
echo "======================== every card, one line ========================"
cat "$RESULTS"
printf '%s\n' "$UNTESTABLE" | while read -r drv reason; do
    [ -n "$drv" ] || continue
    printf 'driver=%s status=untestable reason="%s"\n' "$drv" "$reason"
done
echo "======================================================================"
printf 'cardsweep: cards=%d pass=%d fail=%d fail_assert=%d skip=%d carried_both_ways=%d wall_s=%d\n' \
       "$((NPASS + NFAIL + NSKIP))" "$NPASS" "$((NFAIL - NCARRIED))" \
       "$NCARRIED" "$NSKIP" "$((NPASS + NCARRIED))" "$WALL"

# FOUR OUTCOMES, AND ONLY ONE OF THEM IS GREEN.
#
#   0  every card carried bytes both ways and every arm was clean
#   1  A CARD DID NOT CARRY BYTES BOTH WAYS.  The card claim.
#   3  nothing failed, and a card was not measured -- read the table.
#   4  A CARD CARRIED BYTES BOTH WAYS AND AN ASSERTION IN ITS ARM FAILED.
#
# 1 and 4 are separate because the two claims are separate: "this card does not
# work" and "something in this arm does not work on this card" want different
# people looking at them, and 1 was read as covering both.  Both are FAILURES,
# and 4 used to be folded into 3.
#
# 3 SAYS NOTHING FAILED.  That is the whole content of it: skip_setup, the rig
# refusing an arm before the card was measured, and skip_no_driver, a driver
# that is not in the store.  Neither is a verdict on a card and neither is a
# verdict on this project.  Nothing that produced a failing assertion reaches
# it any more.
#
# The earlier reading -- that 4 belongs with 3 because the arm is flaky -- came
# from two sweeps twelve minutes apart on 2026-08-11 disagreeing about which
# cards failed the three-second TCP send.  That flapping was our own iperf UDP
# sender, and it is fixed; xsurf100z3 sat in exit 3 for weeks behind it, and
# only went red at all because ne2000_pcmcia failed outright beside it.  A gate
# that cannot go red is worse than one that cannot go green, because the second
# one gets noticed.  If the arm flaps again, fix the arm or drop the assertion;
# do not colour a failure green.
if [ "$((NFAIL - NCARRIED))" != 0 ]; then echo "result=fail"; exit 1; fi
if [ "$NCARRIED" != 0 ];             then echo "result=fail_assert"; exit 4; fi
if [ "$NSKIP" != 0 ];                then echo "result=partial"; exit 3; fi
echo "result=ok"
exit 0
