#!/usr/bin/env bash
# Every network card, and whether IPv6 reaches anything past the router.
# NO PEER, AND NO STATIC ADDRESS
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TIMEOUT=240
ONLY=""
LIST=0

TARGET6="${AMINETXDUO_CARDSWEEP6_TARGET:-2606:4700:4700::1111}"
TARGET6_PORT="${AMINETXDUO_CARDSWEEP6_PORT:-53}"
TARGET4="${AMINETXDUO_CARDSWEEP6_TARGET4:-1.1.1.1}"

SETTLE=15

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
MCASTJOIN="$BUILDDIR/tests/tools/McastJoin"
BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"

# shellcheck source=tests/tools/cards.sh
. "$ROOT/tests/tools/cards.sh"
# shellcheck source=tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"

infra() { echo "error=$*"; echo "result=infra"; exit 2; }

if [ "$LIST" = 1 ]; then
    echo "cards this sweep boots:"
    cards_rows "$ONLY" | while read -r board model _addr mac; do
        sana2_select "$board" "$BUILDDIR"
        printf '  card=%s model=%s driver=%s driver_source=%s anxcard=%s mac_tail=%s driver_present=%s\n' \
               "$board" "$model" "$SANA2_SEL_DRIVER" "$SANA2_SEL_SOURCE" \
               "${SANA2_SEL_CARD:-none}" "$mac" \
               "$( [ -n "$SANA2_SEL_PATH" ] && echo yes || echo no)"
    done
    echo "drivers with no board to run them on:"
    printf '%s\n' "$UNTESTABLE" | while read -r drv reason; do
        [ -n "$drv" ] || continue
        printf '  driver=%s status=untestable reason="%s"\n' "$drv" "$reason"
    done
    exit 0
fi

for t in ToolsSmoke AddNetInterface ShowNetStatus ping nc traceroute netstat; do
    [ -x "$TOOLS/$t" ] || infra "no $TOOLS/$t; build $BUILD first"
done
[ -x "$MCASTJOIN" ] || infra "no $MCASTJOIN; build $BUILD first"
[ -f "$BSD" ] || infra "no $BSD; build $BUILD first"

if [ -f "$BUILDDIR/CMakeCache.txt" ] &&
   grep -q '^AMINETXDUO_IPV6:BOOL=OFF' "$BUILDDIR/CMakeCache.txt"; then
    infra "$BUILD was configured with AMINETXDUO_IPV6=OFF"
fi

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

mkdir -p "$ROOT/build"
LOCK="$ROOT/build/cardsweep6.lock"
exec 9>"$LOCK"
flock -n 9 || infra "another cardsweep6 holds $LOCK"

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

replies_of() { # banner
    block "$1" |
        sed -n 's/^[0-9]* packets transmitted, \([0-9]*\) received.*/\1/p' |
        tail -1
}

guest_global6() {
    awk '$1 == "address6" && $2 !~ /^fe80:/ && $0 !~ /\(tentative\)/ \
         { print $2; exit }' "$REPORT"
}

tr_hops() { # banner
    block "$1" |
        awk '/^traceroute to/ { next }
             { for (i = 1; i <= NF; i++)
                   if ($i ~ /^[0-9a-fA-F:]*:[0-9a-fA-F:]+$/) { print $i; break } }' |
        sort -u | grep -c . || true
}

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

while read -r -u 3 board model _addr mac; do
    [ -n "$board" ] || continue

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
        printf 'card=%s model=%s driver=%s driver_source=%s anxcard=%s status=skip_no_driver wall_s=0 reason="%s"\n' \
               "$board" "$model" "$drv" "$SANA2_SEL_SOURCE" \
               "${anxcard:-none}" "$reason" | tee -a "$RESULTS"
        NSKIP=$((NSKIP + 1))
        continue
    fi

    tag="cardsweep6-$board"
    t0=$(date +%s)

    echo
    echo "===================== $board ($drv${anxcard:+ CARD=$anxcard}, $model) ====================="

    STAGE="$ROOT/build/cardsweep6-stage-$tag"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$A2065" "$STAGE/devs/a2065.device"
    cp "$BSD" "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface ShowNetStatus ping nc traceroute netstat; do
        cp "$TOOLS/$t" "$STAGE/$t"
    done
    cp "$MCASTJOIN" "$STAGE/McastJoin"

    cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
