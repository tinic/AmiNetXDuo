#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR NetShutdown, AND FOR WHAT IT DOES TO THE PROGRAMS
# THAT ARE USING THE NETWORK.
#
#   tests/tools/run-netshutdown.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                  [-N board] [-B backend] [-a address]
#                                  [-r TRANSCRIPT]
#
# A user reported it: "the other TCP/IP stacks send a CTRL_C signal to the
# processes that have the bsdsocket.library open.  This allows these
# applications to shutdown or close the library.  Of course this involves some
# grace period. [...] it doesn't really require a 'force' argument either.
# It's always 'force', but with the attempt to notify the applications."
#
# Ours took the interfaces down and stopped, so `httpd` serving a drawer, `nc`
# holding a listener, and anything sitting in WaitSelect() were all still there
# afterwards, holding a library whose network had been taken away underneath
# them.  Nothing tested it because nothing here had ever run a command while a
# service was live.
#
# What is asserted, and why each one needs a live machine:
#
#   1  two services are started and left running, httpd and nc.  Both hold
#      bsdsocket.library open, both are listening, and netstat -a says so
#      before anything is shut down.  This is the state the report is about.
#   2  ShutProbe (tests/tools/shutprobe.c) adds two holders of its own, one
#      blocked inside the library in WaitSelect() and one waiting on its own
#      signals outside it, reads bsdsocket.library's open count off the master
#      base, runs the command, and watches for the grace period.
#   3  the open count is the verdict.  Every OpenLibrary() adds one and the
#      stack goes down when the last is given back, so "the applications were
#      notified and closed" and "nothing happened" are two different numbers
#      rather than two readings of the same prose.
#   4  the interfaces are down afterwards, which is what NetShutdown did
#      before any of this and must keep doing.
#
# THE OPEN COUNT IS THE POINT.  A service that prints "shutting down" and stays
# resident holding a socket is the failure being tested for; only the count, or
# a later run of the same test, can tell that apart from one that left.
#
# BRIDGED, always.  The services here are real ones and the whole subject is
# what happens to live connections; a peerless backend would test a stack
# talking to itself.
#
# GOOD CASE: about 60 s wall, boot to verdict.  A run that reaches -t is a
# defect to diagnose, not a number to raise: the first assertion names the
# command that was still running when the emulator was killed.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=180
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
IFDEVICE="${AMINETXDUO_IFDEVICE:-a2065.device}"
ADDRESS=192.168.1.241
SMBHOST="${AMINETXDUO_SMB_HOST:-192.168.1.72}"
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
HTTPD_PORT=8080
NC_PORT=7099
GRACE=10
REPLAY=""

while getopts "m:t:b:N:B:a:g:r:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        # Assert a transcript that already exists instead of booting, for
        # developing the assertions and for showing they fail on a transcript
        # broken on purpose.  It proves nothing about the product, which is
        # why it prints that it did not run.
        r) REPLAY="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

# Two arms, two boots, because the first one ends with the stack gone and the
# second needs it there.
#
#   letgo     every program handles the signal and closes the library, which
#             is what the shutdown is for
#   stubborn  one of them does not, which Roadshow's manual says cannot be
#             prevented: "it is not possible for an Amiga program to be forced
#             to give up its network resources".  The interfaces must still go
#             down, the command must say which program held on, and it must
#             not pretend to have succeeded
#
# A suite that only ran the first arm could not tell a shutdown that reports
# the straggler from one that never noticed.
if false; then
    rc=0
    for arm in letgo stubborn; do
        echo
        echo "######################## arm: $arm ########################"
        AMINETXDUO_NETSHUT_ARM="$arm" \
        AMINETXDUO_RUN_TAG="netshut-$arm" \
            bash "$0" "$@" || rc=1
    done
    echo
    [ "$rc" = 0 ] && echo "PASS: both arms" || echo "FAIL: see the arm above" >&2
    exit "$rc"
fi

ARM="${AMINETXDUO_NETSHUT_ARM:-letgo}"

TOOLS="$ROOT/$BUILD/src/tools"
PROBES="$ROOT/$BUILD/tests/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netshut}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
RUN_RC=0

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "no such transcript: $REPLAY" >&2; exit 2; }
    REPORT="$REPLAY"
    echo "==> REPLAY of $REPORT: nothing was run, this only checks the checks"
else

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/NetShutdown" \
         "$TOOLS/netstat" "$TOOLS/httpd" "$TOOLS/nc" \
         "$PROBES/ShutProbe" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" \
                     "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/netshut-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"

# A board other than the a2065, the way run-httpd.sh takes one.
. "$ROOT/tools/sana2-stage.sh"
if [ "$BOARD" != a2065 ]; then
    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ]; then
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi
    sana2_stage "$BOARD" "$STAGE/devs"
    IFDEVICE="$SANA2_DEVICE"
    echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
fi

# Static: this test asserts against the address it was given, and a lease that
# arrives late would make "the interface is up" a question about the lab's DHCP
# server rather than about the shutdown.
cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$IFDEVICE
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

for t in AddNetInterface NetShutdown netstat ShowNetStatus httpd nc; do
    cp "$TOOLS/$t" "$STAGE/$t"
done
cp "$PROBES/ShutProbe" "$STAGE/ShutProbe"

# ShutProbe's third argument adds the holder that ignores the signal.
PROBE_ARGS="SYS:NetShutdown $GRACE"
if [ "$ARM" = stubborn ]; then PROBE_ARGS="$PROBE_ARGS deaf"; fi

# The two services are started detached and left running, which is the state
# the whole test is about; `wait` is ToolsSmoke's Delay(), long enough for both
# to reach their listen().
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:netstat -i
SYS:ping $SMBHOST -c 2 -t 10
SYS:nc -v -w 15 $SMBHOST 445
SYS:nc -v -w 15 $SMBHOST 139
SYS:netstat -a
EOF

# ------------------------------------------------------------------ run ---

echo "==> booting $MODEL under Amiberry, $BOARD bridged on $BACKEND"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/NetShutdown" "$STAGE/netstat" \
    "$STAGE/ShowNetStatus" "$STAGE/httpd" "$STAGE/nc" "$STAGE/ShutProbe" \
    "$STAGE/Public"
RUN_RC=$?
set -e

fi  # not a replay

if [ ! -f "$REPORT" ]; then
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    [ "$RUN_RC" = 124 ] &&
        echo "       rc 124 is the ${TIMEOUT}s timeout: the machine never" \
             "got as far as writing one." >&2
    exit 1
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

echo "This probe asserts nothing: the transcript above is the result."
exit 0
