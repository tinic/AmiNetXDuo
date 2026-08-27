#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "the command says the network is down while it is up".
#
#   tests/tools/run-livetools.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                [-B INTERFACE] [-N BOARD]
#
# -N NAMES THE CARD, and every board in tests/tools/cards.sh is an answer.  It
# was a2065 and nothing else, and one card is what let an interface that could
# not be brought back up reach a user.
#
# BRIDGED.  It was SLIRP: the command list embedded 10.0.2.2 in five lines and
# two assertions compared against the literals 10.0.2.15 and 10.0.2.2, so what
# it exercised was the emulator's own NAT rather than this stack on a wire.
# The gateway is now read off the host's routing table for -B's interface, and
# the leased address is read out of the transcript instead of compared to a
# constant -- a DHCP server on a real segment does not hand out the same
# address twice.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_LIVETOOLS_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
GATEWAY="${AMINETXDUO_LIVETOOLS_GATEWAY:-}"
# Somewhere off this segment that answers ICMP, for the second ping.
OFFNET="${AMINETXDUO_LIVETOOLS_OFFNET:-8.8.8.8}"
# A destination for the AddNetRoute/DeleteNetRoute pair.  It must not be a
# network this segment already has a route to, or "the route was added" and
# "the route was removed" are both unreadable.
DUMMYNET="${AMINETXDUO_LIVETOOLS_DUMMYNET:-192.168.77.0}"

while getopts "m:t:b:B:g:N:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2
           echo "          [-B interface] [-g gateway] [-N board]" >&2
           exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound)
        echo "livetools_refused=slirp: this measures the commands against a real" >&2
        echo "segment; -B names the host NIC the guest bridges onto" >&2
        exit 2 ;;
esac

command -v ip >/dev/null 2>&1 || { echo "no ip(8) on this host" >&2; exit 2; }

[ -n "$(ip -o -4 addr show dev "$IFACE" 2>/dev/null)" ] || {
    echo "no IPv4 address on $IFACE; -B names the host NIC the guest bridges onto" >&2
    exit 2; }

if [ -z "$GATEWAY" ]; then
    GATEWAY=$(ip -o -4 route show default dev "$IFACE" 2>/dev/null |
              awk '{ print $3; exit }')
fi
[ -n "$GATEWAY" ] || {
    echo "no default gateway on $IFACE; -g names something on this segment" >&2
    echo "that answers ICMP and can carry a default route" >&2
    exit 2; }
ping -c 1 -W 3 "$GATEWAY" >/dev/null 2>&1 || {
    echo "$GATEWAY does not answer this host, so a guest that cannot ping it" >&2
    echo "says nothing about the commands" >&2
    exit 2; }

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/ShowNetStatus" \
         "$TOOLS/netstat" "$TOOLS/ping" "$TOOLS/Online" "$TOOLS/Offline" \
         "$TOOLS/CheckNetConfig" "$TOOLS/GetNetStatus" "$TOOLS/AddNetRoute" \
         "$TOOLS/DeleteNetRoute" "$TOOLS/NetShutdown" \
         "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ] && [ "$BOARD" = a2065 ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
if [ "$BOARD" = a2065 ] && { [ -z "$A2065" ] || [ ! -f "$A2065" ]; }; then
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
fi

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/livetools-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"

# shellcheck source=../../tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"

if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ]; then
    if [ "$BOARD" = a2065 ]; then
        export AMINETXDUO_SANA2_DRIVER="$A2065"
    else
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi
fi

sana2_stage "$BOARD" "$STAGE/devs"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface ShowNetStatus netstat ping Online Offline \
         CheckNetConfig GetNetStatus AddNetRoute DeleteNetRoute NetShutdown; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

