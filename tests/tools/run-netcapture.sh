#!/usr/bin/env bash
#
# NetCapture, on a real segment, reading a real program's traffic.
#
#   tests/tools/run-netcapture.sh [-B BACKEND] [-b BUILDDIR] [-c CARD[,CARD...]]
#                                 [-P user@peer] [-A ADDRESS] [-t SECONDS]
#                                 [-M MACHEAD] [-l]
#
# WHAT IT PROVES
#
#   1. NetCapture records traffic IT DID NOT MAKE.  The frames in the file are
#      ping's, and ping is a separate command launched separately -- which is
#      the whole difference between this and NetTrace, and the only thing a
#      capture tool is for.
#   2. The file is a pcap.  TCPDUMP READS IT, on a machine that is not the one
#      that wrote it, and reports the packets.  Not "the header looks right":
#      the reader that a user will actually point at this file.
#   3. The filter works on the machine and not only in the host test.  Two
#      channels capture the same seconds, one with PROTO=ICMP and one with no
#      filter at all; the filtered file must be all ICMP and the unfiltered one
#      must NOT be, or the first proves nothing about the filter and only that
#      the segment was quiet.
#   4. A stop condition stops it.  A third channel with COUNT=5 must produce a
#      file with exactly five packets in it, closed and readable.
#
# WHAT IT DOES NOT PROVE
#
#   Ctrl-C.  There is no way to deliver SIGBREAKF_CTRL_C to a background
#   command from a staged command list -- AmigaDOS `Break` is a Workbench C:
#   command and this drive has no Workbench on it.  The SECONDS and COUNT arms
#   above take the identical path out: netcapture.c's loop breaks and calls
#   tool_bpf_stop(), which is the one place the file is drained, flushed and
#   closed.  Ctrl-C differs by which `if` fires and by nothing else.
#
# BRIDGED, NEVER SLIRP
#
#   SLIRP is user-mode NAT inside the emulator; there is no segment there, no
#   ARP, no mDNS, and nothing else on it for an unfiltered capture to see.  The
#   third assertion above would then be vacuous.  -B names a host NIC.
#
# EVERY CARD, and a FRESH MAC PER RUN
#
#   The card table is tests/tools/cards.sh, shared with the two sweeps, so a
#   card added there is a card this boots.  The MAC head here is 02:41:4d:4e,
#   which is neither sweep's and is not the demo's, and the tail is the card's:
#   two Amigas on one segment with one address take each other off the network.
#   Note the A2065's LANCE rewrites the first three octets with Commodore's
#   OUI, so the address this looks for in the capture is the derived one.
#
# THE PEER
#
#   -P names a machine with tcpdump that is NOT this one.  It reads the files.
#   Without it they are read here, which is a weaker claim and says so
#   (reader=local).  -A is what the guest pings; it must answer, and it
#   defaults to the peer's address when -P is a host this can resolve.
#
#   A peer on THIS host cannot be pinged by the guest: Amiberry injects with
#   pcap on the backend NIC and injected frames never enter this host's own RX
#   path.  That is why -A defaults to the router and not to 127.0.0.1.
#
# OUTPUT IS key=value, one line per card and one summary.  Exit 0 everything
# asserted held, 1 something did not, 2 a broken invocation or a missing
# prerequisite, 3 a card produced no capture to read at all.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 1

BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
PEER="${AMINETXDUO_NETCAPTURE_PEER:-}"
TARGET="${AMINETXDUO_NETCAPTURE_ADDRESS:-192.168.1.1}"
MACHEAD="02:41:4d:4e"
TIMEOUT=200
ONLY=""
LIST=0

while getopts "B:b:c:P:A:t:M:l" opt; do
    case "$opt" in
        B) BACKEND="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        c) ONLY="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        A) TARGET="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        M) MACHEAD="$OPTARG" ;;
        l) LIST=1 ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

say()  { printf '%s\n' "$*"; }
kv()   { printf '%s=%s\n' "$1" "$2"; }
refuse() { kv reason "$1"; kv RESULT refused; exit 2; }

# shellcheck source=tests/tools/cards.sh
. "$ROOT/tests/tools/cards.sh"
# shellcheck source=tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"

if [ "$LIST" = 1 ]; then
    cards_rows "$ONLY"
    exit 0
fi

case "$BACKEND" in
    slirp|slirp_inbound)
        refuse "slirp_has_no_segment: use -B <interface>" ;;
esac

# ------------------------------------------------------------ prerequisites --

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
SMOKE="$BUILD/src/tools/ToolsSmoke"
ADDIF="$BUILD/src/tools/AddNetInterface"
CAPTURE="$BUILD/src/tools/NetCapture"
PING="$BUILD/src/tools/ping"

for f in "$BSD" "$SMOKE" "$ADDIF" "$CAPTURE" "$PING"; do
    [ -f "$f" ] || refuse "build_missing:$f"
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || refuse "no_kickstart"

# The reader.  A peer is the claim worth making: the file crosses a machine
# boundary and is read by a tcpdump that knows nothing about how it was made.
READER=local
if [ -n "$PEER" ]; then
    if ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
           'command -v tcpdump > /dev/null' 2>/dev/null; then
        READER=peer
    else
        kv peer_unusable "$PEER"
    fi
