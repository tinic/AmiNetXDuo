#!/usr/bin/env bash
# A SYN FLOOD MUST NOT STOP THE MACHINE ANSWERING.
# BRIDGED, AND NOT SLIRP; A STATIC ADDRESS
# The a2065.device driver is not ours to ship: AMINETXDUO_A2065=<path>, or a
# copy in build/a2065.device.  The peer needs a python with cap_net_raw; see
# synflood.py.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
PEER_IF="${AMINETXDUO_PEER_IFACE:-ens18}"
PEERHOST="${AMINETXDUO_FITZ_PEER:-}"
UNPROTECTED=no
CARDS_ONLY="${AMINETXDUO_SYNFLOOD_CARDS:-}"

GATEWAY="${AMINETXDUO_SYNFLOOD_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0
PORT="${AMINETXDUO_SYNFLOOD_PORT:-8080}"

FLOOD_PPS="${AMINETXDUO_SYNFLOOD_PPS:-100}"
FLOOD_NET="${AMINETXDUO_SYNFLOOD_NET:-10.99.0.0/16}"
FLOOD_SECS="${AMINETXDUO_SYNFLOOD_SECS:-40}"

PYCAP="${AMINETXDUO_PEER_PYCAP:-\$HOME/python3-cap}"

MACHEAD="${AMINETXDUO_SYNFLOOD_MACHEAD:-02:41:4d:53}"

while getopts "P:B:g:b:c:t:p:T:uh" opt; do
    case "$opt" in
        P) PEERHOST="$OPTARG" ;;
        B) PEER_IF="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        c) CARDS_ONLY="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        T) AMINETXDUO_RUN_TAG="$OPTARG" ;;
        u) UNPROTECTED=yes ;;
        h) sed -n '3,7p' "$0"; exit 0 ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$PEERHOST" ] || {
    echo "-P is required: the peer must be a THIRD machine.  A frame the" >&2
    echo "emulator host sends to its own bridged guest never comes back to" >&2
    echo "that NIC's pcap (docs/RESEARCH.md 63)." >&2
    exit 2; }

case "$BUILD" in /*) ;; *) BUILD="$ROOT/${BUILD#./}" ;; esac
TOOLS="$BUILD/src/tools"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-synflood}"
TAG="$AMINETXDUO_RUN_TAG"
OUT="$ROOT/build/synflood-$TAG"

. "$ROOT/tests/tools/cards.sh"

. "$ROOT/tools/sana2-stage.sh"

for f in "$TOOLS/httpd" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

peer_sh() { ssh -o ConnectTimeout=10 -n "$PEERHOST" "$@"; }

peer_sh "command -v python3 >/dev/null" || {
    echo "$PEERHOST has no python3, which is what runs the flood" >&2; exit 2; }
peer_sh "test -x $PYCAP" || {
    echo "$PEERHOST is missing $PYCAP.  The flooder needs a raw socket:" >&2
    echo "  cp \"\$(command -v python3)\" ~/python3-cap" >&2
    echo "  sudo setcap cap_net_raw+ep ~/python3-cap" >&2
    exit 2; }

RTMP="/tmp/synflood-$TAG"
FLOOD_PID=""
RUNNER=""

flood_off() {
    peer_sh "pkill -f '[s]ynflood-$TAG' >/dev/null 2>&1; exit 0" || true
}

cleanup() {
    [ -n "$FLOOD_PID" ] && kill "$FLOOD_PID" 2>/dev/null || true
    [ -n "$RUNNER" ] && kill "$RUNNER" 2>/dev/null || true
    flood_off
}
trap cleanup EXIT INT TERM HUP

rm -rf "$OUT"; mkdir -p "$OUT"
peer_sh "rm -f $RTMP-*; exit 0"
scp -q "$ROOT/tests/tools/synflood.py" "$PEERHOST:$RTMP-synflood-$TAG.py" || {
    echo "cannot copy the flooder to $PEERHOST" >&2; exit 2; }
scp -q "$ROOT/tests/tools/synprobe.py" "$PEERHOST:$RTMP-synprobe-$TAG.py" || {
    echo "cannot copy the probe to $PEERHOST" >&2; exit 2; }

CARD_STATUS=fail
CARD_QUIET_MS=none
CARD_FLOOD_MS=none
CARD_FLOOD_OK=0
CARD_FLOOD_TRIES=0
CARD_SENT=0
CARD_DRIVER=""

peer_probe() {
    local address="$1" count="$2" line
    line=$(ssh -o ConnectTimeout=10 -n "$PEERHOST" \
        "$PYCAP $RTMP-synprobe-$TAG.py $address --port $PORT \
         --path /readme.txt --count $count --gap 1 --timeout 8" 2>/dev/null || true)
    local ok med
    ok=$(printf '%s' "$line" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')
    med=$(printf '%s' "$line" | sed -n 's/.*ms_median=\([0-9]*\).*/\1/p')
    echo "${ok:-0} ${med:-none}"
}

