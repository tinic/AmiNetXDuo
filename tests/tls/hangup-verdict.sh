#!/usr/bin/env bash
#
# The verdict half of tests/tls/run-hangup.sh, in a file that can be sourced
# without an emulator.
#
#   . "$ROOT/tests/tls/hangup-verdict.sh"
#   hangup_verdict <ToolsSmoke-transcript> <hang-timeout-seconds>
#                  [rude-peer-log]
#
# WHAT IT REPLACED
#
#   Nothing.  run-hangup.sh ended `exit "$rc"` on tests/tls/run-fetch.sh's
#   status, and run-fetch.sh does not score a custom command list -- it
#   returns 77, a skip.  So the four rude-peer cases the harness exists for
#   were read by no assertion at all, and its own row in tests/HARNESSES said
#   so: "IT ASSERTS NOTHING TODAY".  Before that change it forwarded a 0 that
#   meant "ToolsSmoke's last command returned 0", which is not a fact about
#   any of the four.
#
# THE BAR
#
#   The handshakes cannot succeed; a peer that resets, closes, says nothing or
#   answers with nonsense is not going to produce a session.  What is asserted
#   is that each one ends in AN ERROR THE CALLER CAN ACT ON, and that the
#   machine is still there afterwards:
#
#     * four fetch blocks, in the order the command list has them.  A guest
#       that went down on the reset writes no block for the three after it,
#       so the count IS the "a peer must not be able to take the machine
#       down" assertion.
#     * each returns 10.  Not "nonzero": 10 is what src/tools/fetch.c returns
#       for a failure it understood, and 0 is the loudest failure there is
#       here -- it would mean a fetch reported success against a peer that
#       sent nonsense.
#     * each prints a sentence from the set the library and the command can
#       produce.  An empty block is a command that died without saying why,
#       which is the same as no answer for anyone reading it.
#     * the `hang` case takes about its TIMEOUT.  A peer that never answers
#       is the one case with no event to end it, so the deadline is the only
#       thing that can, and an instant return means it did not wait while a
#       run-length return means nothing served it.
#     * THE RUDE PEER SAW ALL FOUR.  Without this the whole file passes on a
#       machine whose networking is broken: four connections that never
#       arrived anywhere time out, `fetch` reports a legible timeout for each,
#       and every assertion above holds while none of the four behaviours was
#       ever exercised.  hangup-server.py prints a line per connection, so the
#       count of those lines is the assertion that the cases happened.
#     * and each case failed in ITS OWN WAY.  A peer that resets, closes or
#       says nonsense produces a CLOSE; only `hang` may report a timeout.  A
#       run in which all four say "the server stopped answering" is the same
#       broken-networking run one step along.
#
# Output is key=value on stdout, one per fact, and the return code is the
# verdict.
#
# SPDX-License-Identifier: MIT

# The sentences a caller can act on: src/tlslib/tls_conn.c:250-264 and the
# tool_error() calls in src/tools/fetch.c that a rude peer can reach.  Kept as
# one regex rather than "the block is not empty", because a block that says
# "internal error" is a block that says nothing.
HANGUP_LEGIBLE='the connection is closed|the server stopped answering|the server did not complete a TLS handshake|the network connection failed|the server broke off the connection|closed the connection without answering|stopped answering after|did not answer with HTTP|did not answer within'

# The subset of those that mean "nothing came back".  Only `hang` may say one.
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

    # One line per fetch block: "<rc> <ms> <legible>".  ToolsSmoke's own
    # framing is "===== <command> =====" ... "----- rc N, M ms, free F -----",
    # and a block with no rc line at all (the guest stopped inside it) prints
    # rc as "-" so it fails the numeric test below rather than being skipped.
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

    # Every command in the list has to have run.  ToolsSmoke says so itself,
    # and it is the difference between "the fourth case is missing because the
    # machine went down" and "because the list was short".
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
        # WHAT THE PEER SAW.  hangup-server.py prints "<behaviour>: N bytes
        # from <addr>" the moment it has the ClientHello, so a behaviour with
        # no line never happened, whatever the guest printed about it.
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
            # The one case with no event to end it.
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
            # The other three had something happen TO them.  A timeout here is
            # the guest never having reached the peer at all, which is the run
            # a broken rig produces and the one that used to read as a pass.
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