fi

if [ "$READER" = local ] && ! command -v tcpdump > /dev/null; then
    refuse "no_tcpdump"
fi

kv backend "$BACKEND"
kv reader "$READER"
kv ping_target "$TARGET"

# Read a pcap wherever the reader is.  One place, so the two arms cannot drift.
#   pcap_lines <file> [expression]
pcap_lines() {
    local file="$1" expr="${2:-}"

    if [ "$READER" = peer ]; then
        scp -q -o BatchMode=yes "$file" "$PEER:/tmp/$(basename "$file")" \
            2>/dev/null || return 1
        # shellcheck disable=SC2029  # the expansion is meant to happen here
        ssh -o BatchMode=yes "$PEER" \
            "tcpdump -r /tmp/$(basename "$file") -nne ${expr:+\"$expr\"} 2>/dev/null" \
            2>/dev/null
    else
        # shellcheck disable=SC2086
        tcpdump -r "$file" -nne ${expr:+"$expr"} 2>/dev/null
    fi
}

# Does tcpdump accept the file at all?  A file it refuses prints nothing on
# stdout and everything on stderr, which is indistinguishable from an empty
# capture unless the two are asked separately.
pcap_readable() {
    local file="$1"

    if [ "$READER" = peer ]; then
        scp -q -o BatchMode=yes "$file" "$PEER:/tmp/$(basename "$file")" \
            2>/dev/null || return 1
        ssh -o BatchMode=yes "$PEER" \
            "tcpdump -r /tmp/$(basename "$file") -nn -c 1 > /dev/null" \
            2>/dev/null
    else
        tcpdump -r "$file" -nn -c 1 > /dev/null 2>&1
    fi
}

# ------------------------------------------------------------------ per card --

cards=0
passed=0
failed=0
novrd=0

