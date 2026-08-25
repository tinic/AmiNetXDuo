#!/usr/bin/env bash
#
# WHICH INTERFACE GETS THE HARDWARE, when the drawer describes more of them
# than the stack has slots for.
#
#   tests/tools/run-ifslots.sh [-b BUILDDIR] [-t SECONDS] [-N BOARD]
#                              [-r named|typo|latefail|identity]
#
# FOUR BOOTS, SIX CLAIMS AND TWO GUARDS, and the whole arm exists because half of one user's
# evening was spent on a card that was fine.  They had three files in
# DEVS:NetInterfaces/ and the one they wanted was third.
#
# WHAT IT PROVES
#
#   1. A machine with five interface definitions BOOTS -- one more than there
#      are slots for, which is the configuration a drawer outgrows into.
#   2. ShowNetStatus LISTS ALL FIVE before anything is attached, the ones that
#      are not attached in the "defined" state.  (dd4b3cee; this is the half
#      that already worked, and it is asserted here so the two halves cannot
#      drift apart.)
#   3. A DEFINITION THAT IS NOT FIRST IN DIRECTORY ORDER CAN BE BROUGHT UP.
#      This is the claim the user's machine failed.  `AddNetInterface zeth4`
#      on a drawer sorted aeth0, beth1, meth2, neth3, zeth4 has to produce
#      zeth4 online with an address, carrying traffic -- not ENOSPC because the
#      start-up pass took every slot off the head of the list.
#   4. FOUR ATTACH AT ONCE, AND THE FIFTH IS REFUSED, BY NAME.  Four slots are
#      the cap since 2026-08-25 -- an A1200 with a PiStorm32 has two cards plus
#      genet.device and wifipi.device -- and they are a real limit.  Once all
#      four are held by interfaces somebody NAMED, the fifth is refused, and
#      the refusal says which four are holding them and what to type.  Claim 3
#      must not have been bought by making the cap negotiable.
#   5. THE SLOT REALLY FREES.  RemoveNetInterface, and the interface that was
#      refused a moment ago comes up.  Five add/remove cycles must not leak.
#   6. A LIVE INTERFACE REPORTS ITS OWN NAME.  Another boot, two definitions:
#      aeth0 names a card that is not in the machine, zeth3 names the one that
#      is.  The machine comes up on zeth3's card with zeth3's lease, and it has
#      to SAY zeth3 -- in netstat -i and in ShowNetStatus -- with aeth0 listed
#      as defined and no "the file was changed after the network started" note
#      about a file nobody changed.
#
#   And two guards on the mechanism claim 3 needs, numbered 4b and 4c because
#   they are the other half of "the cap is still real".  A FAILED ADD MUST
#   LEAVE THE MACHINE EXACTLY AS IT FOUND IT, and there are two ways to fail:
#
#     4b  the device never opens -- a mistyped DEVICE=.  A slot only changes
#         hands once the newcomer's device has opened, so this costs a message
#         and nothing else.
#     4c  the device opens and the interface fails anyway -- the gateway in the
#         file is on no interface's network, so the transaction is rolled back
#         after a slot has already changed hands.  The interface that stood
#         down has to come back.  Before this was fixed it did not, and the
#         interface still up went offline with it, because the rolled-back
#         interface's removal sends S2_OFFLINE to the card they share.
#
#   Getting either wrong turns a typo into an outage, which is a worse defect
#   than the one being fixed.
#
# A BOOT THAT DOES NOT FINISH DECIDES NOTHING.  The guest is checked for
# ToolsSmoke's own "done" line before a single claim is read, and a truncated
# transcript exits 2 -- the code tools/ci.sh reads as "the rig refused it".
# Without that check a guest that wedged under host load reported "beth1 was
# taken down" and "aeth0 is gone" about interfaces it had never reached.
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
#   static ones sit on subnets nothing uses, so they cost no DHCP and attach
#   immediately; the DHCP one is last in the alphabet, so the definition that
#   has to prove it reaches the network is the one at the end of the drawer.
#   That is also why -N refuses any board but the a2065.
#
# COST: four boots, about four minutes.  SLIRP is enough -- the gateway answers
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
ROUNDS="named typo latefail identity"

