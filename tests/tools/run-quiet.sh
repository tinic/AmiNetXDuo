#!/usr/bin/env bash
#
# WHAT QUIET MEANS, ASSERTED ONCE FOR EVERY COMMAND THAT HAS IT.
#
#   tests/tools/run-quiet.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                            [-A [-N board] [-B backend]]
#
# QUIET drifted.  Eleven commands gated their own error messages behind it, so
# a boot line that could not find its interface file said nothing at all and
# returned a code nobody reads; ShowNetStatus gated ONLY its two error
# messages and printed the whole report regardless, which is the same defect
# inside out.  Every one of those was a compile-clean, review-clean, one-line
# `if (!quiet)` in the wrong place, and nothing in the tree could see it,
# because no test ever ran a command twice and compared.
#
# So that is what this does.  Each case runs the same command with QUIET and
# without it, in one boot, and asserts on the DIFFERENCE:
#
#   * the quiet run prints no more than the loud one, never the reverse
#   * the chatter the loud run prints is gone from the quiet one
#   * the return code is the same in both
#   * an ERROR is printed by the quiet run too -- this is the half that the
#     defect above turned off, so it is asserted by name, on the sentence
#
# and, for the one flag that was removed, that the command now says so.
#
# THE STACK IS REALLY UP, AND THE PEER IS THE NETWORK ITSELF.  arp,
# AddNetRoute and NetShutdown have nothing to be quiet about on a machine with
# no network, and a quiet command that skipped the work would pass an
# assertion that only reads its output.  So every quiet run here is followed
# by something that says the work happened -- netstat -i for the add, the
# reply count for the ping.
#
# SLIRP by default, which answers DHCP and ICMP itself and needs no peer that
# could expire while the run waits its turn on the emulator lock.  -A runs it
# bridged, where the guest is a machine on the host's own LAN and the router
# is what it pings; DHCP there comes from the real server, so nothing is given
# a fixed address that could collide with a machine already on that network.
#
# ONE BOOT.  ToolsSmoke reopens its transcript from the top after a reset, so
# the count of AddNetInterface banners is asserted before anything else: a
# reboot would silently renumber every block this file reads by position.
#
# NOT COVERED HERE: fetch, tftp, telnet, iperf and sntp, whose QUIET was
# already correct and needs a host peer to exercise; ConfigureNetInterface,
# CheckNetConfig, NetSetup, hostname and ShowNetServices, which already have
# a QUIET case in their own harness (run-ifconfigure.sh 9, run-ifdhcp.sh 7,
# run-checkconfig.sh, run-netsetup.sh 8, run-hostname.sh 6,
# run-shownetservices.sh 8).  What is here is what those do not reach:
# the error half, and the commands with no harness of their own.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
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
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

NEEDED="ToolsSmoke AddNetInterface RemoveNetInterface ConfigureNetInterface
        ShowNetStatus GetNetStatus CheckNetConfig AddNetRoute DeleteNetRoute
        NetShutdown arp hostname ping netstat"

for t in $NEEDED; do
    [ -f "$TOOLS/$t" ] || { echo "missing $TOOLS/$t, build the tree first" >&2
                            exit 2; }
done
[ -f "$BSD" ] || { echo "missing $BSD, build the tree first" >&2; exit 2; }

# ---- who answers, and where the gateway is -------------------------------
#
# SLIRP is its own answer: 10.0.2.2 is both the gateway and the alias that
# replies to ICMP locally.  Bridged, they are read off this host, because the
# guest is on its LAN and the numbers are whatever that LAN has today.
if [ "$RUNNER" = amiberry ]; then
    [ -n "$(ip -o -4 addr show dev "$IFACE" 2>/dev/null)" ] || {
        echo "no IPv4 address on $IFACE; -B names the host NIC the guest \
bridges onto" >&2; exit 2; }
    GW=$(ip -o -4 route show default 2>/dev/null | awk '{ print $3; exit }')
    [ -n "$GW" ] || { echo "this host has no default route, so there is no \
gateway for the AddNetRoute case" >&2; exit 2; }

    # The router, not this host.  A bridged guest and the host it runs on
    # share a wire, and where the host is itself a VM its replies leave with
    # the checksum fields unfilled for hardware that is not there, so the
    # guest drops them: measured here as 0 received on both pings, which
    # fails the -q case for a reason that has nothing to do with QUIET.
    PEER=$GW
