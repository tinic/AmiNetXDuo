#!/usr/bin/env bash
# Request and response latency, on a link that loses packets.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

PEER="${AMINETXDUO_FITZ_PEER:-}"
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-}"
PEER_IF="${AMINETXDUO_PEER_IFACE:-ens18}"
PEER_TC="${AMINETXDUO_PEER_TC:-\$HOME/tc-cap}"
GUEST=""
LOSS=5
DIR=both
COUNT=40
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TAG="${AMINETXDUO_RUN_TAG:-reqresp}"
TIMEOUT=600
PORT=18080
MODEL=A3000
IFACE="${AMINETXDUO_EMU_IFACE:-ens18}"
BOARD="${AMINETXDUO_EMU_BOARD:-a2065}"
SEED="${AMINETXDUO_LOSSGATE_SEED:-20260811}"
TXRAND="${AMINETXDUO_LOSSGATE_TXRAND:-determ}"
STALL_MS="${AMINETXDUO_REQRESP_STALL_MS:-500}"

usage() {
    cat <<'EOF'
usage: tests/perf/run-reqresp.sh -H user@host -A addr -g GUESTADDR
                                 [-l PERCENT] [-D rx|tx|both] [-n COUNT]
                                 [-b BUILDDIR] [-T TAG] [-t SECONDS] [-p PORT]

  -H  the peer, over ssh.  A THIRD machine.
  -A  the peer's address as the guest sees it
  -g  the guest's address, which its MAC's DHCP lease makes predictable
  -l  packet loss percent, 0 for a clean link (default 5)
  -D  which direction loses: rx, tx, both (default both)
  -n  fetches (default 40)
EOF
}

while getopts "H:A:g:l:D:n:b:T:t:p:m:h" opt; do
    case "$opt" in
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        g) GUEST="$OPTARG" ;;
        l) LOSS="$OPTARG" ;;
        D) DIR="$OPTARG" ;;
        n) COUNT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

[ -n "$PEER" ] && [ -n "$PEER_ADDR" ] && [ -n "$GUEST" ] || { usage >&2; exit 2; }
case "$DIR" in rx|tx|both) ;; *) echo "-D takes rx, tx or both" >&2; exit 2 ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
for f in ToolsSmoke AddNetInterface fetch netstat; do
    [ -f "$TOOLS/$f" ] || { echo "no $TOOLS/$f -- build $BUILD first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
[ -n "$A2065" ] || for c in ~/amiga-assets/sana2/a2065.device \
                            ~/amiga-assets/drivers/a2065.device; do
    [ -f "$c" ] && A2065="$c" && break
done
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "no a2065.device -- set AMINETXDUO_A2065" >&2; exit 2; }

peer_tc() { ssh "$PEER" "$PEER_TC $*"; }

RX_ON=0
TX_ON=0

peer_off() {
    [ "$RX_ON" = 1 ] && { peer_tc "qdisc del dev $PEER_IF root" >/dev/null 2>&1 || true; }
    [ "$TX_ON" = 1 ] && { peer_tc "qdisc del dev $PEER_IF ingress" >/dev/null 2>&1 || true; }
    RX_ON=0; TX_ON=0
    ssh -n "$PEER" "pkill -f 'http.server $PORT'" >/dev/null 2>&1 || true
    return 0
}

trap peer_off EXIT INT TERM HUP

peer_off
echo "==> peer: http.server on $PORT"
ssh -n "$PEER" "mkdir -p /tmp/reqresp-$PORT && \
                head -c 1024 /dev/urandom | od -An -tx1 > /tmp/reqresp-$PORT/probe.txt"
ssh -n "$PEER" "( cd /tmp/reqresp-$PORT && exec setsid python3 -m http.server \
                  $PORT --bind 0.0.0.0 ) > /tmp/reqresp-$PORT/log 2>&1 \
                  < /dev/null & sleep 1"
sleep 2
ssh -n "$PEER" "python3 -c \"import urllib.request as u; \
    u.urlopen('http://127.0.0.1:$PORT/probe.txt', timeout=5).read()\"" || {
    echo "the peer's http server did not come up on $PORT" >&2; exit 2; }

if [ "${LOSS%%.*}" != "0" ]; then
    case "$DIR" in
        rx|both)
            peer_tc "qdisc add dev $PEER_IF root handle 1: prio bands 3"
            peer_tc "qdisc add dev $PEER_IF parent 1:3 handle 30: netem \
                     loss ${LOSS}% seed $SEED" 2>/dev/null ||
                peer_tc "qdisc add dev $PEER_IF parent 1:3 handle 30: netem loss ${LOSS}%"
            peer_tc "filter add dev $PEER_IF protocol ip parent 1: prio 1 u32 \
                     match ip dst $GUEST/32 flowid 1:3"
            RX_ON=1
            echo "==> peer $PEER_IF: ${LOSS}% loss towards $GUEST" ;;
    esac
    case "$DIR" in
        tx|both)
            NTH=$(awk -v l="$LOSS" 'BEGIN { printf "%d", (100.0 / l) + 0.5 }')
            peer_tc "qdisc add dev $PEER_IF handle ffff: ingress"
            peer_tc "filter add dev $PEER_IF parent ffff: protocol ip prio 1 u32 \
                     match ip src $GUEST/32 action pass random $TXRAND drop $NTH"
            TX_ON=1
            echo "==> peer $PEER_IF: 1 in $NTH of $GUEST -> peer dropped ($TXRAND)" ;;
    esac
