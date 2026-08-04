#!/usr/bin/env bash
#
# The four-process Fitz stress test, on a bridged emulator against a real
# fileserver.
#
#   tests/stress/run-fitzstress.sh [-H user@host] [-A addr] [-m MODEL]
#                                  [-t SECONDS] [-s SMALL_KB] [-g BIG_KB]
#                                  [-S SAMPLE] [-w MASK] [-T TAG] [-b BUILD]
#                                  [-B IFACE] [-N BOARD] [-p PORT] [-M RAM_MB]
#                                  [-D DEADLINE] [-r] [-k]
#
# WHAT IT RUNS
#
#   host    fitz-serve <share> PORT <port>          on a THIRD machine
#   guest   fitz mount <addr>:<port> FITZ:          one TCP connection
#   guest   FitzStress                              four processes on that one
#
#   1  the share to RAM:, in a loop
#   2  RAM: to the share, in a loop
#   3  a large file, share -> hard disk -> share, alternating
#   4  a system tree to the share, cloned, checked with Fitz's comparetree
#
#   THE SINGLE CONNECTION IS THE TEST.  Every other stress harness in this tree
#   opens a socket per workload; this one opens none at all.  The four
#   processes are DOS clients of one Fitz handler, which owns one socket, so
#   all four workloads are interleaved into one send queue, one receive window
#   and one retransmit queue at DOS-packet granularity.  A defect that only
#   appears under that interleaving is unreachable with four connections.  The
#   run asserts it: `netstat -a` before and after must show exactly one
#   established connection to the server, and `ss` on the peer must agree.
#
# WHY THIS MACHINE AND NOT THE HOST
#
#   The peer must be a THIRD machine.  A frame the emulator's own host sends to
#   the guest's MAC leaves its NIC and never comes back to that NIC's pcap
#   capture, so a server on the host is unreachable from the guest while being
#   reachable from everywhere else (docs/RESEARCH.md 63).  It must also not be
#   an LXC container on a veth: its SYN-ACK carries an uncomputed TX-offload
#   checksum that no NIC fixes up, our stack rejects it correctly, and the run
#   reads as our defect.  This script checks both before it starts.
#
# TWO CLOCKS
#
#   The workload's clock is the GUEST's.  Under this load the emulated A3000
#   runs at half real time, measured, twice, at exactly 0.5, so -t 5400
#   costs about three hours of wall clock.  The emulator deadline is sized for
#   that (2x plus a margin) and -D overrides it; a deadline that assumed 1:1
#   killed a run two thirds of the way through and made a healthy machine
#   report as one that never finished.
#
# SIZES
#
#   -s and -g are the two file sizes in KB.  Their RATIO is the thing to keep
#   (1 : 10); the absolute sizes are bounded by RAM:, workers 1 and 2 hold a
#   copy each, so RAM: alone needs 2 x -s, and by the guest's memory.  -M
#   sets Zorro III Fast RAM, which is what makes the sizes in the original
#   recipe reachable at all.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

PEER="${AMINETXDUO_FITZ_PEER:-}"
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-}"
# $HOME, expanded on the PEER: /tmp there is a 2 GB tmpfs, and a run
# of hours moves several gigabytes through the share.
PEER_DIR="${AMINETXDUO_FITZ_PEER_DIR:-\$HOME/fitzstress-share}"
MODEL=A3000
BUILD="${AMINETXDUO_BUILD:-build/cm}"
SECONDS_RUN=1800
SMALL_KB=10240
BIG_KB=102400
SAMPLE=30
WORKERS=15
COMPARE_EVERY=0
TAG="${AMINETXDUO_RUN_TAG:-fitzstress}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BOARD=a2065
PORT="${AMINETXDUO_FITZ_PORT:-17713}"
RAM_MB=64
DEADLINE=""
RELEASED=0
KEEP=0

while getopts "H:A:m:t:s:g:S:w:c:T:b:B:N:p:M:D:rk" opt; do
    case "$opt" in
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) SECONDS_RUN="$OPTARG" ;;
        s) SMALL_KB="$OPTARG" ;;
        g) BIG_KB="$OPTARG" ;;
        S) SAMPLE="$OPTARG" ;;
        w) WORKERS="$OPTARG" ;;
        c) COMPARE_EVERY="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        M) RAM_MB="$OPTARG" ;;
        D) DEADLINE="$OPTARG" ;;
        r) RELEASED=1 ;;
        k) KEEP=1 ;;
        *) echo "usage: $0 [-H user@host] [-A addr] [-m model] [-t secs]" \
                "[-s small_kb] [-g big_kb] [-S sample] [-w mask] [-c every]" \
                "[-T tag] [-b build] [-B iface] [-N board] [-p port]" \
                "[-M ram_mb] [-D deadline] [-r] [-k]" >&2
           exit 2 ;;
    esac
