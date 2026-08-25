#!/usr/bin/env bash
#
# NetCapture, on a real segment, reading a real program's traffic.
#
#   tests/tools/run-netcapture.sh [-B BACKEND] [-b BUILDDIR] [-c CARD[,CARD...]]
#                                 [-P user@peer] [-A ADDRESS] [-t SECONDS]
#                                 [-M MACHEAD] [-l]
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

READER=local
if [ -n "$PEER" ]; then
    if ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
           'command -v tcpdump > /dev/null' 2>/dev/null < /dev/null; then
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

#   pcap_lines <file> [expression]
pcap_lines() {
    local file="$1" expr="${2:-}"

    if [ "$READER" = peer ]; then
        scp -q -o BatchMode=yes "$file" "$PEER:/tmp/$(basename "$file")" \
            2>/dev/null < /dev/null || return 1
        # shellcheck disable=SC2029  # the expansion is meant to happen here
        ssh -n -o BatchMode=yes "$PEER" \
            "tcpdump -r /tmp/$(basename "$file") -nne ${expr:+\"$expr\"} 2>/dev/null" \
            2>/dev/null < /dev/null
    else
        # shellcheck disable=SC2086
        tcpdump -r "$file" -nne ${expr:+"$expr"} 2>/dev/null
    fi
}

pcap_readable() {
    local file="$1"

    if [ "$READER" = peer ]; then
        scp -q -o BatchMode=yes "$file" "$PEER:/tmp/$(basename "$file")" \
            2>/dev/null < /dev/null || return 1
        ssh -n -o BatchMode=yes "$PEER" \
            "tcpdump -r /tmp/$(basename "$file") -nn -c 1 > /dev/null" \
            2>/dev/null < /dev/null
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
    local board=$1 model=$2 mactail=$4
    local tag="nc-$board"
    local mac="$MACHEAD:$mactail"
    local hd="$ROOT/build/amiberry-testhd-$tag"
    local ok=1
    local run_rc=0
    local guest_mac=none
    local guest_addr=""

    cards=$((cards + 1))

    local stage="$ROOT/build/$tag-stage"
    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"

    cp "$BSD"     "$stage/libs/bsdsocket.library"
    cp "$ADDIF"   "$stage/AddNetInterface"
    cp "$CAPTURE" "$stage/NetCapture"
    cp "$PING"    "$stage/ping"

    sana2_select "$board" "$BUILD"
    if [ -z "$SANA2_SEL_PATH" ]; then
        kv "card_$board" "skip_no_driver driver=$SANA2_SEL_DRIVER source=$SANA2_SEL_SOURCE"
        novrd=$((novrd + 1))
        return
    fi

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

    cat > "$stage/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 30
&SYS:NetCapture OUT=DH0:all.pcap IFACE=eth0 SECONDS=30 SNAP=128 QUIET >DH0:all.txt
&SYS:NetCapture OUT=DH0:icmp.pcap IFACE=eth0 SECONDS=30 SNAP=128 PROTO=ICMP QUIET >DH0:icmp.txt
&SYS:NetCapture OUT=DH0:five.pcap IFACE=eth0 COUNT=5 SECONDS=30 SNAP=64 PROTO=ICMP QUIET >DH0:five.txt
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

    n_noticmp=$(pcap_lines "$all" "not icmp and not icmp6" | grep -c . )

    # ICMP means the pings: echo request or reply, not merely IP protocol 1.
    n_echo=$(pcap_lines "$icmp" | grep -ci 'echo re')

    guest_addr=$(sed -n 's/.*online, address \([0-9.]*\).*/\1/p' \
                 "$hd/tools.txt" 2>/dev/null | head -1)

    if [ -n "$guest_addr" ]; then
        n_mac=$(pcap_lines "$icmp" "host $guest_addr and host $TARGET" |
                grep -c . )
    else
        n_mac=0
    fi

    # Whatever MAC that turned out to be, off the wire rather than derived.
    guest_mac=$(pcap_lines "$icmp" | grep -i 'echo request' | head -1 |
                awk '{print $2}')
    [ -n "$guest_mac" ] || guest_mac=none

    local n_wrong
    n_wrong=$(pcap_lines "$icmp" "not icmp and not icmp6" | grep -c . )

    local said_all said_five said_stop dropped
    said_all=$(sed -n 's/.*written=\([0-9]*\).*/\1/p' "$hd/all.txt" 2>/dev/null | tail -1)
    said_five=$(sed -n 's/.*written=\([0-9]*\).*/\1/p' "$hd/five.txt" 2>/dev/null | tail -1)
    said_stop=$(sed -n 's/.*stop=\([a-z]*\).*/\1/p' "$hd/five.txt" 2>/dev/null | tail -1)
    dropped=$(sed -n 's/.*dropped=\([0-9]*\).*/\1/p' "$hd/all.txt" 2>/dev/null | tail -1)

    [ "$n_all" -gt 0 ]      || { ok=0; kv "card_${board}_why" "all_empty"; }
    [ "$n_icmp" -gt 0 ]     || { ok=0; kv "card_${board}_why" "icmp_empty"; }
    [ "$n_echo" -gt 0 ]     || { ok=0; kv "card_${board}_why" "no_echo_in_icmp"; }
    [ "$n_wrong" -eq 0 ]    || { ok=0; kv "card_${board}_why" "filter_leaked"; }
    [ "$n_noticmp" -gt 0 ]  || { ok=0; kv "card_${board}_why" "unfiltered_is_icmp_only"; }
    [ "$n_all" -ge "$n_icmp" ] || { ok=0; kv "card_${board}_why" "icmp_exceeds_all"; }
    [ "$n_five" -eq 5 ]     || { ok=0; kv "card_${board}_why" "count_stop_gave_$n_five"; }
    [ -n "$guest_addr" ]    || { ok=0; kv "card_${board}_why" "no_address_in_tools_txt"; }
    [ "$n_mac" -gt 0 ]      || { ok=0; kv "card_${board}_why" "no_frame_between_${guest_addr:-?}_and_$TARGET"; }
    [ "${said_all:-x}" = "$n_all" ] ||
        { ok=0; kv "card_${board}_why" "said_${said_all:-none}_wrote_$n_all"; }
    [ "${said_five:-x}" = "5" ] ||
        { ok=0; kv "card_${board}_why" "five_said_${said_five:-none}"; }
    [ "${said_stop:-x}" = "count" ] ||
        { ok=0; kv "card_${board}_why" "five_stopped_on_${said_stop:-none}"; }

    kv "card_$board" \
       "mac=$guest_mac addr=${guest_addr:-none} all=$n_all icmp=$n_icmp echo=$n_echo leaked=$n_wrong nonicmp=$n_noticmp count5=$n_five mine=$n_mac said=${said_all:-none} dropped=${dropped:-none} run_rc=$run_rc"

    if [ "$ok" = 1 ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
}

# The card list arrives on fd 3, not on stdin: everything in the loop body is
# then free to have a stdin of its own, and nothing in it can eat the list.
while read -r -u 3 board model addr mactail; do
    [ -n "$board" ] || continue
    run_card "$board" "$model" "$addr" "$mactail"
done 3< <(cards_rows "$ONLY")

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
