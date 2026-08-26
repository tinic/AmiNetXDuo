#!/usr/bin/env bash
#
# The verdict half of the on-Amiga harnesses, in one place.
#
#   . "$ROOT/tools/test-verdict.sh"
#   verdict_guest <name> <min-checks> <run-rc> <transcript> [more transcripts...]
#
# A verdict needs evidence of work done: a transcript that exists and is not
# empty, the guest's own `N checks, M failures` line with M == 0, and N at or
# above a floor the caller states.
#
# and then, and only then, the emulator's exit status: 124 is a timeout, 4 is a
# guest built for the wrong CPU, and neither can be read as a result.
#
# SPDX-License-Identifier: MIT

# Where a run's guest output landed, by runner.  The tag is the one the caller
# exported before starting the run.

# THE MACHINE-READABLE HALF.  Every harness that reaches its verdict through
# this file prints the same key=value block last, so a caller grades a run by
# reading fields rather than by matching sentences.  The prose above it is for
# a person and may be reworded; these lines are the interface.
#
#   verdict=PASS|FAIL|SKIP     what happened
#   name=<harness>             which harness said so
#   checks=<n> failures=<m>    the guest's own counters
#   min_checks=<n>             the floor the caller stated
#   run_rc=<n>                 the emulator's exit status
#   reason=<token>             why, when it is not a plain pass.  One of
#                              no_transcript, no_summary, failures, timeout,
#                              wrong_cpu, wrong_backend, exit_disagrees,
#                              guest_skipped, too_few_checks
#   transcript=<path>          the file the verdict was read out of
#
# stdout, not stderr: a verdict is the result, and a caller that redirects
# stderr to keep the diagnostics out of a log must still get it.
verdict_kv() {
    local k
    for k in "$@"; do printf '%s\n' "$k"; done
}

verdict_hd_amiberry() { echo "$ROOT/build/amiberry-testhd-${AMINETXDUO_RUN_TAG:-amiberry}"; }
verdict_hd_winuae()   { echo "$ROOT/build/winuae-testhd-${AMINETXDUO_RUN_TAG:-winuae}"; }
verdict_serial_amiberry() { echo "$ROOT/build/amiberry-serial-${AMINETXDUO_RUN_TAG:-amiberry}.log"; }

# The guest's summary, `N checks, M failures[, PASS|FAIL]`, from the last place
# it appears.  tests/stack prints `stack: N checks, M failures`, and
# tests/netstack prints a second one after teardown; the last is the whole run
# in both cases.
# The serial log arrives with CRLF and the summary can be the first thing on
# its line, so neither a trailing \r nor a missing leading character may stop
# this matching.
verdict_summary() {
    tr -d '\r' < "$1" 2>/dev/null |
    awk 'match($0, /[0-9]+ checks, [0-9]+ failures/) {
             s = substr($0, RSTART, RLENGTH)
             gsub(/[^0-9]/, " ", s)
             n = split(s, f, " ")
             if (n >= 2) last = f[1] " " f[2]
         }
         END { if (last != "") print last }'
}

# verdict_guest NAME MIN_CHECKS RUN_RC TRANSCRIPT [TRANSCRIPT...]
#
# The first transcript that exists and has something in it is the one read; the
# rest are fallbacks, for the runs whose output reaches the serial port rather
# than DH0:stdout.txt.  Returns 0 on a pass, 1 on a failure, and prints the
# reason either way.
# verdict_find NAME RUN_RC MATCH TRANSCRIPT...
#
# The first transcript that exists, has something in it, and contains MATCH;
# failing that, the first that merely has something in it.  Echoes the path, or
# says why there is none and returns 1.  MATCH is an ERE, "." for "anything".
verdict_find() {
    local name="$1" run_rc="$2" match="$3"; shift 3
    local t found="" why=no_transcript

    for t in "$@"; do
        if [ -s "$t" ] && grep -Eq "$match" "$t"; then found="$t"; break; fi
    done
    if [ -z "$found" ]; then
        for t in "$@"; do
            if [ -s "$t" ]; then found="$t"; break; fi
        done
    fi
    if [ -n "$found" ]; then echo "$found"; return 0; fi

    echo "FAIL: $name: the guest produced NO transcript." >&2
    echo "      Looked for:" >&2
    printf '        %s\n' "$@" >&2
    if [ "$run_rc" = "124" ]; then
        echo "      The run timed out, so nothing under test reported anything." >&2
        why=timeout
    elif [ "$run_rc" = "4" ]; then
        echo "      The emulator saw an illegal instruction: the guest is built" >&2
        echo "      for a CPU this machine does not have." >&2
        why=wrong_cpu
    elif [ "$run_rc" = "5" ]; then
        echo "      The run did not get the network backend it asked for, so it" >&2
        echo "      was never on the link it was meant to be on." >&2
        why=wrong_backend
    fi
    echo "      This is an infrastructure failure, not a result. The emulator" >&2
    echo "      exited $run_rc and that says nothing about the code." >&2
    echo "$name: FAILED (no transcript)" >&2
    verdict_kv "name=$name" "verdict=FAIL" "reason=$why" \
               "checks=0" "failures=0" "run_rc=$run_rc" "transcript="
    return 1
}

