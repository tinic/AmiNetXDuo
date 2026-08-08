#!/usr/bin/env bash
#
# Profile the SAME fitz transfer against a different bsdsocket.library.
#
#   tests/perf/run-stackprof.sh -s ours|roadshow|amitcpng [-T tag] [options]
#
# WHY THIS EXISTS
#
#   tests/perf/run-fitzbench.sh measures throughput and can swap in Roadshow;
#   tools/profiler/Profile says where the CPU went.  Neither on its own answers
#   the question a user's "stack X is faster" raises, which is whether the
#   difference is CPU work at all.  A stack that is 60% idle on a read is not
#   losing to anything the CPU is doing.
#
#   So: one bridged Amiberry rig, one peer, one fitz binary, one FitzBench, one
#   set of mount parameters, and the library as the only variable.  The stack
#   brings itself up with its OWN AddNetInterface, because a stack that needed
#   somebody else's would not be the stack under test.
#
#   NOTHING OF THEIRS IS COPIED INTO THIS REPOSITORY.  Roadshow and AmiTCP_NG
#   are located at run time by path, exactly as tests/compare/run-compare.sh
#   does it.
#
# THE PEER IS NOT playhouse2.  VMs on one Proxmox host never cross a NIC, so a
# deferred TX checksum is never computed and our stack correctly rejects the
# result, which reads as "6 bad packets, 6 checksum errors" and looks like
# our defect.  The peer needs `ethtool -K <iface> tx off` either way.
#
# OPTIONS
#
#   -s STACK    ours | roadshow | amitcpng | none     (required)
#               `none` stages no bsdsocket.library and brings nothing
#               up: the baseline that says whether a gap between two
#               stacks is either stack's doing.  Requires -c.
#   -T TAG      run tag; results land in build/amiberry-testhd-<tag>/
#   -p          PLAIN: no profiler, throughput only.  This is the control that
#               says whether the sampler moved the figure it is explaining.
#   -d          DIAG: bring the interface up and print the stack's status, and
#               stop there.  A stack that will not come up is diagnosed in
#               thirty seconds rather than inside a workload whose failure
#               mode is a timeout.
#   -c          CONTROL ONLY: the RAM: arm alone, stack up, no traffic.
#               The only CPU-bound number here -- a wire transfer idles
#               more than half the run and so prices the rig, not the
#               code.  Needs no peer.
#   -i FILE     use FILE as DEVS:NetInterfaces/eth0 instead of the tree's.  A
#               stack should be measured with a configuration it agrees is
#               well formed, because "the interface would not come up" has two
#               causes that look identical from outside.
#   -A ADDR     peer address (default 192.168.1.160)
#   -P PORT     peer port (default 17712)
#   -k KB       FitzBench file size (default 4096)
#   -C CHUNK    FitzBench chunk (default 32768)
#   -r REPS     FitzBench reps (default 3)
#   -b DIR      build directory for `ours` (default build/cm)
#   -R DIR      Roadshow Workbench/ directory
#   -G DIR      AmiTCP_NG data/ directory (the one holding Libs/ and C/)
#   -B IFACE    host NIC to bridge onto (default ens18)
#   -m MODEL    emulator profile (default A3000)
#   -t SECS     timeout (default 500)
#   -L DIR      extra files staged into LIBS:
#
#   AMINETXDUO_NETSTAT_ARGS overrides the arguments NetStat is run with,
#   which is how a specific query gets exercised against a live mount.
#   -w          capture the peer's own egress and report the inbound loss
#               rate from it with tests/perf/lossrate.py.  This is the
#               comparison that survives a rig: throughput is downstream of
#               everything and moves on its own, while retransmissions over
#               data segments sent is one number taken where the counts are
#               exact.  It also separates a segment that was LOST from one
#               that merely arrived late, a distinction that decides which
#               of two stacks is actually doing better and that a rate cannot
#               make.
#   -W PCT      -w, and fail above PCT raw loss
#   -E PCT      -w, and fail above PCT with spurious retransmissions removed
#   -M "ARGS"   extra arguments to `fitz mount`, `-M "BUFS 262144"`.  This
#               is NOT part of a matched stack comparison: it changes the
#               client, so a run using it is a diagnostic arm of its own and
#               has to be labelled as one.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. "$ROOT/tests/perf/peercap.sh"

