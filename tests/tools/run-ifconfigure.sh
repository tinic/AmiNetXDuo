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
#   default gateway "and touches nothing else", where the only route before it
#   was RemoveNetInterface followed by AddNetInterface.  Three separate claims,
#   and each fails in its own way:
#
#     * the numbers really change            -- netstat -i and netstat -r
#     * the interface is still the same one  -- Ipkts, the SANA-II receive
#                                               counter, which a remove and an
#                                               add reset to zero because the
#                                               AmiSana2If goes with them
#     * it still carries traffic afterwards  -- a ping that has to get replies
#
#   The third is not decoration.  tests/tools/run-ifremove.sh was written
#   because an interface can be listed with its address and have a link that is
#   down, which every assertion on the address alone passes straight through.
#
# WHAT ITS TEMPLATE OFFERS, AND SO WHAT IS ALSO CHECKED
#
#   INTERFACE/A     required, and a name that is not there is refused
#   QUIET/S         says nothing and still does it
#   ADDRESS/K       on its own, and with a /bits prefix length that sets the
#                   mask as well
#   NETMASK/K       on its own -- the read-modify-write half, where the address
#                   has to survive untouched
#   GATEWAY/K       set, and NONE to clear
#
#   plus the four ways it can end: OK, WARN with the stack down, ERROR on an
#   argument line that cannot be acted on, FAIL when the stack refuses.
#
# THE REFUSALS ARE ASSERTED TO CHANGE NOTHING.  A command that validates after
# it has applied half the list leaves the interface in a state nobody asked
# for, and the only way to see that is to read the interface back after each
# refusal rather than only after the changes.
#
# ONE BOOT.  ToolsSmoke reopens its transcript from the top after a reset, so a
# reboot would turn "reconfigured four times" into a shorter list that still
# reads as a pass; the count of adds is asserted for that reason.  The steps are
# ordered so each leaves the machine in the state the next one needs, and the
# no-network case runs first because it is the only one that needs the stack
# down.
#
# WHY eth0 IS STATIC HERE, AND NOT DHCP
#
#   Because a DHCP interface has a second writer.  The client re-binds and
#   writes an address, a mask and a gateway back, so a transcript showing any
#   of them proves nothing about what this command did.  Under STATIC, what
#   netstat says afterwards is this command and nothing else.  The DHCP side of
#   ConfigureNetInterface has its own harness.
#
#   The command prints a warning line when the interface it changed takes its
#   address by DHCP.  That line NOT appearing is asserted here, which is the
#   half this harness can prove: a warning printed on a static interface would
#   be a warning printed on every interface.
#
# SLIRP by default, which needs no peer: this is the stack's own bookkeeping.
# -A runs it bridged.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# A TIMEOUT IS A DEFECT.  The ceiling below is twice the measured good case,
# which is stated beside it.  A run that burns it produces a partial transcript
# and proves nothing; the verdict names the command the transcript stops at.
# Raising -t is never the fix.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
# THE GOOD CASE IS 21 SECONDS, measured on playhouse3 (Amiberry 68020 A1200 on
# SLIRP) for the whole list below: boot, one add, three pings and twenty-one
# commands.  The ceiling is the same arithmetic tests/tools/run-toolleak.sh
# uses, a boot of its own plus twice the measured work, because an emulator
# that costs more than double what it has already been seen to cost is hung.
# The run prints what it really took, so the number below stays checkable.
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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/ifconfigure-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# Static, and SLIRP's own numbers.  10.0.2.15 is the address it hands out and
# 10.0.2.2 is the gateway it answers on, so both ends of every change below are
# addresses that really exist on this network.
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

# Step by step, and every line is asserted below by its position in this list.
# The three pings are the spine: the first says the machine can reach the
# gateway at all, the second says it can still reach it from a DIFFERENT
# address than the one it booted with, and the third says the interface came
# back usable after everything above.
#
# The first ping comes BEFORE the first `netstat -i` on purpose.  That netstat
# is where the receive counter is read as a baseline, and an interface that has
# been up for a moment and been sent nothing reports 0 -- against which "the
# counter did not go backwards" is true of an interface that was thrown away
# and rebuilt.  The ping puts traffic through it first, so the baseline is a
# number that a reset would destroy.
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

