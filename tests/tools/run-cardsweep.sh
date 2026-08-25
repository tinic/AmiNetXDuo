#!/usr/bin/env bash
#
# Every network card this project supports, booted once each.
#
#   tests/tools/run-cardsweep.sh [-P PEERHOST] [-B IFACE] [-b BUILDDIR]
#                                [-t SECONDS] [-c CARD[,CARD...]] [-l]
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

PORTBASE="${AMINETXDUO_CARDSWEEP_PORTBASE:-7400}"

SWEEP_ID="${AMINETXDUO_CARDSWEEP_ID:-$(basename "$ROOT")}"
SWEEP_ID=$(printf '%s' "$SWEEP_ID" | tr -c 'A-Za-z0-9' '-' | cut -c1-16)

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


# shellcheck source=tests/tools/cards.sh
. "$ROOT/tests/tools/cards.sh"

. "$ROOT/tools/sana2-stage.sh"


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

mkdir -p "$ROOT/build"
LOCK="$ROOT/build/cardsweep.lock"
exec 9>"$LOCK"
flock -n 9 || {
    echo "another cardsweep holds $LOCK; not starting a second one" >&2
    exit 2
}


block_rc() { # transcript banner
    awk -v banner="$2" '
        index($0, "===== " banner " =====") == 1 { on = 1; next }
        on && /^----- rc / { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
    ' "$1"
}

guest_bytes() { # transcript dir
    awk -v want="$2" '
        $0 ~ ("dir=" want "([^-a-z]|$)") {
            for (i = 1; i <= NF; i++)
                if ($i ~ /^bytes=/) { sub(/^bytes=/, "", $i); t += $i }
        }
        END { print t + 0 }
    ' "$1"
}

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

    if [ "$SWEEP_SLOT" -ne 0 ]; then
        addr="${addr%.*}.$(( ${addr##*.} - SWEEP_SLOT * 10 ))"
        mac="${mac%:*}:$(printf '%02x' $(( 0x${mac##*:} + SWEEP_SLOT * 32 )))"
    fi

    sana2_select "$board" "$BUILDDIR"
    drv=$SANA2_SEL_DRIVER
    drvpath=$SANA2_SEL_PATH
    anxcard=$SANA2_SEL_CARD

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

    tag="cardsweep-$SWEEP_ID-$board"
    t0=$(date +%s)
    base=$((PORTBASE + IDX * 10))
    IDX=$((IDX + 1))

    echo
    echo "===================== $board ($drv${anxcard:+ CARD=$anxcard}, $model, $addr, ports $base+) ====================="

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
    peer_rx=$(peer_bytes "$peers" tcp size)
    peer_tx=$(peer_bytes "$peers" srvtcp)
    peer_urx=$(peer_bytes "$peers" udp)

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
        status=fail_assert
        why=" reason=\"bytes moved both ways and agreed off-box; another assertion in the arm failed -- read the log\""
    elif [ "$rc" = 2 ]; then
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

if [ "$((NFAIL - NCARRIED))" != 0 ]; then echo "result=fail"; exit 1; fi
if [ "$NCARRIED" != 0 ];             then echo "result=fail_assert"; exit 4; fi
if [ "$NSKIP" != 0 ];                then echo "result=partial"; exit 3; fi
echo "result=ok"
exit 0