STACK=""
TAG=""
PROFILE=1
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-192.168.1.160}"
PEER="${AMINETXDUO_FITZ_PEER:-}"
PEER_DIR="${AMINETXDUO_FITZ_PEER_DIR:-/tmp/fitzbench-share}"
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

[ -n "$STACK" ] || { sed -n '3,50p' "$0" >&2; exit 2; }
[ -n "$TAG" ] || TAG="sp-$STACK$([ "$PROFILE" = 1 ] || echo -plain)$([ "$DIAG" = 0 ] || echo -diag)"

# The peer port, when nobody asked for one.  A run that shares a port with
# another run does not fail: it reports a number measured against somebody
# else's traffic, which is worse.  tools/amiberry-run.sh derives its own the
# same way, and TAG is only known here.
if [ -z "$PORT" ]; then
    # TAG and the pid: two runs with the same tag -- both instances asking
    # for -s ours with no -T -- would otherwise still share a port.
    PORT=$((17000 + ($(printf %s "$TAG" | cksum | cut -d" " -f1) + $$) % 700))
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
        # The baseline: no bsdsocket.library at all, nothing brought up.  Only
        # meaningful with -c, and it is what says whether a gap between two
        # stacks is either stack's doing or the rig's.
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
    # Neither -c nor -d transfers anything over the wire, so there is no server
    # to start and nothing to check.  -d in particular exists to bring an
    # interface up and stop, which is exactly the run that wants no peer, and
    # refusing it for the want of one made the diagnostic unusable.
    if [ "$DIAG" = 1 ]; then
        echo "==> diagnostic only: bring the interface up and report"
    else
        echo "==> control only: RAM: arm, stack up, no traffic"
    fi
elif [ -n "$PEER" ]; then
    PEERLOG="$ROOT/build/stackprof-$TAG-peer.log"
    # The bracket is not decoration: pkill -f matches the remote shell's own
    # command line, so an unbracketed pattern kills the connection issuing it.
    ssh "$PEER" "pkill -f '[f]itz-serve .* PORT $PORT\$' || true" >/dev/null 2>&1 || true
    ssh "$PEER" "rm -rf $PEER_DIR; mkdir -p $PEER_DIR;
                 nohup $PEER_BIN $PEER_DIR PORT $PORT > /tmp/fitzbench-peer.log 2>&1 &
                 sleep 1; ps -o args= -C fitz-serve" > "$PEERLOG" 2>&1
    cat "$PEERLOG"
    cleanup() { ssh "$PEER" "pkill -f '[f]itz-serve .* PORT $PORT\$' || true" >/dev/null 2>&1 || true; }
    trap cleanup EXIT INT TERM HUP

    # A forked child outlives the TERM its parent took and keeps the port, so
    # the server started just above may never have bound it.  Nothing later in
    # the run distinguishes that from a stack that cannot transfer.
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
    # No -H and no AMINETXDUO_FITZ_PEER: nothing here starts a server, so the
    # only thing that makes the run mean anything is one already listening.
    #
    # The check above is thorough and was unreachable without a peer, so the
    # run booted the emulator, brought the interface up, mounted nothing and
    # reported "RESULT read FAILED" -- a stack verdict for a missing server,
    # indistinguishable from a stack that cannot transfer.
    if ! timeout 4 bash -c "exec 3<>/dev/tcp/$PEER_ADDR/$PORT" 2>/dev/null; then
        cat >&2 <<EOF
Nothing is listening on $PEER_ADDR:$PORT, and no peer was given to start one.

  -H user@host                        start fitz-serve there over ssh
  AMINETXDUO_FITZ_PEER=user@host       the same, from the environment

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

# The device goes in BOTH places: ours opens it out of DEVS:, both foreign
# stacks look in DEVS:Networks/.  One driver in two directories removes a
# variable that would otherwise look like a bug in somebody's SANA-II code.
if [ -n "$IFCONFIG" ]; then
    [ -f "$IFCONFIG" ] || { echo "no such interface file: $IFCONFIG" >&2; exit 2; }
    cp "$IFCONFIG" "$STAGE/devs/NetInterfaces/eth0"
    echo "==> interface configuration: $IFCONFIG"
