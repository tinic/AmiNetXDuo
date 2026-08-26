#!/usr/bin/env bash
#
# Profile the SAME fitz transfer against a different bsdsocket.library.
#
#   tests/perf/run-stackprof.sh -s ours|roadshow|amitcpng [-T tag] [options]
#
# NOTHING OF THEIRS IS COPIED INTO THIS REPOSITORY.  Roadshow and AmiTCP_NG are
# located at run time by path, exactly as tests/compare/run-compare.sh does it.
#
# THE PEER IS NOT playhouse2.  VMs on one Proxmox host never cross a NIC, so a
# deferred TX checksum is never computed and our stack correctly rejects the
# result.  The peer needs `ethtool -K <iface> tx off` either way.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. "$ROOT/tests/perf/peercap.sh"

STACK=""
TAG=""
PROFILE=1
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-}"
PEER="${AMINETXDUO_FITZ_PEER:-}"
PEER_DIR="${AMINETXDUO_FITZ_PEER_DIR:-}"
PEER_BIN="${AMINETXDUO_FITZ_PEER_BIN:-\$HOME/fitzsrc/fitz-serve}"
PORT="${AMINETXDUO_FITZ_PORT:-17712}"
KB=4096
CHUNK=32768
REPS=3
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RSDIR="${AMINETXDUO_CMP_ROADSHOW:-}"
NGDIR="${AMINETXDUO_CMP_AMITCPNG:-}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
MODEL=A3000
TIMEOUT=500
EXTRALIBS=""
MOUNTARGS=""
DIAG=0
CTLONLY=0
IFCONFIG=""
LOSSCAP=0
MAXLOSS=""
MAXEFF=""

while getopts "s:T:pdci:A:P:k:C:r:b:R:G:B:m:t:L:M:wW:E:" opt; do
    case "$opt" in
        s) STACK="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        p) PROFILE=0 ;;
        d) DIAG=1 ;;
        c) CTLONLY=1 ;;
        i) IFCONFIG="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        P) PORT="$OPTARG" ;;
        k) KB="$OPTARG" ;;
        C) CHUNK="$OPTARG" ;;
        r) REPS="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        R) RSDIR="$OPTARG" ;;
        G) NGDIR="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        L) EXTRALIBS="$OPTARG" ;;
        M) MOUNTARGS="$OPTARG" ;;
        w) LOSSCAP=1 ;;
        W) LOSSCAP=1; MAXLOSS="$OPTARG" ;;
        E) LOSSCAP=1; MAXEFF="$OPTARG" ;;
        *) sed -n '3,60p' "$0" >&2; exit 2 ;;
    esac
done

PEER_DIR="${PEER_DIR:-/tmp/fitzbench-share-$PORT}"

[ -n "$STACK" ] || { sed -n '3,50p' "$0" >&2; exit 2; }
[ -n "$TAG" ] || TAG="sp-$STACK$([ "$PROFILE" = 1 ] || echo -plain)$([ "$DIAG" = 0 ] || echo -diag)"

if [ -z "$PORT" ]; then
    # shellcheck source=../../tools/emu-rig-lock.sh
    . "$ROOT/tools/emu-rig-lock.sh"
    if rig_claim_port "stackprof-peer $TAG" 17000 700 2> /dev/null; then
        PORT="$RIG_PORT"
    else
        PORT=$((17000 + ($(printf %s "$TAG" | cksum | cut -d" " -f1) + $$) % 700))
    fi
fi

case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

case "$PEER" in
    *playhouse2*) echo "not playhouse2, see the header" >&2; exit 2 ;;
esac

# ---------------------------------------------------------------- the stack --

