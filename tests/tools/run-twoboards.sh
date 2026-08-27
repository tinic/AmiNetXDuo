#!/usr/bin/env bash
#
# TWO NETWORK BOARDS IN ONE MACHINE, and the second SANA-II unit.
#
#   tests/tools/run-twoboards.sh [-b builddir] [-t seconds] [-m model]
#                                [-B backend] [-N board,board] [-p pingtarget]
#
# Every other arm in this tree boots one card, so anxnet.device has only ever
# been asked for unit 0 and netdev_find_unit()'s `&dev->nd_Units[unit]` branch
# (src/netdev/netdev_device.c:1552) has never been reached with a non-zero
# number.  This boots two Zorro boards, points eth0 at unit 0 and eth1 at unit
# 1, and takes a DHCP lease on each -- which is the proof that both boards
# carried frames, in both directions, without needing a peer of its own: the
# two leases come from the LAN's own server and they are different because the
# two boards have different station addresses.
#
# The two interfaces are DHCP and NOT static: an address claimed here would
# have to be arbitrated against every other run on the rig, and the router
# already arbitrates leases.
#
# THE PAIR IS NOT FREE.  Amiberry emulates each Ethernet chip in file-scope
# statics -- a2065.cpp:44-78 for the LANCE, qemuvga/ne2000.cpp:77-133 and
# :1563 for the NE2000 -- so two boards that share a device model are one chip
# with two sets of registers aimed at it, and cfgfile.cpp's romtype_restricted()
# silently deletes all but the first.  A LANCE and a PC Card share nothing, and
# that is the pair here.  Two Zorro NE2000s (ariadne2 + xsurf100z2) come back
# with `ROMTYPE 00100059 removed` in the emulator log, one card in the machine
# and eth1 refused ENXIO.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
MODEL=A1200
TIMEOUT=300
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BOARDS="${AMINETXDUO_TWOBOARDS:-a2065,ne2000_pcmcia}"
PINGTARGET="${AMINETXDUO_TWOBOARDS_PING:-}"

while getopts "b:t:m:B:N:p:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        N) BOARDS="$OPTARG" ;;
        p) PINGTARGET="$OPTARG" ;;
        *) sed -n '3,9p' "$0" >&2; exit 2 ;;
    esac
done

case "$BACKEND" in
    slirp|slirp_inbound|none)
        echo "twoboards_backend=refused:$BACKEND" >&2
        echo "SLIRP gives each guest a NAT of its own and one address, so two" >&2
        echo "boards on it cannot take two leases.  -B names a host interface." >&2
        exit 2 ;;
esac

BOARD_A="${BOARDS%%,*}"
BOARD_B="${BOARDS##*,}"
[ -n "$BOARD_A" ] && [ -n "$BOARD_B" ] && [ "$BOARD_A" != "$BOARD_B" ] || {
    echo "twoboards_boards=refused:$BOARDS" >&2
    echo "-N takes two DIFFERENT board keys, comma separated." >&2
    exit 2; }

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/ShowNetStatus" \
         "$TOOLS/netstat" "$TOOLS/ping" "$BSD"; do
    [ -f "$f" ] || { echo "twoboards_stage=missing:$f" >&2; exit 2; }
done

# shellcheck source=../../tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"
# shellcheck source=../../tools/emu-board.sh
. "$ROOT/tools/emu-board.sh"
# shellcheck source=../../tools/amiberry-resolve.sh
. "$ROOT/tools/amiberry-resolve.sh"
amiberry_resolve || exit 2

# BOTH EMULATOR PATCHES, ASKED OF THE BINARY.  A stock Amiberry deletes the
# second board without a word and hands the PCMCIA card the host interface
# address, and both faults read from here as a driver that did not find its
# card.  tools/patches/amiberry/ has the two diffs and how to build them.
emu_two_board_capable() {
    local missing=""

    emu_board_mac_honoured ne2000_pcmcia || missing="pcmcia-ne2000-mac.diff"
    LC_ALL=C grep -aqF "net boards: one per chip" "$AMIBERRY" 2>/dev/null ||
        missing="${missing:+$missing }one-board-per-chip.diff"

    [ -z "$missing" ] && return 0

    echo "twoboards_amiberry_patches=missing:$missing" >&2
    echo "$AMIBERRY does not carry: $missing" >&2
    echo "Both are in tools/patches/amiberry/.  Build a copy of Amiberry with" >&2
    echo "them, then setcap cap_net_admin,cap_net_raw=eip on it -- every relink" >&2
    echo "clears the capability and the run then fails with an EMPTY serial log." >&2
    return 1
}