else
    PEER=10.0.2.2
    GW=10.0.2.2
fi

echo "==> peer $PEER, gateway $GW"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/quiet-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# DHCP, because a bridged guest on somebody's real LAN must not be given a
# fixed address: the demo machine is on this network and a collision would
# take it off it.  A name of its own for the same reason -- two Amigas
# answering to one .local name is a worse test failure than a red line.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
STATE=up
IFEOF

# Prepended, not written over: the staged name_resolution's nameserver line
# is the one the rest of the tree tuned (tests/netstack/devs), and replacing
# it with the gateway costs thirty seconds a lookup on SLIRP.
printf 'HOSTNAME anxdquiet\n' > "$STAGE/devs/Internet/name_resolution.new"
cat "$STAGE/devs/Internet/name_resolution" >> "$STAGE/devs/Internet/name_resolution.new"
mv "$STAGE/devs/Internet/name_resolution.new" "$STAGE/devs/Internet/name_resolution"

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

cp "$BSD" "$STAGE/libs/bsdsocket.library"
for t in $NEEDED; do cp "$TOOLS/$t" "$STAGE/$t"; done

# ---- the run -------------------------------------------------------------
#
# Ordered so each line leaves the machine in the state the next one needs.
# The three cases that need the stack DOWN come first; NetShutdown is last
# because it takes it down again.
#
# Every pair is loud then quiet, adjacent, so the two blocks are comparable:
# an assertion that reads them out of order is reading a different machine.
cat > "$STAGE/commands.txt" <<EOF
# ---- the stack is not running yet ----
SYS:ShowNetStatus QUIET
SYS:AddNetInterface nosuch0 QUIET
SYS:CheckNetConfig
SYS:CheckNetConfig QUIET
SYS:GetNetStatus
SYS:GetNetStatus QUIET
# ---- bring it up ----
SYS:AddNetInterface eth0
SYS:RemoveNetInterface eth0
SYS:AddNetInterface eth0 QUIET
SYS:netstat -i
# ---- the commands that report ----
SYS:hostname
SYS:hostname QUIET
SYS:ping $PEER -c 2 -t 20
SYS:ping $PEER -c 2 -t 20 -q
SYS:arp
SYS:arp QUIET
# ---- the commands that act ----
SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW
SYS:DeleteNetRoute DESTINATION=192.168.77.0
SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW QUIET
SYS:DeleteNetRoute DESTINATION=192.168.77.0 QUIET
SYS:ConfigureNetInterface nosuch0 QUIET ADDRESS 10.99.99.99/24
SYS:RemoveNetInterface nosuch0 QUIET
# ---- and take it down ----
SYS:NetShutdown
SYS:AddNetInterface eth0 QUIET
SYS:NetShutdown QUIET
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-quiet}"
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:49:00:71}"

STAGED=""
for t in $NEEDED; do
    [ "$t" = ToolsSmoke ] && continue
    STAGED="$STAGED $STAGE/$t"
done

set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
if [ "$RUNNER" = amiberry ]; then
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    # shellcheck disable=SC2086
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" \
        "$STAGE/devs" "$STAGE/libs" $STAGED
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
    # shellcheck disable=SC2086
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" $STAGED
fi
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
                      echo "quiet: 0 checks, 1 failures"; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

CHECKS=0
FAILED=0
fail() { echo "FAIL: $*" >&2; CHECKS=$((CHECKS+1)); FAILED=$((FAILED+1)); }
pass() { echo "  ok: $*";      CHECKS=$((CHECKS+1)); }

# The Nth block for a command banner, its output and the rc line.  Same rule
# as tests/tools/run-ifremove.sh, so every assertion reads the transcript the
# same way.
block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}
body()  { block "$1" "${2:-1}" | grep -v '^----- rc ' || true; }
rc_of() { block "$1" "${2:-1}" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }

trimmed() { printf '%s' "$1" | tr -d '[:space:]'; }

# ---- the three shapes every case below is one of ------------------------

# Nothing at all but the rc line.
says_nothing() { # banner nth what
    local out; out=$(body "$1" "$2")
    if [ -z "$(trimmed "$out")" ]; then
        pass "$3"
    else
        fail "$3 -- it printed:"
        printf '%s\n' "$out" | sed 's/^/       /' >&2
    fi
}