# ------------------------------------------------------------------ run ---

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

# A TIMEOUT IS A DEFECT, NOT A RESULT.  A partial transcript makes every
# assertion below read the wrong block, and the first few would still pass, so
# the shortfall is caught here and named before anything else runs.  Exit 2:
# infrastructure, not a verdict on the command.
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

# The Nth block for a given command banner: its output and nothing else, so
# every assertion below reads the transcript by the same rule.
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

# The receive counter out of the Nth `netstat -i`.  A remove and an add reset
# it, because the AmiSana2If that holds it is freed with the interface, so this
# is what says the interface was reconfigured rather than replaced.
ipkts() {
    ifaces "$1" | awk '$1 == "eth0" { print $5; exit }'
}

# The nth ping, which must have got both replies back.  ping returns non-zero
# when nothing came back, so the rc and the count are both read: a count printed
# by a command that then failed is not a delivery.
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

# ---- one boot ------------------------------------------------------------
ADDS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$ADDS" -eq 1 ]; then
    pass "the machine booted once and ran the whole list"
elif [ "$ADDS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never got as far as bringing eth0 up, something hung"
fi

# ---- 1. nothing to configure, because nothing is up ----------------------
says "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20" 1 \
     "The network is not running" \
     "configuring before any add says the network is not running"
want_rc "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20" 1 5 \
        "and returns WARN, not a failure"

# ---- 2. what it started with --------------------------------------------
#
# The control for everything below: if eth0 does not come up addressed with a
# link that carries traffic, no assertion after a change says anything.
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

# ---- 3. a name that is not there -----------------------------------------
says "SYS:ConfigureNetInterface nosuch0 ADDRESS 10.0.2.20" 1 \
     'there is no interface called "nosuch0"' \
     "an unknown name is reported by name"
want_rc "SYS:ConfigureNetInterface nosuch0 ADDRESS 10.0.2.20" 1 20 \
        "and returns FAIL"

# ---- 4. the four argument lines that cannot be acted on ------------------
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

# ...and none of the four changed anything.  This is the assertion that catches
# a command which applies as it parses.
if up_on 2 10.0.2.15; then
    pass "and after all four refusals eth0 is still 10.0.2.15 with the link up"
else
    fail "a refused argument line changed the interface anyway"
    ifaces 2 | sed 's/^/       /' >&2
fi

# ---- 5. ADDRESS with a prefix length -------------------------------------
want_rc "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24" 1 0 \
        "ADDRESS with a /bits is accepted"
says "SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20/24" 1 \
     "eth0: 10\.0\.2\.20 netmask 255\.255\.255\.0" \
     "and reports what the interface now has, read back from the stack"

# The DHCP warning must NOT be here: this interface is STATIC.  A line printed
# on every interface would be a line that says nothing.
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

# THE CLAIM THIS FILE EXISTS FOR: the interface was reconfigured, not replaced.
# A remove and an add would have freed the AmiSana2If and started this at zero.
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

# ---- 6. NETMASK on its own is a read-modify-write ------------------------
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

# ---- 7. a gateway this machine cannot reach ------------------------------
says "SYS:ConfigureNetInterface eth0 GATEWAY 192.0.2.1" 1 \
     "not on any of this machine's own subnets" \
     "a gateway off every subnet is refused, and says why"
want_rc "SYS:ConfigureNetInterface eth0 GATEWAY 192.0.2.1" 1 20 "and returns FAIL"

# ---- 8. GATEWAY NONE clears the default route ----------------------------
want_rc "SYS:ConfigureNetInterface eth0 GATEWAY NONE" 1 0 "GATEWAY NONE is accepted"
says "SYS:ConfigureNetInterface eth0 GATEWAY NONE" 1 \
     "default gateway is cleared" "and says so"
if routes 4 | grep -Eq '^default +'; then
    fail "the default route is still there after GATEWAY NONE"
    routes 4 | sed 's/^/       /' >&2
else
    pass "and the default route is gone from the table"
fi

# ---- 9. QUIET, several keywords at once, and back where it started -------
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
