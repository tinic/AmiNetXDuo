#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR ConfigureNetInterface, ACROSS ITS WHOLE SURFACE.
#
#   tests/tools/run-ifconfigure.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                  [-A [-N board] [-B backend]]
#
# WHAT THE COMMAND CLAIMS, AND SO WHAT IS CHECKED HERE
#
#   Its header says it changes a running interface's address, netmask and
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=100
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RUNNER=slirp
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:AN:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) RUNNER=amiberry; BOARD="$OPTARG" ;;
        B) RUNNER=amiberry; IFACE="$OPTARG" ;;
        *) sed -n '3,10p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TOOLS/ConfigureNetInterface" "$TOOLS/netstat" "$TOOLS/ping" "$BSD"; do
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


STAGE="$ROOT/build/ifconfigure-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

if [ "$RUNNER" = amiberry ]; then
    . "$ROOT/tools/sana2-stage.sh"

    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi

    sana2_stage "$BOARD" "$STAGE/devs"
    echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
fi

cp "$BSD"                         "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface"       "$STAGE/AddNetInterface"
cp "$TOOLS/ConfigureNetInterface" "$STAGE/ConfigureNetInterface"
cp "$TOOLS/netstat"               "$STAGE/netstat"
cp "$TOOLS/ping"                  "$STAGE/ping"

cat > "$STAGE/commands.txt" <<'EOF'
SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20
SYS:AddNetInterface eth0
SYS:ping 10.0.2.2 -c 2 -t 20
SYS:netstat -i
SYS:netstat -r
SYS:ConfigureNetInterface nosuch0 ADDRESS 10.0.2.20
SYS:ConfigureNetInterface eth0
SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24 NETMASK 255.255.0.0
SYS:ConfigureNetInterface eth0 NETMASK 255.0.255.0
SYS:ConfigureNetInterface eth0 ADDRESS notanaddress
SYS:netstat -i
SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24
SYS:netstat -i
SYS:netstat -r
SYS:ping 10.0.2.2 -c 2 -t 20
SYS:ConfigureNetInterface eth0 NETMASK 255.255.255.128
SYS:netstat -i
SYS:netstat -r
SYS:ConfigureNetInterface eth0 GATEWAY 192.0.2.1
SYS:ConfigureNetInterface eth0 GATEWAY NONE
SYS:netstat -r
SYS:ConfigureNetInterface eth0 QUIET ADDRESS 10.0.2.15/24 GATEWAY 10.0.2.2
SYS:netstat -i
SYS:netstat -r
SYS:ping 10.0.2.2 -c 2 -t 20
EOF


export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifconfigure}"

STARTED=$(date +%s)
set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
if [ "$RUNNER" = "amiberry" ]; then
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/ConfigureNetInterface" \
        "$STAGE/netstat" "$STAGE/ping"
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/ConfigureNetInterface" \
        "$STAGE/netstat" "$STAGE/ping"
fi
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
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

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

# The rc ToolsSmoke recorded for that block.  RETURN_OK 0, WARN 5, ERROR 10,
# FAIL 20, which is what the command returns and what a script calling it sees.
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

ifaces() { block "SYS:netstat -i" "$1"; }
routes() { block "SYS:netstat -r" "$1"; }

# netstat -i prints "Name Mtu Address Link Ipkts Ierrs Opkts Oerrs", so the
# address AND the link state are one line: the address alone is what a stranded
# interface also shows.
up_on() { # nth address
    ifaces "$1" | grep -Eq "^eth0 +[0-9]+ +${2//./\\.} +up( |$)"
}

ipkts() {
    ifaces "$1" | awk '$1 == "eth0" { print $5; exit }'
}