case "$STACK" in
    ours)
        LIBBSD="$BUILD/src/bsdsocket/bsdsocket.library"
        LIBUG="$BUILD/src/usergroup/usergroup.library"
        CMD_ADDIF="$BUILD/src/tools/AddNetInterface"
        CMD_STAT="$BUILD/src/tools/netstat"
        STATARGS="${AMINETXDUO_NETSTAT_ARGS:--s}"
        NOTE="AmiNetXDuo from $BUILD"
        ;;
    roadshow)
        [ -n "$RSDIR" ] && [ -d "$RSDIR" ] || {
            echo "no Roadshow: -R <dir>/Workbench or AMINETXDUO_CMP_ROADSHOW" >&2
            exit 2; }
        LIBBSD="$RSDIR/Libs/bsdsocket.library"
        LIBUG="$RSDIR/Libs/usergroup.library"
        CMD_ADDIF="$RSDIR/C/AddNetInterface"
        CMD_STAT="$RSDIR/C/GetNetStatus"
        STATARGS=""
        NOTE="Roadshow from $RSDIR"
        ;;
    amitcpng)
        [ -n "$NGDIR" ] && [ -d "$NGDIR" ] || {
            echo "no AmiTCP_NG: -G <dir>/AmiTCP_NG/data or" \
                 "AMINETXDUO_CMP_AMITCPNG" >&2; exit 2; }
        LIBBSD="$NGDIR/Libs/bsdsocket.library"
        LIBUG="$NGDIR/Libs/usergroup.library"
        CMD_ADDIF="$NGDIR/C/AddNetInterface"
        CMD_STAT="$NGDIR/C/GetNetStatus"
        STATARGS=""
        NOTE="AmiTCP_NG from $NGDIR"
        ;;
    none)
        [ "$CTLONLY" = 1 ] || {
            echo "-s none measures a machine with no stack, so it only makes" >&2
            echo "sense with -c; there is nothing to transfer over." >&2
            exit 2; }
        LIBBSD=""; LIBUG=""; CMD_ADDIF=""; CMD_STAT=""
        STATARGS=""
        NOTE="no stack at all (baseline)"
        ;;
    *) echo "unknown stack '$STACK'" >&2; exit 2 ;;
esac

if [ "$STACK" != none ]; then
    [ -f "$LIBBSD" ] || { echo "missing $LIBBSD" >&2; exit 2; }
    [ -f "$CMD_ADDIF" ] || { echo "missing $CMD_ADDIF" >&2; exit 2; }
fi

FITZ="$ROOT/build/fitz/Fitz/fitz"
BENCH="$BUILD/tests/perf/FitzBench"
SMOKE="$BUILD/src/tools/ToolsSmoke"
PROF="$BUILD/tools/profiler/Profile"
for f in "$FITZ" "$BENCH" "$SMOKE"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done
[ "$PROFILE" = 0 ] || [ -f "$PROF" ] || {
    echo "missing $PROF -- cmake --build $BUILD --target Profile" >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-$ROOT/build/a2065.device}"
[ -f "$A2065" ] || { echo "no a2065.device, set AMINETXDUO_A2065" >&2; exit 2; }

# ------------------------------------------------------------- the server ---

if [ "$CTLONLY" = 1 ] || [ "$DIAG" = 1 ]; then
    if [ "$DIAG" = 1 ]; then
        echo "==> diagnostic only: bring the interface up and report"
    else
        echo "==> control only: RAM: arm, stack up, no traffic"
    fi
elif [ -n "$PEER" ]; then
    PEER_ADDRS=$(ssh "$PEER" \
        "ip -4 -o addr show scope global 2>/dev/null | awk '{print \$4}' | cut -d/ -f1" \
        2>/dev/null)
    [ -n "$PEER_ADDRS" ] || {
        echo "$PEER did not report an address of its own; pass -A <addr>" >&2
        exit 2; }
    if [ -z "$PEER_ADDR" ]; then
        PEER_ADDR=$(printf '%s\n' "$PEER_ADDRS" | head -1)
        [ "$(printf '%s\n' "$PEER_ADDRS" | wc -l)" -eq 1 ] || {
            echo "note: $PEER holds more than one address, using $PEER_ADDR" >&2
            echo "      pass -A to choose the one the guest's bridge reaches" >&2; }
    else
        printf '%s\n' "$PEER_ADDRS" | grep -qx "$PEER_ADDR" || {
            echo "$PEER does not hold $PEER_ADDR -- the server would be staged" >&2
            echo "on one machine and the guest pointed at another.  It holds:" >&2
            printf '%s\n' "$PEER_ADDRS" | sed 's/^/  /' >&2
            exit 2; }
    fi

    PEERLOG="$ROOT/build/stackprof-$TAG-peer.log"
    ssh "$PEER" "pkill -f '[f]itz-serve .* PORT $PORT\$' || true" >/dev/null 2>&1 || true
    ssh "$PEER" "rm -rf $PEER_DIR; mkdir -p $PEER_DIR;
                 nohup $PEER_BIN $PEER_DIR PORT $PORT > /tmp/fitzbench-peer.log 2>&1 &
                 sleep 1; ps -o args= -C fitz-serve" > "$PEERLOG" 2>&1
    cat "$PEERLOG"
    cleanup() { ssh "$PEER" "pkill -f '[f]itz-serve .* PORT $PORT\$' || true" >/dev/null 2>&1 || true; }
    trap cleanup EXIT INT TERM HUP

    PEER_BOUND=0
    for _try in 1 2 3 4 5 6 7 8 9 10; do
        if ssh "$PEER" "ss -lnt 2>/dev/null | grep -q ':$PORT '" 2>/dev/null; then
            PEER_BOUND=1
            break
        fi
        sleep 1
    done
    [ "$PEER_BOUND" = "1" ] || {
        echo "the peer is not listening on $PORT after ten seconds." >&2
        echo "A fitz-serve child from an earlier run may still hold it:" >&2
        ssh "$PEER" "pgrep -af fitz-serve | head -5" >&2 2>/dev/null || true
        echo "Clear it with: ssh $PEER \"pkill -9 -f 'fitz-serve .* PORT $PORT\$'\"" >&2
        exit 2
    }
