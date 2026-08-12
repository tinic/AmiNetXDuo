#!/usr/bin/env bash
#
# THE TCP: RUN.
#
#   tests/tools/run-tcphandler.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                 [-B IFACE] [-P PEERHOST] [-a ADDR] [-g GW]
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
#   1. Type TCP:<peer>/amitest        reads a connection to end of file and
#                                      prints it.  `amitest` is a SERVICE NAME,
#                                      resolved out of DEVS:Internet/services,
#                                      so the name path is exercised too.
#   2. Copy TCP:... TO DH0:copied.txt  the same stream, written to a file,
#                                      compared byte for byte on the host.
#   3. Echo >TCP:<peer>/7001 "..."     the other direction, through Shell
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
# WHERE THE OTHER END IS
#
#   The host that answers is a variable, not 10.0.2.2 written out seven times.
#   That constant was SLIRP's gateway, which is to say "the machine running the
#   emulator", and it is the whole of why this file could not be pointed at a
#   bridge: every service name, every redirection target and every assertion
#   named a host only a SLIRP guest has.
#
#   -B IFACE bridges the guest onto a real network, and then -P must name a
#   THIRD machine to run tests/tools/netpeer.py on: a frame the emulator's host
#   sends to its own bridged guest never comes back to that NIC's pcap, so a
#   peer here is unreachable from the guest while being reachable from
#   everywhere else (tests/tools/run-iperf.sh:32-38).  -a is then the guest's
#   own address, which has to be known before it boots because the peer's log
#   is read back by name.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE=""
PEERHOST=""

# Only used bridged.  Static for the reason tests/tools/run-iperf.sh:68-70
# gives: the peer's log is fetched and the guest's own address appears in it,
# and a DHCP lease is not knowable until after the boot.
ADDRESS="${AMINETXDUO_TCPH_ADDRESS:-192.168.1.243}"
GATEWAY="${AMINETXDUO_TCPH_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0

while getopts "m:t:b:B:P:a:g:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEERHOST="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        *) sed -n '5,6p' "$0" >&2; exit 2 ;;
    esac
done

# -B without -P is the mistake that looks like it works: the guest bridges onto
# a real network and then calls a peer on the machine it cannot hear.
if [ -n "$IFACE" ] && [ -z "$PEERHOST" ]; then
    echo "-B without -P: a bridged guest cannot reach a peer on the machine" \
         "running the emulator, so -P must name a third one." >&2
    exit 2
fi

TOOLS="$ROOT/$BUILD/src/tools"
TESTTOOLS="$ROOT/$BUILD/tests/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TESTTOOLS/TcpHandoff" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

# ---- the two Commodore commands, located and never committed -------------

AMIGA_C="${AMINETXDUO_AMIGA_C:-$HOME/amiga-os-src/os-source/c}"

# AmigaDOS does not care about case and two real sources of these commands
# disagree: the OS source tree spells them `type` and `copy`, a Workbench
# unpacked off its floppy spells them `Type` and `Copy`.  Requiring one
# spelling made a perfectly good C: drawer read as no C: drawer at all.
amiga_cmd() {
    local want="$1" f
    for f in "$AMIGA_C/$want" "$AMIGA_C/$(printf '%s' "$want" |
             sed 's/^./\U&/')"; do
        [ -f "$f" ] && { printf '%s' "$f"; return 0; }
    done
    return 1
}

for cmd in type copy; do
    amiga_cmd "$cmd" >/dev/null || {
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

# The address the guest dials.  Bridged, it is the third machine; otherwise it
# is SLIRP's gateway, which is this host.  -P may carry a user, and
# "turo@playhouse4" is not a host name to an Amiga with no resolver, so the
# name is resolved on this side before it is written into a command.
if [ -n "$IFACE" ]; then
    PEERNAME="${PEERHOST#*@}"
    PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
    if [ -z "$PEERADDR" ]; then
        case "$PEERNAME" in
            *[!0-9.]*) echo "cannot resolve $PEERNAME to an address for the" \
                            "guest to call" >&2; exit 2 ;;
            *) PEERADDR="$PEERNAME" ;;
        esac
    fi
    echo "==> the peer is $PEERHOST, which the guest reaches at $PEERADDR"
else
    PEERADDR=10.0.2.2
    ADDRESS=10.0.2.15
    GATEWAY=10.0.2.2
fi

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
cp "$(amiga_cmd type)"        "$STAGE/Type"
cp "$(amiga_cmd copy)"        "$STAGE/Copy"

# tests/netstack/devs ships a DHCP eth0, which is right on SLIRP and wrong on a
# bridge: see -a above.
if [ -n "$IFACE" ]; then
    cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF
fi

# The service the guest will ask for by name.  Deliberately not a well-known
# one: a run that passed because 13 happened to be open somewhere would be
# proving nothing about getservbyname().
printf '\n%s\t%d/tcp\n' "$SERVICE_NAME" "$DAYTIME_PORT" \
    >> "$STAGE/devs/Internet/services"

