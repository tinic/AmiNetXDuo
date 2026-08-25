#!/usr/bin/env bash
#
# WHEN A COMMAND HAS NO ANSWER, IT MUST STILL SAY SOMETHING.
#
#   tests/tools/run-toolsay.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                              [-N BOARD] [-B IFACE]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

# Its own /24, so nothing on the host's LAN is claimed or answered for.
SELF=10.77.0.2
MASK=255.255.255.0
GW=10.77.0.1
OFFNET=203.0.113.9                  # RFC 5737, and not on the /24 above

while getopts "m:t:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

NEEDED="ToolsSmoke AddNetInterface arp NetTrace"

for t in $NEEDED; do
    [ -f "$TOOLS/$t" ] || { echo "missing $TOOLS/$t, build the tree first" >&2
                            exit 2; }
done
[ -f "$BSD" ] || { echo "missing $BSD, build the tree first" >&2; exit 2; }

[ -n "$(ip -o -4 addr show dev "$IFACE" 2>/dev/null)" ] || {
    echo "no IPv4 address on $IFACE; -B names the host NIC the guest \
bridges onto" >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" \
                     "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }


STAGE="$ROOT/build/toolsay-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$SELF
NETMASK=$MASK
GATEWAY=$GW
STATE=up
IFEOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"
echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"

cp "$BSD" "$STAGE/libs/bsdsocket.library"
for t in $NEEDED; do cp "$TOOLS/$t" "$STAGE/$t"; done

# The two absence cases come FIRST, while the cache is still small: after the
# 33 permanent entries below, 10.77.0.50 would be in it.
{
    echo "SYS:AddNetInterface eth0"
    echo "SYS:arp 10.77.0.50"
    echo "SYS:arp $OFFNET"
    echo "SYS:NetTrace WIRE HOST 127.0.0.1 PORT 21 NOCAPTURE"
    # 33 permanent entries: one more than TOOL_MAX_ARP.
    for i in $(seq 1 33); do
        printf 'SYS:arp 10.77.0.%d SET=02:11:22:33:44:%02x\n' "$((100 + i))" "$i"
    done
    echo "SYS:arp"
} > "$STAGE/commands.txt"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-toolsay}"
# A MAC of its own: a reused one lets the router's cache answer for a guest
# that never came up.
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:49:00:79}"

STAGED=""
for t in $NEEDED; do
    [ "$t" = ToolsSmoke ] && continue
    STAGED="$STAGED $STAGE/$t"
done

set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE, guest $SELF"
# shellcheck disable=SC2086
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" \
    "$STAGE/devs" "$STAGE/libs" $STAGED
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
                      echo "toolsay=0 checks=0 failures=1"; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

CHECKS=0
FAILED=0
fail() { echo "FAIL: $*" >&2; CHECKS=$((CHECKS+1)); FAILED=$((FAILED+1)); }
pass() { echo "  ok: $*";      CHECKS=$((CHECKS+1)); }

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}
body() { block "$1" "${2:-1}" | grep -v '^----- rc ' || true; }

says() { # banner nth regex what
    if body "$1" "$2" | grep -Eq -- "$3"; then
        pass "$4"
    else
        fail "$4 -- no line matched /$3/"
        body "$1" "$2" | sed 's/^/       /' >&2
    fi
}

says "SYS:arp 10.77.0.50" 1 'is not in the cache' \
     "arp says the address is not there"
says "SYS:arp 10.77.0.50" 1 "on this machine's network" \
     "and says why an address on this network can be missing"
says "SYS:arp 10.77.0.50" 1 '^ +ping 10\.77\.0\.50' \
     "and names the command that would make an entry"

says "SYS:arp $OFFNET" 1 "not on this machine's network" \
     "arp says an off-network address is never asked for"
says "SYS:arp $OFFNET" 1 "The router is $GW" \
     "and names the router entry to look at instead"

says "SYS:NetTrace WIRE HOST 127.0.0.1 PORT 21 NOCAPTURE" 1 \
     'cannot connect to 127\.0\.0\.1 port 21' \
     "NetTrace's connect failure is a sentence"

SEEDED=$(body "SYS:arp" 1 | grep -c 'permanent' || true)
REFUSED=$(grep -c 'was not added to the cache' "$REPORT" || true)

if [ "$SEEDED" -lt 32 ] && [ "$REFUSED" -gt 0 ]; then
    pass "the ARP cache fills at $SEEDED entries, under TOOL_MAX_ARP's 32"
else
    fail "the ARP cache took $SEEDED entries with $REFUSED refusals: the\
 truncation branch in arp.c may now be reachable and has no test"
fi

echo
echo "toolsay=$( [ "$FAILED" -eq 0 ] && echo PASSED || echo FAILED )" \
     "checks=$CHECKS failures=$FAILED"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