else
    [ -n "$PEER_ADDR" ] || {
        echo "no peer and no -A: give -A <addr> for a server already listening," >&2
        echo "or -H <user@host> to have this script start one" >&2; exit 2; }
    if ! timeout 4 bash -c "exec 3<>/dev/tcp/$PEER_ADDR/$PORT" 2>/dev/null; then
        cat >&2 <<EOF
Nothing is listening on $PEER_ADDR:$PORT, and no peer was given to start one.

  AMINETXDUO_FITZ_PEER=user@host       who to start fitz-serve on, over ssh

The peer must be a THIRD machine on real Ethernet; see the note at the top of
this file for why it cannot be playhouse2 or the emulator's own host.
EOF
        exit 2
    fi
    echo "==> using the fitz-serve already on $PEER_ADDR:$PORT"
fi
echo "==> $NOTE"
echo "==> fitz-serve on $PEER_ADDR:$PORT"

# ---------------------------------------------------------------- staging ---

STAGE="$ROOT/build/stackprof-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"

if [ -n "$IFCONFIG" ]; then
    [ -f "$IFCONFIG" ] || { echo "no such interface file: $IFCONFIG" >&2; exit 2; }
    cp "$IFCONFIG" "$STAGE/devs/NetInterfaces/eth0"
    echo "==> interface configuration: $IFCONFIG"
fi

cp "$A2065" "$STAGE/devs/a2065.device"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
[ -z "${AMINETXDUO_EXTRA_DRIVER:-}" ] || {
    cp "$AMINETXDUO_EXTRA_DRIVER" "$STAGE/devs/$(basename "$AMINETXDUO_EXTRA_DRIVER")"
    mkdir -p "$STAGE/devs/Networks"
    cp "$AMINETXDUO_EXTRA_DRIVER" "$STAGE/devs/Networks/$(basename "$AMINETXDUO_EXTRA_DRIVER")"
}
[ -z "${AMINETXDUO_IFCONFIG:-}" ] || \
    cp "$AMINETXDUO_IFCONFIG" "$STAGE/devs/NetInterfaces/eth0"

if [ -n "$LIBBSD" ]; then cp "$LIBBSD" "$STAGE/libs/bsdsocket.library"; fi
if [ -n "$LIBUG" ] && [ -f "$LIBUG" ]; then
    cp "$LIBUG" "$STAGE/libs/usergroup.library"
else
    echo "==> $STACK ships no usergroup.library; staging none"
