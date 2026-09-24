#!/usr/bin/env bash
# THE REGRESSION TEST FOR ConfigureNetInterface'S DHCP HALF.
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# BRIDGED.  -B names the host NIC (default $AMINETXDUO_AMIBERRY_BACKEND, else
# ens18); -g is the segment's router, which the pings go to.
#
#   run-ifdhcp.sh [-m model] [-t seconds] [-b builddir] [-N board] [-B iface]
#                 [-g gateway]
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=140
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
GATEWAY="${AMINETXDUO_IFDHCP_GATEWAY:-192.168.1.1}"

while getopts "m:t:b:N:B:g:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        *) sed -n '9,10p' "$0" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound)
        echo "run-ifdhcp.sh runs bridged: -B names the host NIC the guest\
 bridges onto" >&2
        exit 2 ;;
esac

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TOOLS/ConfigureNetInterface" "$TOOLS/ShowNetStatus" \
         "$TOOLS/netstat" "$TOOLS/ping" "$TOOLS/NetCapture" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

STAGE="$ROOT/build/ifdhcp-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
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

cp "$BSD"                         "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface"       "$STAGE/AddNetInterface"
cp "$TOOLS/ConfigureNetInterface" "$STAGE/ConfigureNetInterface"
cp "$TOOLS/ShowNetStatus"         "$STAGE/ShowNetStatus"
cp "$TOOLS/netstat"               "$STAGE/netstat"
cp "$TOOLS/ping"                  "$STAGE/ping"
cp "$TOOLS/NetCapture"            "$STAGE/NetCapture"

# The ping target is written before boot; the lease names the segment's real
# router, and the two are compared below.
PING_TARGET="$GATEWAY"
if ! awk -v ip="$PING_TARGET" 'BEGIN {
        n = split(ip, a, "."); if (n != 4) exit 1
        for (i = 1; i <= 4; i++)
            if (a[i] !~ /^[0-9]+$/ || a[i] + 0 > 255) exit 1
    }'; then
    echo "invalid IPv4 gateway '$PING_TARGET' (-g)" >&2
    exit 2
fi
PING_COMMAND="SYS:ping $PING_TARGET -c 2 -t 20"

cat > "$STAGE/commands.txt" <<EOF
SYS:ConfigureNetInterface eth0 RELEASE
SYS:AddNetInterface eth0
$PING_COMMAND
SYS:netstat -i
SYS:ShowNetStatus eth0
SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20
SYS:netstat -i
SYS:ShowNetStatus eth0
$PING_COMMAND
&SYS:NetCapture OUT=SYS:dhcpwire.pcap IFACE=eth0 PORT=67 SNAP=400 COUNT=32 SECONDS=8 QUIET >SYS:netcapture.txt
wait 2
SYS:ConfigureNetInterface eth0 RELEASE
SYS:ShowNetStatus eth0
SYS:ConfigureNetInterface eth0 RELEASE
SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20
wait 9
SYS:netstat -i
SYS:ShowNetStatus eth0
$PING_COMMAND
SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP NETMASK 255.255.255.0
SYS:ConfigureNetInterface eth0 CONFIGURE=AUTO
SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 3
SYS:ConfigureNetInterface eth0 TIMEOUT 30
SYS:ConfigureNetInterface nosuch0 RELEASE
SYS:ShowNetStatus eth0
SYS:ConfigureNetInterface eth0 QUIET RELEASEADDRESS
SYS:ShowNetStatus eth0
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifdhcp}"

# This run names its own interface in the command list below; its first claim
# is deliberately about the state before that command runs.

STARTED=$(date +%s)
set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/ConfigureNetInterface" \
    "$STAGE/ShowNetStatus" "$STAGE/netstat" "$STAGE/ping" \
    "$STAGE/NetCapture"
RUN_RC=$?
set -e
ELAPSED=$(( $(date +%s) - STARTED ))

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
RIG=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
rig()  { echo "  RIG  $*"; RIG=1; }

