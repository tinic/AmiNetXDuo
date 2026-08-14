#!/usr/bin/env bash
#
# Every network card, and whether IPv6 reaches anything past the router.
#
#   tests/tools/run-cardsweep6.sh [-B IFACE] [-b BUILDDIR] [-t SECONDS]
#                                 [-c CARD[,CARD...]] [-6 ADDR] [-p PORT]
#                                 [-4 ADDR] [-l]
#
# WHAT IT PROVES, PER CARD
#
#   The guest forms a global IPv6 address from a router advertisement, and
#   then exchanges packets with an address that is NOT on this LAN -- an echo
#   reply, or a TCP connection, from a host one hop or more past the default
#   router.  Both halves have to work: the request leaves the card, and the
#   ANSWER comes back, which is the half that needs the router to be able to
#   solicit the guest at its solicited-node multicast address.
#
# WHY OFF-LAN IS THE WHOLE POINT
#
#   x-surf.device and x-surf-100.device answer S2ERR_BAD_ADDRESS to every
#   S2_ADDMULTICASTADDRESS -- they test bit 7 of ios2_SrcAddr[0], where the
#   Ethernet group bit is bit 0 -- so the hash filter stayed empty and no
#   multicast frame was ever delivered to those three cards.  Everything
#   on-link kept working: DHCP, ARP, ping, and SLAAC too, because the router
#   advertisement that answers a solicitation comes back unicast.  What broke
#   is the return path from off-link: the router solicits the guest at
#   33:33:ff:xx:xx:xx, the card drops it, the neighbour entry never resolves,
#   and the answer is discarded one hop away.  Three shipping cards had no
#   off-LAN IPv6 for as long as they were supported.
#
#   BUT THIS SWEEP CANNOT SEE THAT DEFECT, and must not be read as proving
#   a driver takes multicast.  src/sana2/sana2_device.c re-asks for the same
#   hash bucket with a synthetic 80:00:00:00:00:xx that the bit-7 test
#   accepts, so those cards pass here through THIS stack and would fail
#   through Roadshow or AmiTCP.  What discriminates is tests/tools/mcastjoin.c,
#   which asks the driver with no stack in the way.
#
#   An on-LAN IPv6 check passes happily through all of that.  So does
#   tests/tools/run-cardsweep.sh, which is IPv4 and one segment.  So does
#   every runner under tests/ipv6/, which is WinUAE or SLIRP -- and SLIRP
#   answers a router solicitation itself, so it cannot have this defect to
#   find.  The destination has to be past the router, and the preflight below
#   refuses to run against one that is not.
#
#   The on-link probe is the LAST command the guest runs, not the first.  A
#   neighbour solicitation the guest sends carries its own link-layer address,
#   so probing the router first would teach the router what the broken card
#   cannot tell it, and the off-LAN arm behind it could pass on a card that
#   fails alone.
#
# EVERY CARD, ONE BOOT EACH
#
#   The table is tests/tools/cards.sh, shared with run-cardsweep.sh so the two
#   sweeps cannot disagree about what "every card" is.  Board to driver is
#   tools/sana2-stage.sh, the same mapping the other sweep uses.  Each card
#   gets its own AMINETXDUO_RUN_TAG, its own MAC and its own guest name,
#   because tools/amiberry-run.sh:245-259 derives the drive, the serial log
#   and the serial PORT from the tag.  Cards run in sequence under a lock, and
#   NOTHING here pkills anything.
#
# NO PEER, AND NO STATIC ADDRESS
#
#   Nothing has to call into the guest here, which is what forces a third
#   machine and a known address on run-cardsweep.sh.  The guest takes a DHCP
#   lease for IPv4 and forms its IPv6 address from the wire.  A bridge is the
#   only thing this needs beyond a ROM.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TIMEOUT=240
ONLY=""
LIST=0

# The destination, and why it is a literal address.
#
# A name would test the resolver as well, and a lab whose DNS is fine while
# its uplink is down would read as nine broken cards.  1.1.1.1's v6 answers
# both ICMPv6 echo and TCP 53, so one host serves both proofs, and the
# preflight below makes the host demonstrate both before any card is booted.
TARGET6="${AMINETXDUO_CARDSWEEP6_TARGET:-2606:4700:4700::1111}"
TARGET6_PORT="${AMINETXDUO_CARDSWEEP6_PORT:-53}"
TARGET4="${AMINETXDUO_CARDSWEEP6_TARGET4:-1.1.1.1}"

