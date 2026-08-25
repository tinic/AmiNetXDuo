#!/usr/bin/env bash
#
# Score a bsdsocktest TAP log.
#
#   . "$ROOT/tests/conformance/tap-verdict.sh"
#   tap_verdict <log> <emulator-exit-status>
#
# Returns 0 pass, 1 fail, 3 not measured.  Output is key=value.
#
# SPDX-License-Identifier: MIT

tap_verdict() {
    local log="$1" status="${2:-0}"
    local plan ran unexpected known skipped bailed rc=0

    if [ ! -s "$log" ]; then
        echo "conformance_log=missing"
        echo "conformance=NOT_MEASURED"
        return 3
    fi
    echo "conformance_log=present"

    plan=$(sed -n 's/^1\.\.\([0-9]*\)$/\1/p' "$log" | head -1)
    ran=$(grep -c '^\(not \)\?ok [0-9]' "$log" || true)
    unexpected=$(grep '^not ok [0-9]' "$log" | grep -vc '# KNOWN ' || true)
    known=$(grep '^not ok [0-9]' "$log" | grep -c '# KNOWN ' || true)
    skipped=$(grep -c '^ok [0-9][0-9]* - # SKIP' "$log" || true)
    bailed=$(grep -c '^Bail out!' "$log" || true)

    echo "conformance_plan=${plan:-none}"
    echo "conformance_ran=$ran"
    echo "conformance_unexpected_failures=$unexpected"
    echo "conformance_known_failures=$known"
    echo "conformance_skipped=$skipped"
    echo "conformance_bailed=$bailed"
    sed -n 's/^# Results: /conformance_results=/p' "$log" | head -1

    if [ -z "$plan" ]; then
        echo "conformance=NOT_MEASURED"
        return 3
    fi

    if [ "$bailed" != "0" ]; then
        grep '^Bail out!' "$log" | sed 's/^/  /' >&2
        echo "conformance_fail=bailed_out"
        rc=1
    fi
    if [ "$ran" -lt "$plan" ]; then
        echo "conformance_fail=short_run_${ran}_of_${plan}"
        rc=1
    fi
    if [ "$unexpected" != "0" ]; then
        grep '^not ok [0-9]' "$log" | grep -v '# KNOWN ' | head -20 |
            sed 's/^/  /' >&2
        echo "conformance_fail=${unexpected}_unexpected_failures"
        rc=1
    fi
    if [ "$status" != "0" ]; then
        echo "conformance_fail=emulator_exit_$status"
        rc=1
    fi

    if [ "$rc" = "0" ]; then
        echo "conformance=PASS"
    else
        echo "conformance=FAIL"
    fi
    return "$rc"
}
