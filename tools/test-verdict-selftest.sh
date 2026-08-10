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

n=0; bad=0
case_() { # description expected-rc args...
    local what="$1" want="$2"; shift 2
    local out rc
    out=$(verdict_guest "$@" 2>&1); rc=$?
    n=$((n + 1))
    if [ "$rc" = "$want" ]; then
        printf 'ok   %-34s -> %s\n' "$what" "$rc"
    else
        printf 'FAIL %-34s -> %s, wanted %s\n' "$what" "$rc" "$want"
        bad=$((bad + 1))
    fi
    printf '%s\n' "$out" | sed 's/^/       | /'
}

case_ "a good transcript"        0  selftest 30 0 "$T/good"
case_ "CRLF and no leading char" 0  selftest 30 0 "$T/crlf"
case_ "NO transcript at all"     1  selftest 30 0 "$T/does-not-exist"
case_ "an EMPTY transcript"      1  selftest 30 0 "$T/empty"
case_ "a transcript, no summary" 1  selftest 30 0 "$T/nosummary"
case_ "the guest reported failures" 1 selftest 30 0 "$T/failures"
case_ "fewer checks than the floor" 1 selftest 30 0 "$T/short"
case_ "a good run that TIMED OUT" 1 selftest 30 124 "$T/good"
case_ "a wrong-CPU guest (rc 4)"  1 selftest 30 4 "$T/does-not-exist"
case_ "the guest SKIPPED itself" 77 selftest 1 0 "$T/skipped"

echo
echo "verdict-selftest: $n cases, $bad wrong"
[ "$bad" -eq 0 ]