pinged() { # nth description
    local out
    out=$(block "SYS:ping 10.0.2.2 -c 2 -t 20" "$1")
    if printf '%s\n' "$out" | grep -q '2 received' &&
       [ "$(rc_of "SYS:ping 10.0.2.2 -c 2 -t 20" "$1")" = "0" ]; then
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

says "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20" 1 \
     "The network is not running" \
     "configuring before any add says the network is not running"
want_rc "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20" 1 5 \
        "and returns WARN, not a failure"

pinged 1 "eth0 came up and the gateway answers over it"

if up_on 1 10.0.2.15; then
    pass "eth0 came up on 10.0.2.15 with the link up"
else
    fail "eth0 never came up with the link up, so nothing below proves anything"
    ifaces 1 | sed 's/^/       /' >&2
fi
if routes 1 | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.0 '; then
    pass "and with the 10.0.2.0/24 attached route"
else
    fail "eth0 started without the /24, so there is nothing to change"
    routes 1 | sed 's/^/       /' >&2
fi
if routes 1 | grep -Eq '^default +10\.0\.2\.2 '; then
    pass "and the default route through 10.0.2.2"
else
    fail "eth0 started with no default route, so GATEWAY NONE proves nothing"
    routes 1 | sed 's/^/       /' >&2
fi

BASE_IPKTS=$(ipkts 1)
if [ -n "$BASE_IPKTS" ] && [ "$BASE_IPKTS" -gt 0 ]; then
    pass "the receive counter is running ($BASE_IPKTS packets in)"
else
    fail "netstat -i reports no packets in, so the counter cannot witness anything"
    ifaces 1 | sed 's/^/       /' >&2
fi

says "SYS:ConfigureNetInterface nosuch0 ADDRESS 10.0.2.20" 1 \
     'there is no interface called "nosuch0"' \
     "an unknown name is reported by name"
want_rc "SYS:ConfigureNetInterface nosuch0 ADDRESS 10.0.2.20" 1 20 \
        "and returns FAIL"

says "SYS:ConfigureNetInterface eth0" 1 "nothing to change" \
     "an interface and no keyword says there is nothing to change"
want_rc "SYS:ConfigureNetInterface eth0" 1 10 "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24 NETMASK 255.255.0.0" 1 \
     "do not agree" \
     "a prefix length that contradicts NETMASK is refused"
want_rc "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24 NETMASK 255.255.0.0" 1 10 \
        "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 NETMASK 255.0.255.0" 1 \
     "the ones must come first" \
     "a netmask with a hole in it is refused"
want_rc "SYS:ConfigureNetInterface eth0 NETMASK 255.0.255.0" 1 10 "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 ADDRESS notanaddress" 1 \
     "is not an address" \
     "text that is not an address is refused"
want_rc "SYS:ConfigureNetInterface eth0 ADDRESS notanaddress" 1 10 "and returns ERROR"

if up_on 2 10.0.2.15; then
    pass "and after all four refusals eth0 is still 10.0.2.15 with the link up"
else
    fail "a refused argument line changed the interface anyway"
    ifaces 2 | sed 's/^/       /' >&2
fi

want_rc "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24" 1 0 \
        "ADDRESS with a /bits is accepted"
says "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24" 1 \
     "eth0: 10\.0\.2\.20 netmask 255\.255\.255\.0" \
     "and reports what the interface now has, read back from the stack"

if block "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24" 1 |
   grep -q "takes its address by DHCP"; then
    fail "the DHCP warning was printed for a STATIC interface"
else
    pass "and says nothing about DHCP, which this interface does not use"
fi

if up_on 3 10.0.2.20; then
    pass "eth0 is 10.0.2.20 with the link still up"
else
    fail "eth0 is not 10.0.2.20, or the link went down with the change"
    ifaces 3 | sed 's/^/       /' >&2
fi

if routes 2 | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.0 '; then
    pass "and the attached route followed the address"
else
    fail "the attached route did not follow the address"
    routes 2 | sed 's/^/       /' >&2
fi

NOW_IPKTS=$(ipkts 3)
if [ -n "$NOW_IPKTS" ] && [ "$NOW_IPKTS" -ge "$BASE_IPKTS" ]; then
    pass "the receive counter went on climbing ($BASE_IPKTS -> $NOW_IPKTS): the" \
         "interface was changed in place, not taken out and put back"
else
    fail "the receive counter went backwards ($BASE_IPKTS -> ${NOW_IPKTS:-nothing}):" \
         "the interface was replaced, which is what this command exists to avoid"
    ifaces 3 | sed 's/^/       /' >&2
fi

pinged 2 "and it carries traffic from the new address"

want_rc "SYS:ConfigureNetInterface eth0 NETMASK 255.255.255.128" 1 0 \
        "NETMASK on its own is accepted"
if up_on 4 10.0.2.20; then
    pass "and the address it did not mention is untouched"
else
    fail "changing the mask alone moved the address"
    ifaces 4 | sed 's/^/       /' >&2
fi
if routes 3 | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.128 '; then
    pass "and the attached route is the /25 that was asked for"
else
    fail "the mask did not reach the routing table"
    routes 3 | sed 's/^/       /' >&2
fi

says "SYS:ConfigureNetInterface eth0 GATEWAY 192.0.2.1" 1 \
     "not on any of this machine's own subnets" \
     "a gateway off every subnet is refused, and says why"
want_rc "SYS:ConfigureNetInterface eth0 GATEWAY 192.0.2.1" 1 20 "and returns FAIL"

want_rc "SYS:ConfigureNetInterface eth0 GATEWAY NONE" 1 0 "GATEWAY NONE is accepted"
says "SYS:ConfigureNetInterface eth0 GATEWAY NONE" 1 \
     "default gateway is cleared" "and says so"
if routes 4 | grep -Eq '^default +'; then
    fail "the default route is still there after GATEWAY NONE"
    routes 4 | sed 's/^/       /' >&2
else
    pass "and the default route is gone from the table"
fi

QOUT=$(block "SYS:ConfigureNetInterface eth0 QUIET ADDRESS 10.0.2.15/24 GATEWAY 10.0.2.2" 1 |
       grep -v '^----- rc ' || true)
if [ -z "$(printf '%s' "$QOUT" | tr -d '[:space:]')" ]; then
    pass "QUIET printed nothing"
else
    fail "QUIET printed something"
    printf '%s\n' "$QOUT" | sed 's/^/       /' >&2
fi
want_rc "SYS:ConfigureNetInterface eth0 QUIET ADDRESS 10.0.2.15/24 GATEWAY 10.0.2.2" 1 0 \
        "and still did it"

if up_on 5 10.0.2.15; then
    pass "eth0 is back on 10.0.2.15 with the link up"
else
    fail "eth0 did not come back to 10.0.2.15"
    ifaces 5 | sed 's/^/       /' >&2
fi
if routes 5 | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.0 ' &&
   routes 5 | grep -Eq '^default +10\.0\.2\.2 '; then
    pass "and the /24 and the default route are both back in one call"
else
    fail "the address and the gateway did not both come back"
    routes 5 | sed 's/^/       /' >&2
fi
pinged 3 "and after every change above the gateway still answers"

echo
echo "==> the whole run took ${ELAPSED}s against a ${TIMEOUT}s ceiling"
if [ "$FAILED" -eq 0 ]; then
    echo "PASS: ConfigureNetInterface, across its whole surface, on one boot"
    exit 0
fi
echo "the transcript above is the whole run" >&2
exit 1
