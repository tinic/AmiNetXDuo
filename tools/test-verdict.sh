#!/usr/bin/env bash
#
# The verdict half of the on-Amiga harnesses, in one place.
#
#   . "$ROOT/tools/test-verdict.sh"
#   verdict_guest <name> <min-checks> <run-rc> <transcript> [more transcripts...]
#
# WHY THIS EXISTS
#
# Fourteen harnesses used to end in `exec tools/amiberry-run.sh ...`, so the
# script's exit status was the emulator's, which is the guest's own exit code
# out of DH0:.done.  A guest that opened nothing, ran no checks and returned 0
# was a pass, and so was one whose transcript never arrived.  Those runs bought
# confidence and tested nothing.
#
# A verdict needs evidence of work done, so this asks for three things:
#
#   1. a transcript that exists and is not empty -- named as a fact, and a
#      failure of its own, because "no output" is an infrastructure fault and
#      reads nothing like "the code is wrong"
#   2. the guest's own summary line, `N checks, M failures`, which every test
#      program under tests/ prints last, with M == 0
#   3. N at or above a floor the caller states.  A test that silently stops
#      calling half its cases still prints `12 checks, 0 failures`, and the
#      floor is the only thing that notices.
#
# and then, and only then, the emulator's exit status: 124 is a timeout, 4 is a
# guest built for the wrong CPU, and neither can be read as a result.
#
# SPDX-License-Identifier: MIT

# Where a run's guest output landed, by runner.  The tag is the one the caller
# exported before starting the run.
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
    local t found=""

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
    elif [ "$run_rc" = "4" ]; then
        echo "      The emulator saw an illegal instruction: the guest is built" >&2
        echo "      for a CPU this machine does not have." >&2
    fi
    echo "      This is an infrastructure failure, not a result. The emulator" >&2
    echo "      exited $run_rc and that says nothing about the code." >&2
    echo "$name: FAILED (no transcript)" >&2
    return 1
}

verdict_guest() {
    local name="$1" min="$2" run_rc="$3"; shift 3
    local found checks failures summary

    found=$(verdict_find "$name" "$run_rc" \
                         '[0-9]+ checks, [0-9]+ failures' "$@") || return 1

    echo "==> verdict from $found"

    summary=$(verdict_summary "$found")
    if [ -z "$summary" ]; then
        echo "FAIL: $name: $found has no '<N> checks, <M> failures' line, so the" >&2
        echo "      guest never reached its own verdict. The emulator exited" >&2
        echo "      $run_rc; that is not a result." >&2
        tail -20 "$found" | sed 's/^/       /' >&2
        echo "$name: FAILED (no summary)" >&2
        return 1
    fi

    checks=${summary%% *}
    failures=${summary##* }

    # A guest that decided it could not run is a SKIP, and a skip is not a
    # pass: 77 is the status the automake convention gives it, so a caller and
    # CI can record it as one rather than tick it off as work done.
    if grep -q "SKIPPED" "$found"; then
        echo "SKIP: $name: the guest skipped its own work" >&2
        grep -n "SKIPPED" "$found" | head -3 | sed 's/^/       /' >&2
        echo "$name: SKIPPED after $checks checks" >&2
        return 77
    fi

    local bad=0
    if [ "$failures" -ne 0 ]; then
        echo "FAIL: $name: $failures of $checks checks failed" >&2
        grep -n "FAIL" "$found" | head -20 | sed 's/^/       /' >&2
        bad=1
    fi
    if [ "$checks" -lt "$min" ]; then
        echo "FAIL: $name: only $checks checks ran, at least $min were expected." >&2
        echo "      A run that stops calling its own cases still reports zero" >&2
        echo "      failures; the count is what notices." >&2
        bad=1
    fi
    if [ "$run_rc" != "0" ]; then
        case "$run_rc" in
            124) echo "FAIL: $name: the run TIMED OUT, so the transcript above is a" >&2
                 echo "      partial run whatever it says." >&2 ;;
            4)   echo "FAIL: $name: illegal instruction, the guest is built for the" >&2
                 echo "      wrong CPU." >&2 ;;
            *)   echo "FAIL: $name: the guest exited $run_rc" >&2 ;;
        esac
        bad=1
    fi

    if [ "$bad" -ne 0 ]; then
        echo "$name: FAILED" >&2
        return 1
    fi

    echo "$name: PASSED, $checks checks, 0 failures"
    return 0
}
