#!/usr/bin/env bash
#
# NO TEST RUNS LONGER THAN TEN SECONDS.
#
# rfb_roundtrip was one ctest case covering 29 captures, 16 strategies, three
# tile sizes and two source layouts.  On the CI runner it was 29.27 s of a
# 32.52 s host run and 93.51 s of a 98.66 s sanitizer run -- 90% and 95% of
# the suite -- while the next slowest of the other 137 tests was 0.70 s.  It
# had grown there a capture at a time, with nothing to notice.
#
# A budget catches the next one while it is still cheap to split.  The limit
# is per TEST, not per suite: a suite gets faster by running its cases in
# parallel, and a case that holds one core for a minute stops that working.
#
# Reads a ctest transcript rather than Testing/Temporary/CTestCostData.txt,
# which is an average over previous runs: on a fresh CI directory that is the
# same number, and on a developer's it hides a regression behind nine old runs.
#
# Usage: check-test-duration.sh <ctest-output-log> [budget-seconds]
#
# SPDX-License-Identifier: MIT

set -u

LOG="${1:-}"
BUDGET="${2:-10}"

if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
    echo "test_duration=FAIL reason=no_log path=${LOG:-<none>}" >&2
    echo "!! check-test-duration.sh needs the ctest transcript.  A gate that" >&2
    echo "!! cannot see what it guards has to fail." >&2
    exit 1
fi

#   6/174 Test  #61: rfb_roundtrip_plane_8x8_4 ......   Passed    8.34 sec
# The name is the field after "#N:"; the seconds are the field before "sec".
# Failed, timed-out and skipped tests carry the same shape and are counted
# too: a test that fails slowly is still a test that runs too long.
#
# awk, not sed.  The first version matched the status word with
# \(Passed\|Failed\|...\), which is a GNU sed extension: BSD sed on the macOS
# runner matched nothing and this failed the build saying it could see nothing.
# That is the right failure for a gate to have, and it was the gate at fault.
# Field positions are the same in both awks.
seen=0
over=0
worst_name=""
worst_secs=0
worst_cs=-1

while read -r name secs; do
    seen=$((seen + 1))
    # Integer compare in shell, to two decimals, so this needs no awk -v
    # round-trip and no locale.
    whole=${secs%%.*}
    if [ "${whole:-0}" -ge "$BUDGET" ]; then
        over=$((over + 1))
        echo "test_duration=OVER test=$name secs=$secs budget=$BUDGET" >&2
    fi
    # Hundredths, so "slowest" is the actual slowest and not whichever test
    # reached the same whole second first.
    frac=${secs#*.}; [ "$frac" = "$secs" ] && frac=0
    frac=${frac}00; frac=${frac:0:2}
    cs=$(( ${whole:-0} * 100 + 10#$frac ))
    if [ "$cs" -gt "$worst_cs" ]; then
        worst_cs=$cs; worst_name="$name"; worst_secs="$secs"
    fi
done < <(awk '
    {
        idx = 0
        for (i = 1; i < NF; i++)
            if ($i ~ /^#[0-9]+:$/) { idx = i; break }
    }
    idx > 0 && $NF == "sec" {
        name = $(idx + 1)
        sub(/\.+$/, "", name)
        if (name != "") print name, $(NF - 1)
    }
' "$LOG")

if [ "$seen" = 0 ]; then
    echo "test_duration=FAIL reason=no_tests_in_log path=$LOG" >&2
    echo "!! No \"Test #N: <name> ... N.NN sec\" lines in $LOG.  Either ctest" >&2
    echo "!! did not run or its output format changed; either way this gate" >&2
    echo "!! checked nothing and says so rather than passing." >&2
    exit 1
fi

if [ "$over" != 0 ]; then
    echo "test_duration=FAIL tests=$seen over=$over budget=${BUDGET}s" >&2
    echo "!! $over test(s) run for $BUDGET s or more.  Split the case rather" >&2
    echo "!! than raising the budget: src/rfb/CMakeLists.txt shards the round" >&2
    echo "!! trip along the axes the sweep already had, so the union of the" >&2
    echo "!! cases is the same work." >&2
    exit 1
fi

echo "test_duration=PASS tests=$seen budget=${BUDGET}s slowest=$worst_name/${worst_secs}s"
