#!/usr/bin/env bash
#
# THE TCP: RUN.
#
#   tests/tools/run-tcphandler.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT IS PROVING, and why none of it is a unit test
#
#   The claim behind src/bsdsocket/tcp_handler.c is that a socket becomes an
#   ORDINARY AmigaDOS file handle.  The only way to show that is to hand one to
#   a program that has never heard of a network and watch it work, so the
#   commands below are Commodore's own `Type` and `Copy`, the binaries out of
#   the AmigaOS 3.1 C: drawer, unmodified, with no networking code in them at
#   all, plus the Shell's own `>` redirection, which is dos.library and
#   nothing else.
#
#   1. Type TCP:10.0.2.2/amitest       reads a connection to end of file and
#                                      prints it.  `amitest` is a SERVICE NAME,
#                                      resolved out of DEVS:Internet/services,
#                                      so the name path is exercised too.
#   2. Copy TCP:... TO DH0:copied.txt  the same stream, written to a file,
#                                      compared byte for byte on the host.
#   3. Echo >TCP:10.0.2.2/7001 "..."   the other direction, through Shell
#                                      redirection.  What arrived is read out
#                                      of the HOST's log, not ours.
#   4. TcpHandoff                      accept() -> ReleaseCopyOfSocket() ->
#                                      Open("TCP:OBTAIN=<id>") ->
#                                      SystemTagList(SYS_Output = that handle).
#                                      Two stock commands end up talking to
#                                      each other over a socket neither of them
#                                      opened.
#   5. two failures                    a service that does not exist and a
#                                      malformed name, so that "it works" is
#                                      not merely "it never says no".
#
# WHAT IS NOT COMMITTED
#
#   `Type` and `Copy` are Commodore's, and copyrighted.  They are located at
#   run time, exactly as the Kickstart ROM and a2065.device already are:
#   AMINETXDUO_AMIGA_C=<dir containing type and copy>.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
TESTTOOLS="$ROOT/$BUILD/tests/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TESTTOOLS/TcpHandoff" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# ---- the two Commodore commands, located and never committed -------------

AMIGA_C="${AMINETXDUO_AMIGA_C:-$HOME/amiga-os-src/os-source/c}"
for cmd in type copy; do
    [ -f "$AMIGA_C/$cmd" ] || {
        cat >&2 <<EOF
No AmigaOS C: commands found.  This run's whole point is that STOCK commands
work, so it will not substitute anything of ours for them.

  export AMINETXDUO_AMIGA_C=<a directory containing 'type' and 'copy'>

(looked in $AMIGA_C)
EOF
        exit 2
    }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------- host ports ---

DAYTIME_PORT=7013           # finite stream: sends a body and closes
ECHO_PORT=7001              # logs whatever it is sent
SERVICE_NAME=amitest        # what DEVS:Internet/services will call 7013

DAYTIME_BODY=$'AmiNetXDuo daytime, line one\r\nand line two\r\n'
HANDOFF_TEXT="handoff payload: a shell command wrote this down a socket"
REDIRECT_TEXT="AmigaDOS redirection reached the socket"
LISTEN_TEXT="a listening TCP: handle received this"

# --------------------------------------------------------------- staging ---

STAGE="$ROOT/build/tcphandler-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface"   "$STAGE/AddNetInterface"
cp "$TESTTOOLS/TcpHandoff"    "$STAGE/TcpHandoff"
cp "$AMIGA_C/type"            "$STAGE/Type"
cp "$AMIGA_C/copy"            "$STAGE/Copy"

# The service the guest will ask for by name.  Deliberately not a well-known
# one: a run that passed because 13 happened to be open somewhere would be
# proving nothing about getservbyname().
printf '\n%s\t%d/tcp\n' "$SERVICE_NAME" "$DAYTIME_PORT" \
    >> "$STAGE/devs/Internet/services"

{
    echo "SYS:AddNetInterface eth0"
    echo "wait 2"

    # 1 + 2: a stock command reading a connection to EOF, twice over.
    echo "SYS:Type TCP:10.0.2.2/$SERVICE_NAME"
    echo "SYS:Copy TCP:10.0.2.2/$SERVICE_NAME TO DH0:copied.txt"
    echo "SYS:Type DH0:copied.txt"

    # 3: the write direction, through the Shell and nothing else.
    echo "Echo >TCP:10.0.2.2/$ECHO_PORT \"$REDIRECT_TEXT\""

    # The other half of the name syntax: no host means "wait for somebody".
    # Two TCP: handles, one listening and one connecting, and neither program
    # is ours.
    echo "&SYS:Type TCP:2400 >DH0:listened.txt"
    echo "wait 3"
    echo "Echo >TCP:localhost/2400 \"$LISTEN_TEXT\""
    echo "wait 3"
    echo "SYS:Type DH0:listened.txt"

    # 6: TCP: is a stream, not a drive.  Info walks the DOS list asking each
    # device for its disk info, and a device that answers is what Workbench
    # then draws an icon for, so refusing is what keeps it out of both.
    echo "SYS:Info"

    # 5: two ways of being wrong, both of which must fail fast.
    echo "SYS:Type TCP:10.0.2.2/nosuchservice"
    echo "SYS:Type TCP:"

    # 4: the hand-off, and the two commands that end up joined by it.
    echo "SYS:TcpHandoff"
    echo "wait 3"
    echo "SYS:Type DH0:handoff.txt"
    echo "SYS:Type DH0:handoff-peer.txt"
} > "$STAGE/commands.txt"

