#!/usr/bin/env bash
# WHAT THE SYN DEFENCE COSTS WHEN NOTHING IS ATTACKING.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD_A=""
BUILD_B=""
PEERHOST="${AMINETXDUO_FITZ_PEER:-}"
PYCAP="${AMINETXDUO_PEER_PYCAP:-\$HOME/python3-cap}"
ROUNDS=3
BOARD="${AMINETXDUO_SYNCOST_BOARD:-a2065}"
MODEL=A1200
IFACE="${AMINETXDUO_PEER_IFACE:-ens18}"
TIMEOUT=300
ADDRESS="${AMINETXDUO_SYNCOST_ADDRESS:-192.168.1.237}"
GATEWAY="${AMINETXDUO_SYNCOST_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0
PORT="${AMINETXDUO_SYNCOST_PORT:-8080}"
SMALL_N=10
BIG_N=3

while getopts "A:B:P:n:N:I:m:t:a:T:h" opt; do
    case "$opt" in
        A) BUILD_A="$OPTARG" ;;
        B) BUILD_B="$OPTARG" ;;
        P) PEERHOST="$OPTARG" ;;
        n) ROUNDS="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        I) IFACE="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        T) AMINETXDUO_RUN_TAG="$OPTARG" ;;
        h) sed -n '3,7p' "$0"; exit 0 ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$BUILD_A" ] && [ -n "$BUILD_B" ] || {
    echo "-A and -B are both required: the two trees to compare." >&2; exit 2; }

[ -n "$PEERHOST" ] || {
    echo "-P is required: the client must be a THIRD machine.  The emulator" >&2
    echo "host cannot reach its own bridged guest (docs/RESEARCH.md 63)." >&2
    exit 2; }

case "$BUILD_A" in /*) ;; *) BUILD_A="$ROOT/${BUILD_A#./}" ;; esac
case "$BUILD_B" in /*) ;; *) BUILD_B="$ROOT/${BUILD_B#./}" ;; esac

for b in "$BUILD_A" "$BUILD_B"; do
    [ -f "$b/src/tools/httpd" ] || { echo "missing $b/src/tools/httpd" >&2; exit 2; }
    [ -f "$b/src/bsdsocket/bsdsocket.library" ] ||
        { echo "missing $b/src/bsdsocket/bsdsocket.library" >&2; exit 2; }
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

. "$ROOT/tools/sana2-stage.sh"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-syncost}"
OUT="$ROOT/build/syncost-$AMINETXDUO_RUN_TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

PTMP="/tmp/syncost-$AMINETXDUO_RUN_TAG"
ssh -o ConnectTimeout=10 -n "$PEERHOST" "rm -f $PTMP-*; exit 0" || {
    echo "cannot reach $PEERHOST" >&2; exit 2; }
scp -q "$ROOT/tests/tools/synprobe.py" "$PEERHOST:$PTMP-synprobe.py" || {
    echo "cannot copy the probe to $PEERHOST" >&2; exit 2; }

peer_probe() {
    local address="$1" path="$2" count="$3" want="$4" line ok med bps
    line=$(ssh -o ConnectTimeout=10 -n "$PEERHOST" \
        "$PYCAP $PTMP-synprobe.py $address --port $PORT --path $path \
         --count $count --gap 1 --timeout 120 --expect-bytes $want" 2>/dev/null || true)
    ok=$(printf '%s' "$line" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')
    med=$(printf '%s' "$line" | sed -n 's/.*ms_median=\([0-9]*\).*/\1/p')
    bps=$(printf '%s' "$line" | sed -n 's/.*bps=\([0-9]*\).*/\1/p')
    if [ "$want" -gt 0 ]; then
        echo "${ok:-0} ${bps:-0}"
    else
        echo "${ok:-0} ${med:-none}"
    fi
}

RUNNER=""
cleanup() { [ -n "$RUNNER" ] && kill "$RUNNER" 2>/dev/null || true; }
trap cleanup EXIT INT TERM HUP

ARM_HANDSHAKE_MS=none
ARM_BPS=none

median() {
    sort -n | awk '{ v[n++] = $1 }
                   END { if (n == 0) { print "none"; exit }
                         if (n % 2) print v[int(n/2)]
                         else print int((v[n/2 - 1] + v[n/2]) / 2) }'
}