fi

cp "$A2065" "$STAGE/devs/a2065.device"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"

if [ -n "$LIBBSD" ]; then cp "$LIBBSD" "$STAGE/libs/bsdsocket.library"; fi
if [ -n "$LIBUG" ] && [ -f "$LIBUG" ]; then
    cp "$LIBUG" "$STAGE/libs/usergroup.library"
else
    echo "==> $STACK ships no usergroup.library; staging none"
fi
[ -z "$EXTRALIBS" ] || cp -R "$EXTRALIBS"/* "$STAGE/libs/"

if [ -n "$CMD_ADDIF" ]; then cp "$CMD_ADDIF" "$STAGE/AddNetInterface"; fi
# Optional: a stack that ships no status tool is still measurable, and the
# plan tests for NetStat before naming it.  As an && list this aborted the
# whole run under set -e, with no message, for a file nothing needed.
if [ -n "$CMD_STAT" ] && [ -f "$CMD_STAT" ]; then cp "$CMD_STAT" "$STAGE/NetStat"; fi
cp "$FITZ"  "$STAGE/fitz"
cp "$BENCH" "$STAGE/FitzBench"
[ "$PROFILE" = 0 ] || cp "$PROF" "$STAGE/Profile"

# AmiTCP_NG reads its configuration through an AmiTCP: assign and falls back
# to SYS:AmiTCP, which a bare directory hard drive can supply without the
# assign the boot shell does not make.
if [ "$STACK" = amitcpng ] && [ -d "$NGDIR/db" ]; then
    mkdir -p "$STAGE/AmiTCP"
    cp -R "$NGDIR/db" "$STAGE/AmiTCP/"
fi

# `&` is SYS_Asynch: a fitz mount stays resident as a DOS handler and never
# returns, so the line after it would never run otherwise.  Profile wraps
# FitzBench rather than the mount, because the sampler records every task
# anyway and a handler that never returns never writes a profile.
{
    if [ "$STACK" != none ]; then
        echo "SYS:AddNetInterface DEVS:NetInterfaces/eth0"
        echo "wait 6"
    fi
    # A `{ }` group is not a subshell, so an `exit` here would end the run
    # rather than the plan.  DIAG stops by writing nothing more.
    # AMINETXDUO_PROF_WATCH=<library>/<hexoff>/<hexlen> asks the sampler for a
    # caller window over that range; see prof.h.  AMINETXDUO_PROF_VERBOSE=1
    # drops QUIET so Profile reports each stage, for diagnosing a run that
    # produces no profile.
    PROF_QUIET=QUIET
    [ -z "${AMINETXDUO_PROF_VERBOSE:-}" ] || PROF_QUIET=
    PROF_PREFIX="SYS:Profile $PROF_QUIET OUT=DH0:fitz.prof FOLDED=DH0:fitz.folded${AMINETXDUO_PROF_WATCH:+ WATCH=$AMINETXDUO_PROF_WATCH}"

    if [ "$DIAG" = 1 ]; then
        [ ! -f "$STAGE/NetStat" ] || echo "SYS:NetStat $STATARGS"
    elif [ "$CTLONLY" = 1 ]; then
        # The RAM: arm alone: the stack is up, its threads are running and no
        # traffic passes.  This is the harness's only CPU-BOUND number.  A
        # transfer over the wire idles more than half the run waiting on the
        # link, so it prices the rig; what is left after this one is cycles.
        # AddNetInterface still runs, because a library nobody opened costs
        # nothing and measuring that would answer a different question.
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
    peercap_start "$PEER" "$PORT" "$CAPDIR" "$TAG" && CAPTURING=1
fi

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" "${STAGED[@]}"
RUN_RC=$?
set -e

[ "$CAPTURING" = "1" ] && { peercap_stop "$PEER" "$CAPDIR" "$TAG" || CAPTURING=0; }

HD="$ROOT/build/amiberry-testhd-$TAG"
REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: no $REPORT (rc=$RUN_RC)" >&2; exit 1; }

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
fi

exit "$LOSS_RC"