IFEOF
    printf 'hostname anx6-%s\n' "$board" > "$STAGE/devs/Internet/name_resolution"

    export AMINETXDUO_SANA2_DRIVER="$drvpath"
    export AMINETXDUO_SANA2_DRIVER_NAME="$drv"
    export AMINETXDUO_SANA2_DEVICE="$drv"
    export AMINETXDUO_SANA2_CARD="$anxcard"
    sana2_stage "$board" "$STAGE/devs"
    echo "==> $board: $SANA2_DRIVER, opened as '$SANA2_DEVICE'${SANA2_CARD:+, CARD=$SANA2_CARD}"

    C_IFUP="SYS:AddNetInterface eth0"
    C_STATUS="SYS:ShowNetStatus ALL"
    C_PING4="SYS:ping $TARGET4 -c 2 -t 8 -n"
    C_PING6="SYS:ping $TARGET6 -c 3 -t 15 -n"
    C_TCP6="SYS:nc $TARGET6 $TARGET6_PORT -z -v -w 15 -6"
    C_TRACE="SYS:traceroute $TARGET6 -m 6 -q 1 -w 2 -n"
    C_ONLINK="SYS:ping $ONLINK6 -c 3 -t 10 -n"
    C_ROUTES="SYS:netstat -r"

    C_MCAST="SYS:McastJoin DEVICE=$SANA2_DEVICE UNIT=0"
    {
        echo "$C_MCAST"
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
            "$STAGE/McastJoin" \
        > "$LOGDIR/$board.log" 2>&1 < /dev/null
    rc=$?

    wall=$(( $(date +%s) - t0 ))
    tail -20 "$LOGDIR/$board.log"

    REPORT="$ROOT/build/amiberry-testhd-$tag/tools.txt"

    iface_rc=""; global6=""; v4=0; p6=0; tcp6=no; hops=0; onlink=skip
    mcast=none
    if [ -f "$REPORT" ]; then
        cp "$REPORT" "$LOGDIR/$board.tools.txt"
        iface_rc=$(rc_of "$C_IFUP")
        mcast=$(block "$C_MCAST" | sed -n 's/.*verdict=\([A-Z]*\).*/\1/p' | tail -1)
        mcast=${mcast:-none}
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

    offlan=no
    [ "${p6:-0}" -gt 0 ] && offlan=yes
    [ "$tcp6" = yes ] && offlan=yes

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
    elif [ "$SANA2_SEL_SOURCE" = anxnet ] && [ "$mcast" != PASS ]; then
        status=fail_mcast
        why=" reason=\"McastJoin verdict=$mcast: the driver refused a real"
        why="$why group address, which is the whole point of this driver\""
    elif [ "$offlan" = yes ]; then
        status=pass
    elif [ "$onlink" = yes ]; then
        status=fail_offlan
        why=" reason=\"IPv6 works on this segment and reaches nothing past the router -- the answer is dropped one hop away\""
    else
        status=fail_offlan
        why=" reason=\"a global address formed and no IPv6 came back from anywhere\""
    fi

    case "$status" in
        pass)                    NPASS=$((NPASS + 1)) ;;
        skip_offline)            NSKIP=$((NSKIP + 1)) ;;
        *)                       NFAIL=$((NFAIL + 1)) ;;
    esac

    printf 'card=%s model=%s driver=%s driver_source=%s anxcard=%s status=%s rc=%s iface_rc=%s global6=%s ping6_offlan=%s tcp6_offlan=%s trace6_hops=%s ping6_onlink=%s ping4_offlan=%s mcast=%s wall_s=%s log=%s%s\n' \
           "$board" "$model" "$drv" "$SANA2_SEL_SOURCE" "${anxcard:-none}" \
           "$status" "$rc" "${iface_rc:-none}" \
           "${global6:-none}" "$p6" "$tcp6" "$hops" "$onlink" "$v4" \
           "$mcast" "$wall" "$LOGDIR/$board.log" "$why" | tee -a "$RESULTS"
done 3<<EOF
$(cards_rows "$ONLY")
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
printf 'cardsweep6: cards=%d pass=%d fail=%d skip=%d target6=%s wall_s=%d\n' \
       "$((NPASS + NFAIL + NSKIP))" "$NPASS" "$NFAIL" "$NSKIP" "$TARGET6" "$WALL"

if [ "$NFAIL" != 0 ]; then echo "result=fail"; exit 1; fi
if [ "$NSKIP" != 0 ]; then echo "result=partial"; exit 3; fi
echo "result=ok"
exit 0
