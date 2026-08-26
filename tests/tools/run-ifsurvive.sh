#!/usr/bin/env bash
#
# REMOVE ONE INTERFACE, THEN USE THE OTHER ONE.
#
#   tests/tools/run-ifsurvive.sh [-b builddir] [-t seconds]
#
# Two interfaces on a2065.device UNIT=0 -- the shipping rig config, which
# run-ifslots.sh already stages four of.  Traffic goes out over A (an ARP/ping
# exchange and three name resolutions), A is removed, and B has to still have a
# wire AND still reach something OFF-LINK.  Then B goes too and nothing may be
# left holding the unit.
#
# THE TWO BUGS THAT SHIPPED THROUGH THIS GAP, and what each needs to bite:
#
#   77896ac2  S2_OFFLINE was addressed to the DEVICE, not to the last opener of
#             the unit, so removing one interface took the sibling's wire.  An
#             ON-LINK ping over B catches it.
#   afedeb06  nx_ip_interface_detach clears nx_ip_gateway_address MACHINE-WIDE,
#             and the LAST interface to bind owns it, so removing that one left
#             the survivor able to reach its own subnet and nothing else.  An
#             on-link ping passes with no gateway at all, so only an OFF-LINK
#             destination catches it.  A is therefore the DHCP interface and is
#             added SECOND: NetX installs the gateway at bind.
#
# Exit: 0 pass, 1 a claim failed, 2 the rig did not produce something to read.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

. "$ROOT/tools/test-verdict.sh"
. "$ROOT/tools/serial-log.sh"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD=a2065
TIMEOUT=300
MIN_CHECKS=13

# A resolution that comes back at all but takes longer than this is the user's
# symptom: the sibling did not fail, it stalled.
STALL_MS="${AMINETXDUO_IFSURVIVE_STALL_MS:-8000}"

# Off-link by construction: it is not in SLIRP's 10.0.2.0/24, so reaching it
# needs the gateway the removed interface installed.
OFFLINK="${AMINETXDUO_IFSURVIVE_OFFLINK:-8.8.8.8}"

while getopts "b:t:N:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-N board]" >&2
           exit 2 ;;
    esac
done

if [ "$BOARD" != a2065 ]; then
    echo "run-ifsurvive.sh stages a2065.device in both interface files: the\
 point is two interfaces on ONE unit, so -N $BOARD would bring nothing up." >&2
    exit 2
fi

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
NEEDED="ToolsSmoke AddNetInterface RemoveNetInterface ShowNetStatus
        netstat ping nslookup"

for t in $NEEDED; do
    [ -f "$TOOLS/$t" ] || { echo "build $BUILD first: no $TOOLS/$t" >&2
                            exit 2; }
done
[ -f "$BSD" ] || { echo "build $BUILD first: no $BSD" >&2; exit 2; }

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

# ------------------------------------------------------------- the stage ---

STAGE="$ROOT/build/ifsurvive-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$A2065" "$STAGE/devs/a2065.device"

# B SURVIVES and binds FIRST; A is DHCP, binds SECOND and therefore owns the
# machine's gateway, which is what makes the detach path's clear observable.
# B carries a GATEWAY line of its own: the hand-off has nothing to hand over
# otherwise, and an interface with no gateway is not a candidate for one.
printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=10.0.2.16\nNETMASK=255.255.255.0\nGATEWAY=10.0.2.2\n' \
    > "$STAGE/devs/NetInterfaces/aeth0"
printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=DHCP\n' \
    > "$STAGE/devs/NetInterfaces/zeth1"

{
    echo "SYS:AddNetInterface aeth0"
    echo "SYS:AddNetInterface zeth1"
    echo "SYS:netstat -i"
    echo "SYS:ShowNetStatus"
    echo "SYS:ping 10.0.2.2 -c 3 -t 20"
    echo "SYS:ping $OFFLINK -c 3 -t 20"
    echo "SYS:nslookup example.com $OFFLINK"
    echo "SYS:nslookup www.example.com $OFFLINK"
    echo "SYS:nslookup example.org $OFFLINK"
    echo "SYS:RemoveNetInterface zeth1"
    echo "SYS:netstat -i"
    echo "SYS:ShowNetStatus"
    echo "SYS:ping 10.0.2.2 -c 3 -t 20"
    echo "SYS:ping $OFFLINK -c 3 -t 20"
    echo "SYS:nslookup example.net $OFFLINK"
    echo "SYS:RemoveNetInterface aeth0"
    echo "SYS:netstat -i"
    echo "SYS:ShowNetStatus INTERFACES"
    echo "SYS:ShowNetStatus EVENTS"
} > "$STAGE/commands.txt"