run_one_card() {
    local board="$1" model="$2" address="$3" mactail="$4"
    local stage hd ok

    CARD_STATUS=fail
    CARD_QUIET_MS=none
    CARD_FLOOD_MS=none
    CARD_FLOOD_OK=0
    CARD_FLOOD_TRIES=0
    CARD_SENT=0
    CARD_DRIVER=""

    export AMINETXDUO_RUN_TAG="synflood-$board"
    export AMINETXDUO_AMIBERRY_MAC="$MACHEAD:$mactail"
    hd="$ROOT/build/amiberry-testhd-synflood-$board"

    stage="$ROOT/build/synflood-stage-$board"
    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/Public"
    cp -R "$ROOT/tests/netstack/devs" "$stage/devs"
    mkdir -p "$stage/devs/Networks"
    cp "$A2065" "$stage/devs/Networks/a2065.device"

    local want have
    want=$(sana2_driver_for "$board")
    if [ "$board" = a2065 ]; then
        have="$A2065"
    else
        have=$(sana2_local_driver "$want")
    fi
    if [ -n "$have" ] && [ -f "$have" ]; then
        export AMINETXDUO_SANA2_DRIVER="$have"
    else
        unset AMINETXDUO_SANA2_DRIVER
        CARD_STATUS=skip_driver
        CARD_DRIVER="$want"
        return 0
    fi

    cat > "$stage/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=$want
UNIT=0
CONFIGURE=STATIC
ADDRESS=$address
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF

    sana2_stage "$board" "$stage/devs"

    cp "$BSD" "$stage/libs/bsdsocket.library"
    echo "served by an Amiga under a SYN flood" > "$stage/Public/readme.txt"
    echo "<html><body>ok</body></html>" > "$stage/Public/index.html"

    echo "==> $board: booting $model, bridged on $PEER_IF, httpd at $address:$PORT"
    set +e
    "$ROOT/tools/amiberry-run.sh" -N "$board" -B "$PEER_IF" -m "$model" \
        -t "$TIMEOUT" -a "DH0:Public $PORT" \
        "$TOOLS/httpd" "$stage/devs" "$stage/libs" "$stage/Public" \
        > "$OUT/$board-boot.log" 2>&1 &
    RUNNER=$!
    set -e

    local answered=no probe_ok
    for _ in $(seq 1 $((TIMEOUT / 2))); do
        sleep 2
        kill -0 "$RUNNER" 2>/dev/null || break
        read -r probe_ok _ <<<"$(peer_probe "$address" 1)"
        if [ "$probe_ok" -ge 1 ]; then
            answered=yes
            break
        fi
    done

    if [ "$answered" != yes ]; then
        kill "$RUNNER" 2>/dev/null || true
        wait "$RUNNER" 2>/dev/null || true
        RUNNER=""
        CARD_STATUS=no_verdict
        return 0
    fi

    read -r ok CARD_QUIET_MS <<<"$(peer_probe "$address" 3)"
    echo "    $board quiet: ok=$ok/3 median_ms=$CARD_QUIET_MS"
    if [ "$ok" -lt 2 ] || [ "$CARD_QUIET_MS" = none ]; then
        kill "$RUNNER" 2>/dev/null || true
        wait "$RUNNER" 2>/dev/null || true
        RUNNER=""
        CARD_STATUS=skip_baseline
        return 0
    fi

    echo "==> $board: $PEERHOST flooding $address:$PORT at ${FLOOD_PPS} pps for ${FLOOD_SECS}s"
    ssh -o ConnectTimeout=10 -n "$PEERHOST" \
        "cp $RTMP-synflood-$TAG.py $RTMP-run-$board.py
         timeout $((FLOOD_SECS + 20)) $PYCAP $RTMP-run-$board.py \
            $address --port $PORT --seconds $FLOOD_SECS --pps $FLOOD_PPS \
            --source-net $FLOOD_NET --report $RTMP-report-$board.txt \
            > $RTMP-flood-$board.out 2> $RTMP-flood-$board.err; exit 0" &
    FLOOD_PID=$!

    sleep 5

    CARD_FLOOD_TRIES=5
    read -r CARD_FLOOD_OK CARD_FLOOD_MS <<<"$(peer_probe "$address" 5)"
    echo "    $board flooded: ok=$CARD_FLOOD_OK/5 median_ms=$CARD_FLOOD_MS"

    wait "$FLOOD_PID" 2>/dev/null || true
    FLOOD_PID=""

    scp -q "$PEERHOST:$RTMP-report-$board.txt" "$OUT/$board-flood.txt" \
        2>/dev/null || : > "$OUT/$board-flood.txt"
    CARD_SENT=$(awk -F'sent=' '/sent=/{split($2,a," "); print a[1]; exit}' \
                    "$OUT/$board-flood.txt" 2>/dev/null || echo 0)
    CARD_SENT="${CARD_SENT:-0}"

    kill "$RUNNER" 2>/dev/null || true
    wait "$RUNNER" 2>/dev/null || true
    RUNNER=""
    [ -f "$hd/stdout.txt" ] && cp "$hd/stdout.txt" "$OUT/$board-guest.txt" 2>/dev/null || true

    if [ "$CARD_SENT" -lt 1000 ]; then
        CARD_STATUS=skip_flood
    elif [ "$UNPROTECTED" = yes ]; then
        if [ "$CARD_FLOOD_OK" -eq 0 ]; then
            CARD_STATUS=unprotected_fell_over
        else
            CARD_STATUS=unprotected_survived
        fi
    else
        local ceil=$(( CARD_QUIET_MS * 4 + 1000 ))
        if [ "$CARD_FLOOD_OK" -eq "$CARD_FLOOD_TRIES" ] &&
           [ "$CARD_FLOOD_MS" != none ] && [ "$CARD_FLOOD_MS" -le "$ceil" ]; then
            CARD_STATUS=answered_under_flood
        else
            CARD_STATUS=stopped_answering
        fi
    fi

    return 0
}