fi
[ -z "$EXTRALIBS" ] || cp -R "$EXTRALIBS"/* "$STAGE/libs/"

if [ -n "$CMD_ADDIF" ]; then cp "$CMD_ADDIF" "$STAGE/AddNetInterface"; fi
if [ -n "$CMD_STAT" ] && [ -f "$CMD_STAT" ]; then cp "$CMD_STAT" "$STAGE/NetStat"; fi
cp "$FITZ"  "$STAGE/fitz"
cp "$BENCH" "$STAGE/FitzBench"
[ "$PROFILE" = 0 ] || cp "$PROF" "$STAGE/Profile"

if [ "$STACK" = amitcpng ] && [ -d "$NGDIR/db" ]; then
    mkdir -p "$STAGE/AmiTCP"
    cp -R "$NGDIR/db" "$STAGE/AmiTCP/"
fi

{
    if [ "$STACK" != none ]; then
        echo "SYS:AddNetInterface DEVS:NetInterfaces/eth0"
        echo "wait 6"
    fi
    # A `{ }` group is not a subshell, so an `exit` here would end the run
    # rather than the plan.  DIAG stops by writing nothing more.
    PROF_QUIET=QUIET
    [ -z "${AMINETXDUO_PROF_VERBOSE:-}" ] || PROF_QUIET=
    PROF_PREFIX="SYS:Profile $PROF_QUIET OUT=DH0:fitz.prof FOLDED=DH0:fitz.folded${AMINETXDUO_PROF_WATCH:+ WATCH=$AMINETXDUO_PROF_WATCH}"

    if [ "$DIAG" = 1 ]; then
        [ ! -f "$STAGE/NetStat" ] || echo "SYS:NetStat $STATARGS"
    elif [ "$CTLONLY" = 1 ]; then
        if [ "$PROFILE" = 1 ]; then
            echo "$PROF_PREFIX SYS:FitzBench RAM: KB=$KB CHUNK=$CHUNK REPS=$REPS"
        else
            echo "SYS:FitzBench RAM: KB=$KB CHUNK=$CHUNK REPS=$REPS"
        fi
    else
        echo "&SYS:fitz mount $PEER_ADDR:$PORT FITZ:${MOUNTARGS:+ $MOUNTARGS}"
        echo "wait 10"
        [ ! -f "$STAGE/NetStat" ] || echo "SYS:NetStat $STATARGS"
        if [ "$PROFILE" = 1 ]; then
            echo "$PROF_PREFIX SYS:FitzBench FITZ: KB=$KB CHUNK=$CHUNK REPS=$REPS"
        else
            echo "SYS:FitzBench FITZ: KB=$KB CHUNK=$CHUNK REPS=$REPS"
        fi
        [ ! -f "$STAGE/NetStat" ] || echo "SYS:NetStat $STATARGS"
        echo "SYS:FitzBench RAM: KB=$KB CHUNK=$CHUNK REPS=$REPS"
    fi
} > "$STAGE/commands.txt"

echo "==> plan"
sed 's/^/    /' "$STAGE/commands.txt"

# -------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="$TAG"
STAGED=("$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs"
        "$STAGE/fitz" "$STAGE/FitzBench")
[ -f "$STAGE/AddNetInterface" ] && STAGED+=("$STAGE/AddNetInterface")
[ -f "$STAGE/NetStat" ] && STAGED+=("$STAGE/NetStat")
[ "$PROFILE" = 0 ] || STAGED+=("$STAGE/Profile")
[ -d "$STAGE/AmiTCP" ] && STAGED+=("$STAGE/AmiTCP")

CAPDIR="$ROOT/build/losscap-$TAG"
CAPTURING=0
if [ "$LOSSCAP" = "1" ] && [ "$DIAG" = "0" ]; then
    if [ -z "$PEER" ]; then
        echo "-w needs an ssh-able peer: the capture is taken there, so set" \
             "AMINETXDUO_FITZ_PEER" >&2
        exit 2
    fi
    peercap_start "$PEER" "$PORT" "$CAPDIR" "$TAG" || exit 2
    CAPTURING=1
fi

set +e
"$ROOT/tools/amiberry-run.sh" -N "${AMINETXDUO_BOARD:-a2065}" -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" "${STAGED[@]}"
RUN_RC=$?
set -e

CAPTURE_LOST=0
if [ "$CAPTURING" = "1" ]; then
    peercap_stop "$PEER" "$CAPDIR" "$TAG" || { CAPTURING=0; CAPTURE_LOST=1; }
fi

HD="$ROOT/build/amiberry-testhd-$TAG"
REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: no $REPORT (rc=$RUN_RC)" >&2; exit 1; }

if [ "$RUN_RC" != "0" ]; then
    echo "FAIL: the emulator run failed (rc=$RUN_RC); the profile it left" >&2
    echo "describes a run its own harness refused." >&2
    exit "$RUN_RC"
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo
echo "==> results ($STACK, $MODEL, $KB KB, chunk $CHUNK, $REPS reps)"
grep "fitzbench: RESULT\|fitzbench: file=" "$REPORT" | sed 's/^/    /' || true
[ ! -f "$HD/fitz.prof" ] || echo "==> profile: $HD/fitz.prof"

LOSS_RC=0
if [ "$CAPTURING" = "1" ]; then
    LOSSARGS=(--per-phase)
    [ -z "$MAXLOSS" ] || LOSSARGS+=(--max-loss "$MAXLOSS")
    [ -z "$MAXEFF" ]  || LOSSARGS+=(--max-effective-loss "$MAXEFF")
    set +e
    peercap_report "$CAPDIR" "$TAG" "${LOSSARGS[@]}"
    LOSS_RC=$?
    set -e
elif [ "$CAPTURE_LOST" = "1" ]; then
    echo "the capture started and did not come back: no loss rate from this" >&2
    echo "run, and any -W/-E asked for did not run." >&2
    LOSS_RC=2
fi

exit "$LOSS_RC"
