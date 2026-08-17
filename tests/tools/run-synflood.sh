#!/usr/bin/env bash
#
# A SYN FLOOD MUST NOT STOP THE MACHINE ANSWERING.
#
#   tests/tools/run-synflood.sh -P PEERHOST [-B IFACE] [-b BUILDDIR]
#                               [-c BOARD[,BOARD...]] [-t SECONDS] [-p PORT]
#                               [-T TAG] [-u]
#
# WHAT IT PROVES
#
#   The guest runs `httpd` on the LAN.  The peer -- a third machine -- floods
#   its listening port with SYNs from thousands of forged addresses that never
#   answer, the attack of RFC 4987.  Throughout the flood a legitimate client
#   on this host completes a real handshake and a GET, and the run reports how
#   long that took and how many SYNs the flood delivered.
#
#   A stack that pins a socket or a packet per SYN stops answering: the flood
#   fills its backlog and its packet pool and the legitimate GET times out.  A
#   stack that holds 80 bytes per half-open connection and answers past its
#   cache with a stateless cookie does not, and the GET goes through while the
#   flood runs.
#
#   MEASURED, every card under Amiberry on playhouse3, flooded from
#   playhouse4 at 100 SYN/s: pre-fix, 0 of 5 legitimate GETs complete under
#   the flood; defended, 5 of 5, at a handshake within reach of the quiet one.
#   The rate is chosen to exhaust half-open STATE without saturating the
#   emulated NIC; see FLOOD_PPS below.
#
#   -u ("unprotected") inverts the verdict.  It is the same run against a tree
#   built from the submodule pin BEFORE the defence -- d1358950, the parent of
#   amiga-syn-defence -- and it PASSES when the legitimate GET FAILS under the
#   flood, which is the proof that the defence is what changed the outcome and
#   not something else.  Point -b at that build.  Without -u the defended tree
#   is expected to keep answering.
#
# WHY THE FORGED SOURCES MUST BE A DEAD RANGE
#
#   synflood.py draws every source from 10.99.0.0/16, a range nothing on the
#   LAN owns.  A SYN from a live host would get the guest's SYN-ACK and RST it,
#   which completes the exchange this end and is a different test.  A dead
#   range leaves every SYN-ACK unanswered, which is the half-open connection
#   the defence is measured against.
#
# THE MEASUREMENT IS THE LEGITIMATE CLIENT, NOT THE FLOOD
#
#   handshake_ms is a real connect-plus-GET FROM THE PEER to the guest's
#   httpd, timed on the peer (tests/tools/synprobe.py), taken three times
#   before the flood and five times during it.  It runs on the peer because
#   the emulator host cannot reach its own bridged guest, and it is timed
#   there because an ssh round trip inside a handshake figure would dwarf the
#   handshake.  The verdict is whether the during-flood GETs still return 200,
#   and whether their median time is within reach of the quiet one -- a
#   legitimate client must not see a slower or less reliable handshake, which
#   is the whole point of a cache over a drop.
#
# EVERY CARD, NOT ONE
#
#   The board list, the model each one needs and the address each one gets come
#   from tests/tools/cards.sh, the same table run-cardsweep.sh and
#   run-cardsweep6.sh read.  One card proving a defence proves it for one
#   driver's receive path: a flood is delivered by the SANA-II driver, and the
#   rate at which a card hands packets up is exactly what differs between them.
#   -c takes a comma-separated subset for a quick run; the gate takes none and
#   sweeps all of them.  The verdict is per card AND overall, so one card
#   falling over cannot be lost in an aggregate.
#
# BRIDGED, AND NOT SLIRP; A STATIC ADDRESS
#
#   -B ens18 puts the guest on the host's LAN with its own MAC, the only way
#   the peer can reach it.  amiberry-run.sh refuses the run if the backend
#   silently fell back to SLIRP.  The address is static and known before boot,
#   because the peer floods it and the client fetches from it.
#
# EXIT CODES
#
#   0  EVERY card kept answering under the flood (or, with -u, every card
#      stopped answering, as expected)
#   1  a card stopped answering (or, with -u, one did not, so the flood was
#      not strong enough there to prove anything)
#   2  the rig could not run: no peer, no capability tooling, no flood
#      delivered, or no card produced a verdict
#   3  no card came up at all
#
# The a2065.device driver is not ours to ship: AMINETXDUO_A2065=<path>, or a
# copy in build/a2065.device.  The peer needs a python with cap_net_raw; see
# synflood.py.
#
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

