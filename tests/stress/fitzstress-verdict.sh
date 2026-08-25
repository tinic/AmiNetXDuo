#!/usr/bin/env bash
#
# The verdict half of tests/stress/run-fitzstress.sh, in a file that can be
# sourced without an emulator, a peer or four hours.
#
#   . "$ROOT/tests/stress/fitzstress-verdict.sh"
#   fitzstress_verdict <stress-summary.txt> <compare.log> <emulator-rc>
#
# SPDX-License-Identifier: MIT

# fitzstress_verdict SUMMARY COMPARELOG RUN_RC -> 0 pass, 1 fail, 3 broken
fitzstress_verdict() {
    local summary="$1" comparelog="$2" run_rc="${3:-0}"
    local failed=0 bad dirty stuck iters clean dirty_seen

    _fv_fail() { echo "fitzstress_fail=$1"; failed=1; }

    if [ "$run_rc" = 124 ]; then
        echo "fitzstress_run_rc=124"
        echo "fitzstress_reason=the_host_deadline_expired_so_the_summary_is_not_a_wind_down"
        echo "fitzstress_result=BROKEN"
        return 3
    fi
    echo "fitzstress_run_rc=$run_rc"

    if [ ! -s "$summary" ]; then
        echo "fitzstress_summary=missing"
        echo "fitzstress_reason=the_supervisor_wrote_no_summary"
        echo "fitzstress_result=BROKEN"
        return 3
    fi
    echo "fitzstress_summary=present"

    stuck=$(awk '/^stuck_workers /{ print $2; exit }' "$summary")
    bad=$(awk   '/^bad_total /{ print $2; exit }' "$summary")
    dirty=$(awk '/^dirty_total /{ print $2; exit }' "$summary")
    iters=$(awk '/^w[0-9] iters /{ n += $3 } END { print n + 0 }' "$summary")

    echo "fitzstress_iterations=$iters"
    [ "$iters" -gt 0 ] || _fv_fail no_worker_completed_an_iteration

    if [ -z "$stuck" ]; then
        echo "fitzstress_stuck=unknown"
        _fv_fail the_summary_has_no_stuck_workers_line
    else
        echo "fitzstress_stuck=$stuck"
        [ "$stuck" = 0 ] || _fv_fail "${stuck}_workers_never_came_back"
    fi

    if [ -z "$bad" ] || [ -z "$dirty" ]; then
        echo "fitzstress_totals=absent"
        _fv_fail the_summary_predates_bad_total_and_dirty_total
    else
        echo "fitzstress_bad=$bad"
        echo "fitzstress_dirty=$dirty"
        [ "$bad" = 0 ] ||
            _fv_fail "${bad}_buffers_came_back_different_from_what_was_sent"
        [ "$dirty" = 0 ] ||
            _fv_fail "${dirty}_trees_did_not_compare"
    fi

    if [ -s "$comparelog" ]; then
        clean=$(grep -ac '^----- rc 0 ' "$comparelog" || true)
        dirty_seen=$(grep -a '^----- rc ' "$comparelog" | grep -vc 'rc 0 ' || true)
        echo "fitzstress_compares=$((clean + dirty_seen))"
        echo "fitzstress_compares_clean=$clean"
        echo "fitzstress_compares_dirty=$dirty_seen"
        [ "$((clean + dirty_seen))" -gt 0 ] ||
            _fv_fail comparetree_never_ran_so_the_data_was_never_checked
        if [ -n "$dirty" ] && [ "$dirty_seen" != "$dirty" ]; then
            _fv_fail "the_guest_counted_${dirty}_dirty_trees_and_the_log_holds_${dirty_seen}"
        fi
    else
        echo "fitzstress_compares=0"
        _fv_fail comparetree_never_ran_so_the_data_was_never_checked
    fi

    if [ "$failed" -ne 0 ]; then
        echo "fitzstress_result=FAIL"
        return 1
    fi
    echo "fitzstress_result=PASS"
    return 0
}