RESULTS="$OUT/results.txt"
: > "$RESULTS"

CARDS_RUN=0
CARDS_PASS=0
CARDS_FAIL=0
CARDS_SKIP=0

while read -r board model address mactail; do
    [ -n "$board" ] || continue

    run_one_card "$board" "$model" "$address" "$mactail"

    case "$CARD_STATUS" in
        answered_under_flood|unprotected_fell_over)
            CARDS_PASS=$((CARDS_PASS + 1)); CARDS_RUN=$((CARDS_RUN + 1)) ;;
        stopped_answering|unprotected_survived)
            CARDS_FAIL=$((CARDS_FAIL + 1)); CARDS_RUN=$((CARDS_RUN + 1)) ;;
        *)
            CARDS_SKIP=$((CARDS_SKIP + 1)) ;;
    esac

    printf 'synflood_card: board=%s status=%s quiet_ms=%s flood_ms=%s flood_ok=%s/%s sent=%s%s\n' \
           "$board" "$CARD_STATUS" "$CARD_QUIET_MS" "$CARD_FLOOD_MS" \
           "$CARD_FLOOD_OK" "$CARD_FLOOD_TRIES" "$CARD_SENT" \
           "${CARD_DRIVER:+ missing_driver=$CARD_DRIVER}" | tee -a "$RESULTS"
done <<CARDLIST
$(cards_rows "$CARDS_ONLY")
CARDLIST

cleanup
trap - EXIT INT TERM HUP

echo
echo "========================== per card ==============================="
cat "$RESULTS"
echo "==================================================================="
echo

if [ "$CARDS_RUN" -eq 0 ]; then
    STATUS=no_verdict
    RC=3
elif [ "$CARDS_FAIL" -gt 0 ]; then
    STATUS=fail
    RC=1
else
    STATUS=pass
    RC=0
fi

printf 'synflood: status=%s protected=%s cards=%s pass=%s fail=%s skip=%s pps=%s log=%s\n' \
       "$STATUS" "$([ "$UNPROTECTED" = yes ] && echo no || echo yes)" \
       "$CARDS_RUN" "$CARDS_PASS" "$CARDS_FAIL" "$CARDS_SKIP" "$FLOOD_PPS" "$OUT"

case "$STATUS" in
    no_verdict)
        echo "no card produced a verdict.  Nothing was measured: check the" >&2
        echo "boot logs under $OUT and that $PYCAP on $PEERHOST carries" >&2
        echo "cap_net_raw." >&2 ;;
    fail)
        if [ "$UNPROTECTED" = yes ]; then
            echo "a card kept answering on the unprotected tree, so the flood" >&2
            echo "was not strong enough there to prove the defence is what" >&2
            echo "changes the outcome.  Raise --pps or lengthen the run." >&2
        else
            echo "a card stopped answering under the flood.  This is the" >&2
            echo "defect the SYN cache exists to fix; the per-card lines" >&2
            echo "above name which one and how far it got." >&2
        fi ;;
esac

exit "$RC"
