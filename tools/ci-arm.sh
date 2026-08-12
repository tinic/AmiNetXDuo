#!/usr/bin/env bash
#
# Run one tier-2 arm, and record that it ran -- or why it did not.
#
#   tools/ci-arm.sh -n "TLS 1.3 handshake, 68020" \
#                   -r AMINETXDUO_A2065 -r AMINETXDUO_TLS13_PEER \
#                   -- tests/tls/run-tls13.sh -b build/ci/default -c 68020
#
# .github/workflows/emulator.yml used to spell the same requirement as a step
# condition, `if: env.AMINETXDUO_A2065 != ''`.  A step that does not run under a
# condition leaves NOTHING: no line in the log, no row in the summary, and the
# job it belongs to still reports success.  Twenty arms were gated that way and
# a run with every variable unset was indistinguishable from a run that tested
# all of them.
#
# So the condition moves in here.  The step always runs, this decides, and
# either way there is a row in the run summary saying which it was.  Same shape
# as tests/tools/run-cardsweep.sh, which prints a status per card rather than
# leaving the untestable ones out of its table.
#
# A skipped arm exits 0: a machine that was never given an SMB share is not a
# machine with an SMB defect.  What it must not do is pass QUIETLY, and the row
# is what stops that.  Whether a whole tier going missing should be a failure is
# a different question and it is answered in the workflow's `gate` job.
#
# OUTPUT is key=value plus an exit code, and nothing about the verdict is left
# in prose:
#
#   arm="TLS 1.3 handshake, 68020" status=pass rc=0 wall_s=41
#   arm="Mount an SMB share" status=skipped reason="AMINETXDUO_PEER is not set"
#
# SPDX-License-Identifier: MIT

set -uo pipefail

NAME=""
declare -a REQUIRED=()
declare -a OKCODES=(0)
NOTE=""

usage() {
    cat >&2 <<'EOF'
usage: tools/ci-arm.sh -n NAME [-r VAR]... [-x CODE]... [-m NOTE] -- CMD [ARG]...

  -n NAME   what this arm is called, in the run summary
  -r VAR    an environment variable that must be non-empty for it to run;
            repeatable, and the FIRST empty one is the reason it skipped
  -x CODE   an additional exit code that counts as a pass, for a harness that
            reports "I could not observe this" separately from "it failed"
  -m NOTE   extra text for the summary row
EOF
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        -n) NAME="$2"; shift 2 ;;
        -r) REQUIRED+=("$2"); shift 2 ;;
        -x) OKCODES+=("$2"); shift 2 ;;
        -m) NOTE="$2"; shift 2 ;;
        --) shift; break ;;
        *)  usage ;;
    esac
done
[ -n "$NAME" ] || usage
[ $# -gt 0 ] || usage

# The run summary, when there is one.  Locally there is not, and everything
# below still prints to stdout -- the point is that the record exists, not that
# GitHub is the only place it can exist.
summary() {
    [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
    printf '%s\n' "$*" >> "$GITHUB_STEP_SUMMARY"
}

# One table for the whole job, opened by whichever arm gets there first.
summary_header() {
    [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
    grep -q '^<!-- ci-arm-table -->$' "$GITHUB_STEP_SUMMARY" 2>/dev/null && return 0
    summary '<!-- ci-arm-table -->'
    summary ''
    summary '## Tier 2 arms'
    summary ''
    summary '| arm | status | detail |'
    summary '|---|---|---|'
}

row() {   # row <status> <detail>
    summary_header
    summary "| $NAME | $1 | $2 |"
}

# Escaping for a markdown table cell: a path with a pipe in it would otherwise
# split the row into two columns.
cell() { printf '%s' "$1" | sed 's/|/\\|/g'; }

if [ "${#REQUIRED[@]}" -gt 0 ]; then
    for var in "${REQUIRED[@]}"; do
        [ -n "${!var:-}" ] && continue
        printf 'arm="%s" status=skipped reason="%s is not set"\n' "$NAME" "$var"
        printf '::warning::%s did NOT run: %s is not set on this runner, so\n' \
               "$NAME" "$var" >&2
        printf '::warning::that part of tier 2 is unverified for this commit.\n' >&2
        row 'SKIPPED' "\`$(cell "$var")\` is not set"
        exit 0
    done
fi

start=$SECONDS
"$@"
rc=$?
wall=$((SECONDS - start))

for ok in "${OKCODES[@]}"; do
    if [ "$rc" = "$ok" ]; then
        printf 'arm="%s" status=pass rc=%s wall_s=%s\n' "$NAME" "$rc" "$wall"
        row 'pass' "${NOTE:+$(cell "$NOTE"), }rc=$rc, ${wall}s"
        exit 0
    fi
done

printf 'arm="%s" status=fail rc=%s wall_s=%s\n' "$NAME" "$rc" "$wall"
row '**FAIL**' "${NOTE:+$(cell "$NOTE"), }rc=$rc, ${wall}s"
exit "$rc"
