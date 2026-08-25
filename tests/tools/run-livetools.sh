#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "the command says the network is down while it is up".
#
#   tests/tools/run-livetools.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
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

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/livetools-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface ShowNetStatus netstat ping Online Offline \
         CheckNetConfig GetNetStatus AddNetRoute DeleteNetRoute NetShutdown; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

cat > "$STAGE/commands.txt" <<'EOF'
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
SYS:ping 10.0.2.2 -c 3 -t 20
SYS:ping 8.8.8.8 -c 2 -t 20
SYS:Offline eth0
SYS:ShowNetStatus
SYS:Online eth0
SYS:ShowNetStatus
SYS:CheckNetConfig
SYS:GetNetStatus
SYS:GetNetStatus CHECK=INTERFACES,BCASTINTERFACES,RESOLVER,ROUTES,DEFAULTROUTE
SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=10.0.2.2
SYS:netstat -r
SYS:DeleteNetRoute DESTINATION=192.168.77.0
SYS:DeleteNetRoute DEFAULTGATEWAY=10.0.2.2
SYS:AddNetRoute DEFAULTGATEWAY=10.0.2.2
SYS:NetShutdown
SYS:GetNetStatus CHECK=INTERFACES
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-livetools}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
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
    BOOT_SRC="the transcript (the serial log is empty; build with
       -DAMINETXDUO_LOG=ON for the stronger instrument)"
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

if grep -q "10\.0\.2\.15" "$REPORT"; then
    pass "the leased address 10.0.2.15 was reported"
else
    fail "no command reported the leased address 10.0.2.15"
fi

if grep -q "10\.0\.2\.2" "$REPORT"; then
    pass "the gateway 10.0.2.2 was reported"
else
    fail "no command reported the gateway 10.0.2.2"
fi

if grep -Eqi '(packets received|received)[^0-9]*[1-9][0-9]*' "$REPORT"; then
    pass "a non-zero receive counter was reported"
else
    fail "every counter reported was zero, the stats path is not live"
fi

if grep -Eq 'bytes from 10\.0\.2\.2' "$REPORT"; then
    pass "ping 10.0.2.2 got a reply"
else
    fail "ping 10.0.2.2 got no reply"
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

check_rc 0 "SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=10.0.2.2" \
    "AddNetRoute added a route"

if awk '$0 == "===== SYS:netstat -r =====" { on = 1; next }
        on && /^----- / { on = 0 }
        on && /192\.168\.77\.0/ { found = 1 }
        END { exit !found }' "$REPORT"; then
    pass "netstat -r shows the route that was added"
else
    fail "netstat -r does not show 192.168.77.0, AddNetRoute added nothing"
fi

check_rc 0 "SYS:DeleteNetRoute DESTINATION=192.168.77.0" \
    "DeleteNetRoute removed it again"

check_rc 0 "SYS:DeleteNetRoute DEFAULTGATEWAY=10.0.2.2" \
    "DeleteNetRoute cleared the default route"
check_rc 0 "SYS:AddNetRoute DEFAULTGATEWAY=10.0.2.2" \
    "AddNetRoute set it back"

check_rc 0 "SYS:NetShutdown" "NetShutdown stopped the interfaces"
check_rc 5 "SYS:GetNetStatus CHECK=INTERFACES" \
    "and the network is no longer ready"

echo
if [ "$FAILED" -ne 0 ]; then
    echo "livetools: FAILED" >&2
    exit 1
fi

echo "livetools: PASSED"
exit 0