done

[ -n "$PEER" ] && [ -n "$PEER_ADDR" ] || {
    echo "set -H user@host and -A addr (or AMINETXDUO_FITZ_PEER" \
         "and AMINETXDUO_FITZ_PEER_ADDR): a third machine on real hardware," >&2
    echo "not this emulator's host and not an LXC container" >&2
    exit 2
}

OUT="$ROOT/build/stress"
TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$OUT/FitzStress" "$OUT/comparetree" "$BSD" \
         "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree and run" \
                          "tests/stress/build.sh" >&2; exit 2; }
done

if [ "$RELEASED" = "1" ]; then
    AMIGA_FITZ="$OUT/fitz-release"
else
    AMIGA_FITZ="$OUT/fitz-debug"
fi
[ -f "$AMIGA_FITZ" ] || { echo "missing $AMIGA_FITZ" >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

# ------------------------------------------------------------- the peer ----
#
# TX checksum offload on the peer is not a preference.  With it on, the
# server's segments leave with an uncomputed checksum, our stack rejects them
# correctly, and the run reads as "connection made, N bad packets, N
# checksum errors" and no transfer, which looks exactly like our defect.

echo "==> peer $PEER ($PEER_ADDR)"

OFFLOAD=$(ssh "$PEER" "/sbin/ethtool -k \$(ip -o -4 route show to default |
                       awk '{print \$5}') 2>/dev/null |
                       grep -E '^tx-checksumming' | awk '{print \$2}'" || true)
if [ "$OFFLOAD" != "off" ]; then
    echo "!! the peer's TX checksum offload is '$OFFLOAD', not 'off'." >&2
    echo "!! Every segment it sends will arrive with an uncomputed checksum" >&2
    echo "!! and the run will read as our defect.  On the peer:" >&2
    echo "!!     sudo ethtool -K <iface> tx off gso off tso off" >&2
    exit 2
fi
echo "==> peer TX checksum offload: off"

PEERLOG="$ROOT/build/stress-$TAG-peer.log"

# The bracket in the pattern is not decoration: pkill -f matches the remote
# shell's own command line, which contains the pattern, so an unbracketed one
# kills the connection that issued it.
#
# The share is in the pattern too, so that a second run against the same peer
# kills its own server and not somebody else's: a bare '[f]itz-serve' takes
# down every run on that machine, including a three-hour one an hour in.
KILLPAT="[f]itz-serve $PEER_DIR "
ssh "$PEER" "pkill -f \"$KILLPAT\" || true" >/dev/null 2>&1 || true

# The server is the peer's own build, not one shipped from here: build/fitz
# travels between a Mac and two Linux boxes and the binary travels with it.
PEER_BIN="${AMINETXDUO_FITZ_PEER_BIN:-\$HOME/fitzsrc/fitz-serve}"

scp -q "$ROOT/tests/stress/pattern.py" "$ROOT/tests/stress/treecheck.py" \
       "$PEER:/tmp/" || {
    echo "could not stage the checkers on $PEER" >&2; exit 2; }

echo "==> seeding the share (${SMALL_KB} KB + ${BIG_KB} KB of pattern)"
ssh "$PEER" "set -e
    rm -rf $PEER_DIR; mkdir -p $PEER_DIR
    python3 /tmp/pattern.py write $PEER_DIR/down.bin $((SMALL_KB * 1024))
    python3 /tmp/pattern.py write $PEER_DIR/big.bin  $((BIG_KB * 1024))
    ls -l $PEER_DIR"

ssh "$PEER" "nohup $PEER_BIN $PEER_DIR PORT $PORT \
             > /tmp/fitzstress-peer.log 2>&1 &
             sleep 1; ps -o args= -C fitz-serve" > "$PEERLOG" 2>&1 || true
cat "$PEERLOG"
grep -q fitz-serve "$PEERLOG" || {
    echo "!! fitz-serve is not running on $PEER" >&2
    ssh "$PEER" "cat /tmp/fitzstress-peer.log" >&2 || true
    exit 2
}

cleanup_peer() {
    ssh "$PEER" "pkill -f \"$KILLPAT\" || true" >/dev/null 2>&1 || true
}
trap cleanup_peer EXIT INT TERM HUP

echo "==> fitz-serve on $PEER_ADDR:$PORT sharing $PEER_DIR"

# ------------------------------------------------------------- the tree ----

TREE="$ROOT/build/stress-tree"
if [ ! -d "$TREE" ]; then
    # All six disks of a 3.1 set, each into a drawer of its own: what comes
    # out is a Workbench installation, which is what the recipe copies.
    ADFS=()
    for a in "${AMINETXDUO_WB_ADFS:-$HOME/amiga-assets/wb/wb3.1}"/*.adf; do
        [ -f "$a" ] && ADFS+=("$a")
    done
    python3 "$ROOT/tests/stress/mktree.py" "$TREE" "${ADFS[@]+"${ADFS[@]}"}"
fi
TREE_FILES=$(find "$TREE" -type f | wc -l | tr -d ' ')
TREE_BYTES=$(find "$TREE" -type f -print0 | xargs -0 wc -c |
             awk 'END { print $1 + 0 }')
echo "==> tree: $TREE_FILES files, $TREE_BYTES bytes"

# ------------------------------------------------------------- staging -----

STAGE="$ROOT/build/stress-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/netstat"         "$STAGE/netstat"
cp "$AMIGA_FITZ"            "$STAGE/fitz"
cp "$OUT/comparetree"       "$STAGE/comparetree"
cp "$OUT/FitzStress"        "$STAGE/FitzStress"
cp -R "$TREE"               "$STAGE/sysimage"

# comparetree's snprintf("%lld") drags libnix's double formatting in, which
# opens mathieeedoubbas.library, a disk library, not a ROM one.  Without it
# comparetree exits 20 with "mathieeedoubbas.library failed to load" and the
# content check silently never happens.  The extracted Workbench has it.
MATHLIB=$(find "$TREE" -iname 'mathieeedoubbas.library' -print -quit)
if [ -n "$MATHLIB" ]; then
    cp "$MATHLIB" "$STAGE/libs/mathieeedoubbas.library"
else
    echo "!! no mathieeedoubbas.library -- comparetree will not run" >&2
fi

cat > "$STAGE/fitzstress.cfg" <<EOF
# generated by tests/stress/run-fitzstress.sh
share   FITZ:
ram     RAM:
disk    DH0:work
tree    DH0:sysimage
smallkb $SMALL_KB
bigkb   $BIG_KB
seconds $SECONDS_RUN
sample  $SAMPLE
iobuf   32768
seed    20260729
compare $COMPARE_EVERY
workers $WORKERS
EOF

# `&` is SYS_Asynch: a Fitz mount stays resident as a DOS handler and never
# returns, so nothing after it would run otherwise.  netstat -a brackets the
# workload so the connection count can be read off both ends of it, that is
# the single-connection assertion, and it is why the mount is given time to
# settle before the first one is taken.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 6
&SYS:fitz mount $PEER_ADDR:$PORT FITZ:
wait 12
SYS:netstat -a
SYS:FitzStress
SYS:netstat -a
SYS:netstat -h
EOF

# ------------------------------------------------------------- the run -----

export AMINETXDUO_RUN_TAG="$TAG"

# 8 MB of Zorro II is what amiberry-run.sh gives every machine, and two copies
# of a 10 MB file in RAM: do not fit in it.  An A3000 with Kickstart 3.1
# autoconfigures Zorro III, so the sizes in the original recipe are reachable
# by asking for the memory rather than by shrinking the test.
export AMINETXDUO_AMIBERRY_EXTRA="z3mem_size=$RAM_MB${AMINETXDUO_AMIBERRY_EXTRA:+;$AMINETXDUO_AMIBERRY_EXTRA}"

# The emulator must outlive the workload, and the workload is counted in GUEST
# seconds while this deadline is in HOST seconds.  Under four concurrent copies
# the emulated A3000 runs at half real time here, so the factor is 2 and not 1:
# with 1, a run asked for three hours of Amiga time was killed at t=5500 with
# every counter healthy, and reported as a machine that never finished.
#
# -t is a deadline on DH0:.done, not on the test.  A run that wedges never
# writes .done at all, and the timeout IS the result, which is exactly why
# a deadline that fires for any other reason has to be ruled out first.
DEADLINE="${DEADLINE:-$((SECONDS_RUN * 2 + 1800))}"

echo "==> $SECONDS_RUN s of GUEST time, $DEADLINE s of host deadline"
echo "==> $MODEL, ${RAM_MB} MB Zorro III, $BOARD on $IFACE"
echo "==> small ${SMALL_KB} KB, big ${BIG_KB} KB, workers mask $WORKERS"
echo "==> fitz: $(basename "$AMIGA_FITZ")"

# The single-connection claim, measured from the OTHER end and sampled
# throughout rather than only at the brackets.  The guest's own netstat could
# in principle be wrong about what it has open; `ss` on the server cannot be
# wrong about what is connected to it.  Its per-socket counters come back too:
# rwnd_limited is the share of the server's busy time spent blocked on the
# Amiga's receive window, which is the number this workload exists to move.
CONNLOG="$ROOT/build/stress-$TAG-conn.log"
ssh "$PEER" "for i in \$(seq 1 $((DEADLINE / 30))); do
                 printf 'C %s ' \"\$(date +%s)\"
                 ss -tn state established \"( sport = :$PORT )\" |
                     tail -n +2 | wc -l
                 if [ \$((i % 10)) = 1 ]; then
                     ss -tnie state established \"( sport = :$PORT )\" |
                         tr '\n' ' ' | sed 's/^/D /'
                     echo
                 fi
                 sleep 30
             done" > "$CONNLOG" 2>&1 &
CONN_PID=$!
cleanup_all() { kill -TERM "$CONN_PID" 2>/dev/null || true; cleanup_peer; }
trap cleanup_all EXIT INT TERM HUP

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$DEADLINE" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/fitzstress.cfg" \
    "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/netstat" \
    "$STAGE/fitz" "$STAGE/comparetree" "$STAGE/FitzStress" \
    "$STAGE/sysimage" > "$ROOT/build/stress-$TAG-run.log" 2>&1
RUN_RC=$?
set -e

HD="$ROOT/build/amiberry-testhd-$TAG"
SER="$ROOT/build/amiberry-serial-$TAG.log"

echo
echo "==== did it stay up? ===================================================="
if [ "$RUN_RC" = "124" ]; then
    echo "  NO .done -- the guest never finished.  Emulator deadline hit."
else
    echo "  guest exit status $RUN_RC"
fi
if [ -f "$HD/stress-summary.txt" ]; then
    sed 's/^/  /' "$HD/stress-summary.txt"
else
    echo "  (no summary -- the supervisor never got that far)"
fi


# Guest seconds against host seconds.  It is not a curiosity: everything the
# stack sees, timers, retransmit backoff, keepalive, runs on the guest's
# clock, and everything scheduling this run runs on the host's.
WALL=$(grep -o "after [0-9]* s of host wall clock" \
       "$ROOT/build/stress-$TAG-run.log" 2>/dev/null | grep -o "[0-9]*" || true)
GUEST=$(awk "/^seconds /{print \$2}" "$HD/stress-summary.txt" 2>/dev/null || true)
if [ -n "$WALL" ] && [ -n "$GUEST" ] && [ "$WALL" -gt 0 ]; then
    echo "  $GUEST s of guest time in $WALL s of host wall clock" \
         "($(awk "BEGIN{printf \"%.2f\", $GUEST / $WALL}")x real time)"
fi

echo
echo "  last heartbeats (serial; the machine was executing when it wrote one)"
grep "^FS t=" "$SER" 2>/dev/null | tail -5 | sed 's/^/    /' || echo "    (none)"

# The verdict, spelled out, because "seemed fine" is not a result and a
# duration is what makes a null one worth anything.  A stuck worker is the
# freeze; a deadline with no stuck worker is this harness being mis-sized,
# which has happened and must not be read as the fault it looks like.
STUCK=$(awk '/^stuck_workers /{print $2}' "$HD/stress-summary.txt" 2>/dev/null || true)
echo
if [ "${STUCK:-}" = "0" ] && [ "$RUN_RC" != "124" ]; then
    echo "  VERDICT: no freeze in ${GUEST:-?} s of guest time."
elif [ -n "${STUCK:-}" ] && [ "${STUCK:-0}" != "0" ]; then
    echo "  VERDICT: FROZE -- $STUCK worker(s) never came back.  The phase and"
    echo "  stamp above name the DOS call each was in; health.log's last block"
    echo "  is netstat -h at the time."
else
    echo "  VERDICT: inconclusive -- the supervisor did not write a summary."
    echo "  Check whether the heartbeat above stopped (a freeze) or merely ran"
    echo "  out of host deadline with the counters still moving (-D)."
fi

echo
echo "==== was the data correct? ============================================="
if [ -f "$HD/compare.log" ]; then
    CLEAN=$(grep -ac "^----- rc 0 " "$HD/compare.log" || true)
    DIRTY=$(grep -a "^----- rc " "$HD/compare.log" | grep -vc "rc 0 " || true)
    echo "  comparetree (guest, Fitz's own): $CLEAN clean, $DIRTY not"
    grep -av "^-----\|^=====\|^$" "$HD/compare.log" | head -20 |
        sed 's/^/    /' || true
else
    echo "  (no compare.log)"
fi

echo
echo "  host-side check of what the guest wrote to the share:"
ssh "$PEER" "cd $PEER_DIR &&
    for f in up.bin bigback.bin; do
        if [ -f \$f ]; then python3 /tmp/pattern.py check \$f; \
        else echo \"absent \$f\"; fi
    done" 2>&1 | sed 's/^/    /' || true

# Content only.  The metadata half is comparetree's, and it is the half only
# the guest can see: a POSIX filesystem has no Amiga protection word.
echo "  host-side tree compare (content only, $TREE_FILES files in the source):"
python3 "$ROOT/tests/stress/treecheck.py" manifest "$TREE" > "$ROOT/build/stress-$TAG.manifest"
scp -q "$ROOT/build/stress-$TAG.manifest" "$PEER:/tmp/stress-$TAG.manifest" || true
ssh "$PEER" "for s in $PEER_DIR/testsys*; do
                 [ -d \$s ] && python3 /tmp/treecheck.py check /tmp/stress-$TAG.manifest \$s
             done" 2>&1 | sed 's/^/    /' || true

echo
echo "==== did it leak? ====================================================="
python3 "$ROOT/tests/stress/healthseries.py" "$HD/health.log" || true

echo
echo "==== was it one connection? ==========================================="
#
# Both brackets must show ONE established connection to the server, and it must
# be the SAME local port: a second connection, or a different port on the far
# side of the workload, means the mount reconnected and the four workloads were
# not sharing one send queue for the whole run.
awk -v addr="$PEER_ADDR:$PORT" '
    /^===== SYS:netstat -a/ { on = 1; n = 0; ports = ""; next }
    /^===== /               { if (on) { print "  bracket: " n " to " addr,
                                              "(" ports ")" }
                              on = 0; next }
    on && /ESTABLISHED/ && index($0, addr) { n++; ports = ports " " $2 }
    END { if (on) print "  bracket: " n " to " addr " (" ports ")" }
' "$HD/tools.txt" 2>/dev/null || echo "  (no netstat -a output)"

kill -TERM "$CONN_PID" 2>/dev/null || true
if [ -s "$CONNLOG" ]; then
    awk '/^C /  { n[$3]++; if ($3 > max) max = $3; t++ }
         END    { printf "  peer side: %d samples, most at once %d\n", t, max
                  for (k in n)
                      printf "    %s connection(s): %d samples\n", k, n[k] }
        ' "$CONNLOG" | sort
    # What the server's own socket says about the guest's window.  A large
    # rwnd_limited share is the server sitting on data it is not allowed to
    # send, which is the receive window and not the link.
    grep '^D ' "$CONNLOG" | tail -1 | tr ' ' '\n' |
        grep -E '^(snd_wnd|rcv_wnd|rwnd_limited|retrans|bytes_retrans|rto|rtt|cwnd|delivery_rate|rcv_ooopack):' |
        sed 's/^/    /' || true
else
    echo "  peer side: no samples"
fi

echo
echo "==== errors ==========================================================="
if [ -s "$HD/stress-events.txt" ]; then
    echo "  lines: $(wc -l < "$HD/stress-events.txt" | tr -d ' ')"
    head -30 "$HD/stress-events.txt" | sed 's/^/  /'
else
    echo "  (none)"
fi

echo
echo "==== Fitz's own diagnostics (serial) =================================="
if [ -f "$SER" ]; then
    echo "  EAGAIN on a blocking socket: $(grep -c 'EAGAIN' "$SER" || true)"
    echo "  send/recv failures:          $(grep -cE 'send error|recv error|recv maxretry' "$SER" || true)"
    grep -nE "EAGAIN|send error|recv error|recv maxretry" "$SER" | head -10 |
        sed 's/^/  /' || true
else
    echo "  (no serial log)"
fi

echo
echo "==> artefacts: $HD  $SER  $PEERLOG"

[ "$KEEP" = "1" ] || cleanup_peer

exit "$RUN_RC"