fi

OUT="$ROOT/build/reqresp-$TAG"
STAGE="$ROOT/build/reqresp-stage-$TAG"
rm -rf "$OUT" "$STAGE"; mkdir -p "$OUT" "$STAGE/devs" "$STAGE/libs"

cp -r "$ROOT/tests/netstack/devs/." "$STAGE/devs/" 2>/dev/null || true
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$ROOT/$BUILD/src/bsdsocket/bsdsocket.library" "$STAGE/libs/bsdsocket.library"
[ -f "$ROOT/$BUILD/src/usergroup/usergroup.library" ] &&
    cp "$ROOT/$BUILD/src/usergroup/usergroup.library" "$STAGE/libs/usergroup.library"
cp "$TOOLS/AddNetInterface" "$TOOLS/fetch" "$STAGE/"
cp "$TOOLS/netstat" "$STAGE/NetStat"

{
    echo "SYS:AddNetInterface eth0"
    echo "wait 6"
    echo "SYS:NetStat -s"
    i=0
    while [ "$i" -lt "$COUNT" ]; do
        echo "SYS:fetch http://$PEER_ADDR:$PORT/probe.txt TO RAM:p QUIET"
        i=$((i + 1))
    done
    echo "SYS:NetStat -s"
} > "$STAGE/commands.txt"

echo "==> $COUNT fetches, model $MODEL, board $BOARD on $IFACE"
set +e
tools/amiberry-run.sh -N "$BOARD" -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/NetStat" "$STAGE/fetch" \
    > "$OUT/run.txt" 2>&1
EMURC=$?
set -e

HD="$ROOT/build/amiberry-testhd-$TAG"
[ -f "$HD/tools.txt" ] && cp "$HD/tools.txt" "$OUT/tools.txt"
[ -f "$OUT/tools.txt" ] || cp "$OUT/run.txt" "$OUT/tools.txt"

SAW=$(grep -oE '192\.168\.[0-9]+\.[0-9]+' "$OUT/tools.txt" | grep -v "^$PEER_ADDR$" |
      sort | uniq -c | sort -rn | head -1 | awk '{print $2}' || true)
echo "guest_addr=$SAW"
if [ "${LOSS%%.*}" != "0" ] && [ -n "$SAW" ] && [ "$SAW" != "$GUEST" ]; then
    echo "guest_addr=$SAW expected=$GUEST" >&2
    echo "the peer's filter was built for $GUEST, so this run met no loss." >&2
    exit 2
fi

if [ "$TX_ON" = 1 ]; then
    peer_tc "-s filter show dev $PEER_IF parent ffff:" 2>/dev/null |
        sed -n 's/.*(dropped \([0-9]*\),.*/tx_dropped=\1/p' | tail -1
fi

peer_off

awk '/^===== /   { isfetch = /fetch/ }
     /^----- rc /{ if (isfetch) for (i = 1; i <= NF; i++)
                       if ($i == "ms,") print $(i - 1) }' \
    "$OUT/tools.txt" > "$OUT/ms.txt"

echo
awk -v stall="$STALL_MS" -v n="$COUNT" '
    { v[c++] = $1 + 0; total += $1 + 0; if ($1 + 0 >= stall) { stalls++; lost += $1 + 0 } }
    END {
        if (c == 0) { print "no timed commands in the transcript"; exit 1 }
        for (i = 0; i < c; i++) for (j = i + 1; j < c; j++)
            if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }
        med = (c % 2) ? v[int(c / 2)] : (v[c / 2 - 1] + v[c / 2]) / 2
        p90 = v[int(c * 0.9)]
        printf "commands=%d\n", c
        printf "median_ms=%d\n", med
        printf "p90_ms=%d\n", p90
        printf "max_ms=%d\n", v[c - 1]
        printf "total_ms=%d\n", total
        printf "stalls=%d\n", stalls + 0
        printf "stall_ms=%d\n", lost + 0
        printf "stall_threshold_ms=%d\n", stall
    }' "$OUT/ms.txt"

{ grep -E '^[[:space:]]*[0-9]+ retransmitted' "$OUT/tools.txt" || true; } |
    awk 'NR == 1 { r0 = $1; d0 = $3 } { r = $1; d = $3 }
         END { if (NR) printf "retransmitted=%d dropped_rx=%d\n", r - r0, d - d0 }'

echo "emulator_rc=$EMURC"
echo "out=$OUT"
[ "$EMURC" = 0 ] || exit 1
