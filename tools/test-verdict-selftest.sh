#!/usr/bin/env bash
#
# Prove tools/test-verdict.sh can fail, one deliberately broken input at a time.
#
#   tools/test-verdict-selftest.sh
#
# The fourteen on-Amiga harnesses all reach their verdict through
# verdict_guest(), so an assertion that quietly stopped firing there would
# stop firing in all of them at once, and every one of those runs needs an
# emulator to notice.  This needs nothing: ten fixtures, ten expected exit
# codes, under a second.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
. "$ROOT/tools/test-verdict.sh"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

printf 'ok a\nok b\n40 checks, 0 failures, PASS\n'        > "$T/good"
printf 'ok a\n40 checks, 0 failures, PASS\r\n'            > "$T/crlf"
: > "$T/empty"
printf 'the guest booted and said nothing useful\n'       > "$T/nosummary"
printf '  FAIL nope\n40 checks, 3 failures, FAIL\n'       > "$T/failures"
printf '4 checks, 0 failures, PASS\n'                     > "$T/short"
printf '2 checks, 0 failures, SKIPPED (no network)\n'     > "$T/skipped"
# Both facts in one transcript.  This is the ordering case: the SKIPPED grep
# used to run before the failure count, so this reported SKIPPED and named
# none of the three assertions that had gone off.
printf '  FAIL nope\n9 checks, 3 failures, SKIPPED (no network)\n' > "$T/skipfail"

n=0; bad=0

# The key=value block is the interface a caller grades a run by, so every case
# asserts BOTH halves: the exit code, and verdict=/reason= on stdout.  A block
# that stopped being printed, or that said PASS beside exit 1, would be exactly
# the disease this file is here to catch -- and the prose is free to change
# without breaking anyone.
case_() { # description expected-rc expected-verdict expected-reason args...
    local what="$1" want="$2" wantv="$3" wantr="$4"; shift 4
    local out rc gotv gotr
    # One call, both streams kept apart: the key=value block is stdout and the
    # diagnostics are stderr, and a block that leaked onto stderr would not be
    # readable by a caller that redirects it.
    out=$(verdict_guest "$@" 2>"$T/err"); rc=$?
    gotv=$(printf '%s\n' "$out" | sed -n 's/^verdict=//p' | tail -1)
    gotr=$(printf '%s\n' "$out" | sed -n 's/^reason=//p'  | tail -1)
    out="$out
$(cat "$T/err")"
    n=$((n + 1))
    if [ "$rc" = "$want" ] && [ "$gotv" = "$wantv" ] && [ "$gotr" = "$wantr" ]; then
        printf 'ok   %-34s -> %s %s/%s\n' "$what" "$rc" "$gotv" "$gotr"
    else
        printf 'FAIL %-34s -> %s %s/%s, wanted %s %s/%s\n' \
               "$what" "$rc" "$gotv" "$gotr" "$want" "$wantv" "$wantr"
        bad=$((bad + 1))
    fi
    printf '%s\n' "$out" | sed 's/^/       | /'
}

case_ "a good transcript"        0 PASS ok             selftest 30 0 "$T/good"
case_ "CRLF and no leading char" 0 PASS ok             selftest 30 0 "$T/crlf"
case_ "NO transcript at all"     1 FAIL no_transcript  selftest 30 0 "$T/does-not-exist"
case_ "an EMPTY transcript"      1 FAIL no_transcript  selftest 30 0 "$T/empty"
case_ "a transcript, no summary" 1 FAIL no_summary     selftest 30 0 "$T/nosummary"
case_ "the guest reported failures" 1 FAIL failures    selftest 30 0 "$T/failures"
case_ "fewer checks than the floor" 1 FAIL too_few_checks selftest 30 0 "$T/short"
case_ "a good run that TIMED OUT" 1 FAIL timeout       selftest 30 124 "$T/good"
case_ "a wrong-CPU guest (rc 4)"  1 FAIL wrong_cpu     selftest 30 4 "$T/does-not-exist"
# rc 5 is tools/amiberry-run.sh's "did not get the backend it asked for".  It
# used to be reported as rc 1, and verdict_guest rendered that as "the guest
# exited 1" over a transcript ending `113 checks, 0 failures, PASS`.
case_ "a good run on the WRONG backend" 1 FAIL wrong_backend selftest 30 5 "$T/good"
case_ "no transcript, wrong backend"    1 FAIL wrong_backend selftest 30 5 "$T/does-not-exist"
# A guest that returned nonzero while its own counters say nothing failed.
case_ "exit code contradicts the count" 1 FAIL exit_disagrees selftest 30 20 "$T/good"
case_ "the guest SKIPPED itself" 77 SKIP guest_skipped selftest 1 0 "$T/skipped"
case_ "SKIPPED *and* 3 failures"  1 FAIL failures      selftest 1 0 "$T/skipfail"
case_ "SKIPPED and a TIMEOUT"     1 FAIL timeout       selftest 1 124 "$T/skipped"
case_ "SKIPPED under the floor"  77 SKIP guest_skipped selftest 40 0 "$T/skipped"

echo
echo "verdict-selftest: $n cases, $bad wrong"
[ "$bad" -eq 0 ]
