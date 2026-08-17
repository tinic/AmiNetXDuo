#!/usr/bin/env bash
#
# Prove tests/conformance/tap-verdict.sh can fail.
#
#   tests/conformance/tap-verdict-selftest.sh
#
# The conformance suite runs on one self-hosted runner, needs Commodore's
# a2065.device and takes ten minutes, and for as long as it has existed it
# could not report a failure: run-fsuae.sh exited with the guest's status and
# the guest returned RETURN_OK unconditionally.  Something has to be able to
# tell the new scorer apart from the old one without a ROM.  These fixtures
# are the literal output of third_party/bsdsocktest/src/tap.c.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
. "$ROOT/tests/conformance/tap-verdict.sh"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

hdr='# bsdsocktest 1.7
# bsdsocket.library: AmiNetXDuo 0.21.0'

{ echo "$hdr"; echo "1..4"
  echo "ok 1 - socket(AF_INET, SOCK_STREAM)"
  echo "ok 2 - bind() to a free port"
  echo "ok 3 - getsockname() agrees"
  echo "ok 4 - CloseSocket()"
  echo "# Results: 4 passed, 0 failed, 0 known, 0 skipped (4 total)"
} > "$T/clean"

# The shape the old harness graded PASS: real conformance failures, and the
# launcher returned RETURN_OK regardless.
{ echo "$hdr"; echo "1..4"
  echo "ok 1 - socket(AF_INET, SOCK_STREAM)"
  echo "not ok 2 - bind() to a free port"
  echo "not ok 3 - getsockname() agrees"
  echo "ok 4 - CloseSocket()"
  echo "# Results: 2 passed, 2 failed, 0 known, 0 skipped (4 total)"
} > "$T/failures"

# Expected failures against the recorded limitations are not regressions.
{ echo "$hdr"; echo "1..3"
  echo "ok 1 - socket(AF_INET, SOCK_STREAM)"
  echo "not ok 2 - SO_LINGER  # KNOWN AmiNetXDuo: not implemented"
  echo "ok 3 - # SKIP no IPv6 on this build"
  echo "# Results: 1 passed, 0 failed, 1 known, 1 skipped (3 total)"
} > "$T/known"

# A known limitation that started passing is still a pass.
{ echo "$hdr"; echo "1..2"
  echo "ok 1 - SO_LINGER  # KNOWN AmiNetXDuo: not implemented"
  echo "ok 2 - CloseSocket()"
  echo "# Results: 2 passed, 0 failed, 0 known, 0 skipped (2 total)"
} > "$T/unexpected_pass"

# Stopped part way: no `not ok` at all, and a pass under any scorer that only
# counts failures.
{ echo "$hdr"; echo "1..40"
  echo "ok 1 - socket(AF_INET, SOCK_STREAM)"
  echo "ok 2 - bind() to a free port"
} > "$T/short"

{ echo "$hdr"; echo "1..40"
  echo "ok 1 - socket(AF_INET, SOCK_STREAM)"
  echo "Bail out! the library went away"
} > "$T/bail"

# No plan: nothing says how much was meant to run.
{ echo "$hdr"; echo "ok 1 - socket(AF_INET, SOCK_STREAM)"; } > "$T/noplan"

: > "$T/empty"

n=0; bad=0
# BOTH HALVES.  This used to grade the exit code alone, and tap_verdict's
# documented interface is `conformance=` on stdout: the two could drift apart
# -- a run reporting conformance=PASS beside exit 1 -- and nothing would say
# so.  tools/ci.sh and .github/workflows/emulator.yml read the field.
case_() { # description expected-rc expected-conformance log [emulator-status]
    local what="$1" want="$2" wantc="$3" log="$4" st="${5:-0}"
    local out rc gotc
    out=$(tap_verdict "$log" "$st" 2>"$T/err"); rc=$?
    gotc=$(printf '%s\n' "$out" | sed -n 's/^conformance=//p' | tail -1)
    out="$out
$(cat "$T/err")"
    n=$((n + 1))
    if [ "$rc" = "$want" ] && [ "$gotc" = "$wantc" ]; then
        printf 'ok   %-40s -> %s %s\n' "$what" "$rc" "$gotc"
    else
        printf 'FAIL %-40s -> %s %s, wanted %s %s\n' \
               "$what" "$rc" "$gotc" "$want" "$wantc"
        bad=$((bad + 1))
    fi
    printf '%s\n' "$out" | sed 's/^/       | /'
}

case_ "a clean run"                        0 PASS         "$T/clean"
case_ "two conformance failures"           1 FAIL         "$T/failures"
case_ "known failure and a skip"           0 PASS         "$T/known"
case_ "a known limitation that passed"     0 PASS         "$T/unexpected_pass"
case_ "stopped part way, no not-ok at all" 1 FAIL         "$T/short"
case_ "the suite bailed out"               1 FAIL         "$T/bail"
case_ "no plan line"                       3 NOT_MEASURED "$T/noplan"
case_ "an empty log"                       3 NOT_MEASURED "$T/empty"
case_ "clean, but the emulator timed out"  1 FAIL         "$T/clean" 124
case_ "clean, but the guest was wrong-CPU" 1 FAIL         "$T/clean" 4

# What the superseded harness graded, for the record: it exited with the
# emulator's status, and conf_launcher.c:134 makes that 0 whatever happened.
echo
echo "-- what the superseded verdict graded --"
for f in clean failures short bail; do
    printf '   %-16s old_exit=0 (conf_launcher.c:134 returns RETURN_OK)\n' "$f"
done

echo
echo "tap-verdict-selftest: $n cases, $bad wrong"
[ "$bad" -eq 0 ]