# "netstack: starting ThreadX" is at AMI_LOG_INFO, and the boot count below
# is read off it.  Ask the guest for that tier.
serial_log_stage_env "$STAGE" 2

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:ShowNetStatus
SYS:ShowNetStatus ALL
SYS:ShowNetStatus INTERFACES ARP ROUTES
SYS:ShowNetStatus IP ICMP TCP UDP MEMORY
SYS:ShowNetStatus TCPSOCKETS UDPSOCKETS
SYS:netstat -i
SYS:netstat -r
SYS:netstat -s
SYS:netstat -a
SYS:ping $GATEWAY -c 3 -t 20
SYS:ping $OFFNET -c 2 -t 20
SYS:Offline eth0
SYS:ShowNetStatus
SYS:Online eth0
SYS:ShowNetStatus
SYS:CheckNetConfig
SYS:GetNetStatus
SYS:GetNetStatus CHECK=INTERFACES,BCASTINTERFACES,RESOLVER,ROUTES,DEFAULTROUTE
SYS:AddNetRoute NETDESTINATION=$DUMMYNET GATEWAY=$GATEWAY
SYS:netstat -r
SYS:DeleteNetRoute DESTINATION=$DUMMYNET
SYS:DeleteNetRoute DEFAULTGATEWAY=$GATEWAY
SYS:AddNetRoute DEFAULTGATEWAY=$GATEWAY
SYS:NetShutdown
SYS:GetNetStatus CHECK=INTERFACES
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-livetools}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL, $BOARD bridged on $IFACE, gateway $GATEWAY"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/env" \
    "$STAGE/AddNetInterface" "$STAGE/ShowNetStatus" "$STAGE/netstat" \
    "$STAGE/ping" "$STAGE/Online" "$STAGE/Offline" \
    "$STAGE/CheckNetConfig" "$STAGE/GetNetStatus" "$STAGE/AddNetRoute" \
    "$STAGE/DeleteNetRoute" "$STAGE/NetShutdown"
RUN_RC=$?
set -e

SERIAL=$(serial_log_path "$AMINETXDUO_RUN_TAG")
REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

# ------------------------------------------------------------ assertions ---

FAILED=0

fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# ---- did the machine stay up? --------------------------------------------
if serial_log_have "$SERIAL" "$BUILD" "the boot count off the serial log" \
     2> /dev/null; then
    BOOTS=$(grep -c "netstack: starting ThreadX" "$SERIAL" || true)
    BOOT_SRC="the serial log"
else
    BOOTS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
    BOOT_SRC="the transcript (the serial log is empty; the stronger
       instrument reads it, so find out why nothing was written)"
fi

