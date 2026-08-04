#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "the configuration is wrong and nothing says so".
#
#   tests/tools/run-checkconfig.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT PROVES
#
#   CheckNetConfig exists to name a file, a line and a fix.  A checker that
#   printed "there are problems" would pass any test written against its exit
#   code and be worth nothing to the person at the keyboard, so every assertion
#   here is on a SPECIFIC finding against a SPECIFIC file, and one of them is
#   the reverse: a correct file that must not be complained about, because a
#   checker that fires on correct configuration gets ignored and then protects
#   nothing.
#
#   The staged DEVS: in tests/tools/checkconfig/devs is wrong in seven ways
#   that a parser cannot see, because each needs either the rest of the
#   configuration or the hardware:
#
#     bad0    names a driver that is not on this machine
#     bad1    is the broadcast address of its own network, on eth0's card
#     bad2    has a netmask whose ones are not contiguous, on a unit that
#             does not open
#     routes  points at a router on a network this machine is not on
#     hosts   has a line whose first column is not an address
#     services has a line whose port column is not a port
#     name_resolution is off-network and REACHABLE, and must be left alone
#
#   The same boot also runs the four other new commands with no stack running,
#   which is the state their argument handling has to survive.
#
# ONE BOOT, for the reason run-livetools.sh gives: the FS-UAE lock serialises
# every agent on this machine.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  It is needed even though no network
# is started, because the probe that separates "wrong driver" from "wrong
# unit" opens the real device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=180
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

for f in "$TOOLS/ToolsSmoke" "$TOOLS/CheckNetConfig" "$TOOLS/GetNetStatus" \
         "$TOOLS/NetShutdown" "$TOOLS/AddNetRoute" "$TOOLS/DeleteNetRoute"; do
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

STAGE="$ROOT/build/checkconfig-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$ROOT/tests/tools/checkconfig/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
for t in CheckNetConfig GetNetStatus NetShutdown AddNetRoute DeleteNetRoute; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

# bsdsocket.library is deliberately NOT staged.  Every command below meets a
# machine with no stack running, which is the state CheckNetConfig is FOR and
# the state the other four have to answer sensibly rather than crash in.
cat > "$STAGE/commands.txt" <<'EOF'
SYS:CheckNetConfig
SYS:CheckNetConfig VERBOSE
SYS:CheckNetConfig QUIET
SYS:GetNetStatus
SYS:GetNetStatus CHECK=INTERFACES,DEFAULTROUTE
SYS:GetNetStatus CHECK=NOTACONDITION
SYS:NetShutdown
SYS:AddNetRoute
SYS:AddNetRoute DEFAULTGATEWAY=192.168.1.1
SYS:AddNetRoute NETDESTINATION=192.168.10.0 GATEWAY=192.168.1.1
SYS:DeleteNetRoute DESTINATION=192.168.10.0
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-checkconfig}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with a deliberately broken DEVS:"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
    "$STAGE/CheckNetConfig" "$STAGE/GetNetStatus" "$STAGE/NetShutdown" \
    "$STAGE/AddNetRoute" "$STAGE/DeleteNetRoute"
RUN_RC=$?
set -e

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

# Everything one command printed, from its header to its return-code line.
block() {
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- / { exit }
        on { print }
    ' "$REPORT"
}

rc_of() {
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- rc / { print $3; exit }
    ' "$REPORT"
}

expect() {
    local what="$1"; shift
    if grep -qiF, "$*" "$REPORT"; then
        pass "$what"
    else
        fail "$what, nothing printed \"$*\""
    fi
}

reject() {
    local what="$1"; shift
    if grep -qiF, "$*" "$REPORT"; then
        fail "$what, \"$*\" was printed and should not have been"
        grep -niF, "$*" "$REPORT" | sed 's/^/       /' >&2
    else
        pass "$what"
    fi
}

# ---- did the run reach the end? -------------------------------------------
if grep -q "===== done" "$REPORT"; then
    pass "every command ran to completion"
else
    fail "the run stopped early, a command hung or took the machine down"
fi

# ---- CheckNetConfig names the file and what is wrong with it --------------

expect "bad0's missing driver is named"        "DEVS:NetInterfaces/bad0"
expect "and so is the driver it names"         "nosuchcard.device"
expect "bad1's broadcast address is caught"    "broadcast address of its own network"
expect "bad2's netmask is caught"              "is not a netmask"
expect "the off-network router is caught"      "not on any network this machine is"
expect "the router's file is named"            "DEVS:Internet/routes"
expect "two interfaces on one card is caught"  "both claim"
expect "the bad hosts line is caught"          "DEVS:Internet/hosts, line 2"
expect "the bad services line is caught"       "DEVS:Internet/services, line 2"

# The one that must NOT fire.  8.8.8.8 is not on this machine's network, and a
# default route exists to reach it through, so it is correct and complaining
# about it would be the false positive that gets a checker ignored.
reject "the reachable name server is left alone" "DEVS:Internet/name_resolution"

# The correct interface is not accused of anything.  eth0 may be NAMED (it is
# half of the "two interfaces, one card" finding); what it must not carry is a
# complaint about its own address.
if block "SYS:CheckNetConfig" | grep -q "NetInterfaces/eth0, line"; then
    fail "the correct interface eth0 was complained about"
else
    pass "the correct interface eth0 was not complained about"
fi

# ---- the return codes, which are what a script reads ----------------------

RC=$(rc_of "SYS:CheckNetConfig")
if [ "$RC" = "5" ]; then
    pass "CheckNetConfig returned 5, which IF WARN tests"
else
    fail "CheckNetConfig returned '$RC', not 5"
fi

# QUIET means QUIET: the block between the header and the rc line is empty.
if [ -z "$(block 'SYS:CheckNetConfig QUIET' | tr -d '[:space:]')" ]; then
    pass "CheckNetConfig QUIET printed nothing"
else
    fail "CheckNetConfig QUIET printed something"
    block 'SYS:CheckNetConfig QUIET' | sed 's/^/       /' >&2
fi

RC=$(rc_of "SYS:CheckNetConfig QUIET")
if [ "$RC" = "5" ]; then
    pass "CheckNetConfig QUIET still returned 5"
else
    fail "CheckNetConfig QUIET returned '$RC', not 5, QUIET changed the answer"
fi

# ---- the other four, with no stack running -------------------------------

RC=$(rc_of "SYS:GetNetStatus")
if [ "$RC" = "5" ]; then
    pass "GetNetStatus returned 5 with the network down"
else
    fail "GetNetStatus returned '$RC', not 5, with the network down"
fi

RC=$(rc_of "SYS:GetNetStatus CHECK=NOTACONDITION")
if [ "$RC" = "10" ]; then
    pass "an unknown CHECK condition returned 10"
else
    fail "an unknown CHECK condition returned '$RC', not 10"
fi
expect "and it lists the conditions there are" "DEFAULTROUTE"

expect "NetShutdown says there is nothing to stop" "nothing to stop"
expect "AddNetRoute says routes need a running stack" "the network is not running"

# None of them may claim to have done anything.
reject "no route was reported as added"   "now go through"
reject "no default route was reported set" "now goes through"

echo
if [ "$FAILED" -ne 0 ]; then
    echo "checkconfig: FAILED" >&2
    exit 1
fi

echo "checkconfig: PASSED"
exit 0