# ------------------------------------------------------- the host servers ---
#
# Sized against the LOCK QUEUE, not the run: build/.fsuae.lock serialises every
# emulator run in the tree, so a server that lived for TIMEOUT seconds would
# routinely be dead before the guest booted.

PEERLOG="$ROOT/build/tcphandler-peer.log"
python3 "$ROOT/tests/tools/netpeer.py" \
    --daytime-port "$DAYTIME_PORT" --echo-port "$ECHO_PORT" \
    --log "$PEERLOG" --seconds "$((TIMEOUT + 3600))" \
    > "$ROOT/build/tcphandler-peer.out" 2>&1 &
PEER_PID=$!
cleanup_peer() { kill -TERM "$PEER_PID" 2>/dev/null || true; }
trap cleanup_peer EXIT INT TERM HUP
sleep 2
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "the host peer did not start:" >&2
    cat "$ROOT/build/tcphandler-peer.out" >&2
    exit 2
}

# ------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tcph}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
SERIAL="$ROOT/build/serial-$AMINETXDUO_RUN_TAG.log"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/TcpHandoff" \
    "$STAGE/Type" "$STAGE/Copy"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "==================== what the commands printed ====================="
cat "$REPORT"
echo "==================================================================="
echo
echo "======================= what the host saw =========================="
cat "$PEERLOG" 2>/dev/null || true
echo "==================================================================="

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
note() { echo "  --: $*"; }

STARTS=$(grep -c "SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset)"
else
    fail "the command list ran $STARTS times, the machine reset"
fi

# ---- 6: not a drive, so not in Info and not on the Workbench --------------

if grep -qi "^Unit  *Size" "$REPORT" || grep -qi "Volume.*Size" "$REPORT"; then
    if grep -E "^(TCP|TCP:)" "$REPORT" | grep -qv "^TCP:[0-9a-zA-Z]"; then
        fail "Info lists TCP: as a device, so Workbench will draw it as a drive"
    else
        pass "Info does not list TCP:"
    fi
else
    note "Info printed nothing recognisable, cannot judge the device list"
fi

# ---- 1: Type read a connection -------------------------------------------

if grep -q "AmiNetXDuo daytime, line one" "$REPORT" && \
   grep -q "and line two" "$REPORT"; then
    pass "Type TCP:10.0.2.2/$SERVICE_NAME printed the whole stream"
else
    fail "Type TCP: printed nothing recognisable"
fi

# ---- 2: Copy wrote it to a file, byte for byte ---------------------------

if [ -f "$HD/copied.txt" ]; then
    printf '%s' "$DAYTIME_BODY" > "$ROOT/build/tcphandler-expect.txt"
    if cmp -s "$HD/copied.txt" "$ROOT/build/tcphandler-expect.txt"; then
        pass "Copy TCP:... TO DH0:copied.txt is byte-for-byte the stream"
    else
        fail "DH0:copied.txt does not match what the server sent"
        od -c "$HD/copied.txt" | head -5
    fi
else
    fail "Copy wrote no DH0:copied.txt"
fi

# ---- 3: the Shell wrote INTO a socket ------------------------------------

if grep -q "$REDIRECT_TEXT" "$PEERLOG"; then
    pass "Echo >TCP:... arrived at the host's echo server"
else
    fail "the host never saw what Echo wrote to TCP:"
fi

# ---- the listening half of the syntax ------------------------------------

if [ -f "$HD/listened.txt" ] && grep -q "$LISTEN_TEXT" "$HD/listened.txt"; then
    pass "TCP:<service> accepted a connection and Type read it"
else
    fail "the listening TCP: handle received nothing"
    [ -f "$HD/listened.txt" ] && od -c "$HD/listened.txt" | head -5
fi

# ---- 4: the hand-off -----------------------------------------------------

if grep -q "socket parked under id" "$REPORT"; then
    pass "ReleaseCopyOfSocket() parked the accepted connection"
else
    fail "ReleaseCopyOfSocket() did not park anything"
fi

if grep -q "is now a file handle" "$REPORT"; then
    pass "Open(\"TCP:OBTAIN=<id>\") turned it into a BPTR"
else
    fail "TCP:OBTAIN= did not open"
fi

if [ -f "$HD/handoff.txt" ] && grep -q "$HANDOFF_TEXT" "$HD/handoff.txt"; then
    pass "a command's output crossed the socket into Copy's file"
else
    fail "DH0:handoff.txt does not hold what the handed-over command wrote"
    [ -f "$HD/handoff.txt" ] && od -c "$HD/handoff.txt" | head -5
fi

# ---- 5: the failures, and that they are failures -------------------------

if grep -q "nosuchservice" "$REPORT"; then
    if grep -qiE "can.t open .*nosuchservice|object not found" "$REPORT"; then
        pass "an unknown service name failed rather than hanging"
    else
        note "an unknown service produced no recognisable complaint"
    fi
fi

# ---- what the handler itself said ----------------------------------------

if [ -f "$SERIAL" ]; then
    echo
    echo "=================== the handler's own log ========================="
    grep -i "TCP:" "$SERIAL" | head -40 || true
    echo "==================================================================="

    UNHANDLED=$(grep -o "unhandled packet [0-9-]*" "$SERIAL" | sort -u || true)
    if [ -n "$UNHANDLED" ]; then
        echo "  --: packet types nothing answers yet:"
        echo "$UNHANDLED" | sed 's/^/        /'
    else
        pass "no DOS packet went unanswered"
    fi
fi

echo
if [ "$FAILED" = "0" ]; then
    echo "PASS: TCP: is an AmigaDOS device, and stock commands use it."
else
    echo "FAILURES above."
fi

exit "$FAILED"