# BOTH BOARDS MUST BE ON ONE DRIVER, or the units are two device bases with a
# unit 0 each and nothing here is exercised.
sana2_select "$BOARD_A" "$ROOT/$BUILD"
DRV_A="$SANA2_SEL_DRIVER"; PATH_A="$SANA2_SEL_PATH"; SRC_A="$SANA2_SEL_SOURCE"
sana2_select "$BOARD_B" "$ROOT/$BUILD"
DRV_B="$SANA2_SEL_DRIVER"; SRC_B="$SANA2_SEL_SOURCE"

echo "twoboards_driver_a=$DRV_A source=$SRC_A"
echo "twoboards_driver_b=$DRV_B source=$SRC_B"

# The emulator has to be one that keeps the PCMCIA card's mac=, or both boards
# wear one address and "two boards" cannot be told from "one board twice".
if ! emu_two_board_capable; then
    echo "twoboards_emulator=refused" >&2
    exit 2
fi

[ "$DRV_A" = "$DRV_B" ] || {
    echo "twoboards_driver=refused:$DRV_A+$DRV_B" >&2
    echo "$BOARD_A and $BOARD_B want different drivers, so each gets a unit 0" >&2
    echo "of its own and a second unit is never opened.  Pick two boards one" >&2
    echo "driver covers (tools/sana2-stage.sh anxnet_card_for)." >&2
    exit 2; }
[ -n "$PATH_A" ] && [ -f "$PATH_A" ] || {
    echo "twoboards_stage=missing:$DRV_A" >&2; exit 2; }

if [ -z "$PINGTARGET" ]; then
    PINGTARGET=$(ip route 2>/dev/null | awk '$1 == "default" { print $3; exit }')
fi
[ -n "$PINGTARGET" ] || {
    echo "twoboards_ping=refused:no-default-route" >&2
    echo "-p takes an address on the bridge that answers an echo request." >&2
    exit 2; }


STAGE="$ROOT/build/twoboards-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$PATH_A" "$STAGE/devs/$DRV_A"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface ShowNetStatus netstat ping; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

# NO CARD= ON EITHER.  A pinned name would send netdev_find_unit() down its
# by-name branch and the number would stop being a position in the probe
# order, which is the branch this arm exists to reach.
for n in 0 1; do
    printf 'DEVICE=%s\nUNIT=%s\nCONFIGURE=DHCP\n' "$DRV_A" "$n" \
        > "$STAGE/devs/NetInterfaces/eth$n"
done

{
    echo "SYS:AddNetInterface eth0"
    echo "SYS:AddNetInterface eth1"
    echo "SYS:ShowNetStatus"
    echo "SYS:netstat -i"
    echo "SYS:ping $PINGTARGET COUNT 3"
} > "$STAGE/commands.txt"

TAG="${AMINETXDUO_RUN_TAG:-twoboards}"
export AMINETXDUO_RUN_TAG="$TAG"
HD="$ROOT/build/amiberry-testhd-$TAG"
rm -f "$HD/tools.txt"

echo "==> $MODEL, $BOARD_A + $BOARD_B on $BACKEND, $DRV_A units 0 and 1"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD_A" -N "$BOARD_B" -B "$BACKEND" \
    -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/ShowNetStatus" "$STAGE/netstat" \
    "$STAGE/ping" | tee "$ROOT/build/twoboards-run.log"
RUN_RC="${PIPESTATUS[0]}"
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || {
    echo "twoboards_report=missing run_rc=$RUN_RC" >&2
    echo "result=fail"; exit 1; }

echo
echo "==================== what the guest printed ===================="
cat "$REPORT"
echo "==============================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); }