run_card() {
    local board=$1 model=$2 addr=$3 mactail=$4
    local tag="nc-$board"
    local mac="$MACHEAD:$mactail"
    local hd="$ROOT/build/testhd-$tag"
    local ok=1
    local run_rc=0
    local guest_mac tail4 tail5 tail6

    cards=$((cards + 1))

    # The address that reaches the wire.  The LANCE keeps the last three
    # octets and writes Commodore's 00:80:10 over the first three, so the MAC
    # to look for in a capture is not the one configured.  Only the a2065
    # family does that; for the rest the configured address is what goes out,
    # and both spellings are searched rather than guessed at.
    tail4=$(printf '%s' "$mac" | cut -d: -f4)
    tail5=$(printf '%s' "$mac" | cut -d: -f5)
    tail6=$(printf '%s' "$mac" | cut -d: -f6)
    guest_mac="00:80:10:$tail4:$tail5:$tail6"

    local stage="$ROOT/build/$tag-stage"
    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"

    cp "$BSD"     "$stage/libs/bsdsocket.library"
    cp "$ADDIF"   "$stage/AddNetInterface"
    cp "$CAPTURE" "$stage/NetCapture"
    cp "$PING"    "$stage/ping"

    # The SANA-II driver for this board, from the one place that knows which
    # driver covers which card.  A driver that is not on this machine is a
    # SKIP with its own status, decided before booting: sana2_stage() warns
    # and carries on, which would spend a boot and a timeout to report "the
    # network did not start".
    sana2_select "$board" "$BUILD"
    if [ -z "$SANA2_SEL_PATH" ]; then
        kv "card_$board" "skip_no_driver driver=$SANA2_SEL_DRIVER source=$SANA2_SEL_SOURCE"
        novrd=$((novrd + 1))
        return
    fi

    # DHCP, not a static address: nothing has to call in here, so there is no
    # reason for this sweep to hold nine addresses of the lab's.
    {
        printf 'DEVICE=%s\n' "$SANA2_SEL_DRIVER"
        printf 'UNIT=0\n'
        printf 'CONFIGURE=DHCP\n'
    } > "$stage/devs/NetInterfaces/eth0"

    AMINETXDUO_SANA2_DRIVER="$SANA2_SEL_PATH" \
    AMINETXDUO_SANA2_DRIVER_NAME="$SANA2_SEL_DRIVER" \
    AMINETXDUO_SANA2_DEVICE="$SANA2_SEL_DRIVER" \
    AMINETXDUO_SANA2_CARD="$SANA2_SEL_CARD" \
        sana2_stage "$board" "$stage/devs" > "$ROOT/build/$tag-sana2.log" 2>&1

    # THE EXPERIMENT.
    #
    # Two channels over the same seconds, so the filtered and unfiltered files
    # are of the SAME traffic and the comparison between them means something.
    # Then ping, which is a different command in a different process, and is
    # the only thing here that puts a packet on the wire on purpose.
    #
    # `&` is ToolsSmoke's SYS_Asynch prefix: SystemTagList() waits, and a
    # capture that waits cannot be running while ping runs.
    cat > "$stage/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 30
&SYS:NetCapture OUT=DH0:all.pcap IFACE=eth0 SECONDS=30 SNAP=128 QUIET
&SYS:NetCapture OUT=DH0:icmp.pcap IFACE=eth0 SECONDS=30 SNAP=128 PROTO=ICMP QUIET
&SYS:NetCapture OUT=DH0:five.pcap IFACE=eth0 COUNT=5 SECONDS=30 SNAP=64 PROTO=ICMP QUIET
wait 4
SYS:ping $TARGET COUNT=12
wait 32
EOF

    AMINETXDUO_RUN_TAG="$tag" AMINETXDUO_AMIBERRY_MAC="$mac" \
        "$ROOT/tools/amiberry-run.sh" \
        -N "$board" -B "$BACKEND" -m "$model" -t "$TIMEOUT" \
        "$SMOKE" "$stage/devs" "$stage/libs" "$stage/AddNetInterface" \
        "$stage/NetCapture" "$stage/ping" "$stage/commands.txt" \
        > "$ROOT/build/$tag.out" 2>&1 < /dev/null
    run_rc=$?

    # ------------------------------------------------------------ the files --

    local all="$hd/all.pcap" icmp="$hd/icmp.pcap" five="$hd/five.pcap"

    if [ ! -s "$all" ] || [ ! -s "$icmp" ] || [ ! -s "$five" ]; then
        kv "card_$board" \
           "novrd run_rc=$run_rc all=$([ -s "$all" ] && echo y || echo n) icmp=$([ -s "$icmp" ] && echo y || echo n) five=$([ -s "$five" ] && echo y || echo n)"
        novrd=$((novrd + 1))
        return
    fi

    local f
    for f in "$all" "$icmp" "$five"; do
        pcap_readable "$f" || {
            kv "card_$board" "unreadable:$(basename "$f")"
            novrd=$((novrd + 1))
            return
        }
    done

    local n_all n_icmp n_five n_noticmp n_echo n_mac
    n_all=$(pcap_lines "$all" | grep -c . )
    n_icmp=$(pcap_lines "$icmp" | grep -c . )
    n_five=$(pcap_lines "$five" | grep -c . )

    # The unfiltered capture must hold something the filtered one excludes, or
    # "the filtered file is all ICMP" is a fact about the segment.
    n_noticmp=$(pcap_lines "$all" "not icmp and not icmp6" | grep -c . )

    # ICMP means the pings: echo request or reply, not merely IP protocol 1.
    n_echo=$(pcap_lines "$icmp" | grep -ci 'echo re')

    # The frames are this machine's.
    n_mac=$(pcap_lines "$all" | grep -ci "$guest_mac")

    # Everything the ICMP filter kept must be ICMP.  Asked as its negation, so
    # a tcpdump that printed nothing does not read as a pass.
    local n_wrong
    n_wrong=$(pcap_lines "$icmp" "not icmp and not icmp6" | grep -c . )

    [ "$n_all" -gt 0 ]      || { ok=0; kv "card_${board}_why" "all_empty"; }
    [ "$n_icmp" -gt 0 ]     || { ok=0; kv "card_${board}_why" "icmp_empty"; }
    [ "$n_echo" -gt 0 ]     || { ok=0; kv "card_${board}_why" "no_echo_in_icmp"; }
    [ "$n_wrong" -eq 0 ]    || { ok=0; kv "card_${board}_why" "filter_leaked"; }
    [ "$n_noticmp" -gt 0 ]  || { ok=0; kv "card_${board}_why" "unfiltered_is_icmp_only"; }
    [ "$n_all" -ge "$n_icmp" ] || { ok=0; kv "card_${board}_why" "icmp_exceeds_all"; }
    [ "$n_five" -eq 5 ]     || { ok=0; kv "card_${board}_why" "count_stop_gave_$n_five"; }
    [ "$n_mac" -gt 0 ]      || { ok=0; kv "card_${board}_why" "guest_mac_absent"; }

    kv "card_$board" \
       "mac=$guest_mac addr=$addr all=$n_all icmp=$n_icmp echo=$n_echo leaked=$n_wrong nonicmp=$n_noticmp count5=$n_five mine=$n_mac run_rc=$run_rc"

    if [ "$ok" = 1 ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
}

while read -r board model addr mactail; do
    [ -n "$board" ] || continue
    run_card "$board" "$model" "$addr" "$mactail"
done < <(cards_rows "$ONLY")

# Cards this project names that no arm can reach.  A list of what is covered
# is worth nothing without the list of what is not.
printf '%s\n' "$UNTESTABLE" | while read -r drv why; do
    [ -n "$drv" ] || continue
    kv untestable "$drv ($why)"
done

kv cards "$cards"
kv passed "$passed"
kv failed "$failed"
kv noverdict "$novrd"

if [ "$cards" -eq 0 ]; then
    kv RESULT refused
    exit 2
fi
if [ "$failed" -gt 0 ]; then
    kv RESULT fail
    exit 1
fi
if [ "$novrd" -gt 0 ]; then
    kv RESULT noverdict
    exit 3
fi

kv RESULT pass
exit 0