# How long the guest waits after AddNetInterface for the router advertisement
# and for duplicate address detection.  Bring-up including IPv6 measures about
# 2.5 s; this is an order of magnitude of slack and it is spent nine times, so
# it is not free.
SETTLE=15

# ONE BYTE OF THE MAC IS NEW EVERY RUN, AND IT HAS TO BE.
#
# A card's SLAAC address is derived from its MAC, so a fixed MAC gives the
# same guest address every run -- and the ROUTER remembers it.  A neighbour
# cache entry it already holds is refreshed by unicast probes, which any card
# accepts, so the second run of a broken card reaches off-LAN on an entry the
# first run left behind and the gate reads green.  Measured: with a fixed MAC
# the three X-Surf cards passed on a build with the multicast fix reverted,
# fifteen minutes after a run that had passed with it.
#
# A fresh byte per sweep means the router has never heard of these addresses
# and has to solicit them at their solicited-node multicast address, which is
# the frame the defect drops.  It also keeps two sweeps on one LAN apart.
# ne2000_pcmcia IS THE EXCEPTION: the emulated PCMCIA card ignores the MAC it
# is given and comes up with the HOST's, one bit flipped, so its guest address
# is the same every run and a stale entry in the router can still cover a
# failure there.  Nothing in this harness can change that; the other eight
# cards are covered.
RUNBYTE="${AMINETXDUO_CARDSWEEP6_RUNBYTE:-$(printf '%02x' $((RANDOM % 256)))}"

while getopts "B:b:t:c:6:p:4:l" opt; do
    case "$opt" in
        B) IFACE="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) ONLY="$OPTARG" ;;
        6) TARGET6="$OPTARG" ;;
        p) TARGET6_PORT="$OPTARG" ;;
        4) TARGET4="$OPTARG" ;;
        l) LIST=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac
TOOLS="$BUILDDIR/src/tools"
BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"

# shellcheck source=tests/tools/cards.sh
. "$ROOT/tests/tools/cards.sh"
# shellcheck source=tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"

infra() { echo "error=$*"; echo "result=infra"; exit 2; }

# ----------------------------------------------------------------- listing --

if [ "$LIST" = 1 ]; then
    echo "cards this sweep boots:"
    cards_rows "$ONLY" | while read -r board model _addr mac; do
        drv=$(sana2_driver_for "$board")
        have=$(sana2_local_driver "$drv")
        printf '  card=%s model=%s driver=%s mac_tail=%s driver_present=%s\n' \
               "$board" "$model" "$drv" "$mac" \
               "$( [ -n "$have" ] && echo yes || echo no)"
    done
    echo "drivers with no board to run them on:"
    printf '%s\n' "$UNTESTABLE" | while read -r drv reason; do
        [ -n "$drv" ] || continue
        printf '  driver=%s status=untestable reason="%s"\n' "$drv" "$reason"
    done
    exit 0
fi

# --------------------------------------------------------------- preflight --
#
# Everything that is about the LAB rather than about a card, decided once and
# before any boot.  A lab that cannot do this is exit 2 and nine cards
# unmeasured; it is never a red card.  The IPv6 delegation here has gone away
# for an afternoon before, and an outage that reads as a card defect costs a
# day.

for t in ToolsSmoke AddNetInterface ShowNetStatus ping nc traceroute netstat; do
    [ -x "$TOOLS/$t" ] || infra "no $TOOLS/$t; build $BUILD first"
done
[ -f "$BSD" ] || infra "no $BSD; build $BUILD first"

# A tree built with -DAMINETXDUO_IPV6=OFF has no IPv6 to gate, and would
# report every card red for a reason that is in the build.
if [ -f "$BUILDDIR/CMakeCache.txt" ] &&
   grep -q '^AMINETXDUO_IPV6:BOOL=OFF' "$BUILDDIR/CMakeCache.txt"; then
    infra "$BUILD was configured with AMINETXDUO_IPV6=OFF"
fi

# A missing ROM is nine boots that each cost their own rc 2.  It is the lab,
# it is knowable here, and it is worth one line.
[ -n "${AMINETXDUO_KICKSTART:-}${AMINETXDUO_KICKSTART_A1200:-}" ] ||
    infra "no boot ROM; export AMINETXDUO_KICKSTART (. ~/amiga-assets/env.sh)"

command -v ip >/dev/null 2>&1 || infra "no ip(8) on this host"
command -v python3 >/dev/null 2>&1 || infra "no python3 on this host"

