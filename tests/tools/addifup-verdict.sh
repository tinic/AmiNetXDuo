#!/usr/bin/env bash
#
# The verdict half of tests/tools/run-addifup.sh, in a file that can be
# sourced without an emulator.
#
#   . "$ROOT/tests/tools/addifup-verdict.sh"
#   addifup_verdict <ShowNetStatus-transcript> <interface-name>
#
# It is separate from the harness so that
# tests/tools/addifup-verdict-selftest.sh can drive it against transcripts
# that stand for runs nobody can produce on demand: a lease that never
# arrived, an RFC 3927 fallback, a stack that came up offline.  The harness
# itself needs a Kickstart, a driver and three minutes.
#
# What replaced what
#
#   The two assertions this supersedes were `grep -qiE "online|address"` and
#   `grep -qE "[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+"` over the whole transcript.
#   Neither could go red.  src/tools/shownetstatus.c:552 prints the column
#   header "Name            State    Link     Address" before any interface,
#   and :456 prints "address     handed out by DHCP when the interface comes
#   up" on exactly the interface that did not get one, so a case-insensitive
#   search for "address" matched the failure it was there to catch.  The
#   dotted quad matched a netmask, a route, a name server or the 169.254
#   address the stack assigns itself when DHCP does not answer.
#
#   So the assertions are on the interfaces table row instead, which carries
#   the three facts separately: src/tools/shownetstatus.c:571 prints
#   "%-15s %-8s %-8s %s" as name, online|offline|?, up|down|?, and the address
#   or "-".
#
# Output is key=value on stdout, one per fact, and the return code is the
# verdict.
#
# SPDX-License-Identifier: MIT

# addifup_verdict REPORT IFNAME -> 0 pass, 1 fail
addifup_verdict() {
    local report="$1" ifname="${2:-eth0}"
    local failed=0 row state link addr

    _av_fail() { echo "addifup_fail=$1"; failed=1; }

    if [ ! -s "$report" ]; then
        echo "addifup_report=missing"
        _av_fail no_transcript
        echo "addifup_result=FAIL"
        return 1
    fi
    echo "addifup_report=present"

    # The row under the "Interfaces" heading, not the per-interface detail
    # block above it: the detail block prints the configured name even for an
    # interface that never attached, so a row there proves nothing.
    row=$(sed -n '/^Interfaces$/,$p' "$report" | tr -d '\r' |
          grep -E "^${ifname}[[:space:]]" | head -1)

    if [ -z "$row" ]; then
        echo "addifup_row=absent"
        _av_fail "no_${ifname}_row_in_interfaces_table"
        echo "addifup_result=FAIL"
        return 1
    fi

    state=$(printf '%s\n' "$row" | awk '{print $2}')
    link=$(printf '%s\n' "$row" | awk '{print $3}')
    addr=$(printf '%s\n' "$row" | awk '{print $4}')

    echo "addifup_state=${state:-none}"
    echo "addifup_link=${link:-none}"
    echo "addifup_address=${addr:-none}"

    # "?" is what the command prints when the stack is up inside another
    # program and this one cannot read the flags, which is not a pass.
    [ "$state" = "online" ] || _av_fail "state_${state:-none}_not_online"
    [ "$link" = "up" ]      || _av_fail "link_${link:-none}_not_up"

    case "$addr" in
        # "-" is the literal the command prints for an interface that never
        # attached (src/tools/shownetstatus.c:547).
        -|"")            _av_fail no_address ;;
        0.0.0.0)         _av_fail address_unset ;;
        169.254.*)
            # RFC 3927 self-assignment. The interface is up and has an
            # address, and the lease this test exists to watch did not
            # complete, so it is named rather than counted as one.
            _av_fail "dhcp_fell_back_to_linklocal_$addr" ;;
        127.*)           _av_fail "address_is_loopback_$addr" ;;
        *.*.*.*)
            case "$addr" in
                *[!0-9.]*) _av_fail "address_not_a_quad_$addr" ;;
            esac ;;
        *)               _av_fail "address_not_a_quad_$addr" ;;
    esac

    # The DHCP placeholder is printed only where no lease arrived, so its
    # presence contradicts a row that claims an address. Asserted separately
    # because it names the failure in the transcript's own words.
    if grep -q "handed out by DHCP when the interface comes up" "$report"; then
        echo "addifup_dhcp_placeholder=present"
        _av_fail lease_never_arrived
    else
        echo "addifup_dhcp_placeholder=absent"
    fi

    if [ "$failed" = "0" ]; then
        echo "addifup_result=PASS"
        return 0
    fi
    echo "addifup_result=FAIL"
    return 1
}
