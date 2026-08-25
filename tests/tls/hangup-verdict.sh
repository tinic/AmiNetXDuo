#!/usr/bin/env bash
# The verdict half of tests/tls/run-hangup.sh, in a file that can be sourced
# without an emulator.
# SPDX-License-Identifier: MIT

HANGUP_LEGIBLE='the connection is closed|the server stopped answering|the server did not complete a TLS handshake|the network connection failed|the server broke off the connection|closed the connection without answering|stopped answering after|did not answer with HTTP|did not answer within'

HANGUP_TIMEOUT_WORDS='the server stopped answering|stopped answering after|did not answer within'

# hangup_verdict REPORT HANG_TIMEOUT [PEERLOG] -> 0 pass, 1 fail
hangup_verdict() {
    local report="$1" hang_timeout="${2:-15}" peerlog="${3:-}"
    local failed=0 n rc ms legible timedout name lo hi seen
    local -a rcs mss legs tos

    _hv_fail() { echo "hangup_fail=$1"; failed=1; }

    if [ ! -s "$report" ]; then
        echo "hangup_transcript=missing"
        _hv_fail no_transcript
        echo "hangup_result=FAIL"
        return 1
    fi
    echo "hangup_transcript=present"

    while read -r rc ms legible timedout; do
        rcs+=("$rc"); mss+=("$ms"); legs+=("$legible"); tos+=("$timedout")
    done < <(awk -v legible="$HANGUP_LEGIBLE" -v tmo="$HANGUP_TIMEOUT_WORDS" '
        /^===== SYS:fetch / { if (inblock) print rc, ms, leg, to
                              inblock = 1; rc = "-"; ms = "-"; leg = "no"
                              to = "no"
                              next }
        inblock && /^----- rc / {
            split($0, f, /[ ,]+/); rc = f[3]; ms = f[4]
            print rc, ms, leg, to
            inblock = 0
            next
        }
        inblock && $0 ~ legible { leg = "yes" }
        inblock && $0 ~ tmo     { to = "yes" }
        END { if (inblock) print rc, ms, leg, to }
    ' "$report")

    echo "hangup_fetches=${#rcs[@]}"

    if grep -q '^===== done, 0 command(s) would not run' "$report"; then
        echo "hangup_command_list=complete"
    else
        echo "hangup_command_list=short"
        _hv_fail command_list_did_not_finish
    fi

    if [ "$(sed -n 's/^----- rc \([0-9-]*\),.*/\1/p' \
            <(sed -n '/^===== SYS:AddNetInterface /,/^----- rc /p' "$report") \
            | head -1)" = 0 ]; then
        echo "hangup_interface=up"
    else
        echo "hangup_interface=down"
        _hv_fail the_interface_never_came_up
    fi

    if [ "${#rcs[@]}" -ne 4 ]; then
        _hv_fail "expected_4_fetch_blocks_got_${#rcs[@]}"
    fi

    lo=$(( hang_timeout * 500 ))            # half the deadline
    hi=$(( hang_timeout * 4000 ))           # four times it

    n=0
    for name in rst fin hang garbage; do
        if [ -n "$peerlog" ]; then
            if [ ! -s "$peerlog" ]; then
                seen=nolog
            elif grep -q "^$name: [0-9]* bytes from " "$peerlog"; then
                seen=yes
            else
                seen=no
            fi
            echo "hangup_${name}_reached_the_peer=$seen"
            [ "$seen" = yes ] ||
                _hv_fail "the_rude_peer_never_saw_the_${name}_connection"
        fi

        if [ "$n" -ge "${#rcs[@]}" ]; then
            echo "hangup_$name=absent"
            _hv_fail "${name}_never_ran"
            n=$((n + 1))
            continue
        fi

        rc="${rcs[$n]}"; ms="${mss[$n]}"; legible="${legs[$n]}"
        timedout="${tos[$n]}"
        echo "hangup_$name=rc=$rc ms=$ms legible=$legible timeout=$timedout"

        case "$rc" in
            10) ;;
            0)  _hv_fail "${name}_returned_0_a_rude_peer_was_reported_as_a_fetch" ;;
            *)  _hv_fail "${name}_returned_${rc}_not_10" ;;
        esac
        [ "$legible" = yes ] ||
            _hv_fail "${name}_printed_no_error_a_caller_can_act_on"

        if [ "$name" = hang ]; then
            [ "$timedout" = yes ] ||
                _hv_fail "hang_did_not_report_a_timeout"
            case "$ms" in
                ''|*[!0-9]*) _hv_fail "hang_has_no_elapsed_time" ;;
                *)
                    if [ "$ms" -lt "$lo" ]; then
                        _hv_fail "hang_returned_after_${ms}ms_which_is_not_a_${hang_timeout}s_deadline"
                    elif [ "$ms" -gt "$hi" ]; then
                        _hv_fail "hang_took_${ms}ms_so_nothing_served_the_deadline"
                    fi
                    ;;
            esac
        else
            [ "$timedout" = no ] ||
                _hv_fail "${name}_reported_a_timeout_rather_than_the_close_the_peer_sent"
        fi
        n=$((n + 1))
    done

    if [ "$failed" -ne 0 ]; then
        echo "hangup_result=FAIL"
        return 1
    fi
    echo "hangup_result=PASS"
    return 0
}