ip -6 route show default dev "$IFACE" 2>/dev/null | grep -q . ||
    infra "no IPv6 default route on $IFACE, so this wire has no router advertisement"

HOST_GLOBAL=$(ip -6 -o addr show dev "$IFACE" scope global 2>/dev/null |
              awk '{print $4}' | cut -d/ -f1 | head -1)
[ -n "$HOST_GLOBAL" ] ||
    infra "no global IPv6 address on $IFACE; the delegation is down, not the cards"

# The destination is off this LAN, asserted rather than assumed.  A target
# that resolves on-link measures the segment, which is the check that was
# already passing while three cards were broken.
ROUTE6=$(ip -6 route get "$TARGET6" 2>/dev/null)
case "$ROUTE6" in
    *" via "*) ;;
    *) infra "$TARGET6 is on-link from here, which is not what this gate measures" ;;
esac
VIA=$(printf '%s\n' "$ROUTE6" | awk '{for (i=1;i<NF;i++) if ($i=="via") print $(i+1)}')

ping -6 -c 1 -W 3 "$TARGET6" >/dev/null 2>&1 ||
    infra "this host cannot reach $TARGET6 over IPv6"

python3 -c 'import socket, sys
s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
s.settimeout(5)
s.connect((sys.argv[1], int(sys.argv[2])))
s.close()' "$TARGET6" "$TARGET6_PORT" >/dev/null 2>&1 ||
    infra "this host cannot open TCP [$TARGET6]:$TARGET6_PORT"

ping -c 1 -W 3 "$TARGET4" >/dev/null 2>&1 ||
    infra "this host cannot reach $TARGET4 over IPv4"

# The on-link contrast: a global address on this segment that answers.  The
# default router's own is the one address guaranteed to be there without a
# second machine, and it is the SAME box the off-LAN arm goes through, so
# "on-link works, off-LAN does not" localises the fault to the hop past it.
# Not having one is not a failure -- the contrast is context, not the gate.
[ -z "$VIA" ] || ping -6 -c 1 -W 2 "$VIA%$IFACE" >/dev/null 2>&1 || true
ONLINK6=$(ip -6 neigh show dev "$IFACE" 2>/dev/null |
          awk '$1 !~ /^fe80:/ && /router/ {print $1; exit}')
if [ -n "$ONLINK6" ]; then
    ping -6 -c 1 -W 3 "$ONLINK6" >/dev/null 2>&1 || ONLINK6=""
fi

echo "host/iface=$IFACE global=$HOST_GLOBAL router=${VIA:-unknown}"
echo "host/target6=$TARGET6 port=$TARGET6_PORT offlink=yes target4=$TARGET4"
echo "host/onlink6=${ONLINK6:-none}"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] ||
    infra "no a2065.device; set AMINETXDUO_A2065"

# ---------------------------------------------------------------- the lock --
#
# One sweep at a time in this tree: nine guests in sequence on an eight-core
# host, sharing a serial port derived from the tag.

mkdir -p "$ROOT/build"
LOCK="$ROOT/build/cardsweep6.lock"
exec 9>"$LOCK"
flock -n 9 || infra "another cardsweep6 holds $LOCK"

# ----------------------------------------------------------------- reading --

REPORT=""

block() { # banner
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- / { exit }
        on { print }
    ' "$REPORT"
}

rc_of() { # banner
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- rc / { print; exit }
    ' "$REPORT" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'
}

# How many echo replies came back, out of the guest's own summary line.
replies_of() { # banner
    block "$1" |
        sed -n 's/^[0-9]* packets transmitted, \([0-9]*\) received.*/\1/p' |
        tail -1
}

# The non-tentative global address the guest formed, the same read
# tests/tools/run-multiaddr.sh:273 takes.
guest_global6() {
    awk '$1 == "address6" && $2 !~ /^fe80:/ && $0 !~ /\(tentative\)/ \
         { print $2; exit }' "$REPORT"
}

# Distinct addresses that answered a traceroute, which is what says the
# destination really is past something.
tr_hops() { # banner
    block "$1" |
        awk '/^traceroute to/ { next }
             { for (i = 1; i <= NF; i++)
                   if ($i ~ /^[0-9a-fA-F:]*:[0-9a-fA-F:]+$/) { print $i; break } }' |
        sort -u | grep -c . || true
}

# -------------------------------------------------------------------- run ---

RESULTS="$ROOT/build/cardsweep6-results.txt"
: > "$RESULTS"
LOGDIR="$ROOT/build/cardsweep6-logs"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