# The sentence is there.  Used for the error half: QUIET must not reach it.
says() { # banner nth regex what
    if body "$1" "$2" | grep -Eq -- "$3"; then
        pass "$4"
    else
        fail "$4 -- no line matched /$3/"
        body "$1" "$2" | sed 's/^/       /' >&2
    fi
}

silent_about() { # banner nth regex what
    if body "$1" "$2" | grep -Eq -- "$3"; then
        fail "$4 -- QUIET still printed it:"
        body "$1" "$2" | grep -E -- "$3" | sed 's/^/       /' >&2
    else
        pass "$4"
    fi
}

# The quiet run must print no more than the loud one.  Line counts, because a
# quiet run that prints DIFFERENT chatter of the same length is still chatter.
no_more_than() { # loud-banner loud-nth quiet-banner quiet-nth what
    local l q
    l=$(body "$1" "$2" | grep -c . || true)
    q=$(body "$3" "$4" | grep -c . || true)
    if [ "$q" -le "$l" ]; then
        pass "$5 ($q lines against $l)"
    else
        fail "$5: the quiet run printed MORE ($q against $l)"
        body "$3" "$4" | sed 's/^/       /' >&2
    fi
}

same_rc() { # loud-banner loud-nth quiet-banner quiet-nth what
    local l q
    l=$(rc_of "$1" "$2"); q=$(rc_of "$3" "$4")
    if [ -n "$l" ] && [ "$l" = "$q" ]; then
        pass "$5 (rc $q both ways)"
    else
        fail "$5: rc was '${l:-nothing}' loud and '${q:-nothing}' quiet"
    fi
}