# ------------------------------------------------------------------ run ---

TAG=ifsurvive
REPORT="$ROOT/build/amiberry-testhd-$TAG/tools.txt"
rm -f "$REPORT"

(
    export AMINETXDUO_RUN_TAG="$TAG"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -m A1200 -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/devs" "$STAGE/libs" \
        "$TOOLS/AddNetInterface" "$TOOLS/RemoveNetInterface" \
        "$TOOLS/ShowNetStatus" "$TOOLS/netstat" "$TOOLS/ping" \
        "$TOOLS/nslookup" "$STAGE/commands.txt"
)
RUN_RC=$?

serial_log_have "$(serial_log_path "$TAG")" "$BUILD" \
                "guest ami_log() output" || true

# EMPTY OR TRUNCATED IS NOT A PASS.  Exit 2 -- the tree's rig code -- so a
# caller records it apart from a claim that went red.
if [ ! -s "$REPORT" ]; then
    echo "!! the guest wrote no $REPORT (amiberry-run rc=$RUN_RC)." >&2
    echo "!! Nothing was checked.  This is the rig, not a result." >&2
    verdict_kv "name=ifsurvive" "verdict=SKIP" "reason=no_transcript" \
               "checks=0" "failures=0" "min_checks=$MIN_CHECKS" \
               "run_rc=$RUN_RC" "transcript="
    exit 2
fi
if ! tr -d '\r' < "$REPORT" | grep -q '^===== done'; then
    echo "!! the guest did not finish (amiberry-run rc=$RUN_RC).  The last of" >&2
    echo "!! what it printed:" >&2
    tr -d '\r' < "$REPORT" | tail -12 | sed 's/^/!!   /' >&2
    verdict_kv "name=ifsurvive" "verdict=SKIP" "reason=no_summary" \
               "checks=0" "failures=0" "min_checks=$MIN_CHECKS" \
               "run_rc=$RUN_RC" "transcript=$REPORT"
    exit 2
fi

echo
echo "------------------ what the guest printed --------------------"
tr -d '\r' < "$REPORT"
echo "--------------------------------------------------------------"
echo

# -------------------------------------------------------------- verdict ---

CHECKS="$ROOT/build/ifsurvive-checks.txt"
: > "$CHECKS"
TOTAL=0
BAD=0

pass() { printf '  ok   %s\n' "$*" | tee -a "$CHECKS"; TOTAL=$((TOTAL + 1)); }
fail() { printf '  FAIL %s\n' "$*" | tee -a "$CHECKS"
         TOTAL=$((TOTAL + 1)); BAD=$((BAD + 1)); }
rig()  { printf '  RIG  %s\n' "$*"; }

block() { # command n
    tr -d '\r' < "$REPORT" |
    awk -v want="$1" -v n="${2:-1}" '
        index($0, "===== ") == 1 {
            cur = substr($0, 7); sub(/[ \t]*=====[ \t]*$/, "", cur)
            if (cur == want) { seen++; on = (seen == n) } else { on = 0 }
            next
        }
        on { print }'
}

ms_of() { # block text -> the ms ToolsSmoke charged the command
    printf '%s\n' "$1" |
    sed -n 's/^----- rc [-0-9]*, \([0-9]*\) ms.*/\1/p' | head -1
}

replied() { # block text
    printf '%s\n' "$1" |
    grep -qE "0(\.0)?% packet loss|[1-9][0-9]* (packets )?received"
}

resolved() { # block text
    printf '%s\n' "$1" |
    grep -qE "^(Name|Address(es)?)[: ]|has address|[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+"
}

gateway_of() { # ShowNetStatus text -> the default route, or empty
    printf '%s\n' "$1" |
    sed -n 's/^Default route:[[:space:]]*\([0-9][0-9.]*\).*/\1/p' | head -1
}

