#!/usr/bin/env bash
#
# WHAT THE MACHINE PUTS ON THE WIRE WHEN NOBODY ASKED IT TO.
#
#   tests/tools/run-wirequiet.sh [-B BACKEND] [-b BUILDDIR] [-c CARD[,CARD...]]
#                                [-s SETTLE] [-n WINDOW] [-x MAX] [-A ADDRESS]
#                                [-t SECONDS] [-l]
#
# WHY IT EXISTS
#
#   A DHCPv6 client bound its lease and then rebound it every 120 ms for the
#   rest of the run: 2544 Rebinds in 305 s of guest uptime, each with a fresh
#   transaction id, and the Renew it was supposed to send never reached the
#   wire once.  Every harness in this tree passed for the whole time that was
#   true, because not one of them looks at what the machine EMITS.  They read
#   the guest's own transcript, and a guest shouting at a router says nothing
#   about it in its transcript.  It was found by a person running tcpdump by
#   hand while chasing something else.
#
#   So: boot, let it settle, and then assert that the link is QUIET.
#
# THE BAR, AND WHERE THE NUMBERS COME FROM
#
#   An idle AmigaOS machine on a real segment is not silent and must not be
#   asserted to be.  It answers ARP for its own address, it re-solicits a
#   router advertisement, the mDNS responder answers and re-announces, and a
#   lease is renewed eventually.  What is asserted is a RATE CEILING over a
#   window that starts after the machine has stopped starting up.
#
#   Measured on this build, on the lab segment, all nine cards:
#
#     the fixed build     0 packets in a 90 s window.  Every card.  The whole
#                         run is 25 to 27 frames and every one of them is
#                         bring-up or one of the two pings this harness asks
#                         for.
#     the pre-fix build   2343 packets in the same window on the same card,
#                         26.0 a second, 2340 of them DHCPv6 Rebind
#                         (third_party/netxduo at a43f57d0^)
#
#   SETTLE is 45 s because that is where bring-up stops, timed off the
#   captures rather than guessed: DHCPv4, duplicate address detection, the
#   multicast listener reports, the router solicitation and the DHCPv6
#   Solicit/Request were all on the wire within 11.3 s of the guest's first
#   frame on the slowest card measured.  45 s is four times that, and the
#   marker it counts from is later still -- the first netstat -i, which runs
#   after AddNetInterface and after a ping.
#
#   WINDOW is 90 s: long enough that anything periodic has to show (the defect
#   this is against had a period of 120 ms, so it puts 750 frames in the
#   shortest window worth having), and short enough that nine cards fit in
#   half an hour.  Nine boots at this size took 26 minutes.
#
#   MAX is 24.  It is NOT a fit to observed noise -- the observation is zero,
#   and a ceiling of one would pass every card measured here.  It is set for
#   what an idle machine on a SHARED segment may legitimately have to do:
#   24 packets in 90 s lets a neighbour ARP the guest every four seconds for
#   the whole window and still pass, and no card has ever come near it.  A
#   storm is two orders of magnitude the other side of it.
#
# WHY IT CANNOT PASS BY BEING DEAD
#
#   A guest that crashed, or an interface that fell over, is also quiet.  So
#   the window is bracketed: a ping BEFORE it, which is what teaches this
#   script the guest's hardware address, and a ping AFTER it, which has to be
#   answered.  A card that is silent in the window and cannot ping when it
#   closes is a failure, not a pass.  The guest's own netstat -i counters are
#   read either side of the window as a second, independent witness and are
#   reported beside the wire count; they are not the gate, because a counter
#   the machine keeps about itself is exactly what a machine with a broken
#   idea of its own state gets wrong.
#
# HOW THE GUEST IS IDENTIFIED
#
#   By the source MAC IN THE CAPTURE, learned from the ping and cross-checked
#   against what netstat -i says the driver holds.  It is not derived, and the
#   nine cards say why -- what each one did to 02:41:4d:49:xx:yy:
#
#     a2065          00:80:10:49:xx:yy   Commodore's OUI over the first three
#     ariadne        00:60:30:49:xx:yy   Village Tronic's
#     ne2000_pcmcia  3e:24:11:93:e8:8b   NOT OURS AT ALL: the host NIC's
#     the other six  unchanged
#
#   A harness that computes the address it expects counts zero frames on the
#   families it computed wrong and reports a quiet link, which is the one
#   failure mode this file cannot have.  The first ping's frames carry the
#   leased IPv4 address that AddNetInterface printed, so their source MAC is
#   the answer, and the address the run asked for is printed beside the one it
#   got.
#
# TCPDUMP IS UNPRIVILEGED HERE
#
#   It reads the emulator host's own NIC, which is where Amiberry injects the
#   guest's frames.  No sudo and no setcap: if `tcpdump -i <iface> -c 1`
#   cannot run as this user, this script refuses rather than running blind.
#
# BRIDGED, NEVER SLIRP.  There is no segment inside SLIRP's NAT, no DHCPv6
# server for a client to shout at, and nothing that would have carried this
# defect.  -B names a host NIC.
#
# OUTPUT IS key=value, one line per card and one summary.  Exit 0 every card
# was quiet, 1 one was not, 2 a broken invocation or a lab that cannot run
# this, 3 a card produced no capture or no transcript to read.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 1

IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
ONLY=""
SETTLE=45
WINDOW=90
MAX=24
TARGET=""
TIMEOUT=0
LIST=0

while getopts "B:b:c:s:n:x:A:t:l" opt; do
    case "$opt" in
        B) IFACE="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        c) ONLY="$OPTARG" ;;
        s) SETTLE="$OPTARG" ;;
        n) WINDOW="$OPTARG" ;;
        x) MAX="$OPTARG" ;;
        A) TARGET="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        l) LIST=1 ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac
TOOLS="$BUILDDIR/src/tools"
BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"

# The guest has to sit still for the whole of settle plus window, and then be
# pinged afterwards, so the ceiling is derived rather than guessed.  A run
# whose timeout expires proves nothing about quiet: it voids the artefact.
GUEST_WAIT=$((SETTLE + WINDOW + 20))
[ "$TIMEOUT" -gt 0 ] || TIMEOUT=$((240 + SETTLE + WINDOW))

kv()   { printf '%s=%s\n' "$1" "$2"; }
infra() { kv reason "$1"; kv RESULT refused; exit 2; }

# shellcheck source=tests/tools/cards.sh
. "$ROOT/tests/tools/cards.sh"
# shellcheck source=tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"
# shellcheck source=tools/emu-mac.sh
. "$ROOT/tools/emu-mac.sh"

if [ "$LIST" = 1 ]; then
    cards_rows "$ONLY"
    exit 0
fi

case "$IFACE" in
    slirp|slirp_inbound)
        infra "slirp_has_no_segment: -B <interface>, this measures a wire" ;;
esac

# --------------------------------------------------------------- preflight --

command -v tcpdump >/dev/null 2>&1 ||
    infra "no tcpdump on this host, so nothing here can see the wire"
command -v ip >/dev/null 2>&1 || infra "no ip(8) on this host"
command -v flock >/dev/null 2>&1 || infra "no flock(1) on this host"

timeout 10 tcpdump -i "$IFACE" -c 1 -n >/dev/null 2>&1 || {
    rc=$?
    [ "$rc" = 124 ] ||
        infra "tcpdump cannot read $IFACE as $(id -un); this needs no sudo on a\
 host where it is set up, and running without it would assert nothing"
}

for t in ToolsSmoke AddNetInterface ping netstat; do
    [ -x "$TOOLS/$t" ] || infra "no $TOOLS/$t; build $BUILD first"
done
[ -f "$BSD" ] || infra "no $BSD; build $BUILD first"

[ -n "${AMINETXDUO_KICKSTART:-}${AMINETXDUO_KICKSTART_A1200:-}" ] ||
    infra "no boot ROM; export AMINETXDUO_KICKSTART (. ~/amiga-assets/env.sh)"

[ -n "$(ip -o -4 addr show dev "$IFACE" 2>/dev/null)" ] ||
    infra "no IPv4 address on $IFACE; -B names the host NIC the guest bridges onto"

if [ -z "$TARGET" ]; then
    TARGET=$(ip -o -4 route show default 2>/dev/null | awk '{ print $3; exit }')
