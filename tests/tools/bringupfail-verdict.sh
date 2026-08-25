#!/usr/bin/env bash
# WHAT A USER SEES WHEN BRING-UP FAILS, graded.
# SPDX-License-Identifier: MIT

BFV_OPERATIONS='open|opening|read|reading|add|adding|start|starting|configure|configuring|attach|attaching|allocat'

BFV_OBJECT='DEVS:[^[:space:]]+|[A-Za-z0-9_-]+\.device'

BFV_CODE='\(-?[0-9]+\)|\(0x[0-9a-fA-F]+\)|\((error|code|IoErr|rc)[[:space:]]+-?[0-9]+\)|(error|code|IoErr|rc)[[:space:]]+-?[0-9]+|0x[0-9a-fA-F]{2,}|line[[:space:]]+[0-9]+'

BFV_LOGADVICE='(debug|the|a|error|serial|trace)[[:space:]]+log([[:space:]]|file|s|\.|,|$)|log[[:space:]]*file|check[[:space:]]+the[[:space:]]+log|enable[[:space:]]+logging|turn[[:space:]]+on[[:space:]]+logging|see[[:space:]]+the[[:space:]]+log'

bfv_first_line() { # transcript [tool]
    local tool="${2:-[A-Z][A-Za-z0-9]*}" line

    line=$(tr -d '\r' < "$1" 2>/dev/null | grep -aE "^${tool}:[[:space:]]" | head -1)
    if [ -n "$line" ]; then
        printf '%s\n' "$line"
        return 0
    fi

    tr -d '\r' < "$1" 2>/dev/null |
        grep -aE '^[[:space:]]*DEVS:[^[:space:]]*,?[[:space:]]*(line[[:space:]]+[0-9]+)?:' |
        head -1 |
        sed 's/^[[:space:]]*//'
}

# bringupfail_verdict TRANSCRIPT CAUSE [TOOL] -> 0 pass, 1 fail
bringupfail_verdict() {
    local report="$1" cause="${2:-unnamed}" tool="${3:-}"
    local failed=0 first offender

    _bf_fail() { echo "bringupfail_fail=$1"; failed=1; }

    echo "bringupfail_cause=$cause"

    if [ ! -s "$report" ]; then
        echo "bringupfail_report=missing"
        _bf_fail no_transcript
        echo "bringupfail_result=FAIL"
        return 1
    fi


    first=$(bfv_first_line "$report" "$tool")
    if [ -z "$first" ]; then
        echo "bringupfail_first_line=none"
        _bf_fail no_message_at_all
        echo "bringupfail_result=FAIL"
        return 1
    fi
    echo "bringupfail_first_line=$first"

    local msg="${first#*: }"
    echo "bringupfail_message=$msg"

    if printf '%s\n' "$msg" | grep -qiE "$BFV_OPERATIONS" ||
       printf '%s\n' "$first" | grep -qE "$BFV_OBJECT"; then
        echo "bringupfail_names_operation=yes"
    else
        echo "bringupfail_names_operation=no"
        _bf_fail first_line_names_no_operation
    fi

    if printf '%s\n' "$first" | grep -qE "$BFV_CODE"; then
        echo "bringupfail_names_code=yes"
    else
        echo "bringupfail_names_code=no"
        _bf_fail first_line_carries_no_code
    fi


    offender=$(tr -d '\r' < "$report" | grep -inE "$BFV_LOGADVICE" | head -3)
    if [ -z "$offender" ]; then
        echo "bringupfail_log_advice=absent"
    else
        echo "bringupfail_log_advice=present"
        printf '%s\n' "$offender" |
            while IFS= read -r l; do echo "bringupfail_log_line=$l"; done
        _bf_fail sends_user_to_a_log_that_shipping_builds_cannot_produce
    fi

    if [ "$failed" = 0 ]; then
        echo "bringupfail_result=PASS"
        return 0
    fi
    echo "bringupfail_result=FAIL"
    return 1
}
