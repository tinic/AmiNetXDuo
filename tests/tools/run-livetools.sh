#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "the command says the network is down while it is up".
#
#   tests/tools/run-livetools.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT WENT WRONG, AND WHY NOTHING CAUGHT IT
#
#   v0.2.0 shipped `ping`, `netstat` and `ShowNetStatus` with their live paths
#   dead.  A Shell command links its own copy of ThreadX and NetX Duo whose
#   kernel never ran; the stack that did run is inside bsdsocket.library.
#   src/tools/netstack_weak.c's weak netstack_ip() returned NULL, so every one
#   of them printed
#
#       netstat: the network is up, but this command cannot read it
#
#   and exited.  That is a well-formed, polite, correctly-spelled message, and
#   every test that existed read it as a pass, because the commands handle
#   the not-up case gracefully and no test asserted that the case was not
#   supposed to be reached.  The one capture that showed live counters came
#   from a purpose-built `LiveTools` binary that linked the netstack and ran
#   each command's main() in-process, which is exactly how a defect in the
#   SHIPPED executables stayed invisible.
#
#   So this test runs THE SHIPPED EXECUTABLES, from the same staged directory
#   a user would install, and asserts on what they print.
#
# THE SHAPE OF THE ASSERTION
#
#   Two halves, and both are needed.  A negative half, these phrases must
#   not appear anywhere after the stack is up, catches the exact regression.
#   A positive half, the leased address, a non-zero counter, a ping reply --
#   catches a command that stops printing the message by printing nothing.
#
#   The negative half is written against the MESSAGES rather than against exit
#   codes on purpose: the failing commands exited 5 (RETURN_ERROR), which a
#   ToolsSmoke run records and nobody reads.  A grep for the sentence is the
#   assertion a human would make.
#
# ONE BOOT
#
#   Everything runs from a single emulator boot through ToolsSmoke, because
#   the FS-UAE lock serialises every agent on this machine and a test that
#   boots six times costs six times as much wall clock for no more coverage.
#
# AND NO HOST-SIDE PEER, DELIBERATELY
#
#   Every other command test starts a Python server on the host and gives it a
#   lifetime.  With several agents queued on the emulator lock that lifetime
#   can expire before the guest has booted, and then every case fails with
#   "connection refused", which looks exactly like the command being broken
#   and is not.  tests/tools/netpeer.py did precisely that.
#
#   This test needs no peer.  SLIRP answers DHCP itself, answers ICMP to its
#   own alias 10.0.2.2 itself, and is alive for exactly as long as the
#   emulator is.  There is nothing here that can time out while the run waits
#   its turn, so a failure from this script is a failure in the commands.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

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
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
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

# The run.  AddNetInterface is FIRST and everything after it is asserted on:
# from that line down, every command is talking to a stack that is up with a
# DHCP lease from SLIRP.
#
# `ping 10.0.2.2` and not 8.8.8.8: SLIRP answers its own alias locally with
# the ICMP identifier AND sequence preserved, while a proxied reply comes back
# with the sequence zeroed (docs/RESEARCH.md 20.2).  Both are pinged, because
# the second is the case that separates a command matching replies properly
# from one that does not, but only the first is asserted on, since a star
# against 8.8.8.8 is the emulator and not us.
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
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/ShowNetStatus" "$STAGE/netstat" \
    "$STAGE/ping" "$STAGE/Online" "$STAGE/Offline" \
    "$STAGE/CheckNetConfig" "$STAGE/GetNetStatus" "$STAGE/AddNetRoute" \
    "$STAGE/DeleteNetRoute" "$STAGE/NetShutdown"
RUN_RC=$?
set -e

SERIAL="$ROOT/build/serial-$AMINETXDUO_RUN_TAG.log"
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
#
# THIS ASSERTION EXISTS BECAUSE A REBOOT WAS TWICE RECORDED AS A HANG.
#
# `ping` used to jump into the middle of another function and take the machine
# down (docs/RESEARCH.md §25).  The guest restarted, ToolsSmoke reopened
# DH0:tools.txt from the top, and the transcript that came back was a short,
# well-formed file that simply stopped, indistinguishable from a command that
# was still running when the timeout fired.  Two separate investigations wrote
# it up as a hang before anyone counted the boots.
#
# One boot starts the netstack once, so `netstack: starting ThreadX` in the
# serial log is a boot counter, and it is the only thing in the whole run that
# can tell the two apart.  A crash that reboots is a DIFFERENT and much worse
# defect than a command that blocks, and a test that cannot say which it saw
# sends the next person looking in the wrong place.
#
# FS-UAE writes nothing to the serial port on some hosts, build/serial-*.log
# is zero bytes for every run on this Mac, INCLUDING runs that demonstrably
# worked.  Failing there reported a broken stack on a host where nothing was
# broken, and it did so for long enough to be recorded twice as a known false
# red, which is exactly how a test stops being read.
#
# So the serial log is preferred and not required.  When it is empty the
# transcript answers the same question, the way tests/tools/run-routes.sh
# already does it: ToolsSmoke reopens DH0:tools.txt from the top after a reset,
# so the FIRST command's banner appearing twice IS a reboot.  Same signal,
# different instrument.
#
# What is NOT done here is passing quietly when neither source can answer. A
# check that cannot tell a reboot from a hang has to say so.
if [ -s "$SERIAL" ]; then
    BOOTS=$(grep -c "netstack: starting ThreadX" "$SERIAL" || true)
    BOOT_SRC="the serial log"