WANTED=$(grep -c . "$STAGE/commands.txt")
RAN=$(grep -c '^===== ' "$REPORT" || true)
if [ "$RAN" -lt "$WANTED" ]; then
    STUCK=$(sed -n "$((RAN + 1))p" "$STAGE/commands.txt")
    echo "INFRA: the guest ran $RAN of $WANTED commands in ${ELAPSED}s against a" \
         "${TIMEOUT}s ceiling." >&2
    echo "       It stopped at: ${STUCK:-<past the end of the list>}" >&2
    echo "       That command hung.  Raising -t is not the fix; the run above" \
         "measured nothing." >&2
    exit 2
fi

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

rc_of() { block "$1" "$2" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }

want_rc() { # banner nth expected description
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"
         block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

says() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        pass "$4"
    else
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

says_not() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    else
        pass "$4"
    fi
}

ifaces() { block "SYS:netstat -i" "$1"; }
status() { block "SYS:ShowNetStatus eth0" "$1"; }

# The IPv4 address on <name>'s own netstat -i line, if it is a usable one:
# not empty, not 0.0.0.0, not link-local.
netstat_addr() { # netstat text, name
    printf '%s\n' "$1" | awk -v n="$2" '$1 == n { print $3; exit }' |
    grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' |
    grep -vE '^(0\.0\.0\.0|169\.254\.)'
}

lease_router() { # ShowNetStatus text -> the router the DHCP lease named
    printf '%s\n' "$1" |
    awk '$1 == "it" && $2 == "offered" && $3 == "router" { print $4; exit }'
}

hw_addr() { # ShowNetStatus text -> the interface's MAC, lower case
    printf '%s\n' "$1" | awk '$1 == "hardware" { print tolower($2); exit }' |
    grep -E '^([0-9a-f]{2}:){5}[0-9a-f]{2}$'
}

# At least one reply, read from ping's count.  Not the loss percentage: "100%
# packet loss" ends in "0% packet loss".
replied() { # block text
    printf '%s\n' "$1" | awk '
        / packets transmitted, / {
            s = $0; sub(/.* packets transmitted, */, "", s)
            if (s ~ /^[1-9][0-9]* (packets )?received/) ok = 1
        }
        END { exit !ok }'
}

address() { netstat_addr "$(ifaces "$1")" eth0 || true; }
lease_server() {
    status "$1" |
        sed -n 's/.*lease[[:space:]][[:space:]]*from \([0-9][0-9.]*\).*/\1/p' |
        head -1
}

pinged() { # nth description
    local out
    [ "$RIG" = 0 ] || return 0
    out=$(block "$PING_COMMAND" "$1")
    if replied "$out"; then
        pass "$2"
    else
        fail "$2: no reply came back over eth0"
        printf '%s\n' "$out" | sed 's/^/       /' >&2
    fi
}

ADDS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$ADDS" -eq 1 ]; then
    pass "the machine booted once and ran the whole list"
