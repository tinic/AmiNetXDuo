#!/usr/bin/env bash
# Can a rude peer take the machine down?
#
#   tests/tls/run-hangup.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                          [-B INTERFACE] [-H user@host] [-A ADDRESS]
#
# BRIDGED, AND THE RUDE PEER IS A DIFFERENT MACHINE.  hangup-server.py used to
# bind 127.0.0.1 on the machine running the emulator and the guest reached it
# at 10.0.2.2, SLIRP's alias for the host loopback.  A bridged guest has no
# such alias and cannot reach the host it runs on -- a frame sent there never
# comes back to that NIC -- so the four listeners are started on a peer on the
# segment over ssh, and the URLs name that peer's address.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"
IFACE="${AMINETXDUO_HANGUP_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"
PEER="${AMINETXDUO_HANGUP_PEER:-${AMINETXDUO_PEER:-}}"
PEER_ADDR="${AMINETXDUO_HANGUP_PEER_ADDR:-}"

while getopts "m:t:c:b:B:H:A:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-B interface] [-H user@host] [-A address]" >&2; exit 2 ;;
    esac
done

kv() { printf '%s=%s\n' "$1" "$2"; }
refuse() { kv reason "$1"; kv RESULT refused; exit 2; }

case "$IFACE" in
    slirp|slirp_inbound)
        refuse "slirp_carries_the_handshake_itself: -B <interface>" ;;
esac

[ -n "$PEER" ] ||
    refuse "no peer: set AMINETXDUO_HANGUP_PEER=<user@host> or pass -H; the rude peer cannot live on the machine running the emulator"

command -v ssh >/dev/null 2>&1 || refuse "no ssh on this host"

if [ -z "$PEER_ADDR" ]; then
    PEER_ADDR=$(ssh -o ConnectTimeout=10 "$PEER" \
                    "ip -o -4 addr show scope global |
                     awk '{split(\$4,a,\"/\"); print a[1]}'" 2>/dev/null |
                head -1)
fi
[ -n "$PEER_ADDR" ] ||
    refuse "$PEER did not report an address of its own; pass -A <addr>"

# On this segment, and not through a router: a rude peer behind one is
# measuring the router's idea of a reset rather than the peer's.
ip -o route get "$PEER_ADDR" 2>/dev/null | grep -q "dev $IFACE " ||
    refuse "$PEER_ADDR is not on $IFACE's segment"

TAG="${AMINETXDUO_RUN_TAG:-hangup}"
HOST="$PEER_ADDR"

COMMANDS="$ROOT/build/hangup-commands.txt"
cat > "$COMMANDS" <<EOF
SYS:AddNetInterface eth0
SYS:fetch https://$HOST:4443/ QUIET
SYS:fetch https://$HOST:4444/ QUIET
SYS:fetch https://$HOST:4445/ QUIET TIMEOUT 15
SYS:fetch https://$HOST:4446/ QUIET
EOF

SERVER_LOG="$ROOT/build/hangup-server.log"
: > "$SERVER_LOG"

PEER_SCRIPT="/tmp/hangup-server-$TAG.py"
PEER_LOG="/tmp/hangup-server-$TAG.log"
KILLPAT="[h]angup-server-$TAG.py"
PEER_PYTHON="${AMINETXDUO_HANGUP_PYTHON:-python3}"

ssh "$PEER" "pkill -f \"$KILLPAT\" || true" >/dev/null 2>&1 || true
scp -q "$ROOT/tests/tls/hangup-server.py" "$PEER:$PEER_SCRIPT" ||
    refuse "could not stage the rude peer on $PEER"

cleanup() {
    ssh "$PEER" "pkill -f \"$KILLPAT\" || true" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM HUP

ssh "$PEER" "nohup $PEER_PYTHON $PEER_SCRIPT --bind $PEER_ADDR \
             --seconds $((TIMEOUT + 120)) \
             < /dev/null > $PEER_LOG 2>&1 & sleep 1" >/dev/null 2>&1 || true

READY=no
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if ssh "$PEER" "grep -q '^ready$' $PEER_LOG" 2>/dev/null; then
        READY=yes
        break
    fi
    sleep 1
done
[ "$READY" = yes ] || {
    ssh "$PEER" "cat $PEER_LOG" 2>/dev/null | sed 's/^/!! /' >&2
    refuse "the rude peer did not start on $PEER"; }

echo "==> rude peer up on $PEER ($PEER_ADDR:4443-4446)"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
AMINETXDUO_RUN_TAG="$TAG" \
AMINETXDUO_FETCH_COMMANDS="$COMMANDS" \
AMINETXDUO_BUILD="$BUILD" \
    "$ROOT/tests/tls/run-fetch.sh" -m "$MODEL" -t "$TIMEOUT" -b "$BUILD" \
        -B "$IFACE" "${CPUARG[@]}"
rc=$?
set -e

ssh "$PEER" "cat $PEER_LOG" > "$SERVER_LOG" 2>/dev/null || : > "$SERVER_LOG"
cleanup
trap - EXIT INT TERM HUP

echo "---- what the rude peer saw ----"
cat "$SERVER_LOG"

echo
echo "---- the verdict ----"
# shellcheck source=tests/tls/hangup-verdict.sh
. "$ROOT/tests/tls/hangup-verdict.sh"

REPORT="$ROOT/build/amiberry-testhd-$TAG/tools.txt"

kv peer "$PEER"
kv peer_address "$PEER_ADDR"
kv iface "$IFACE"
printf 'run_rc=%s\n' "$rc"
case "$rc" in
    0|77) ;;
    *) printf 'reason=%s\n' "the emulator did not come back cleanly"
       printf 'RESULT=broken\n'
       exit 3 ;;
esac

if hangup_verdict "$REPORT" 15 "$SERVER_LOG"; then
    printf 'RESULT=pass\n'
    exit 0
fi
printf 'RESULT=fail\n'
exit 1