run_arm() {
    local build="$1" mac="$2" label="$3"
    local stage answered i

    ARM_HANDSHAKE_MS=none
    ARM_BPS=none

    export AMINETXDUO_RUN_TAG="syncost-$label"
    export AMINETXDUO_AMIBERRY_MAC="$mac"
    export AMINETXDUO_SANA2_DRIVER="$A2065"

    stage="$ROOT/build/syncost-stage-$label"
    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/Public"
    cp -R "$ROOT/tests/netstack/devs" "$stage/devs"

    cat > "$stage/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=$(sana2_driver_for "$BOARD")
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF
    sana2_stage "$BOARD" "$stage/devs" >/dev/null

    cp "$build/src/bsdsocket/bsdsocket.library" "$stage/libs/bsdsocket.library"
    echo "twenty bytes here." > "$stage/Public/small.txt"
    dd if=/dev/zero bs=1024 count=512 2>/dev/null | tr '\0' 'A' > "$stage/Public/blob.bin"

    set +e
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" -a "DH0:Public $PORT" \
        "$build/src/tools/httpd" "$stage/devs" "$stage/libs" "$stage/Public" \
        > "$OUT/$label-boot.log" 2>&1 &
    RUNNER=$!
    set -e

    answered=no
    for _ in $(seq 1 $((TIMEOUT / 2))); do
        sleep 2
        kill -0 "$RUNNER" 2>/dev/null || break
        read -r i _ <<<"$(peer_probe "$ADDRESS" /small.txt 1 0)"
        if [ "$i" -ge 1 ]; then
            answered=yes
            break
        fi
    done

    if [ "$answered" != yes ]; then
        kill "$RUNNER" 2>/dev/null || true; wait "$RUNNER" 2>/dev/null || true
        RUNNER=""
        return 0
    fi

    read -r i ARM_HANDSHAKE_MS <<<"$(peer_probe "$ADDRESS" /small.txt "$SMALL_N" 0)"
    [ "$i" -ge 1 ] || ARM_HANDSHAKE_MS=none

    read -r i ARM_BPS <<<"$(peer_probe "$ADDRESS" /blob.bin "$BIG_N" 524288)"
    [ "$i" -ge 1 ] || ARM_BPS=none

    kill "$RUNNER" 2>/dev/null || true; wait "$RUNNER" 2>/dev/null || true
    RUNNER=""

    return 0
}

: > "$OUT/rounds.txt"

for ((r = 1; r <= ROUNDS; r++)); do
    run_arm "$BUILD_A" "02:41:4d:63:0a:$(printf '%02x' "$r")" "a-$r"
    printf 'syncost_round: round=%s arm=defended handshake_ms=%s bps=%s\n' \
           "$r" "$ARM_HANDSHAKE_MS" "$ARM_BPS" | tee -a "$OUT/rounds.txt"

    run_arm "$BUILD_B" "02:41:4d:63:0b:$(printf '%02x' "$r")" "b-$r"
    printf 'syncost_round: round=%s arm=plain handshake_ms=%s bps=%s\n' \
           "$r" "$ARM_HANDSHAKE_MS" "$ARM_BPS" | tee -a "$OUT/rounds.txt"
done

cleanup
trap - EXIT INT TERM HUP

pick() { awk -v arm="$1" -v key="$2" '
    $0 ~ ("arm=" arm " ") {
        for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == key && kv[2] != "none") print kv[2]
        }
    }' "$OUT/rounds.txt"; }

A_HS=$(pick defended handshake_ms | median)
B_HS=$(pick plain    handshake_ms | median)
A_BPS=$(pick defended bps | median)
B_BPS=$(pick plain    bps | median)

echo
echo "========================= per round ==============================="
cat "$OUT/rounds.txt"
echo "==================================================================="
echo

STATUS=pass
RC=0

if [ "$A_HS" = none ] || [ "$B_HS" = none ] ||
   [ "$A_BPS" = none ] || [ "$B_BPS" = none ]; then
    STATUS=no_verdict
    RC=3
else
    HS_CEIL=$(( B_HS * 125 / 100 + 1 ))
    BPS_FLOOR=$(( B_BPS * 90 / 100 ))
    HS_PCT=$(( (A_HS - B_HS) * 100 / (B_HS > 0 ? B_HS : 1) ))
    BPS_PCT=$(( (A_BPS - B_BPS) * 100 / (B_BPS > 0 ? B_BPS : 1) ))

    if [ "$A_HS" -gt "$HS_CEIL" ]; then
        STATUS=handshake_regressed
        RC=1
    elif [ "$A_BPS" -lt "$BPS_FLOOR" ]; then
        STATUS=throughput_regressed
        RC=1
    fi
fi

printf 'syncost: status=%s rounds=%s board=%s defended_handshake_ms=%s plain_handshake_ms=%s handshake_pct=%s defended_bps=%s plain_bps=%s throughput_pct=%s log=%s\n' \
       "$STATUS" "$ROUNDS" "$BOARD" "$A_HS" "$B_HS" "${HS_PCT:-none}" \
       "$A_BPS" "$B_BPS" "${BPS_PCT:-none}" "$OUT"

case "$STATUS" in
    no_verdict)
        echo "a round produced no figure.  Nothing was measured; the boot" >&2
        echo "logs are under $OUT." >&2 ;;
    handshake_regressed)
        echo "the defended tree's median handshake is ${A_HS}ms against" >&2
        echo "${B_HS}ms, past the 25% band.  Every inbound connection now" >&2
        echo "takes a cache entry and a keyed hash; this says that shows." >&2 ;;
    throughput_regressed)
        echo "the defended tree moved ${A_BPS} bit/s against ${B_BPS}, past" >&2
        echo "the 10% band.  Nothing in the defence touches an established" >&2
        echo "connection, so this is somewhere it was not supposed to reach." >&2 ;;
esac

exit "$RC"