if [ "$BOOTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: $BOOT_SRC shows $BOOTS starts in one run"
    echo "       A command crashed hard enough to reset the Amiga. This is" >&2
    echo "       not a hang, whatever the transcript looks like, see" >&2
    echo "       docs/RESEARCH.md 25." >&2
elif [ "$BOOTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset), per $BOOT_SRC"
else
    fail "no start found in $BOOT_SRC, the run did not get far enough to judge"
fi

# ---- the negative half: the sentences that mean "I cannot see the stack" ----
while IFS= read -r phrase; do
    [ -n "$phrase" ] || continue
    if grep -qiF -- "$phrase" "$REPORT"; then
        fail "a command printed \"$phrase\" while the network was up"
        grep -niF -- "$phrase" "$REPORT" | sed 's/^/       /' >&2
    else
        pass "nothing printed \"$phrase\""
    fi
done <<'EOF'
the network is up, but this command cannot read it
the network has not been started
the network stack is not running
the stack is running but has no IP instance
there is no call yet
cannot be taken offline
individual interfaces cannot be taken up and down
the network kernel is not running
could not join the network kernel
EOF

# ---- the positive half: what a command that CAN see the stack must show ----

# THE LEASED ADDRESS IS READ, NOT ASSERTED.  A real DHCP server hands out
# whatever it has; what can be checked is that the guest got one, and that it
# is an address on the segment the emulator was bridged onto rather than a
# link-local or a leftover.
GUESTIP=$(sed -n 's/^.*online, address \([0-9][0-9.]*\).*$/\1/p' "$REPORT" |
          head -1)
if [ -z "$GUESTIP" ]; then
    fail "the guest never printed an address of its own"
elif ! ip -o route get "$GUESTIP" 2>/dev/null |
         grep -q "dev $IFACE .*src "; then
    fail "the guest's address $GUESTIP is not on $IFACE's segment"
    ip -o route get "$GUESTIP" 2>&1 | sed 's/^/       /' >&2
else
    pass "the guest leased $GUESTIP, which is on $IFACE's segment"
fi

# More than once: AddNetInterface prints it as it comes up, and a command that
# can see the stack prints it again.  Exactly one occurrence means every
# reader of the live configuration came back empty, which is the defect this
# file is named after.
SAW=0
[ -z "$GUESTIP" ] || SAW=$(grep -cF -- "$GUESTIP" "$REPORT" || true)
if [ "$SAW" -gt 1 ]; then
    pass "the leased address was read back by $((SAW - 1)) further line(s)"
else
    fail "only the bring-up line mentioned ${GUESTIP:-the leased address}: no command read it back"
fi

if grep -qF -- "$GATEWAY" "$REPORT"; then
    pass "the gateway $GATEWAY was reported"
else
    fail "no command reported the gateway $GATEWAY"
fi

if grep -Eqi '(packets received|received)[^0-9]*[1-9][0-9]*' "$REPORT"; then
    pass "a non-zero receive counter was reported"
else
    fail "every counter reported was zero, the stats path is not live"
fi

if grep -qF "bytes from $GATEWAY" "$REPORT"; then
    pass "ping $GATEWAY got a reply"
else
    fail "ping $GATEWAY got no reply"
fi

if grep -Eq '0% packet loss|[1-9][0-9]* received' "$REPORT"; then
    pass "ping reported packets received"
else
    fail "ping reported no packets received at all"
fi

# ---- the lifecycle commands, against the stack that is up -----------------

rc_of() {
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- rc / { print; exit }
    ' "$REPORT" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'
}

check_rc() {
    local want="$1" cmd="$2" why="$3"
    local got
    got=$(rc_of "$cmd")
    if [ "$got" = "$want" ]; then
        pass "$why (rc $got)"
    else
        fail "$why, '$cmd' returned '$got', not $want"
    fi
}

check_rc 0 "SYS:CheckNetConfig" \
    "CheckNetConfig passes a configuration that works"

check_rc 0 \
    "SYS:GetNetStatus CHECK=INTERFACES,BCASTINTERFACES,RESOLVER,ROUTES,DEFAULTROUTE" \
    "GetNetStatus finds the network ready"

check_rc 0 "SYS:AddNetRoute NETDESTINATION=$DUMMYNET GATEWAY=$GATEWAY" \
    "AddNetRoute added a route"

if awk -v want="$DUMMYNET" '$0 == "===== SYS:netstat -r =====" { on = 1; next }
        on && /^----- / { on = 0 }
        on && index($0, want) { found = 1 }
        END { exit !found }' "$REPORT"; then
    pass "netstat -r shows the route that was added"
else
    fail "netstat -r does not show $DUMMYNET, AddNetRoute added nothing"
fi

check_rc 0 "SYS:DeleteNetRoute DESTINATION=$DUMMYNET" \
    "DeleteNetRoute removed it again"

check_rc 0 "SYS:DeleteNetRoute DEFAULTGATEWAY=$GATEWAY" \
    "DeleteNetRoute cleared the default route"
check_rc 0 "SYS:AddNetRoute DEFAULTGATEWAY=$GATEWAY" \
    "AddNetRoute set it back"

check_rc 0 "SYS:NetShutdown" "NetShutdown stopped the interfaces"
check_rc 5 "SYS:GetNetStatus CHECK=INTERFACES" \
    "and the network is no longer ready"

echo
printf 'iface=%s\n' "$IFACE"
printf 'gateway=%s\n' "$GATEWAY"
printf 'guest_ip=%s\n' "${GUESTIP:-none}"
printf 'boots=%s\n' "$BOOTS"
printf 'run_rc=%s\n' "$RUN_RC"
if [ "$FAILED" -ne 0 ]; then
    printf 'RESULT=fail\n'
    exit 1
fi

printf 'RESULT=pass\n'
exit 0
