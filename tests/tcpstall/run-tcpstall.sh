#!/usr/bin/env bash
# Is a stalled TCP connection visible, and does TCP_USER_TIMEOUT cut one short.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE=""
PEER_SSH="${AMINETXDUO_PEER:-}"
PORT=5051
BOARD=a2065

while getopts "m:t:b:B:P:p:N:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEER_SSH="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        *) echo "usage: $0 -B <host-nic> -P [user@]peerhost [-p port]"\
                " [-m model] [-t seconds] [-b builddir] [-N board]" >&2
           exit 2 ;;
    esac
done

[ -n "$IFACE" ]    || { echo "result=failed reason=no_bridge_interface" >&2; exit 2; }
[ -n "$PEER_SSH" ] || { echo "result=failed reason=no_peer" >&2; exit 2; }

PEER_NAME="${PEER_SSH#*@}"
PEER=$(getent ahostsv4 "$PEER_NAME" 2>/dev/null | awk 'NR==1{print $1}')
if [ -z "$PEER" ]; then
    case "$PEER_NAME" in
        *[!0-9.]*) echo "result=failed reason=cannot_resolve:$PEER_NAME" >&2
                   exit 2 ;;
        *) PEER="$PEER_NAME" ;;
    esac
fi

EXE="$ROOT/$BUILD/tests/tcpstall/tcpstall_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
NETSTAT="$ROOT/$BUILD/src/tools/netstat"

for f in "$EXE" "$BSD" "$NETSTAT"; do
    [ -f "$f" ] || { echo "result=failed reason=missing:$f" >&2; exit 2; }
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
    echo "result=failed reason=no_a2065_device" >&2; exit 2; }

TAG="${AMINETXDUO_RUN_TAG:-tcpstall}"
STAGE="$ROOT/build/tcpstall-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065"   "$STAGE/devs/a2065.device"
cp "$BSD"     "$STAGE/libs/bsdsocket.library"
cp "$NETSTAT" "$STAGE/netstat"

UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"


PEER_PIDFILE="/tmp/tcpstall-peer-$TAG.pid"

peer_stop() {
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PEER_SSH" \
        "test -f $PEER_PIDFILE && kill \$(cat $PEER_PIDFILE) 2>/dev/null; \
         rm -f $PEER_PIDFILE" >/dev/null 2>&1 || true
}

trap peer_stop EXIT

echo "==> starting the sink on $PEER_SSH port $PORT"
ssh -o BatchMode=yes -o ConnectTimeout=8 "$PEER_SSH" \
    "nohup python3 -c '
import socket, threading
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((\"0.0.0.0\", $PORT))
s.listen(8)
def serve(c):
    try:
        while c.recv(65536):
            pass
    except Exception:
        pass
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
' > /tmp/tcpstall-peer-$TAG.log 2>&1 &
     echo \$! > $PEER_PIDFILE" || {
    echo "result=failed reason=peer_unreachable" >&2; exit 2; }

sleep 2


export AMINETXDUO_RUN_TAG="$TAG"
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:49:00:57}"

echo "==> booting $MODEL, $BOARD bridged on $IFACE, MAC $AMINETXDUO_AMIBERRY_MAC"

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" -a "$PEER $PORT" \
    "$EXE" "$STAGE/devs" "$STAGE/libs" "$STAGE/netstat"
RUN_RC=$?
set -e


. "$ROOT/tools/test-verdict.sh"

REPORT=$(verdict_find "tcpstall" "$RUN_RC" 'tcpstall: [0-9]+ checks' \
                      "$(verdict_hd_amiberry)/stdout.txt" \
                      "$(verdict_serial_amiberry)") || {
    echo "result=failed reason=no_transcript rc=$RUN_RC"; exit 1; }

echo
echo "===================== what the guest printed ====================="
cat "$REPORT"
echo "================================================================="
echo

FAILED=0

field() { grep -oE "$1" "$REPORT" | tail -1 | grep -oE '[0-9]+' | tail -1; }

DEADLINE_AT=$(field 'deadline15 failed_at=[0-9]+' || true)
DEFAULT_AT=$(grep -oE 'default   failed_at=-?[0-9]+' "$REPORT" | tail -1 |
             sed 's/.*=//' || true)
CHECKS=$(grep -oE 'tcpstall: [0-9]+ checks' "$REPORT" | tail -1 |
         grep -oE '[0-9]+' || true)
FAILURES=$(grep -oE '[0-9]+ failures' "$REPORT" | tail -1 |
           grep -oE '[0-9]+' || true)

STALL_SEEN=$(grep -cE 'stalled [0-9]+ms retx [1-9]' "$REPORT" || true)
NETSTAT_SEEN=$(grep -cE 'stalled [0-9]+s, [1-9][0-9]* retx' "$REPORT" || true)

echo "checks=${CHECKS:-0}"
echo "failures=${FAILURES:-1}"
echo "deadline_failed_at=${DEADLINE_AT:-none}"
echo "default_failed_at=${DEFAULT_AT:-alive}"
echo "stallinfo_readings_with_retx=${STALL_SEEN:-0}"
echo "netstat_stalled_rows=${NETSTAT_SEEN:-0}"

[ "${FAILURES:-1}" = "0" ]            || { echo "reason=guest_checks_failed"; FAILED=1; }
[ "${STALL_SEEN:-0}" -gt 0 ] 2>/dev/null || { echo "reason=stall_not_observable"; FAILED=1; }
[ "${NETSTAT_SEEN:-0}" -gt 0 ] 2>/dev/null || { echo "reason=netstat_showed_nothing"; FAILED=1; }

if [ "$FAILED" = 0 ]; then
    echo "result=ok"
    exit 0
fi

echo "result=failed"
exit 1