while getopts "b:t:N:r:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        r) ROUNDS="${OPTARG//,/ }" ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-N board]\
 [-r named|typo|latefail|identity]" >&2; exit 2 ;;
    esac
done

# Every interface file this arm stages names a2065.device, because the point
# is to run out of SLOTS rather than out of cards.  On any other board none of
# them opens, nothing comes up, and every claim below reads the absence as a
# defect -- so the board is refused here rather than proved wrong three boots
# later.
if [ "$BOARD" != a2065 ]; then
    echo "run-ifslots.sh stages a2065.device in every interface file, so\
 -N $BOARD would bring nothing up." >&2
    echo "There is nothing board-specific to prove here: run it on a2065." >&2
    exit 2
fi

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
RIG=0
CLAIMS="$ROOT/build/ifslots-claims.txt"
: > "$CLAIMS"

pass() { echo "  ok   $*"; TOTAL=$((TOTAL + 1)); }
fail() { echo "  FAIL $*"; TOTAL=$((TOTAL + 1)); BAD=$((BAD + 1)); }

claim() { # n verdict text
    printf 'claim %s %-4s %s\n' "$1" "$2" "$3" >> "$CLAIMS"
}

# A BOOT THAT DID NOT FINISH IS NOT A CLAIM THAT IS FALSE, and telling them
# apart is the whole of this function.  A guest that hung part way through the
# command list leaves a transcript with real lines in it, and every assertion
# below then reads the answers that are missing as answers that are wrong: the
# arm reported "beth1 was taken down for zbad2" and "aeth0 is gone" for a run
# in which neither interface had been touched, because the machine never
# reached netstat -i.  That is an accusation against the code, made by the
# rig, and it cost a session.
rig() { echo "  RIG  $*"; RIG=$((RIG + 1)); }

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
        return 2
    fi

    # ToolsSmoke writes this line last, whatever the commands did.  Without it
    # the machine stopped part way and the rest of this file would be asking
    # questions of a transcript that does not contain the answers.
    if ! tr -d '\r' < "$REPORT" | grep -q '^===== done'; then
        echo
        echo "!! the guest did not finish (amiberry-run rc=$rc).  The last of"
        echo "!! what it printed:"
        tr -d '\r' < "$REPORT" | tail -12 | sed 's/^/!!   /'
        echo "!! No claim is decided by this run.  A hang here is the rig or a"
        echo "!! wedge under host load -- re-run it quiet before reading it as"
        echo "!! a defect, and if it is repeatable it is a defect worth a"
        echo "!! harness of its own."
        echo
        return 2
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
    local first third many second_meth n i rc

    echo
    echo "=============================================================="
    echo "==> five definitions, four slots; the one asked for is last"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    # Sorted, these are aeth0, beth1, meth2, neth3, zeth4 -- ONE MORE THAN
    # THERE ARE SLOTS, which is what makes the drawer describe more interfaces
    # than the machine can attach whatever the cap is.  The first four are
    # static on subnets nothing uses: they attach without asking anyone for
    # anything, which is what makes them able to hold a slot.  zeth4 is the
    # DHCP one and is therefore the definition that has to reach the network.
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.77.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/aeth0"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.78.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/beth1"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.79.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/meth2"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.80.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/neth3"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=DHCP\n' \
        > "$stage/devs/NetInterfaces/zeth4"

    {
        # Asked of a freshly booted machine, which is when a user asks it.
        echo "SYS:ShowNetStatus INTERFACES"
        echo "SYS:CheckNetConfig"
        # CLAIM 3.
        echo "SYS:AddNetInterface zeth4"
        echo "SYS:netstat -i"
        echo "SYS:ping 10.0.2.2 -c 2 -t 20"
        echo "SYS:netstat -h"
        # Naming an interface that is already up is what makes it one somebody
        # asked for, which is what makes claim 4 a refusal rather than another
        # slot changing hands.  Three names here plus zeth4 is all four slots
        # spoken for.
        echo "SYS:AddNetInterface aeth0"
        echo "SYS:AddNetInterface beth1"
        echo "SYS:AddNetInterface meth2"
        # CLAIM 4: the fifth simultaneous attach.
        echo "SYS:AddNetInterface neth3"
        # CLAIM 5.
        echo "SYS:RemoveNetInterface aeth0"
        echo "SYS:AddNetInterface neth3"
        echo "SYS:RemoveNetInterface neth3"
        for i in 1 2 3 4 5; do
            echo "SYS:AddNetInterface aeth0"
            echo "SYS:RemoveNetInterface aeth0"
        done
        echo "SYS:netstat -h"
        echo "SYS:ShowNetStatus INTERFACES"
    } > "$stage/commands.txt"

    boot ifslots-named "$stage"
    rc=$?
    if [ "$rc" != 0 ]; then
        rig "the named round did not produce a transcript to read"
        return 2
    fi

    # ---- CLAIM 1: it booted and ran the whole list ----------------------
    local wanted ran
    wanted=$(grep -c . "$stage/commands.txt")
    ran=$(tr -d '\r' < "$REPORT" | grep -c '^===== ')
    if [ "$ran" -ge "$wanted" ]; then
        pass "the machine booted with five definitions and ran all $wanted commands"
        claim 1 PASS "boots with 5 definitions ($ran/$wanted commands)"
    else
        fail "the guest ran only $ran of $wanted commands"
        claim 1 FAIL "boots with 5 definitions ($ran/$wanted commands)"
    fi

    # ---- CLAIM 2: every definition is listed, and listed as defined -----
    first=$(block "SYS:ShowNetStatus INTERFACES" 1 |
            sed -n '/^Interfaces$/,/^$/p' |
            grep -vE '^(Interfaces|Name[[:space:]]+State)')
    local listed=0
    for n in aeth0 beth1 meth2 neth3 zeth4; do
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
    if [ "$listed" = 5 ]; then
        claim 2 PASS "ShowNetStatus lists all five definitions"
    else
        claim 2 FAIL "ShowNetStatus listed $listed of 5 definitions"
    fi

    # CheckNetConfig must not call a full drawer a fault.
    if block "SYS:CheckNetConfig" 1 | grep -qi "drawer holds"; then
        fail "CheckNetConfig still reports the drawer size as a fault"
    else
        pass "CheckNetConfig does not call five interface files a fault"
    fi

    # ---- CLAIM 3: the definition that is not first comes up -------------
    local ok3=1
    if block "SYS:AddNetInterface zeth4" 1 | grep -qiE "ENOSPC|cannot come up"
    then
        fail "zeth4 was refused a slot: the start-up pass took them all"
        ok3=0
    else
        pass "zeth4 was not refused a slot"
    fi
    if block "SYS:netstat -i" 1 | grep -qE "^zeth4[[:space:]]"; then
        pass "zeth4 -- last in the drawer -- is a live interface"
    else
        fail "netstat -i does not show zeth4 as a live interface"
        ok3=0
    fi
    if block "SYS:netstat -i" 1 | grep -qE "^zeth4[[:space:]].*10\.0\.2\.15"
    then
        pass "and it took the SLIRP lease 10.0.2.15"
    else
        fail "zeth4 has no address"
        ok3=0
    fi
    if block "SYS:ping 10.0.2.2 -c 2 -t 20" 1 |
       grep -qE "0(\.0)?% packet loss|[12] (packets )?received"; then
        pass "and the gateway answers over it"
    else
        fail "no ping replies over zeth4"
        ok3=0
    fi
    [ "$ok3" = 1 ] && claim 3 PASS "a definition that is not first can be brought up" \
                   || claim 3 FAIL "a definition that is not first can be brought up"

    # ---- CLAIM 4: FOUR AT ONCE, AND THE FIFTH REFUSED BY NAME ----------
    #
    # The cap moved from two to four and this is what says four is real in
    # both directions: the four that are up are up together and carrying
    # their own addresses, and the fifth attach is refused with all four
    # holders named in it.
    local ok4=1 named=0 up4=0
    many=$(block "SYS:netstat -i" 1)
    for n in aeth0 beth1 meth2 zeth4; do
        printf '%s\n' "$many" | grep -qE "^${n}[[:space:]]" &&
            up4=$((up4 + 1))
    done
    if [ "$up4" = 4 ]; then
        pass "four interfaces are attached at once"
    else
        fail "only $up4 of 4 interfaces are attached at once"
        ok4=0
    fi

    third=$(block "SYS:AddNetInterface neth3" 1)
    if printf '%s\n' "$third" | grep -qi "slots are in use"; then
        pass "a fifth simultaneous attach is refused"
    else
        fail "the fifth attach was not refused"
        ok4=0
    fi
    for n in aeth0 beth1 meth2 zeth4; do
        printf '%s\n' "$third" | grep -q "$n" && named=$((named + 1))
    done
    if [ "$named" = 4 ]; then
        pass "and the refusal NAMES all four interfaces that hold the slots"
    else
        fail "the refusal names $named of the 4 interfaces that are up"
        ok4=0
    fi
    if printf '%s\n' "$third" | grep -qE "all 4 interface slots"; then
        pass "and says how many slots there are, which is four"
    else
        fail "the refusal does not say there are 4 slots"
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
    [ "$ok4" = 1 ] && claim 4 PASS "four attach at once and the fifth is refused by name" \
                   || claim 4 FAIL "four attach at once and the fifth is refused by name"

    # ---- CLAIM 5: the slot frees, and five cycles do not leak -----------
    local ok5=1
    second_meth=$(block "SYS:AddNetInterface neth3" 2)
    if printf '%s\n' "$second_meth" | grep -qi "slots are in use"; then
        fail "neth3 still refused after RemoveNetInterface freed a slot"
        ok5=0
    else
        pass "neth3 comes up once RemoveNetInterface has freed a slot"
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
    local ok=1 refusal ifaces rc

    echo
    echo "=============================================================="
    echo "==> every slot held, three of them by interfaces nobody asked"
    echo "==> for, and the one asked for names a driver that is not there"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.77.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/aeth0"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.78.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/beth1"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.79.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/ceth2"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.80.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/deth3"
    printf 'DEVICE=nosuchcard.device\nUNIT=0\nCONFIGURE=DHCP\n' \
        > "$stage/devs/NetInterfaces/zbad4"

    {
        echo "SYS:AddNetInterface aeth0"
        echo "SYS:AddNetInterface zbad4"
        echo "SYS:netstat -i"
    } > "$stage/commands.txt"

    boot ifslots-typo "$stage"
    rc=$?
    if [ "$rc" != 0 ]; then
        rig "the typo round did not produce a transcript to read"
        return 2
    fi

    refusal=$(block "SYS:AddNetInterface zbad4" 1)
    if printf '%s\n' "$refusal" | grep -qi "nosuchcard.device"; then
        pass "the refusal names the driver that is not there"
    else
        fail "the refusal does not name nosuchcard.device"
        ok=0
    fi

    # THE POINT OF THE ROUND.  aeth0 was named and cannot yield; the other
    # three were not, and one of them is the slot a yield would have taken.
    # All four have to still be here.
    ifaces=$(block "SYS:netstat -i" 1)
    for n in beth1 ceth2 deth3; do
        if printf '%s\n' "$ifaces" | grep -qE "^${n}[[:space:]]"; then
            pass "$n is still up: no slot was taken for an interface that\
 could not come up"
        else
            fail "$n was taken down for zbad4, which never opened its device"
            ok=0
        fi
    done
    if printf '%s\n' "$ifaces" | grep -qE "^aeth0[[:space:]]"; then
        pass "and aeth0 is still up too"
    else
        fail "aeth0 is gone"
        ok=0
    fi
    if printf '%s\n' "$ifaces" | grep -qE "^zbad4[[:space:]]"; then
        fail "zbad4 is listed as a live interface and its driver does not exist"
        ok=0
    else
        pass "and zbad4 is not a live interface"
    fi

    [ "$ok" = 1 ] && claim 4b PASS "a slot is never taken for an interface that cannot come up" \
                  || claim 4b FAIL "a slot is never taken for an interface that cannot come up"

    return 0
}