block_rc() { # banner
    awk -v banner="$1" '
        index($0, "===== " banner " =====") == 1 { on = 1; next }
        on && /^----- rc / { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
    ' "$REPORT"
}

# The ShowNetStatus block, one field per interface.  `Interface eth0 (anxnet
# .device unit 0)` opens each one, so the unit the stack really opened is read
# back off the guest rather than off the file that asked for it.
iface_field() { # iface field
    awk -v want="$1" -v field="$2" '
        $1 == "Interface" && $2 == want { on = 1; next }
        on && $1 == "Interface" { exit }
        on && $1 == field { print $2; exit }
    ' "$REPORT"
}
iface_unit() { # iface
    awk -v want="$1" '
        $1 == "Interface" && $2 == want {
            sub(/.*unit /, ""); sub(/\).*/, ""); print; exit }
    ' "$REPORT"
}
# netstat -i: Name Mtu Address Link Ipkts Ierrs Opkts Oerrs
iface_pkts() { # iface column
    awk -v want="$1" -v col="$2" '
        $1 == want { print $col; exit }
    ' "$REPORT"
}

RC0=$(block_rc "SYS:AddNetInterface eth0")
RC1=$(block_rc "SYS:AddNetInterface eth1")
PING_RC=$(block_rc "SYS:ping $PINGTARGET COUNT 3")

UNIT0=$(iface_unit eth0); UNIT1=$(iface_unit eth1)
MAC0=$(iface_field eth0 hardware); MAC1=$(iface_field eth1 hardware)
ADDR0=$(iface_field eth0 address); ADDR1=$(iface_field eth1 address)
IN0=$(iface_pkts eth0 5); OUT0=$(iface_pkts eth0 7)
IN1=$(iface_pkts eth1 5); OUT1=$(iface_pkts eth1 7)

echo "twoboards_board_a=$BOARD_A board_b=$BOARD_B driver=$DRV_A backend=$BACKEND"
echo "twoboards_eth0 addif_rc=${RC0:-none} unit=${UNIT0:-none} mac=${MAC0:-none} address=${ADDR0:-none} ipkts=${IN0:-none} opkts=${OUT0:-none}"
echo "twoboards_eth1 addif_rc=${RC1:-none} unit=${UNIT1:-none} mac=${MAC1:-none} address=${ADDR1:-none} ipkts=${IN1:-none} opkts=${OUT1:-none}"
echo "twoboards_ping target=$PINGTARGET rc=${PING_RC:-none}"
echo "twoboards_run_rc=$RUN_RC"

[ "${RC0:-1}" = 0 ] || fail "AddNetInterface eth0 exited ${RC0:-nothing}"
[ "${RC1:-1}" = 0 ] || fail "AddNetInterface eth1 exited ${RC1:-nothing}"
[ "${UNIT1:-}" = 1 ] ||
    fail "eth1 came up on unit '${UNIT1:-none}', not unit 1, so the second\
 board was never opened"
[ -n "${MAC0:-}" ] && [ -n "${MAC1:-}" ] && [ "$MAC0" != "$MAC1" ] ||
    fail "the two interfaces report one station address ('${MAC0:-none}' and\
 '${MAC1:-none}'), so they are one board"
[ -n "${ADDR0:-}" ] && [ -n "${ADDR1:-}" ] && [ "$ADDR0" != "$ADDR1" ] ||
    fail "the two interfaces did not take two leases ('${ADDR0:-none}' and\
 '${ADDR1:-none}')"

for pair in "eth0 ${IN0:-0} ${OUT0:-0}" "eth1 ${IN1:-0} ${OUT1:-0}"; do
    # shellcheck disable=SC2086  # the split is the point: three fields
    set -- $pair
    [ "${2:-0}" -gt 0 ] 2>/dev/null ||
        fail "$1 received nothing: that board carried no frames"
    [ "${3:-0}" -gt 0 ] 2>/dev/null ||
        fail "$1 sent nothing: that board carried no frames"
done

[ "${PING_RC:-1}" = 0 ] || fail "ping $PINGTARGET exited ${PING_RC:-nothing}"

echo
if [ "$FAILED" -ne 0 ]; then
    echo "twoboards: FAILED $FAILED assertion(s)" >&2
    echo "result=fail"
    exit 1
fi
echo "twoboards: PASSED"
echo "result=ok"
exit 0