else
    BOOTS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
    BOOT_SRC="the transcript (no serial output on this host)"
fi

if [ "$BOOTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: $BOOT_SRC shows $BOOTS starts in one run"
    echo "       A command crashed hard enough to reset the Amiga. This is" >&2
    echo "       not a hang, whatever the transcript looks like -- see" >&2
    echo "       docs/RESEARCH.md 25." >&2
elif [ "$BOOTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset), per $BOOT_SRC"
else
    fail "no start found in $BOOT_SRC -- the run did not get far enough to judge"
fi

# ---- the negative half: the sentences that mean "I cannot see the stack" ----
#
# Each of these is a real string from src/tools/.  If any of them reaches the
# transcript, a shipped command has gone blind again.
#
# They are matched as whole phrases, not as fragments.  An earlier draft of
# this list had a bare "there is no", which "there is no interface called
# \"eth9\"", a correct answer to a mistyped name, trips on.  A regression
# test that fires on correct behaviour gets disabled, and then it protects
# nothing.
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

# SLIRP's first lease.  It appears only if an interface snapshot really came
# out of the running NX_IP.
if grep -q "10\.0\.2\.15" "$REPORT"; then
    pass "the leased address 10.0.2.15 was reported"
else
    fail "no command reported the leased address 10.0.2.15"
fi

# The default route SLIRP hands out.  netstat -r prints it; if the routing
# answer were empty this would be missing.
if grep -q "10\.0\.2\.2" "$REPORT"; then
    pass "the gateway 10.0.2.2 was reported"
else
    fail "no command reported the gateway 10.0.2.2"
fi

# A counter that cannot be zero on a machine that completed DHCP.  Written as
# "a line with a counter name and a number that is not 0", so it does not
# depend on the exact column layout.
if grep -Eqi '(packets received|received)[^0-9]*[1-9][0-9]*' "$REPORT"; then
    pass "a non-zero receive counter was reported"
else
    fail "every counter reported was zero -- the stats path is not live"
fi

# At least one ICMP echo reply.  10.0.2.2 is answered by SLIRP itself with the
# sequence number intact, so this one must succeed.
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
#
# These five were written after the two halves above and are asserted the same
# way, because they can fail the same way: a route command that prints a
# success line while adding nothing, or a GetNetStatus that answers "not ready"
# because it cannot see in, would both pass a test written against exit status
# alone.  So each one is checked on what it PRINTED and on the number it
# RETURNED, and the two have to agree.

# The return code of one command in the transcript, by its command line.
rc_of() {
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- rc / { print $3; exit }
    ' "$REPORT"
}

check_rc() {
    local want="$1" cmd="$2" why="$3"
    local got
    got=$(rc_of "$cmd")
    if [ "$got" = "$want" ]; then
        pass "$why (rc $got)"
    else
        fail "$why -- '$cmd' returned '$got', not $want"
    fi
}

# CheckNetConfig must find nothing wrong with a configuration that demonstrably
# works.  This is the false-positive half: a checker that complains about the
# machine it is running on correctly is worse than no checker.
check_rc 0 "SYS:CheckNetConfig" \
    "CheckNetConfig passes a configuration that works"

# GetNetStatus, on a machine that has a lease, a gateway and a name server from
# SLIRP: every one of the five must be satisfied.
check_rc 0 \
    "SYS:GetNetStatus CHECK=INTERFACES,BCASTINTERFACES,RESOLVER,ROUTES,DEFAULTROUTE" \
    "GetNetStatus finds the network ready"

# A route really added, and really visible afterwards.  netstat -r reads the
# same table through a different command, so the second line is the one that
# rules out AddNetRoute having printed a success it did not achieve.
check_rc 0 "SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=10.0.2.2" \
    "AddNetRoute added a route"

# Two blocks carry this header, one before the route was added and one
# after, so the scan must cover both rather than stopping at the first.
if awk '$0 == "===== SYS:netstat -r =====" { on = 1; next }
        on && /^----- / { on = 0 }
        on && /192\.168\.77\.0/ { found = 1 }
        END { exit !found }' "$REPORT"; then
    pass "netstat -r shows the route that was added"
else
    fail "netstat -r does not show 192.168.77.0 -- AddNetRoute added nothing"
fi

check_rc 0 "SYS:DeleteNetRoute DESTINATION=192.168.77.0" \
    "DeleteNetRoute removed it again"

# The default route, cleared and set again.  Both halves go through
# NETCTRL_GATEWAY_CLEAR/SET, which exist in every build of this stack.
check_rc 0 "SYS:DeleteNetRoute DEFAULTGATEWAY=10.0.2.2" \
    "DeleteNetRoute cleared the default route"
check_rc 0 "SYS:AddNetRoute DEFAULTGATEWAY=10.0.2.2" \
    "AddNetRoute set it back"

# NetShutdown, and the proof that it did something: the same question that
# answered 0 above must answer 5 afterwards.
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
