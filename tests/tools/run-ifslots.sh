#!/usr/bin/env bash
#
# WHICH INTERFACE GETS THE HARDWARE, when the drawer describes more of them
# than the stack has slots for.
#
#   tests/tools/run-ifslots.sh [-b BUILDDIR] [-t SECONDS] [-N BOARD]
#                              [-r named|typo|identity]
#
# THREE BOOTS, SIX CLAIMS AND A GUARD, and the whole arm exists because half of one user's
# evening was spent on a card that was fine.  They had three files in
# DEVS:NetInterfaces/ and the one they wanted was third.
#
# WHAT IT PROVES
#
#   1. A machine with four interface definitions BOOTS.
#   2. ShowNetStatus LISTS ALL FOUR before anything is attached, each in the
#      "defined" state.  (dd4b3cee; this is the half that already worked, and
#      it is asserted here so the two halves cannot drift apart.)
#   3. A DEFINITION THAT IS NOT FIRST IN DIRECTORY ORDER CAN BE BROUGHT UP.
#      This is the claim the user's machine failed.  `AddNetInterface zeth3`
#      on a drawer sorted aeth0, beth1, meth2, zeth3 has to produce zeth3
#      online with an address, carrying traffic -- not ENOSPC because the
#      start-up pass took both slots off the head of the list.
#   4. A THIRD SIMULTANEOUS ATTACH IS STILL REFUSED, BY NAME.  Two slots are a
#      real limit.  Once both are held by interfaces somebody NAMED, the third
#      is refused and the refusal says which two are holding them and what to
#      type.  Claim 3 must not have been bought by making the cap negotiable.
#   5. THE SLOT REALLY FREES.  RemoveNetInterface, and the interface that was
#      refused a moment ago comes up.  Five add/remove cycles must not leak.
#   6. A LIVE INTERFACE REPORTS ITS OWN NAME.  Second boot, two definitions:
#      aeth0 names a card that is not in the machine, zeth3 names the one that
#      is.  The machine comes up on zeth3's card with zeth3's lease, and it has
#      to SAY zeth3 -- in netstat -i and in ShowNetStatus -- with aeth0 listed
#      as defined and no "the file was changed after the network started" note
#      about a file nobody changed.
#
#   And one guard on the mechanism claim 3 needs, numbered 4b because it is the
#   other half of "the cap is still real": AN INTERFACE THAT IS UP IS NEVER
#   TAKEN DOWN FOR ONE THAT CANNOT COME UP.  A slot only changes hands once the
#   newcomer's device has opened, so `AddNetInterface` on a file with a mistyped
#   DEVICE= costs a message and nothing else.  Getting this wrong would turn a
#   typo into an outage, which is a worse defect than the one being fixed.
#
# HOW THE TWO DEFECTS LOOKED FROM THE OUTSIDE, so a red run can be read
#
#   Claim 3 failed with "NETCTRL_INTERFACE_ADD refused zeth3: ENOSPC (28)" on
#   a machine where nothing else was online, because ami_ns_open_devices()
#   claimed both NX_IP slots walking the description list from the head.  The
#   list is sorted, so the alphabet decided which card the machine could use.
#
#   Claim 6 failed silently, which is worse: the machine worked, and reported
#   the working interface under the failed interface's name, because the tools
#   matched live NX slot N to description index N and the compaction that moves
#   a surviving description down had already broken that.
#
# WHY THE STAGING LOOKS ODD
#
#   Every definition names a2065.device unit 0, because there is one emulated
#   card and the point is to run out of SLOTS rather than out of cards.  The
#   first three are STATIC on subnets nothing uses, so they cost no DHCP and
#   attach immediately; zeth3 is the DHCP one, so the definition that has to
#   prove it reaches the network is the one at the end of the alphabet.
#
# COST: three boots, about three minutes.  SLIRP is enough -- the gateway answers
# ICMP and hands out a lease, and nothing here counts bytes off a third
# machine.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD=a2065
TIMEOUT=300
ROUNDS="named typo identity"

