#!/usr/bin/env bash
#
# Did the machine come up AND carry a packet?  One grader, sourced without an
# emulator.
#
#   . "$ROOT/tests/tools/bringup-verdict.sh"
#   bringup_verdict <netstack_test-transcript> [min-pool-packets]
#
# SPDX-License-Identifier: MIT

_bv_line() { # transcript what
    grep -aF -- "$2" "$1" 2>/dev/null | grep -aE '^[[:space:]]+(ok|FAIL)[[:space:]]' | tail -1
}

# bringup_verdict TRANSCRIPT [MIN_POOL_PACKETS] -> 0 pass, 1 fail
bringup_verdict() {
    local report="$1" poolmin="${2:-0}"
    local failed=0 line what state
    local packets="" bytes=""

    _bv_fail() { echo "bringup_fail=$1"; failed=1; }

    if [ ! -s "$report" ]; then
        echo "bringup_report=missing"
        _bv_fail no_transcript
        echo "bringup_result=FAIL"
        return 1
    fi
    echo "bringup_report=present"

    for what in "interface 0 link is up" \
                "interface 0 has an address" \
                "ICMP echo to gateway"
    do
        line=$(_bv_line "$report" "$what")
        state=absent
        case "$line" in
            *"  ok   "*)  state=ok ;;
            *"FAIL"*)     state=failed ;;
        esac

        printf 'bringup_%s=%s\n' "$(printf '%s' "$what" | tr ' ' '_')" "$state"

        case "$state" in
            ok) ;;
            failed)
                _bv_fail "$(printf '%s' "$what" | tr ' ' '_')_failed" ;;
            absent)
                _bv_fail "$(printf '%s' "$what" | tr ' ' '_')_never_ran" ;;
        esac
    done

    # `pool 84 packets (84 free) of 1592 bytes`
    line=$(grep -aoE 'pool [0-9]+ packets \([0-9]+ free\) of [0-9]+ bytes' \
                "$report" 2>/dev/null | tail -1)
    if [ -n "$line" ]; then
        packets=$(printf '%s\n' "$line" | awk '{print $2}')
        bytes=$(printf '%s\n' "$line" | awk '{print $(NF - 1)}')
    fi
    echo "bringup_pool_packets=${packets:-none}"
    echo "bringup_pool_bytes=${bytes:-none}"

    if [ "$poolmin" != 0 ]; then
        if [ -z "$packets" ]; then
            _bv_fail pool_line_absent
        elif [ "$packets" -lt "$poolmin" ]; then
            _bv_fail "pool_${packets}_below_${poolmin}"
        fi
    fi

    if [ "$failed" = 0 ]; then
        echo "bringup_result=PASS"
        return 0
    fi
    echo "bringup_result=FAIL"
    return 1
}