SWEEP_START=$(date +%s)
NPASS=0; NFAIL=0; NSKIP=0

[ "$(cards_rows "$ONLY" | grep -c .)" -gt 0 ] ||
    infra "-c $ONLY matched no card in tests/tools/cards.sh"

echo "==> bridge $IFACE, build $BUILD, ${TIMEOUT}s per card, ${SETTLE}s settle," \
     "MACs 02:41:4d:4b:$RUNBYTE:xx"

# The card list on fd 3, not on stdin: an ssh or an emulator in the loop body
# with a readable stdin eats the remaining cards, which is how the other sweep
# once ran one card and called itself complete.
while read -r -u 3 board model _addr mac; do
    [ -n "$board" ] || continue

    drv=$(sana2_driver_for "$board")
    drvpath=$(sana2_local_driver "$drv")

    if [ -z "$drvpath" ]; then
        printf 'card=%s model=%s driver=%s status=skip_no_driver wall_s=0 reason="no %s in the driver store; set AMINETXDUO_SANA2_STORE"\n' \
               "$board" "$model" "$drv" "$drv" | tee -a "$RESULTS"
        NSKIP=$((NSKIP + 1))
        continue
    fi

    tag="cardsweep6-$board"
    t0=$(date +%s)

    echo
    echo "===================== $board ($drv, $model) ====================="

    STAGE="$ROOT/build/cardsweep6-stage-$tag"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$A2065" "$STAGE/devs/a2065.device"
    cp "$BSD" "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface ShowNetStatus ping nc traceroute netstat; do
        cp "$TOOLS/$t" "$STAGE/$t"
    done

    # DHCP for IPv4 and AUTO for IPv6: nothing calls in, so no address has to
    # be known before the boot, and AUTO is what a user gets.  CONFIGURE6 is
    # AUTO by default too (src/bsdsocket/interfaces.c:1431); it is written out
    # because a gate should not depend on a default it does not name.
    cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