while getopts "b:t:N:r:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        r) ROUNDS="${OPTARG//,/ }" ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-N board]\
 [-r named|typo|identity]" >&2; exit 2 ;;
    esac
done

# ------------------------------------------------------------------ rig ----

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
NEEDED="ToolsSmoke AddNetInterface RemoveNetInterface ShowNetStatus
        CheckNetConfig netstat ping"

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

TOTAL=0
BAD=0
CLAIMS="$ROOT/build/ifslots-claims.txt"
: > "$CLAIMS"

pass() { echo "  ok   $*"; TOTAL=$((TOTAL + 1)); }
fail() { echo "  FAIL $*"; TOTAL=$((TOTAL + 1)); BAD=$((BAD + 1)); }

claim() { # n verdict text
    printf 'claim %s %-4s %s\n' "$1" "$2" "$3" >> "$CLAIMS"
}

# ------------------------------------------------------------- the boot ----
#
# `report` is what ToolsSmoke wrote: one "===== <command> =====" banner per
# line of commands.txt, then that command's output.  Returns 1 when the guest
# wrote nothing at all, which is a rig failure rather than a claim failing and
# is reported as one.
boot() { # tag stagedir  -> sets REPORT
    local tag="$1" stage="$2" rc

    REPORT="$ROOT/build/amiberry-testhd-$tag/tools.txt"
    rm -f "$REPORT"

    (
        export AMINETXDUO_RUN_TAG="$tag"
        "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -m A1200 -t "$TIMEOUT" \
            "$TOOLS/ToolsSmoke" "$stage/devs" "$stage/libs" \
            "$TOOLS/AddNetInterface" "$TOOLS/RemoveNetInterface" \
            "$TOOLS/ShowNetStatus" "$TOOLS/CheckNetConfig" \
            "$TOOLS/netstat" "$TOOLS/ping" "$stage/commands.txt"
    )
    rc=$?

    if [ ! -s "$REPORT" ]; then
        echo "!! the guest wrote no $REPORT (amiberry-run rc=$rc)" >&2
        return 1
    fi

    echo
    echo "------------------ what the guest printed --------------------"
    tr -d '\r' < "$REPORT"
    echo "--------------------------------------------------------------"
    echo
    return 0
}

# The Nth occurrence of one command's output, without its banner.  Banners are
# the only structure in the transcript, so every assertion below is scoped to
# one command rather than grepping the lot -- a later AddNetInterface prints
# names that would satisfy an earlier clause otherwise.
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

# ===================================================================== A ====
#
# FOUR DEFINITIONS, AND THE ONE AT THE END OF THE ALPHABET IS THE ONE ASKED
# FOR.  Claims 1 to 5.
round_named() {
    local stage="$ROOT/build/ifslots-stage-named"
    local first third many second_meth n i

    echo
    echo "=============================================================="
    echo "==> four definitions; the one asked for is last in the drawer"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    # Sorted, these are aeth0, beth1, meth2, zeth3.  The first three are
    # static on subnets nothing uses: they attach without asking anyone for
    # anything, which is what makes them able to hold a slot.  zeth3 is the
    # DHCP one and is therefore the definition that has to reach the network.
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.77.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/aeth0"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.78.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/beth1"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.79.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/meth2"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=DHCP\n' \
        > "$stage/devs/NetInterfaces/zeth3"

    {
        # Asked of a freshly booted machine, which is when a user asks it.
        echo "SYS:ShowNetStatus INTERFACES"
        echo "SYS:CheckNetConfig"
        # CLAIM 3.
        echo "SYS:AddNetInterface zeth3"
        echo "SYS:netstat -i"
        echo "SYS:ping 10.0.2.2 -c 2 -t 20"
        echo "SYS:netstat -h"
        # Naming an interface that is already up is what makes it one somebody
        # asked for, which is what makes claim 4 a refusal rather than another
        # slot changing hands.
        echo "SYS:AddNetInterface aeth0"
        # CLAIM 4.
        echo "SYS:AddNetInterface meth2"
        # CLAIM 5.
        echo "SYS:RemoveNetInterface aeth0"
        echo "SYS:AddNetInterface meth2"
        echo "SYS:RemoveNetInterface meth2"
        for i in 1 2 3 4 5; do
            echo "SYS:AddNetInterface aeth0"
            echo "SYS:RemoveNetInterface aeth0"
        done
        echo "SYS:netstat -h"
        echo "SYS:ShowNetStatus INTERFACES"
    } > "$stage/commands.txt"

    boot ifslots-named "$stage" || {
        for n in 1 2 3 4 5; do claim "$n" FAIL "the guest never reported"; done
        fail "the guest produced no transcript"
        return 1
    }

    # ---- CLAIM 1: it booted and ran the whole list ----------------------
    local wanted ran
    wanted=$(grep -c . "$stage/commands.txt")
    ran=$(tr -d '\r' < "$REPORT" | grep -c '^===== ')
    if [ "$ran" -ge "$wanted" ]; then
        pass "the machine booted with four definitions and ran all $wanted commands"
        claim 1 PASS "boots with 4 definitions ($ran/$wanted commands)"
    else
        fail "the guest ran only $ran of $wanted commands"
        claim 1 FAIL "boots with 4 definitions ($ran/$wanted commands)"
    fi

    # ---- CLAIM 2: every definition is listed, and listed as defined -----
    first=$(block "SYS:ShowNetStatus INTERFACES" 1 |
            sed -n '/^Interfaces$/,/^$/p' |
            grep -vE '^(Interfaces|Name[[:space:]]+State)')
    local listed=0
    for n in aeth0 beth1 meth2 zeth3; do
        if printf '%s\n' "$first" | grep -qE "^${n}[[:space:]]"; then
            pass "ShowNetStatus lists $n before anything is attached"
            listed=$((listed + 1))
        else
            fail "ShowNetStatus does not list $n: the definition vanished"
        fi
    done
    if printf '%s\n' "$first" | grep -q "defined"; then
        pass "and calls an unattached description 'defined'"
    else
        fail "no 'defined' state in the first ShowNetStatus"
    fi
    if [ "$listed" = 4 ]; then
        claim 2 PASS "ShowNetStatus lists all four definitions"
    else
        claim 2 FAIL "ShowNetStatus listed $listed of 4 definitions"
    fi

    # CheckNetConfig must not call a full drawer a fault.
    if block "SYS:CheckNetConfig" 1 | grep -qi "drawer holds"; then
        fail "CheckNetConfig still reports the drawer size as a fault"
    else
        pass "CheckNetConfig does not call four interface files a fault"
    fi

    # ---- CLAIM 3: the definition that is not first comes up -------------
    local ok3=1
    if block "SYS:AddNetInterface zeth3" 1 | grep -qiE "ENOSPC|cannot come up"
    then
        fail "zeth3 was refused a slot: the start-up pass took them both"
        ok3=0
    else
        pass "zeth3 was not refused a slot"
    fi
    if block "SYS:netstat -i" 1 | grep -qE "^zeth3[[:space:]]"; then
        pass "zeth3 -- last in the drawer -- is a live interface"
    else
        fail "netstat -i does not show zeth3 as a live interface"
        ok3=0
    fi
    if block "SYS:netstat -i" 1 | grep -qE "^zeth3[[:space:]].*10\.0\.2\.15"
    then
        pass "and it took the SLIRP lease 10.0.2.15"
    else
        fail "zeth3 has no address"
        ok3=0
    fi
    if block "SYS:ping 10.0.2.2 -c 2 -t 20" 1 |
       grep -qE "0(\.0)?% packet loss|[12] (packets )?received"; then
        pass "and the gateway answers over it"
    else
        fail "no ping replies over zeth3"
        ok3=0
    fi
    [ "$ok3" = 1 ] && claim 3 PASS "a definition that is not first can be brought up" \
                   || claim 3 FAIL "a definition that is not first can be brought up"

    # ---- CLAIM 4: the third simultaneous attach is refused, by name -----
    local ok4=1 named=0
    third=$(block "SYS:AddNetInterface meth2" 1)
    if printf '%s\n' "$third" | grep -qi "slots are in use"; then
        pass "a third simultaneous attach is refused"
    else
        fail "the third attach was not refused"
        ok4=0
    fi
    printf '%s\n' "$third" | grep -q "aeth0" && named=$((named + 1))
    printf '%s\n' "$third" | grep -q "zeth3" && named=$((named + 1))
    if [ "$named" = 2 ]; then
        pass "and the refusal NAMES both interfaces that hold the slots"
    else
        fail "the refusal names $named of the 2 interfaces that are up"
        ok4=0
    fi
    if printf '%s\n' "$third" | grep -qi "RemoveNetInterface"; then
        pass "and says to take one down rather than to delete a file"
    else
        fail "the refusal does not say what to do next"
        ok4=0
    fi
    if printf '%s\n' "$third" | grep -qiE "DEVS:NetInterfaces|move the unused"
    then
        fail "the refusal still blames the drawer"
        ok4=0
    else
        pass "and does not mention the drawer"
    fi
    [ "$ok4" = 1 ] && claim 4 PASS "a third simultaneous attach is refused by name" \
                   || claim 4 FAIL "a third simultaneous attach is refused by name"

    # ---- CLAIM 5: the slot frees, and five cycles do not leak -----------
    local ok5=1
    second_meth=$(block "SYS:AddNetInterface meth2" 2)
    if printf '%s\n' "$second_meth" | grep -qi "slots are in use"; then
        fail "meth2 still refused after RemoveNetInterface freed a slot"
        ok5=0
    else
        pass "meth2 comes up once RemoveNetInterface has freed a slot"
    fi

    local a1 a2
    a1=$(block "SYS:netstat -h" 1 |
         sed -n 's/.*[^0-9]\([0-9][0-9]*\) allocations outstanding.*/\1/p' |
         head -1)
    a2=$(block "SYS:netstat -h" 2 |
         sed -n 's/.*[^0-9]\([0-9][0-9]*\) allocations outstanding.*/\1/p' |
         head -1)
    if [ -n "$a1" ] && [ -n "$a2" ]; then
        echo "  allocations outstanding: $a1 before the cycles, $a2 after"
        if [ "$a2" -le "$a1" ]; then
            pass "five add/remove cycles left the allocation count where it was"
        else
            fail "allocations climbed from $a1 to $a2 across five cycles"
            ok5=0
        fi
    else
        fail "could not read the allocation count (a1='$a1' a2='$a2')"
        ok5=0
    fi
    [ "$ok5" = 1 ] && claim 5 PASS "the slot frees on RemoveNetInterface, and nothing leaks" \
                   || claim 5 FAIL "the slot frees on RemoveNetInterface, and nothing leaks"

    return 0
}

# ==================================================================== A2 ====
#
# A MISTYPED DRIVER NAME MUST COST A MESSAGE AND NOTHING ELSE.  Claim 4b.
round_typo() {
    local stage="$ROOT/build/ifslots-stage-typo"
    local ok=1 refusal ifaces

    echo
    echo "=============================================================="
    echo "==> both slots held by interfaces nobody asked for, and the"
    echo "==> interface asked for names a driver that is not there"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.77.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/aeth0"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.78.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/beth1"
    printf 'DEVICE=nosuchcard.device\nUNIT=0\nCONFIGURE=DHCP\n' \
        > "$stage/devs/NetInterfaces/zbad2"

    {
        echo "SYS:AddNetInterface aeth0"
        echo "SYS:AddNetInterface zbad2"
        echo "SYS:netstat -i"
    } > "$stage/commands.txt"

    boot ifslots-typo "$stage" || {
        claim 4b FAIL "the guest never reported"
        fail "the guest produced no transcript"
        return 1
    }

    refusal=$(block "SYS:AddNetInterface zbad2" 1)
    if printf '%s\n' "$refusal" | grep -qi "nosuchcard.device"; then
        pass "the refusal names the driver that is not there"
    else
        fail "the refusal does not name nosuchcard.device"
        ok=0
    fi

    # THE POINT OF THE ROUND.  aeth0 was named and cannot yield; beth1 was not
    # and is the slot a yield would have taken.  Both have to still be here.
    ifaces=$(block "SYS:netstat -i" 1)
    if printf '%s\n' "$ifaces" | grep -qE "^beth1[[:space:]]"; then
        pass "beth1 is still up: no slot was taken for an interface that\
 could not come up"
    else
        fail "beth1 was taken down for zbad2, which never opened its device"
        ok=0
    fi
    if printf '%s\n' "$ifaces" | grep -qE "^aeth0[[:space:]]"; then
        pass "and aeth0 is still up too"
    else
        fail "aeth0 is gone"
        ok=0
    fi
    if printf '%s\n' "$ifaces" | grep -qE "^zbad2[[:space:]]"; then
        fail "zbad2 is listed as a live interface and its driver does not exist"
        ok=0
    else
        pass "and zbad2 is not a live interface"
    fi

    [ "$ok" = 1 ] && claim 4b PASS "a slot is never taken for an interface that cannot come up" \
                  || claim 4b FAIL "a slot is never taken for an interface that cannot come up"

    return 0
}

# ===================================================================== B ====
#
# THE FIRST DEFINITION NAMES A CARD THAT IS NOT THERE.  Claim 6.
round_identity() {
    local stage="$ROOT/build/ifslots-stage-identity"
    local ok6=1 ifaces table detail n

    echo
    echo "=============================================================="
    echo "==> the first definition's card is missing; who does the"
    echo "==> machine say it is running on?"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    printf 'DEVICE=nosuchcard.device\nUNIT=0\nCONFIGURE=DHCP\n' \
        > "$stage/devs/NetInterfaces/aeth0"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=DHCP\n' \
        > "$stage/devs/NetInterfaces/zeth3"

    {
        echo "SYS:AddNetInterface zeth3"
        echo "SYS:netstat -i"
        echo "SYS:ShowNetStatus INTERFACES"
        echo "SYS:ShowNetStatus zeth3"
        # The block for the definition that did NOT come up, which is where the
        # bogus note was: paired with NX slot 0 by subscript, aeth0's block
        # printed the live a2065 beside aeth0's nosuchcard.device and called
        # the difference a file somebody had edited.
        echo "SYS:ShowNetStatus aeth0"
    } > "$stage/commands.txt"

    boot ifslots-identity "$stage" || {
        claim 6 FAIL "the guest never reported"
        fail "the guest produced no transcript"
        return 1
    }

    ifaces=$(block "SYS:netstat -i" 1)

    # The live interface is zeth3's card with zeth3's lease.  It has to say so.
    if printf '%s\n' "$ifaces" | grep -qE "^zeth3[[:space:]]"; then
        pass "netstat -i calls the live interface zeth3"
    else
        fail "netstat -i does not name the live interface zeth3"
        ok6=0
    fi
    if printf '%s\n' "$ifaces" | grep -qE "^aeth0[[:space:]]"; then
        fail "netstat -i reports a live interface called aeth0, whose card is\
 not in this machine"
        ok6=0
    else
        pass "and does not report a live interface called aeth0"
    fi
    if printf '%s\n' "$ifaces" | grep -qE "^zeth3[[:space:]].*10\.0\.2\.15"; then
        pass "and the lease is on zeth3's line"
    else
        fail "zeth3 has no address in netstat -i"
        ok6=0
    fi

    # ShowNetStatus's table: zeth3 attached, aeth0 merely defined.
    table=$(block "SYS:ShowNetStatus INTERFACES" 1 |
            sed -n '/^Interfaces$/,/^$/p' |
            grep -vE '^(Interfaces|Name[[:space:]]+State)')
    if printf '%s\n' "$table" | grep -qE "^zeth3[[:space:]]+(online|offline)"
    then
        pass "ShowNetStatus shows zeth3 attached"
    else
        fail "ShowNetStatus does not show zeth3 attached"
        ok6=0
    fi
    if printf '%s\n' "$table" | grep -qE "^aeth0[[:space:]]+defined"; then
        pass "and aeth0, whose card is missing, as defined"
    else
        fail "ShowNetStatus does not show aeth0 as defined"
        ok6=0
    fi

    # And the detail block, which is where the bogus note was.
    detail=$(block "SYS:ShowNetStatus zeth3" 1)
    if printf '%s\n' "$detail" | grep -q "a2065.device"; then
        pass "ShowNetStatus zeth3 names the driver it is really running on"
    else
        fail "ShowNetStatus zeth3 does not name a2065.device"
        ok6=0
    fi

    # And aeth0's block must not claim the card aeth0 has not got.
    if block "SYS:ShowNetStatus aeth0" 1 | grep -q "a2065.device"; then
        fail "ShowNetStatus aeth0 shows it running on a2065.device, which is\
 zeth3's card"
        ok6=0
    else
        pass "ShowNetStatus aeth0 does not claim zeth3's card"
    fi
    if tr -d '\r' < "$REPORT" | grep -qi "changed after the"; then
        fail "a 'the file was changed after the network started' note about a\
 file nobody changed"
        ok6=0
    else
        pass "and no bogus 'the file was changed' note anywhere"
    fi

    [ "$ok6" = 1 ] && claim 6 PASS "a live interface reports its own name" \
                   || claim 6 FAIL "a live interface reports its own name"

    return 0
}

# ------------------------------------------------------------------------ --

for r in $ROUNDS; do
    case "$r" in
        named)    round_named ;;
        typo)     round_typo ;;
        identity) round_identity ;;
        *) echo "no such round: $r" >&2; exit 2 ;;
    esac
done

echo
echo "=============================== claims ==============================="
cat "$CLAIMS"
echo "======================================================================"
echo "$TOTAL checks, $BAD failures"

if [ "$BAD" = 0 ] && ! grep -q FAIL "$CLAIMS"; then
    echo "PASS: the interface a user names is the interface that gets a slot,\
 and it answers to its own name"
    exit 0
fi

echo "FAIL: see the claims above"
exit 1