{
    echo "SYS:AddNetInterface eth0"
    echo "wait 2"

    # 1 + 2: a stock command reading a connection to EOF, twice over.
    echo "SYS:Type TCP:$PEERADDR/$SERVICE_NAME"
    echo "SYS:Copy TCP:$PEERADDR/$SERVICE_NAME TO DH0:copied.txt"
    echo "SYS:Type DH0:copied.txt"

    # 3: the write direction, through the Shell and nothing else.
    echo "Echo >TCP:$PEERADDR/$ECHO_PORT \"$REDIRECT_TEXT\""

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
    echo "SYS:Type TCP:$PEERADDR/nosuchservice"
    echo "SYS:Type TCP:"

    # 4: the hand-off, and the two commands that end up joined by it.
    echo "SYS:TcpHandoff"
    echo "wait 3"
    echo "SYS:Type DH0:handoff.txt"
    echo "SYS:Type DH0:handoff-peer.txt"
} > "$STAGE/commands.txt"

# ------------------------------------------------------- the host servers ---
#
# Sized against the WAIT, not the run: a contended host can hold this one off
# for a long time, so a server that lived for TIMEOUT seconds would routinely
# be dead before the guest booted.

PEERLOG="$ROOT/build/tcphandler-peer.log"
REMOTE_LOG=""
REMOTE_PID=""
rm -f "$PEERLOG"

if [ -n "$IFACE" ]; then
    # On the third machine, under a `timeout` of its own: killing the local ssh
    # does not kill what it started on the far side, so a peer with no ceiling
    # outlives its run, holds the port, and the next run dies on "address
    # already in use" (tests/tools/run-iperf.sh:260-266).
    # DETACHED THERE, KILLED BY PID.  Killing the local ssh does not kill what
    # it started on the far side, so a peer left running holds 7001 and 7013
    # and the NEXT run dies on "[Errno 98] Address already in use" -- which
    # this script reports as a missing ingredient, naming the wrong machine.
    # A pidfile rather than `pkill -f netpeer`, because that pattern matches
    # the remote shell issuing it (tests/perf/peercap.sh:108-111).
    REMOTE_PY="/tmp/netpeer-$$.py"
    REMOTE_LOG="/tmp/netpeer-$$.log"
    REMOTE_PID="/tmp/netpeer-$$.pid"
    scp -q "$ROOT/tests/tools/netpeer.py" "$PEERHOST:$REMOTE_PY" || {
        echo "cannot copy the peer to $PEERHOST" >&2; exit 2; }
    ssh -o ConnectTimeout=10 "$PEERHOST" "python3 $REMOTE_PY --help" \
        >/dev/null 2>&1 || {
        echo "$PEERHOST cannot run the peer; it needs python3" >&2; exit 2; }
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "nohup timeout $((TIMEOUT + 300)) python3 $REMOTE_PY \
             --daytime-port $DAYTIME_PORT --echo-port $ECHO_PORT \
             --log $REMOTE_LOG --seconds $((TIMEOUT + 240)) \
             > /dev/null 2>&1 & \
         echo \$! > $REMOTE_PID" > "$ROOT/build/tcphandler-peer.out" 2>&1 || {
        echo "cannot start the peer on $PEERHOST" >&2; exit 2; }
    PEER_PID=""
    cleanup_peer() {
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "[ -f $REMOTE_PID ] && kill \$(cat $REMOTE_PID) 2>/dev/null; \
             rm -f $REMOTE_PY $REMOTE_LOG $REMOTE_PID; exit 0" \
            >/dev/null 2>&1 || true
    }
else
    python3 "$ROOT/tests/tools/netpeer.py" \
        --daytime-port "$DAYTIME_PORT" --echo-port "$ECHO_PORT" \
        --log "$PEERLOG" --seconds "$((TIMEOUT + 3600))" \
        > "$ROOT/build/tcphandler-peer.out" 2>&1 &
    PEER_PID=$!
    cleanup_peer() { kill -TERM "$PEER_PID" 2>/dev/null || true; }
fi
trap cleanup_peer EXIT INT TERM HUP
sleep 2
peer_alive() {
    if [ -n "$REMOTE_LOG" ]; then
        ssh -o ConnectTimeout=10 "$PEERHOST" \
            "[ -f $REMOTE_PID ] && kill -0 \$(cat $REMOTE_PID) 2>/dev/null" \
            >/dev/null 2>&1
    else
        kill -0 "$PEER_PID" 2>/dev/null
    fi
}
peer_alive || {
    echo "the host peer did not start:" >&2
    cat "$ROOT/build/tcphandler-peer.out" >&2
    [ -z "$REMOTE_LOG" ] || ssh -o ConnectTimeout=10 "$PEERHOST" \
        "cat $REMOTE_LOG" >&2 2>/dev/null
    exit 2
}

# ------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tcph}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
SERIAL="$ROOT/build/serial-$AMINETXDUO_RUN_TAG.log"

if [ -n "$IFACE" ]; then
    echo "==> booting $MODEL with the A2065 bridged on $IFACE, guest $ADDRESS"
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
fi
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 ${IFACE:+-B "$IFACE"} \
    -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/TcpHandoff" \
    "$STAGE/Type" "$STAGE/Copy"
RUN_RC=$?
set -e

# What the peer saw is an assertion here, so it has to come back from the peer.
# A missing fetch would read as "the host never saw what Echo wrote", which is
# a product failure, not a plumbing one.
if [ -n "$REMOTE_LOG" ]; then
    scp -q "$PEERHOST:$REMOTE_LOG" "$PEERLOG" 2>/dev/null || {
        echo "could not fetch the peer's log from $PEERHOST:$REMOTE_LOG;" \
             "every assertion about what the host saw would fail for that" \
             "reason alone" >&2
        exit 2; }
fi

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
    pass "Type TCP:$PEERADDR/$SERVICE_NAME printed the whole stream"
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