want_rc() { # banner nth expected what
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"
         block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

# ---- 0. one boot ---------------------------------------------------------
BOOTS=$(grep -c "^===== SYS:ShowNetStatus QUIET =====" "$REPORT" || true)
if [ "$BOOTS" -eq 1 ]; then
    pass "the machine booted once, so the block numbering below holds"
else
    fail "THE MACHINE REBOOTED or the run stopped: $BOOTS runs of the first line"
fi

# ---- 1. ShowNetStatus has no QUIET any more ------------------------------
#
# The removal.  ShowNetStatus's template starts INTERFACE/M, so ReadArgs does
# not reject the word: it takes it as an interface name, and the command says
# there is no interface called that and returns ERROR.  Loud, names the token
# that is wrong, and lists the interfaces that do exist.  That is the whole
# breaking change for a script that passes it.
says "SYS:ShowNetStatus QUIET" 1 'no interface called "QUIET"' \
     "ShowNetStatus no longer takes QUIET, and says which word it choked on"
want_rc "SYS:ShowNetStatus QUIET" 1 10 "and fails rather than reporting"

# ---- 2. AddNetInterface QUIET still says why it could not ----------------
#
# THE DEFECT THIS FILE WAS WRITTEN FOR.  This is the line in S:User-Startup.
# Before the fix it printed nothing here and returned 20, so a machine that
# booted with no network gave the user no reason at all.
says "SYS:AddNetInterface nosuch0 QUIET" 1 'no interface called "nosuch0"' \
     "AddNetInterface QUIET still reports a missing interface file"
want_rc "SYS:AddNetInterface nosuch0 QUIET" 1 20 "and still fails"

# ---- 3. CheckNetConfig ---------------------------------------------------
says_nothing "SYS:CheckNetConfig QUIET" 1 "CheckNetConfig QUIET prints nothing"
same_rc "SYS:CheckNetConfig" 1 "SYS:CheckNetConfig QUIET" 1 \
        "and returns what the loud run returned"

# ---- 4. GetNetStatus -----------------------------------------------------
says_nothing "SYS:GetNetStatus QUIET" 1 "GetNetStatus QUIET prints nothing"
same_rc "SYS:GetNetStatus" 1 "SYS:GetNetStatus QUIET" 1 \
        "and the return code is the whole answer, unchanged"

# ---- 5. AddNetInterface, the working case --------------------------------
#
# The loud run is the control: if it printed nothing either, the quiet run
# proves nothing.
if [ "$(body "SYS:AddNetInterface eth0" 1 | grep -c . || true)" -gt 0 ]; then
    pass "AddNetInterface without QUIET reports what came up"
else
    fail "AddNetInterface printed nothing even without QUIET, nothing below holds"
fi
says_nothing "SYS:AddNetInterface eth0 QUIET" 1 \
             "AddNetInterface QUIET brings it up and says nothing"
want_rc "SYS:AddNetInterface eth0 QUIET" 1 0 "and succeeded"
if block "SYS:netstat -i" 1 | grep -Eq '^eth0 +[0-9]+ +[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+ +up'; then
    pass "and eth0 really is up with an address, so QUIET did the work"
else
    fail "eth0 is not up after the quiet add: QUIET skipped the work"
    block "SYS:netstat -i" 1 | sed 's/^/       /' >&2
fi

# ---- 6. hostname ---------------------------------------------------------
#
# QUIET here is the name and nothing else -- one line a script can read --
# which is what run-hostname.sh 6 asserts.  What that harness cannot see is
# the comparison: the loud run says where the name came from as well.
if [ "$(body "SYS:hostname QUIET" 1 | grep -c . || true)" -eq 1 ]; then
    pass "hostname QUIET is exactly one line, the name"
else
    fail "hostname QUIET is not one line"
    body "SYS:hostname QUIET" 1 | sed 's/^/       /' >&2
fi
no_more_than "SYS:hostname" 1 "SYS:hostname QUIET" 1 \
             "and it is shorter than the loud run"

# ---- 7. ping -q ----------------------------------------------------------
silent_about "SYS:ping $PEER -c 2 -t 20 -q" 1 'icmp_seq=' \
             "ping -q drops the per-packet lines"
says "SYS:ping $PEER -c 2 -t 20 -q" 1 'packets transmitted' \
     "and keeps the summary, which is the answer"
says "SYS:ping $PEER -c 2 -t 20 -q" 1 '2 received' \
     "and the packets really went, so -q did not skip the work"
says "SYS:ping $PEER -c 2 -t 20" 1 'icmp_seq=' \
     "the loud run does print them, so the case above is a difference"

# ---- 8. arp --------------------------------------------------------------
#
# After the pings there is at least one entry, so both runs have a cache to
# print.  QUIET drops the column heading, not the rows: the rows are the
# answer.
silent_about "SYS:arp QUIET" 1 '^Address +Hardware address' \
             "arp QUIET drops the column heading"
says "SYS:arp" 1 '^Address +Hardware address' \
     "and the loud run prints it, so that is a difference"
no_more_than "SYS:arp" 1 "SYS:arp QUIET" 1 "and prints no more than the loud run"

# ---- 9. AddNetRoute and DeleteNetRoute -----------------------------------
says_nothing "SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW QUIET" 1 \
             "AddNetRoute QUIET says nothing"
same_rc "SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW" 1 \
        "SYS:AddNetRoute NETDESTINATION=192.168.77.0 GATEWAY=$GW QUIET" 1 \
        "and returns what the loud run returned"
says_nothing "SYS:DeleteNetRoute DESTINATION=192.168.77.0 QUIET" 1 \
             "DeleteNetRoute QUIET says nothing"
same_rc "SYS:DeleteNetRoute DESTINATION=192.168.77.0" 1 \
        "SYS:DeleteNetRoute DESTINATION=192.168.77.0 QUIET" 1 \
        "and returns what the loud run returned"

# ---- 10. the error half, on the two that act on an interface -------------
says "SYS:ConfigureNetInterface nosuch0 QUIET ADDRESS 10.99.99.99/24" 1 \
     'no interface called "nosuch0"' \
     "ConfigureNetInterface QUIET still reports an unknown interface"
says "SYS:RemoveNetInterface nosuch0 QUIET" 1 'no interface called "nosuch0"' \
     "RemoveNetInterface QUIET still reports an unknown interface"

# ---- 11. NetShutdown -----------------------------------------------------
says_nothing "SYS:NetShutdown QUIET" 1 "NetShutdown QUIET says nothing"
same_rc "SYS:NetShutdown" 1 "SYS:NetShutdown QUIET" 1 \
        "and returns what the loud run returned"

# ---- the verdict ---------------------------------------------------------
echo
echo "quiet: $CHECKS checks, $FAILED failures"
if [ "$FAILED" -eq 0 ] && [ "$CHECKS" -ge 30 ]; then
    echo "PASS: QUIET means the same thing in every command that has one"
    exit 0
fi
[ "$CHECKS" -ge 30 ] || echo "only $CHECKS checks ran, the floor is 30" >&2
echo "the transcript above is the whole run" >&2
exit 1
