#!/usr/bin/env bash
# The verdict half of tests/tools/run-addifup.sh, in a file that can be
# sourced without an emulator.
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

    [ "$state" = "online" ] || _av_fail "state_${state:-none}_not_online"
    [ "$link" = "up" ]      || _av_fail "link_${link:-none}_not_up"

    case "$addr" in
        -|"")            _av_fail no_address ;;
        0.0.0.0)         _av_fail address_unset ;;
        169.254.*)
            _av_fail "dhcp_fell_back_to_linklocal_$addr" ;;
        127.*)           _av_fail "address_is_loopback_$addr" ;;
        *.*.*.*)
            case "$addr" in
                *[!0-9.]*) _av_fail "address_not_a_quad_$addr" ;;
            esac ;;
        *)               _av_fail "address_not_a_quad_$addr" ;;
    esac

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