fi
[ -n "$TARGET" ] ||
    infra "no default gateway to ping; -A names something on this segment that\
 answers ICMP"
ping -c 1 -W 3 "$TARGET" >/dev/null 2>&1 ||
    infra "$TARGET does not answer this host, so a guest that cannot ping it\
 says nothing"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || infra "no a2065.device; set AMINETXDUO_A2065"

[ "$(cards_rows "$ONLY" | grep -c .)" -gt 0 ] ||
    infra "-c $ONLY matched no card in tests/tools/cards.sh"

mkdir -p "$ROOT/build"
LOCK="$ROOT/build/wirequiet.lock"
exec 9>"$LOCK"
flock -n 9 || infra "another run-wirequiet holds $LOCK"

# ----------------------------------------------------------------- reading --

REPORT=""

rc_of() { # banner -- the return code of the first occurrence
    awk -v want="===== $1 =====" '
        $0 == want && !seen { on = 1; seen = 1; next }
        on && /^----- rc / { print; exit }
    ' "$REPORT" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'
}

last_rc_of() { # banner -- the return code of the LAST occurrence
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- rc / { line = $0; on = 0 }
        END { print line }
    ' "$REPORT" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'
}

# Echo replies out of the guest's own summary line, from the LAST ping block.
replies_last() {
    awk '/^[0-9]+ packets transmitted, [0-9]+ received/ { print $4 }' \
        "$REPORT" | tail -1
}
replies_first() {
    awk '/^[0-9]+ packets transmitted, [0-9]+ received/ { print $4; exit }' \
        "$REPORT"
}

# The leased address, off AddNetInterface's own line
# ("eth0: online, address 192.168.1.181 and fe80::...").
guest_v4() {
    sed -n 's/^.*online, address \([0-9][0-9.]*\).*$/\1/p' "$REPORT" | head -1
}

# The hardware address the DRIVER reports, printed by netstat -i under the
# interface row.  This is the authoritative answer and it costs nothing: it is
# what the card will put in the source field, after whatever mangling that
# family does to the address the emulator was configured with.
guest_mac_reported() {
    sed -n 's/^ *hardware \([0-9a-fA-F:]\{17\}\).*$/\1/p' "$REPORT" |
        head -1 | tr 'A-F' 'a-f'
}

# Opkts for the first attached interface, out of the nth netstat -i.  The
# columns are Name Mtu Address Link Ipkts Ierrs Opkts Oerrs (netstat.c:86).
opkts_nth() { # n
    awk -v n="$1" '
        $1 == "Name" && $2 == "Mtu" { hdr++; want = 1; next }
        want && hdr == n { print $7; want = 0 }
    ' "$REPORT" | head -1
}

# ------------------------------------------------------------------ the run --

RESULTS="$ROOT/build/wirequiet-results.txt"
: > "$RESULTS"
LOGDIR="$ROOT/build/wirequiet-logs"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

NPASS=0; NFAIL=0; NSKIP=0; NBROKE=0

echo "==> bridge $IFACE, build $BUILD, ping target $TARGET"
echo "==> settle ${SETTLE}s, window ${WINDOW}s, ceiling $MAX packets," \
     "${TIMEOUT}s per card"

while read -r -u 3 board model _addr _mac; do
    [ -n "$board" ] || continue

    sana2_select "$board" "$BUILDDIR"
    drv=$SANA2_SEL_DRIVER
    drvpath=$SANA2_SEL_PATH
    anxcard=$SANA2_SEL_CARD

    if [ -z "$drvpath" ]; then
        printf 'card=%s status=skip_no_driver reason="no %s in the driver store"\n' \
               "$board" "$drv" | tee -a "$RESULTS"
        NSKIP=$((NSKIP + 1))
        continue
    fi

    tag="wirequiet-$board"
    WANTMAC=$(emu_mac_for_tag "$tag")

    echo
    echo "===================== $board ($drv, $model) ====================="

    STAGE="$ROOT/build/wirequiet-stage-$board"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$A2065" "$STAGE/devs/a2065.device"
    cp "$BSD" "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface ping netstat; do cp "$TOOLS/$t" "$STAGE/$t"; done

    # DHCP for IPv4 and AUTO for IPv6, which is what a user gets and what the
    # defect this file is against needed: the lease that spun was a DHCPv6
    # one, handed out by the lab router in answer to an address the guest
    # configured itself from a router advertisement.  A static configuration
    # would have been quiet with the defect still in it.
    cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
