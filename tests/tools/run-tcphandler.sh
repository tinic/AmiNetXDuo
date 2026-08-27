#!/usr/bin/env bash
# THE TCP: RUN.
#   `Type` and `Copy` are Commodore's, and copyrighted.  They are located at
#   run time, exactly as the Kickstart ROM and a2065.device already are:
#   AMINETXDUO_AMIGA_C=<dir containing type and copy>.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE=""
PEERHOST=""

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

AMIGA_C="${AMINETXDUO_AMIGA_C:-$HOME/amiga-os-src/os-source/c}"

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

DAYTIME_PORT=7013           # finite stream: sends a body and closes
ECHO_PORT=7001              # logs whatever it is sent
SERVICE_NAME=amitest        # what DEVS:Internet/services will call 7013

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

printf '\n%s\t%d/tcp\n' "$SERVICE_NAME" "$DAYTIME_PORT" \
    >> "$STAGE/devs/Internet/services"

{
    echo "SYS:AddNetInterface eth0"
    echo "wait 2"

    echo "SYS:Type TCP:$PEERADDR/$SERVICE_NAME"
    echo "SYS:Copy TCP:$PEERADDR/$SERVICE_NAME TO DH0:copied.txt"
    echo "SYS:Type DH0:copied.txt"

    echo "Echo >TCP:$PEERADDR/$ECHO_PORT \"$REDIRECT_TEXT\""

    echo "&SYS:Type TCP:2400 >DH0:listened.txt"
    echo "wait 3"
    echo "Echo >TCP:localhost/2400 \"$LISTEN_TEXT\""
    echo "wait 3"
    echo "SYS:Type DH0:listened.txt"

    echo "SYS:Info"

    echo "SYS:Type TCP:$PEERADDR/nosuchservice"
    echo "SYS:Type TCP:"

    echo "SYS:TcpHandoff"
    echo "wait 3"
    echo "SYS:Type DH0:handoff.txt"
    echo "SYS:Type DH0:handoff-peer.txt"
} > "$STAGE/commands.txt"

PEERLOG="$ROOT/build/tcphandler-peer.log"
REMOTE_LOG=""
REMOTE_PID=""
rm -f "$PEERLOG"

if [ -n "$IFACE" ]; then
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

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tcph}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
SERIAL=$(serial_log_path "$AMINETXDUO_RUN_TAG")

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

if grep -qi "^Unit  *Size" "$REPORT" || grep -qi "Volume.*Size" "$REPORT"; then
    if grep -E "^(TCP|TCP:)" "$REPORT" | grep -qv "^TCP:[0-9a-zA-Z]"; then
        fail "Info lists TCP: as a device, so Workbench will draw it as a drive"
    else
        pass "Info does not list TCP:"
    fi
else
    note "Info printed nothing recognisable, cannot judge the device list"
fi

if grep -q "AmiNetXDuo daytime, line one" "$REPORT" && \
   grep -q "and line two" "$REPORT"; then
    pass "Type TCP:$PEERADDR/$SERVICE_NAME printed the whole stream"
else
    fail "Type TCP: printed nothing recognisable"
fi

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

if grep -q "$REDIRECT_TEXT" "$PEERLOG"; then
    pass "Echo >TCP:... arrived at the host's echo server"
else
    fail "the host never saw what Echo wrote to TCP:"
fi

if [ -f "$HD/listened.txt" ] && grep -q "$LISTEN_TEXT" "$HD/listened.txt"; then
    pass "TCP:<service> accepted a connection and Type read it"
else
    fail "the listening TCP: handle received nothing"
    [ -f "$HD/listened.txt" ] && od -c "$HD/listened.txt" | head -5
fi

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

if grep -q "nosuchservice" "$REPORT"; then
    if grep -qiE "can.t open .*nosuchservice|object not found" "$REPORT"; then
        pass "an unknown service name failed rather than hanging"
    else
        note "an unknown service produced no recognisable complaint"
    fi
fi

if serial_log_have "$SERIAL" "$BUILD" "which DOS packets went unanswered"; then
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
else
    note "which DOS packets went unanswered was NOT CHECKED: the serial log is
       empty.  The sentences are in every build, so raise the level the guest
       runs at (serial_log_stage_env) to make this assertion exist"
fi

echo
if [ "$FAILED" = "0" ]; then
    echo "PASS: TCP: is an AmigaDOS device, and stock commands use it."
else
    echo "FAILURES above."
fi

exit "$FAILED"