verdict_guest() {
    local name="$1" min="$2" run_rc="$3"; shift 3
    local found checks failures summary why=""

    # verdict_find prints the path on stdout and its own key=value block when
    # it fails, so the substitution takes the path and the block goes through
    # only on the failing branch.
    if ! found=$(verdict_find "$name" "$run_rc" \
                              '[0-9]+ checks, [0-9]+ failures' "$@"); then
        printf '%s\n' "$found"
        return 1
    fi

    echo "==> verdict from $found"

    summary=$(verdict_summary "$found")
    if [ -z "$summary" ]; then
        echo "FAIL: $name: $found has no '<N> checks, <M> failures' line, so the" >&2
        echo "      guest never reached its own verdict. The emulator exited" >&2
        echo "      $run_rc; that is not a result." >&2
        tail -20 "$found" | sed 's/^/       /' >&2
        echo "$name: FAILED (no summary)" >&2
        verdict_kv "name=$name" "verdict=FAIL" "reason=no_summary" \
                   "checks=0" "failures=0" "min_checks=$min" \
                   "run_rc=$run_rc" "transcript=$found"
        return 1
    fi

    checks=${summary%% *}
    failures=${summary##* }

    # ORDER MATTERS, AND IT USED TO BE WRONG.  The SKIPPED grep ran FIRST, so a
    # transcript carrying "3 failures" and one SKIPPED line reported SKIPPED
    # and named neither of the three -- red either way, and red pointing at the
    # rig instead of at the assertions that had gone off.  A guest with
    # failures in it is a guest with failures in it, whatever else it skipped.
    #
    # Same for the emulator's own exit status: a run that TIMED OUT is not a
    # skip, it is a partial run whatever the transcript says.
    local bad=0
    if [ "$failures" -ne 0 ]; then
        echo "FAIL: $name: $failures of $checks checks failed" >&2
        grep -n "FAIL" "$found" | head -20 | sed 's/^/       /' >&2
        bad=1
        why=failures
    fi
    # NAME WHAT ACTUALLY WENT WRONG.  4 and 5 are the emulator's, not the
    # guest's -- tools/amiberry-run.sh's header lists them -- and the generic
    # arm used to render every one of them as "the guest exited N".  That
    # sentence over a transcript ending `113 checks, 0 failures, PASS` is what
    # this whole file exists to stop, pointed at the rig instead of the code.
    if [ "$run_rc" != "0" ]; then
        case "$run_rc" in
            124) echo "FAIL: $name: the run TIMED OUT, so the transcript above is a" >&2
                 echo "      partial run whatever it says." >&2
                 [ -n "$why" ] || why=timeout ;;
            4)   echo "FAIL: $name: illegal instruction, the guest is built for the" >&2
                 echo "      wrong CPU." >&2
                 [ -n "$why" ] || why=wrong_cpu ;;
            5)   echo "FAIL: $name: the run did NOT get the network backend it asked" >&2
                 echo "      for. The transcript above is of a guest on some other" >&2
                 echo "      link, whatever it says. This is the RIG, not the code." >&2
                 [ -n "$why" ] || why=wrong_backend ;;
            *)   echo "FAIL: $name: the run exited $run_rc and the transcript reports" >&2
                 echo "      $failures failures. The two disagree, so this is not a" >&2
                 echo "      pass in either direction." >&2
                 [ -n "$why" ] || why=exit_disagrees ;;
        esac
        bad=1
    fi

    if [ "$bad" -ne 0 ]; then
        echo "$name: FAILED" >&2
        verdict_kv "name=$name" "verdict=FAIL" "reason=$why" \
                   "checks=$checks" "failures=$failures" "min_checks=$min" \
                   "run_rc=$run_rc" "transcript=$found"
        return 1
    fi

    # A guest that decided it could not run is a SKIP, and a skip is not a
    # pass: 77 is the status the automake convention gives it, so a caller and
    # CI can record it as one rather than tick it off as work done.
    #
    # Before the check floor, not after: a guest that skipped its own work runs
    # fewer cases BY DEFINITION, and "only 2 checks ran, at least 40 were
    # expected" is the same wrong signpost pointed the other way.
    if grep -q "SKIPPED" "$found"; then
        echo "SKIP: $name: the guest skipped its own work" >&2
        grep -n "SKIPPED" "$found" | head -3 | sed 's/^/       /' >&2
        echo "$name: SKIPPED after $checks checks, 0 failures" >&2
        verdict_kv "name=$name" "verdict=SKIP" "reason=guest_skipped" \
                   "checks=$checks" "failures=0" "min_checks=$min" \
                   "run_rc=$run_rc" "transcript=$found"
        return 77
    fi

    if [ "$checks" -lt "$min" ]; then
        echo "FAIL: $name: only $checks checks ran, at least $min were expected." >&2
        echo "      A run that stops calling its own cases still reports zero" >&2
        echo "      failures; the count is what notices." >&2
        echo "$name: FAILED" >&2
        verdict_kv "name=$name" "verdict=FAIL" "reason=too_few_checks" \
                   "checks=$checks" "failures=0" "min_checks=$min" \
                   "run_rc=$run_rc" "transcript=$found"
        return 1
    fi

    echo "$name: PASSED, $checks checks, 0 failures"
    verdict_kv "name=$name" "verdict=PASS" "reason=ok" \
               "checks=$checks" "failures=0" "min_checks=$min" \
               "run_rc=$run_rc" "transcript=$found"
    return 0
}