STATE=up
IFEOF
    printf 'hostname anxwq-%s\n' "$board" > "$STAGE/devs/Internet/name_resolution.new"
    grep -v '^hostname' "$STAGE/devs/Internet/name_resolution" \
        >> "$STAGE/devs/Internet/name_resolution.new" 2>/dev/null
    mv "$STAGE/devs/Internet/name_resolution.new" \
       "$STAGE/devs/Internet/name_resolution"

    export AMINETXDUO_SANA2_DRIVER="$drvpath"
    export AMINETXDUO_SANA2_DRIVER_NAME="$drv"
    export AMINETXDUO_SANA2_DEVICE="$drv"
    export AMINETXDUO_SANA2_CARD="$anxcard"
    sana2_stage "$board" "$STAGE/devs"

    C_IFUP="SYS:AddNetInterface eth0"
    C_PING="SYS:ping $TARGET -c 3 -t 10 -n"
    C_NETSTAT="SYS:netstat -i"
    {
        echo "$C_IFUP"
        echo "$C_PING"
        echo "$C_NETSTAT"
        echo "wait $GUEST_WAIT"
        echo "$C_NETSTAT"
        echo "$C_PING"
    } > "$STAGE/commands.txt"

    PCAP="$LOGDIR/$board.pcap"
    HD="$ROOT/build/amiberry-testhd-$tag"
    REPORT="$HD/tools.txt"
    rm -rf "$HD"

    # The capture starts BEFORE the machine does, so the bring-up that the
    # window deliberately excludes is still on record and can be read when a
    # card goes red.  `not tcp port 22` keeps this host's own ssh sessions --
    # which is how the tree gets here -- out of a file that is otherwise
    # written for minutes.  It cannot hide anything the guest sends: nothing
    # on this machine has an ssh server the guest knows about, and a guest
    # that did open port 22 would be a finding in its own right rather than a
    # quiet link.
    tcpdump -i "$IFACE" -n -e -s 128 -U -w "$PCAP" 'not tcp port 22' \
        > "$LOGDIR/$board.tcpdump.log" 2>&1 &
    TCPDUMP_PID=$!
    capture_stop() {
        kill "$TCPDUMP_PID" 2>/dev/null
        wait "$TCPDUMP_PID" 2>/dev/null
    }
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        grep -q 'listening on' "$LOGDIR/$board.tcpdump.log" 2>/dev/null && break
        sleep 0.5
    done
    if ! grep -q 'listening on' "$LOGDIR/$board.tcpdump.log" 2>/dev/null; then
        capture_stop
        cat "$LOGDIR/$board.tcpdump.log" >&2
        printf 'card=%s status=fail_no_capture reason="tcpdump did not start"\n' \
               "$board" | tee -a "$RESULTS"
        NBROKE=$((NBROKE + 1))
        continue
    fi

    env AMINETXDUO_RUN_TAG="$tag" \
        "$ROOT/tools/amiberry-run.sh" -N "$board" -B "$IFACE" -m "$model" \
            -t "$TIMEOUT" \
            "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
            "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/ping" \
            "$STAGE/netstat" \
        > "$LOGDIR/$board.log" 2>&1 < /dev/null &
    RUNPID=$!

    # The window opens on the guest's clock, not on this one.  A boot behind
    # another run on this host takes minutes to reach AddNetInterface, and a
    # window timed from here would then measure an empty segment and pass.
    # The first netstat -i is the marker: it runs after the interface is up
    # and after the first ping, so its return code appearing in the transcript
    # is the moment bring-up finished.
    T_UP=0
    while kill -0 "$RUNPID" 2>/dev/null; do
        if [ -s "$REPORT" ] && [ -n "$(rc_of "$C_NETSTAT")" ]; then
            T_UP=$(date +%s)
            break
        fi
        sleep 1
    done

    if [ "$T_UP" = 0 ]; then
        wait "$RUNPID"; rc=$?
        capture_stop
        tail -20 "$LOGDIR/$board.log"
        printf 'card=%s status=fail_no_bringup run_rc=%s reason="the guest never finished bringing the interface up"\n' \
               "$board" "$rc" | tee -a "$RESULTS"
        NBROKE=$((NBROKE + 1))
        continue
    fi

    QSTART=$((T_UP + SETTLE))
    QEND=$((QSTART + WINDOW))
    echo "==> bring-up done, quiet window $SETTLE..$((SETTLE + WINDOW))s from now"

    while [ "$(date +%s)" -lt $((QEND + 2)) ]; do
        kill -0 "$RUNPID" 2>/dev/null || break
        sleep 2
    done

    wait "$RUNPID"; rc=$?
    sleep 1
    capture_stop

    tail -15 "$LOGDIR/$board.log"
    [ -f "$REPORT" ] && cp "$REPORT" "$LOGDIR/$board.tools.txt"

    # ------------------------------------------------------- what was read --

    if [ ! -s "$REPORT" ]; then
        printf 'card=%s status=fail_no_transcript run_rc=%s reason="the guest wrote nothing"\n' \
               "$board" "$rc" | tee -a "$RESULTS"
        NBROKE=$((NBROKE + 1))
        continue
    fi

    GUESTIP=$(guest_v4)
    PING_BEFORE=$(replies_first); PING_BEFORE=${PING_BEFORE:-0}
    PING_AFTER=$(replies_last);   PING_AFTER=${PING_AFTER:-0}
    IFUP_RC=$(rc_of "$C_IFUP")

    # The guest's own idea of what it sent, either side of the window.
    OP_BEFORE=$(opkts_nth 1)
    OP_AFTER=$(opkts_nth 2)

    # The hardware address, taken from the machine and then CONFIRMED on the
    # wire.  netstat -i prints what the driver holds; the first ping's echo
    # requests carry the leased address, and their source MAC is what the card
    # really put in the frame.  They agree on every card measured, and a run
    # where they do not is reported rather than resolved silently, because
    # counting on the wrong address is how this assertion would go quietly
    # vacuous.
    REPORTED_MAC=$(guest_mac_reported)
    WIRE_MAC=""
    if [ -n "$GUESTIP" ]; then
        WIRE_MAC=$(tcpdump -r "$PCAP" -n -e -c 1 "icmp and src host $GUESTIP" \
                       2>/dev/null | awk '{ print $2; exit }')
        [ -n "$WIRE_MAC" ] ||
            WIRE_MAC=$(tcpdump -r "$PCAP" -n -e -c 1 "src host $GUESTIP" \
                           2>/dev/null | awk '{ print $2; exit }')
    fi

    GUESTMAC="$WIRE_MAC"; MACSRC=wire
    if [ -z "$GUESTMAC" ]; then
        GUESTMAC="$REPORTED_MAC"; MACSRC=driver
    elif [ -n "$REPORTED_MAC" ] && [ "$REPORTED_MAC" != "$WIRE_MAC" ]; then
        MACSRC="wire_disagrees_with_driver:$REPORTED_MAC"
    fi

    if [ -z "$GUESTMAC" ]; then
        # Neither the machine nor the wire named an address.  Fall back to the
        # three bytes of the configured one that survive every card, and say
        # that is what happened: a count taken this way could in principle be
        # somebody else's machine, and a reader has to know which it was.
        FILTER=$(printf '%s' "$WANTMAC" |
                 awk -F: '{ printf "ether[9] = 0x%s and ether[10] = 0x%s and ether[11] = 0x%s", $4, $5, $6 }')
        MACSRC=suffix
    else
        FILTER="ether src $GUESTMAC"
    fi

    QUIET_LINES="$LOGDIR/$board.quiet.txt"
    tcpdump -r "$PCAP" -n -tt "$FILTER" 2>/dev/null |
        awk -v a="$QSTART" -v b="$QEND" '$1 >= a && $1 < b' > "$QUIET_LINES"
    PACKETS=$(grep -c . "$QUIET_LINES")

    # Everything the guest sent over the whole run, so a red line can be read
    # without going back to the pcap.
    tcpdump -r "$PCAP" -n -tt "$FILTER" 2>/dev/null > "$LOGDIR/$board.guest.txt"
    TOTAL=$(grep -c . "$LOGDIR/$board.guest.txt")

    # What it was, bucketed.  Reported for every card, red or green: "quiet"
    # with no breakdown is a number nobody can act on.
    TOP=$(awk '
        /DHCP6|dhcp6/            { c["dhcpv6"]++; next }
        /\.5353|5353 >|mdns/     { c["mdns"]++;   next }
        /ARP,|arp who-has|Reply/ { c["arp"]++;    next }
        /ICMP6|icmp6/            { c["icmpv6"]++; next }
        /ICMP echo|icmp/         { c["icmp"]++;   next }
        /\.67 >|\.68 >|BOOTP/    { c["dhcp"]++;   next }
                                 { c["other"]++ }
        END { for (k in c) printf "%s:%d ", k, c[k] }
    ' "$QUIET_LINES")
    TOP=${TOP:-none}

    # ---------------------------------------------------------- the verdict --

    status=pass; why=""
    if [ "$rc" = 124 ]; then
        status=fail_hang
        why="the guest never finished; ${TIMEOUT}s timeout, so the window is not a measurement"
    elif [ "${IFUP_RC:-1}" != 0 ]; then
        status=fail_no_interface
        why="AddNetInterface returned ${IFUP_RC:-nothing}"
    elif [ -z "$GUESTIP" ]; then
        status=fail_no_address
        why="the guest never printed an address, so nothing identifies its frames"
    elif [ -z "$(last_rc_of "$C_PING")" ] || [ "$PING_AFTER" -eq 0 ]; then
        # THE ANTI-VACUOUS CHECK.  A machine that crashed during the window is
        # perfectly quiet.  It has to still be on the wire when the window
        # closes for the count to have meant anything.
        status=fail_dead_after
        why="the guest answered no ping after the window: a quiet count from a machine that is off the network asserts nothing"
    elif [ "$PACKETS" -gt "$MAX" ]; then
        status=fail_noisy
        why="$PACKETS packets in ${WINDOW}s from a machine that was asked to do nothing; ceiling is $MAX ($TOP)"
    fi

    case "$status" in
        pass)  NPASS=$((NPASS + 1)) ;;
        fail_hang|fail_no_interface|fail_no_address) NBROKE=$((NBROKE + 1)) ;;
        *)     NFAIL=$((NFAIL + 1)) ;;
    esac

    printf 'card=%s driver=%s model=%s status=%s packets=%s ceiling=%s window_s=%s settle_s=%s rate_pps=%s breakdown="%s" run_total=%s guest_ip=%s guest_mac=%s mac_source=%s mac_asked=%s ping_before=%s ping_after=%s opkts_before=%s opkts_after=%s run_rc=%s%s\n' \
        "$board" "$drv" "$model" "$status" "$PACKETS" "$MAX" "$WINDOW" \
        "$SETTLE" \
        "$(awk -v p="$PACKETS" -v w="$WINDOW" 'BEGIN { printf "%.3f", p / w }')" \
        "$TOP" "$TOTAL" "${GUESTIP:-none}" "${GUESTMAC:-none}" "$MACSRC" \
        "$WANTMAC" "$PING_BEFORE" "$PING_AFTER" "${OP_BEFORE:-none}" \
        "${OP_AFTER:-none}" "$rc" \
        "${why:+ reason=\"$why\"}" | tee -a "$RESULTS"

    if [ "$status" != pass ]; then
        echo "---- the first 20 frames in the window ----"
        head -20 "$QUIET_LINES"
    fi
done 3< <(cards_rows "$ONLY")

echo
echo "================================ summary ================================"
cat "$RESULTS"
kv cards_pass "$NPASS"
kv cards_fail "$NFAIL"
kv cards_skip "$NSKIP"
kv cards_broken "$NBROKE"
kv window_s "$WINDOW"
kv settle_s "$SETTLE"
kv ceiling "$MAX"

if [ "$NBROKE" -gt 0 ]; then kv RESULT broken; exit 3; fi
if [ "$NFAIL" -gt 0 ];  then kv RESULT fail;   exit 1; fi
if [ "$NPASS" = 0 ];    then kv RESULT nothing_ran; exit 3; fi
kv RESULT pass
exit 0