ONLINK="SYS:ping 10.0.2.2 -c 3 -t 20"
OFFPING="SYS:ping $OFFLINK -c 3 -t 20"

before=$(block "SYS:netstat -i" 1)
if printf '%s\n' "$before" | grep -qE "^aeth0[[:space:]]"; then
    pass "aeth0 -- the one that must survive -- is up"
else
    fail "aeth0 never came up, so there is no sibling to survive"
fi
if printf '%s\n' "$before" | grep -qE "^zeth1[[:space:]]"; then
    pass "zeth1 is up on the same unit"
else
    fail "zeth1 never came up, so there is nothing to remove"
fi
if printf '%s\n' "$before" | grep -qE "^zeth1[[:space:]].*10\.0\.2\.15"; then
    pass "and zeth1, added second, took the DHCP lease 10.0.2.15"
else
    fail "zeth1 has no DHCP address, so it did not install the gateway"
fi

gw1=$(gateway_of "$(block "SYS:ShowNetStatus" 1)")
if [ -n "$gw1" ] && [ "$gw1" != 0.0.0.0 ]; then
    pass "the machine has a default route while both are up: $gw1"
else
    fail "no default route while both interfaces are up (read '$gw1')"
fi

if replied "$(block "$ONLINK" 1)"; then
    pass "the gateway answers on-link: real frames went over the wire"
else
    fail "no on-link ping replies while both interfaces are up"
fi

# THE OFF-LINK LEG'S PRECONDITIONS.  A rig with no route off SLIRP cannot
# decide these in either direction, and a claim that cannot be evaluated here
# is not a claim that failed.
offping_up=0
replied "$(block "$OFFPING" 1)" && offping_up=1
dns_up=0
for q in "SYS:nslookup example.com $OFFLINK" \
         "SYS:nslookup www.example.com $OFFLINK" \
         "SYS:nslookup example.org $OFFLINK"; do
    resolved "$(block "$q" 1)" && dns_up=$((dns_up + 1))
done
if [ "$dns_up" -gt 0 ]; then
    pass "$dns_up of 3 names resolved through $OFFLINK -- OFF-LINK, so the\
 gateway carried them -- before zeth1 was removed"
else
    rig "nothing resolved through $OFFLINK with both interfaces up: this rig\
 has no route off SLIRP, so the off-link half is not decided here"
fi
[ "$offping_up" = 1 ] ||
    rig "no off-link ICMP with both up either; only the resolver probes the\
 gateway on this rig"

removal=$(block "SYS:RemoveNetInterface zeth1" 1)
if printf '%s\n' "$removal" | grep -qiE "removed|no longer|^----- rc 0"; then
    pass "RemoveNetInterface zeth1 -- the interface that owns the gateway --\
 was accepted"
else
    fail "RemoveNetInterface zeth1 did not report success"
fi

after=$(block "SYS:netstat -i" 2)
if printf '%s\n' "$after" | grep -qE "^zeth1[[:space:]]"; then
    fail "zeth1 is still a live interface after it was removed"
else
    pass "zeth1 is gone"
fi
if printf '%s\n' "$after" | grep -qE "^aeth0[[:space:]]"; then
    pass "aeth0 is still a live interface"
else
    fail "aeth0 went with zeth1: removing one interface took its sibling"
fi
if printf '%s\n' "$after" | grep -qE "^aeth0[[:space:]].*10\.0\.2\.16"; then
    pass "and still carries its own address"
else
    fail "aeth0 lost its address when zeth1 was removed"
fi
# BUG 1, 77896ac2.  S2_OFFLINE is a unit command and it went to the device.
if printf '%s\n' "$after" | grep -qE "^aeth0[[:space:]].*[[:space:]]down"; then
    fail "aeth0's LINK IS DOWN: removing zeth1 took the wire out from under\
 the other interface on a2065.device unit 0"
else
    pass "and its link is still up: the unit was not taken offline under it"
fi
if replied "$(block "$ONLINK" 2)"; then
    pass "the gateway still answers on-link over aeth0"