IFEOF
    printf 'hostname anx6-%s\n' "$board" > "$STAGE/devs/Internet/name_resolution"

    export AMINETXDUO_SANA2_DRIVER="$drvpath"
    sana2_stage "$board" "$STAGE/devs"
    echo "==> $board: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"

    # The commands, in order, and the order is load-bearing: the IPv4 control
    # first, then the two off-LAN arms, and the on-link probe LAST so it
    # cannot warm the router's neighbour cache for the arms that matter.
    C_IFUP="SYS:AddNetInterface eth0"
    C_STATUS="SYS:ShowNetStatus ALL"
    C_PING4="SYS:ping $TARGET4 -c 2 -t 8 -n"
    C_PING6="SYS:ping $TARGET6 -c 3 -t 15 -n"
    C_TCP6="SYS:nc $TARGET6 $TARGET6_PORT -z -v -w 15 -6"
    C_TRACE="SYS:traceroute $TARGET6 -m 6 -q 1 -w 2 -n"
    C_ONLINK="SYS:ping $ONLINK6 -c 3 -t 10 -n"
    C_ROUTES="SYS:netstat -r"
    {
        echo "$C_IFUP"
        echo "wait $SETTLE"
        echo "$C_STATUS"
        echo "$C_PING4"
        echo "$C_PING6"
        echo "$C_TCP6"
        echo "$C_TRACE"
        [ -z "$ONLINK6" ] || echo "$C_ONLINK"
        echo "$C_ROUTES"
    } > "$STAGE/commands.txt"

    env AMINETXDUO_RUN_TAG="$tag" \
        AMINETXDUO_AMIBERRY_MAC="02:41:4d:4b:$RUNBYTE:${mac##*:}" \
        "$ROOT/tools/amiberry-run.sh" -N "$board" -B "$IFACE" -m "$model" \
            -t "$TIMEOUT" \
            "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
            "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/ShowNetStatus" \
            "$STAGE/ping" "$STAGE/nc" "$STAGE/traceroute" "$STAGE/netstat" \
        > "$LOGDIR/$board.log" 2>&1 < /dev/null
    rc=$?

    wall=$(( $(date +%s) - t0 ))
    tail -20 "$LOGDIR/$board.log"

    REPORT="$ROOT/build/amiberry-testhd-$tag/tools.txt"

    iface_rc=""; global6=""; v4=0; p6=0; tcp6=no; hops=0; onlink=skip
    if [ -f "$REPORT" ]; then
        cp "$REPORT" "$LOGDIR/$board.tools.txt"
        iface_rc=$(rc_of "$C_IFUP")
        global6=$(guest_global6)
        v4=$(replies_of "$C_PING4"); v4=${v4:-0}
        p6=$(replies_of "$C_PING6"); p6=${p6:-0}
        [ "$(rc_of "$C_TCP6")" = 0 ] && block "$C_TCP6" | grep -q ' open' &&
            tcp6=yes
        hops=$(tr_hops "$C_TRACE")
        if [ -n "$ONLINK6" ]; then
            o=$(replies_of "$C_ONLINK")
            [ "${o:-0}" -gt 0 ] && onlink=yes || onlink=no
        fi
    fi

    # THE CLAIM: a global address off the wire, and an answer from a machine
    # past the router.  Either proof of the second is enough -- an echo reply
    # or a completed TCP handshake -- because both need the identical return
    # path and the preflight showed this host getting both.  Requiring both
    # would put a destination's ICMP rate limiting in the gate.
    offlan=no
    [ "${p6:-0}" -gt 0 ] && offlan=yes
    [ "$tcp6" = yes ] && offlan=yes

    # A guest that produced nothing is a FAILURE, not a skip.  run-cardsweep.sh
    # has always scored both of these as fail; this scored them as skip, so a
    # card whose guest hung every night was green.  A timeout voids the
    # artefact it was supposed to produce, which is the same thing as having no
    # artefact, so the two share a status.
    status=fail; why=""
    if [ ! -f "$REPORT" ]; then
        status=fail_no_transcript
        why=" reason=\"the guest wrote no transcript at all (rc=$rc); read the log\""
    elif [ "$rc" = 124 ]; then
        status=fail_hang
        why=" reason=\"the guest never finished; ${TIMEOUT}s timeout\""
    elif [ "${iface_rc:-1}" != 0 ]; then
        status=skip_offline
        why=" reason=\"the interface did not come online, so IPv6 was never measured -- that claim is run-cardsweep.sh's\""
    elif [ -z "$global6" ]; then
        status=fail_no_global
        why=" reason=\"online, and no global IPv6 address formed: no router advertisement reached this card\""
    elif [ "$offlan" = yes ]; then
        status=pass
    elif [ "$onlink" = yes ]; then
        status=fail_offlan
        why=" reason=\"IPv6 works on this segment and reaches nothing past the router -- the answer is dropped one hop away\""
    else
        status=fail_offlan
        why=" reason=\"a global address formed and no IPv6 came back from anywhere\""
    fi

    # skip_offline is the one status here that is a skip on purpose: the claim
    # "this card comes online" belongs to run-cardsweep.sh, whose exit 1 is
    # exactly that, so counting it twice would put one defect in two gates.
    # skip_no_driver above is the other: nothing to boot.
    case "$status" in
        pass)                    NPASS=$((NPASS + 1)) ;;
        skip_offline)            NSKIP=$((NSKIP + 1)) ;;
        *)                       NFAIL=$((NFAIL + 1)) ;;
    esac

    printf 'card=%s model=%s driver=%s status=%s rc=%s iface_rc=%s global6=%s ping6_offlan=%s tcp6_offlan=%s trace6_hops=%s ping6_onlink=%s ping4_offlan=%s wall_s=%s log=%s%s\n' \
           "$board" "$model" "$drv" "$status" "$rc" "${iface_rc:-none}" \
           "${global6:-none}" "$p6" "$tcp6" "$hops" "$onlink" "$v4" \
           "$wall" "$LOGDIR/$board.log" "$why" | tee -a "$RESULTS"
done 3<<EOF
$(cards_rows "$ONLY")
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
printf 'cardsweep6: cards=%d pass=%d fail=%d skip=%d target6=%s wall_s=%d\n' \
       "$((NPASS + NFAIL + NSKIP))" "$NPASS" "$NFAIL" "$NSKIP" "$TARGET6" "$WALL"

# 0  every card formed a global address and reached past the router
# 1  A CARD DID NOT, or a card's guest produced nothing at all.  The gate.
# 2  the lab could not run it, and no card was measured
# 3  nothing failed, and a card was not measured -- read the table.  Only two
#    things reach it: no driver in the store, and an interface that never came
#    online, which is run-cardsweep.sh's exit 1 and not a second gate here.
if [ "$NFAIL" != 0 ]; then echo "result=fail"; exit 1; fi
if [ "$NSKIP" != 0 ]; then echo "result=partial"; exit 3; fi
echo "result=ok"
exit 0