# =================================================================== A3 ====
#
# AND THE OTHER HALF OF IT: AN ADD THAT FAILS AFTER THE DEVICE OPENED.
# Claims 4c and 4d, one boot.
#
# Opening the device before any slot changes hands is only half a guarantee.
# An interface whose device opens perfectly can still fail afterwards, and by
# then a slot has already changed hands.  The failure staged here is the one a
# user reaches by copying an interface file and forgetting to change the
# address: NetX Duo refuses to attach a second interface carrying an IPv4
# address the machine already has (NX_DUPLICATED_ENTRY,
# nx_ip_interface_attach.c:110), which is after the device has opened and
# after the slot has been taken.
#
#   4c  the interface that stood down comes back.  Before the fix it was left
#       `defined`, and the interfaces still attached went `offline` with it,
#       because removing the rolled-back interface sends S2_OFFLINE to the card
#       they all share.  One duplicated ADDRESS line cost the machine its
#       network.
#
#   4d  and the failure that is NOT fatal stays not fatal: a GATEWAY that is on
#       no interface's network is refused by nx_ip_gateway_address_set(), and
#       the interface comes up anyway -- which is what start-up has always
#       done, and what taking the interface down instead disagreed with.  The
#       command says so in one line rather than leaving a machine with an
#       address and no way off its own subnet.
round_latefail() {
    local stage="$ROOT/build/ifslots-stage-latefail"
    local ok=1 ok4d=1 refusal ifaces table rc n

    echo
    echo "=============================================================="
    echo "==> the newcomer's device opens and the interface fails"
    echo "==> anyway: does the slot go back where it came from?"
    echo "=============================================================="

    rm -rf "$stage"
    mkdir -p "$stage/libs" "$stage/devs/NetInterfaces"
    cp "$BSD" "$stage/libs/bsdsocket.library"
    cp "$A2065" "$stage/devs/a2065.device"

    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.77.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/aeth0"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.78.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/beth1"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.79.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/ceth2"
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.80.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/deth3"
    # The copied file: aeth0's address, one card, a device that opens.
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.77.5\nNETMASK=255.255.255.0\n' \
        > "$stage/devs/NetInterfaces/ydup4"
    # And the one that must NOT be fatal: its own subnet, a next hop on
    # nobody's.
    printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=192.168.81.5\nNETMASK=255.255.255.0\nGATEWAY=10.55.55.1\n' \
        > "$stage/devs/NetInterfaces/zgw5"

    {
        echo "SYS:AddNetInterface aeth0"
        echo "SYS:netstat -i"
        # CLAIM 4c: fails after the slot has changed hands.
        echo "SYS:AddNetInterface ydup4"
        echo "SYS:netstat -i"
        echo "SYS:ShowNetStatus INTERFACES"
        # CLAIM 4d: comes up, and says what it did not get.
        echo "SYS:AddNetInterface zgw5"
        echo "SYS:netstat -i"
        # Not asserted on, printed: the yield and the restore are both events,
        # and a red run here is unreadable without them.  A shipped build keeps
        # the ring, which is the whole reason it exists.
        echo "SYS:ShowNetStatus EVENTS"
    } > "$stage/commands.txt"

    boot ifslots-latefail "$stage"
    rc=$?
    if [ "$rc" != 0 ]; then
        rig "the late-failure round did not produce a transcript to read"
        return 2
    fi

    refusal=$(block "SYS:AddNetInterface ydup4" 1)
    if printf '%s\n' "$refusal" | grep -qiE "refused|did not"; then
        pass "the add of an interface that cannot be finished is refused"
    else
        fail "the add reported no failure at all"
        ok=0
    fi

    # The machine, afterwards.  Every interface that was up before the failed
    # add is up after it, carrying the address it had.
    ifaces=$(block "SYS:netstat -i" 2)
    for n in aeth0 beth1 ceth2 deth3; do
        if printf '%s\n' "$ifaces" | grep -qE "^${n}[[:space:]]"; then
            pass "$n is still a live interface after the failed add"
        else
            fail "$n is gone: the failed add took an interface with it"
            ok=0
        fi
    done
    if printf '%s\n' "$ifaces" |
       grep -qE "^aeth0[[:space:]]+[0-9]+[[:space:]]+192\.168\.77\.5"
    then
        pass "and aeth0 still carries its own address"
    else
        fail "aeth0 is listed without 192.168.77.5: the rolled-back interface\
 took its configuration with it"
        ok=0
    fi

    # WHAT IS NOT ASSERTED HERE, AND WHY.  Every interface in this staging is
    # the SAME a2065.device unit 0, because the emulator has one card -- and
    # S2_OFFLINE is the DEVICE's command, not the interface's.  So removing the
    # rolled-back interface marks the link down on the others too, and they
    # read `offline` afterwards; the restored one comes back up because its
    # re-attach sends S2_ONLINE.  That is a real defect and it is filed
    # (docs/BACKLOG.md), but it is not this one: it needs per-device
    # coordination, because S2_OFFLINE is also the only thing that returns a
    # queued CMD_READ on a driver that ignores AbortIO(), which is what
    # Commodore's a2065.device 2.16 does.  A machine with four CARDS never
    # meets it, and it is why the line above asks about the address rather than
    # the link.
    if printf '%s\n' "$ifaces" | grep -qE "^aeth0[[:space:]].*[[:space:]]down" &&
       [ "${AMINETXDUO_IFSLOTS_QUIET:-}" != 1 ]; then
        echo "  note aeth0's link is down: one card, four interfaces, and"
        echo "       S2_OFFLINE belongs to the card (see docs/BACKLOG.md)"
    fi
    if printf '%s\n' "$ifaces" | grep -qE "^ydup4[[:space:]]"; then
        fail "ydup4 is a live interface and its address is aeth0's"
        ok=0
    else
        pass "and ydup4 is not a live interface"
    fi

    # ShowNetStatus is where a stood-down interface reads as `defined`, which
    # is the shape the defect had.
    table=$(block "SYS:ShowNetStatus INTERFACES" 1 |
            sed -n '/^Interfaces$/,/^$/p' |
            grep -vE '^(Interfaces|Name[[:space:]]+State)')
    for n in aeth0 beth1 ceth2 deth3; do
        if printf '%s\n' "$table" | grep -qE "^${n}[[:space:]]+defined"; then
            fail "$n stood down for ydup4 and was left defined rather than\
 being put back"
            ok=0
        fi
    done
    if [ "$ok" = 1 ]; then
        pass "no interface was left merely 'defined' by the failed add"
    fi

    [ "$ok" = 1 ] && claim 4c PASS "an add that fails after its device opened puts the slot back" \
                  || claim 4c FAIL "an add that fails after its device opened puts the slot back"

    # ---- CLAIM 4d: a refused route is not a refused interface -----------
    refusal=$(block "SYS:AddNetInterface zgw5" 1)
    ifaces=$(block "SYS:netstat -i" 3)

    if printf '%s\n' "$ifaces" |
       grep -qE "^zgw5[[:space:]]+[0-9]+[[:space:]]+192\.168\.81\.5[[:space:]]+up"
    then
        pass "an interface whose GATEWAY is unreachable still comes up"
    else
        fail "zgw5 did not come up: a refused route destroyed the interface"
        ok4d=0
    fi
    if printf '%s\n' "$refusal" | grep -qi "default route"; then
        pass "and the command says the default route was refused"
    else
        fail "nothing was said about the route that was refused"
        ok4d=0
    fi
    if printf '%s\n' "$refusal" | grep -qi "GATEWAY line"; then
        pass "and says which line of which file to look at"
    else
        fail "the message does not say where to look"
        ok4d=0
    fi

    [ "$ok4d" = 1 ] && claim 4d PASS "a route that cannot be installed does not destroy the interface" \
                    || claim 4d FAIL "a route that cannot be installed does not destroy the interface"

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

    boot ifslots-identity "$stage"
    if [ "$?" != 0 ]; then
        rig "the identity round did not produce a transcript to read"
        return 2
    fi

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
        latefail) round_latefail ;;
        identity) round_identity ;;
        *) echo "no such round: $r" >&2; exit 2 ;;
    esac
done

echo
echo "=============================== claims ==============================="
cat "$CLAIMS"
echo "======================================================================"
echo "$TOTAL checks, $BAD failures, $RIG round(s) the rig did not finish"

if [ "$RIG" != 0 ]; then
    # Exit 2, which tools/ci.sh reads as "the rig refused it": a boot that
    # never finished decides nothing, and reporting it as a claim that is
    # false is how a wedged emulator gets read as a defect in the stack.
    echo "RIG: $RIG round(s) did not finish.  No verdict -- re-run them quiet."
    exit 2
fi

if [ "$BAD" = 0 ] && ! grep -q FAIL "$CLAIMS"; then
    echo "PASS: the interface a user names is the interface that gets a slot,\
 it answers to its own name, and an add that fails takes nothing with it"
    exit 0
fi

echo "FAIL: see the claims above"
exit 1