else
    fail "NO ON-LINK PING REPLIES over aeth0 after zeth1 was removed: the\
 surviving interface has no wire"
fi

# BUG 2, afedeb06.  READ THE GATEWAY BACK.  An on-link ping passes with no
# gateway at all, so the two checks above cannot see this one.  A FLOOR, not a
# proof: ShowNetStatus can print the CONFIGURED gateway, so this reads non-zero
# either way once the survivor has a GATEWAY line.  The resolution below is
# what actually decides it; see docs/BACKLOG.md.
gw2=$(gateway_of "$(block "SYS:ShowNetStatus" 2)")
echo "  default route: '$gw1' with both up, '$gw2' after zeth1 was removed"
if [ -n "$gw2" ] && [ "$gw2" != 0.0.0.0 ]; then
    pass "the machine still reports a default route after zeth1 was removed:\
 $gw2"
else
    fail "THE DEFAULT ROUTE IS GONE (read '$gw2'): removing one interface took\
 the machine's gateway with it, and the survivor can reach only its own subnet"
fi

if [ "$dns_up" -gt 0 ]; then
    if [ "$offping_up" = 1 ]; then
        if replied "$(block "$OFFPING" 2)"; then
            pass "$OFFLINK -- OFF-LINK -- still answers over aeth0"
        else
            fail "$OFFLINK IS UNREACHABLE over aeth0 after zeth1 was removed,\
 and it answered before"
        fi
    fi

    lookup2=$(block "SYS:nslookup example.net $OFFLINK" 1)
    lookup2_ms=$(ms_of "$lookup2")
    echo "  the surviving interface's resolution took ${lookup2_ms:-?} ms\
 (stall threshold $STALL_MS ms)"
    if resolved "$lookup2"; then
        pass "a name still resolves over aeth0 through the off-link server"
        if [ -n "$lookup2_ms" ] && [ "$lookup2_ms" -gt "$STALL_MS" ]; then
            fail "but it took ${lookup2_ms} ms, over the ${STALL_MS} ms stall\
 threshold: the user's symptom is the wait, not the failure"
        else
            pass "and it came back in ${lookup2_ms:-?} ms, under the threshold"
        fi
    else
        fail "NO NAME RESOLVES over aeth0 after zeth1 was removed, and\
 $dns_up did before it"
    fi
fi

last=$(block "SYS:RemoveNetInterface aeth0" 1)
if printf '%s\n' "$last" | grep -qiE "removed|no longer|^----- rc 0"; then
    pass "RemoveNetInterface aeth0 was accepted"
else
    fail "RemoveNetInterface aeth0 did not report success"
fi

end=$(block "SYS:netstat -i" 3)
if printf '%s\n' "$end" | grep -qE "^(aeth0|zeth1)[[:space:]]"; then
    fail "an interface is still live after both were removed"
else
    pass "no interface is left holding the unit"
fi

# THE FIX MUST NOT BE "NEVER OFFLINE ANYTHING".  The last one out does issue
# S2_OFFLINE, and an S2_OFFLINE that is refused is NETEVENT_OFFLINE_FAILED's to
# report; the drawer entries go back to `defined` either way.
table=$(block "SYS:ShowNetStatus INTERFACES" 1 |
        sed -n '/^Interfaces$/,/^$/p' |
        grep -vE '^(Interfaces|Name[[:space:]]+State)')
detached=0
for n in aeth0 zeth1; do
    printf '%s\n' "$table" | grep -qE "^${n}[[:space:]]+defined" &&
        detached=$((detached + 1))
done
if [ "$detached" = 2 ]; then
    pass "both definitions are back to 'defined'"
else
    fail "$detached of 2 definitions went back to 'defined' after removal"
fi
if block "SYS:ShowNetStatus EVENTS" 1 | grep -qi "offline.*fail"; then
    fail "the last interface out could not take the unit offline"
else
    pass "the last one out took the unit offline without a refusal"
fi

printf 'ifsurvive: %d checks, %d failures\n' "$TOTAL" "$BAD" >> "$CHECKS"
echo
echo "ifsurvive: $TOTAL checks, $BAD failures"

verdict_guest ifsurvive "$MIN_CHECKS" 0 "$CHECKS"
exit $?