# The flood: how hard, and from where.  The default is 100 SYNs a second, and
# the reason it is not higher is measured, not guessed.
#
# THE DEFENCE IS ABOUT STATE, AND 100/s EXHAUSTS IT MANY TIMES OVER.  The
# thing that used to fall over is a queue eight deep and a pool of 512
# packets: 100 new half-open connections a second, each held for the 20-second
# cache timeout, is two thousand outstanding, four times the cache and two
# hundred and fifty times the old backlog.  A pre-fix guest stops answering at
# this rate within the first second (measured: 0 of 5 legitimate GETs
# complete), and a defended one does not (5 of 5).  That is the property, and
# this rate proves it on every card.
#
# HIGHER RATES TEST THE EMULATED NIC, NOT THE DEFENCE.  At 2000/s the a2065's
# emulated LANCE and the 68020 spend the run receiving SYNs and transmitting
# SYN-ACKs, and a legitimate connection is dropped at the wire before the
# stack sees it -- a defended guest falls to 2 of 5 there, on a limit that has
# nothing to do with half-open state.  Raise AMINETXDUO_SYNFLOOD_PPS to
# characterise a card's packet ceiling; leave it here to measure the defence.
FLOOD_PPS="${AMINETXDUO_SYNFLOOD_PPS:-100}"
FLOOD_NET="${AMINETXDUO_SYNFLOOD_NET:-10.99.0.0/16}"
FLOOD_SECS="${AMINETXDUO_SYNFLOOD_SECS:-40}"

# A python on the peer carrying cap_net_raw, the tc-cap / tcpdump-cap pattern.
PYCAP="${AMINETXDUO_PEER_PYCAP:-\$HOME/python3-cap}"

# The MAC head for this sweep, so a flood run and a cardsweep are never one
# machine to the LAN.  The tail comes from the card table.
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

# The card table, shared with run-cardsweep.sh.  CARDS and cards_rows() come
# from there; one table, so "every card" means the same thing in both.
. "$ROOT/tests/tools/cards.sh"

# sana2_driver_for, sana2_local_driver and sana2_stage: the one place that
# knows which driver a board needs and where in DEVS: it has to land.
. "$ROOT/tools/sana2-stage.sh"

# --------------------------------------------------------------- preflight ---

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

# --------------------------------------------------------------- one card ---
#
# Sets CARD_STATUS, CARD_QUIET_MS, CARD_FLOOD_MS, CARD_FLOOD_OK,
# CARD_FLOOD_TRIES and CARD_SENT.  Never exits: a card that cannot come up is
# one card's verdict, not the sweep's.

CARD_STATUS=fail
CARD_QUIET_MS=none
CARD_FLOOD_MS=none
CARD_FLOOD_OK=0
CARD_FLOOD_TRIES=0
CARD_SENT=0
CARD_DRIVER=""

# A legitimate client, RUN ON THE PEER and timed there.  The emulator host
# cannot reach its own bridged guest (docs/RESEARCH.md 63), and an ssh round
# trip inside a handshake figure would dwarf the handshake, so the probe runs
# where the connection is opened.  Echoes "ok_median" e.g. "3 7", the count
# that returned 200 and their median millisecond time, or "0 none".
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

    # Which driver this board needs, and whether the asset store has it.  A
    # board with no driver is still booted: the card is in the machine with
    # nothing able to open it, and that is a result, not a skip -- but it is
    # not a flood result, so it is reported as one.
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

    # Rewrites the DEVICE line and puts the driver where that board's driver
    # has to live, which differs between the a2065 and everything else.
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

    # Poll rather than sleeping a guessed amount: a boot is 30-90 s and the
    # interface comes up under the server.
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

    # Three quiet handshakes from the peer, the baseline the flood is judged
    # against.
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

    # Let the flood get going, so the measurement is taken with the cache under
    # pressure and not before it.
    sleep 5

    # Five legitimate connections DURING the flood, from the peer.  This is
    # the claim: a real client completes a handshake and a GET while the port
    # is under attack.
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

    # The flood has to have actually delivered, or nothing was tested on this
    # card.  A few thousand is a flood; a handful is a broken raw socket.
    if [ "$CARD_SENT" -lt 1000 ]; then
        CARD_STATUS=skip_flood
    elif [ "$UNPROTECTED" = yes ]; then
        if [ "$CARD_FLOOD_OK" -eq 0 ]; then
            CARD_STATUS=unprotected_fell_over
        else
            CARD_STATUS=unprotected_survived
        fi
    else
        # Every flooded GET has to return 200, and its time has to be within
        # reach of the quiet one: a legitimate client must not see a slower
        # handshake under attack.  Four times the quiet time plus a floor --
        # generous, because the claim is "still works", but not so generous
        # that a near-timeout passes.
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

# ------------------------------------------------------------- the sweep ---

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