elif [ "$ADDS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never got as far as bringing eth0 up, something hung"
fi

says "SYS:ConfigureNetInterface eth0 RELEASE" 1 "The network is not running" \
     "releasing before any add says the network is not running"
want_rc "SYS:ConfigureNetInterface eth0 RELEASE" 1 5 "and returns WARN"

# A different router in the lease is -g, not the stack.
ROUTER=$(lease_router "$(status 1)")
if [ -n "$ROUTER" ] && [ "$ROUTER" != "$PING_TARGET" ]; then
    rig "eth0's lease names router $ROUTER, but $PING_TARGET was pinged:"\
        "pass -g $ROUTER"
fi
pinged 1 "eth0 came up by DHCP and $PING_TARGET answers over it"
LEASED=$(address 1)
if [ -n "$LEASED" ]; then
    pass "the DHCP server gave it $LEASED"
else
    fail "eth0 has no usable address after DHCP ('${LEASED:-nothing}')"
    ifaces 1 | sed 's/^/       /' >&2
fi
LEASE_SERVER=$(lease_server 1)
if [ -n "$LEASE_SERVER" ]; then
    pass "and ShowNetStatus says it came from $LEASE_SERVER"
else
    fail "ShowNetStatus did not name the DHCP server"
    status 1 | sed 's/^/       /' >&2
fi

want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20" 1 0 \
        "CONFIGURE=DHCP on a bound interface is answered inside the timeout"
says "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20" 1 \
     "lease renewed" \
     "and says it renewed rather than allocated, which is what it did"
RENEWED=$(address 2)
if [ -n "$RENEWED" ] && [ "$RENEWED" = "$LEASED" ]; then
    pass "and the address did not move ($RENEWED): a renewal keeps it"
else
    fail "the renewal changed the address ($LEASED -> ${RENEWED:-nothing}), which"\
         "makes it an allocation under the wrong name"
    ifaces 2 | sed 's/^/       /' >&2
fi
RENEW_SERVER=$(lease_server 2)
if [ -n "$LEASE_SERVER" ] && [ "$RENEW_SERVER" = "$LEASE_SERVER" ]; then
    pass "and the interface still holds the lease from $RENEW_SERVER"
else
    fail "the renewal's server changed ($LEASE_SERVER -> ${RENEW_SERVER:-nothing})"
    status 2 | sed 's/^/       /' >&2
fi
pinged 2 "and still carries traffic to $PING_TARGET"

want_rc "SYS:ConfigureNetInterface eth0 RELEASE" 2 0 "RELEASE is accepted"
says "SYS:ConfigureNetInterface eth0 RELEASE" 2 "the lease is released" \
     "and says so"

says_not "SYS:ShowNetStatus eth0" 3 "lease +from" \
         "and ShowNetStatus no longer reports a lease on eth0"

if status 3 | grep -q '^[[:space:]]*lease6[[:space:]]'; then
    says "SYS:ConfigureNetInterface eth0 RELEASE" 3 "the lease is released" \
         "a second RELEASE gives back the remaining DHCPv6 lease"
    want_rc "SYS:ConfigureNetInterface eth0 RELEASE" 3 0 "and succeeds"
else
    says "SYS:ConfigureNetInterface eth0 RELEASE" 3 "has no lease to release" \
         "a second RELEASE is refused because the first one dropped the only lease"
    want_rc "SYS:ConfigureNetInterface eth0 RELEASE" 3 20 "and returns FAIL"
fi

want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20" 2 0 \
        "CONFIGURE=DHCP after a release takes a lease again"

# THE WIRE, not the verdict.  NetCapture ran in the guest across the restart
# only; on a shared segment other clients' broadcasts are in the file too, so
# only a DISCOVER carrying eth0's own MAC counts.  A restart that re-armed
# nothing leaves a lease-looking verdict above and no such DISCOVER here.
# Three faults, three verdicts: no capture at all, a capture that caught
# nothing, and a capture whose exchange has no DISCOVER in it.
WIRE="$HD/dhcpwire.pcap"
CAPSAY="$HD/netcapture.txt"
MAC=$(hw_addr "$(status 1)" || true)
if ! command -v python3 >/dev/null 2>&1; then
    fail "no python3 on this host, so the captured wire cannot be read"
elif [ -z "$MAC" ]; then
    fail "ShowNetStatus gave no hardware address for eth0, so no DISCOVER"\
         "on the wire can be tied to it"
    status 1 | sed 's/^/       /' >&2
elif [ ! -f "$WIRE" ]; then
    fail "NetCapture never opened $WIRE: it did not run in the guest"
    [ -f "$CAPSAY" ] && sed 's/^/       /' "$CAPSAY" >&2
    [ -f "$CAPSAY" ] || echo "       and it printed nothing to $CAPSAY" >&2
else
    COUNTS=$(python3 "$ROOT/tests/tools/dhcpwire.py" -c "$MAC" "$WIRE" 2>&1) ||
        COUNTS="frames=0 dhcp=0 discover=0 foreign=0"
    # shellcheck disable=SC2086
    set -- $COUNTS
    WFRAMES=${1#frames=}; WDHCP=${2#dhcp=}; WDISC=${3#discover=}
    if [ "${WDISC:-0}" -ge 1 ] 2>/dev/null; then
        pass "and a DHCP DISCOVER really went out on the wire ($COUNTS)"
    elif [ "${WFRAMES:-0}" = 0 ] 2>/dev/null; then
        fail "NetCapture caught nothing on port 67 across the restart: the tap"\
             "saw no frames, so this run graded no wire ($COUNTS)"
        [ -f "$CAPSAY" ] && sed 's/^/       /' "$CAPSAY" >&2
    else
        fail "THE RESTART WAS SILENT: $WDHCP DHCP frame(s) and no DISCOVER"\
             "from $MAC ($COUNTS)"
        python3 "$ROOT/tests/tools/dhcpwire.py" -v -c "$MAC" "$WIRE" 2>&1 |
            sed 's/^/       /' >&2
    fi
fi
says "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20" 2 \
     "lease taken" \
     "and says it allocated rather than renewed, which is what it did"
RETAKEN=$(address 3)
if [ -n "$RETAKEN" ]; then
    pass "and the interface is addressed again ($RETAKEN)"
else
    fail "eth0 is on '${RETAKEN:-nothing}' after re-acquiring"
    ifaces 3 | sed 's/^/       /' >&2
fi
RETAKEN_SERVER=$(lease_server 4)
if [ -n "$RETAKEN_SERVER" ]; then
    pass "and ShowNetStatus reports a lease from $RETAKEN_SERVER once more"
else
    fail "ShowNetStatus reports no server for the re-acquired lease"
    status 4 | sed 's/^/       /' >&2
fi
pinged 3 "and $PING_TARGET answers over the re-acquired lease"

says "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP NETMASK 255.255.255.0" 1 \
     "takes its netmask and gateway from the server" \
     "NETMASK with CONFIGURE=DHCP is refused, and says why"
want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP NETMASK 255.255.255.0" 1 10 \
        "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 CONFIGURE=AUTO" 1 \
     "CONFIGURE takes DHCP and nothing else" \
     "CONFIGURE=AUTO is refused by name"
want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=AUTO" 1 10 "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 3" 1 \
     "is too short to tell anything about the network" \
     "a TIMEOUT of less than ten seconds is refused"
want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 3" 1 10 \
        "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 TIMEOUT 30" 1 \
     "needs CONFIGURE=DHCP" \
     "a TIMEOUT with nothing to wait for is refused"
want_rc "SYS:ConfigureNetInterface eth0 TIMEOUT 30" 1 10 "and returns ERROR"

says "SYS:ConfigureNetInterface nosuch0 RELEASE" 1 \
     'there is no interface called "nosuch0"' \
     "RELEASE on a name that is not there is reported by name"
want_rc "SYS:ConfigureNetInterface nosuch0 RELEASE" 1 20 "and returns FAIL"

AFTER_REFUSALS_SERVER=$(lease_server 5)
if [ -n "$RETAKEN_SERVER" ] && [ "$AFTER_REFUSALS_SERVER" = "$RETAKEN_SERVER" ]; then
    pass "and none of the five refusals touched the lease"
else
    fail "the lease changed across the refusal checks"
    status 5 | sed 's/^/       /' >&2
fi

QOUT=$(block "SYS:ConfigureNetInterface eth0 QUIET RELEASEADDRESS" 1 |
       grep -v '^----- rc ' || true)
if [ -z "$(printf '%s' "$QOUT" | tr -d '[:space:]')" ]; then
    pass "QUIET printed nothing"
else
    fail "QUIET printed something"
    printf '%s\n' "$QOUT" | sed 's/^/       /' >&2
fi
want_rc "SYS:ConfigureNetInterface eth0 QUIET RELEASEADDRESS" 1 0 \
        "and RELEASEADDRESS was accepted"

says_not "SYS:ShowNetStatus eth0" 6 "lease +from" \
         "and the lease is gone: RELEASEADDRESS is the same switch as RELEASE"

echo
echo "==> the whole run took ${ELAPSED}s against a ${TIMEOUT}s ceiling"
if [ "$RIG" -ne 0 ]; then
    echo "RIG: the run was not set up for this segment.  No verdict." >&2
    exit 2
fi
if [ "$FAILED" -eq 0 ]; then
    echo "PASS: ConfigureNetInterface's DHCP half, on one boot"
    exit 0
fi
echo "the transcript above is the whole run" >&2
exit 1
