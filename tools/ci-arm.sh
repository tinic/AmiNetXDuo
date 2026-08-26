#!/usr/bin/env bash
#
# Run one tier-2 arm, and record that it ran -- or why it did not.
#
#   tools/ci-arm.sh -n "TLS 1.3 handshake, 68020" \
#                   -r AMINETXDUO_A2065 -r AMINETXDUO_TLS13_PEER \
#                   -- tests/tls/run-tls13.sh -b build/ci/default -c 68020
#
# THREE STATES, NOT TWO: an arm ran and passed, or did not run, or failed.
# `-s CODE` names an exit code that means the arm did not run; a skipped arm
# still exits 0 from here, and the row is what stops it passing quietly.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

NAME=""
declare -a REQUIRED=()
declare -a SKIPCODES=()
NOTE=""

usage() {
    cat >&2 <<'EOF'
usage: tools/ci-arm.sh -n NAME [-r VAR]... [-s CODE]... [-m NOTE] -- CMD [ARG]...

  -n NAME   what this arm is called, in the run summary
  -r VAR    an environment variable that must be non-empty for it to run;
            repeatable, and the FIRST empty one is the reason it skipped
  -s CODE   an exit code that means the command DID NOT TEST what this arm
            names.  The row says SKIPPED and this exits 0; it is not a pass
  -m NOTE   extra text for the summary row
EOF
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        -n) NAME="$2"; shift 2 ;;
        -r) REQUIRED+=("$2"); shift 2 ;;
        -s) SKIPCODES+=("$2"); shift 2 ;;
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
        # Both on stdout: GitHub reads workflow commands off the step's own
        # output, and an annotation is the half of this a person sees without
        # opening the log.
        printf 'arm="%s" status=skipped reason="%s is not set"\n' "$NAME" "$var"
        printf '::warning::%s did NOT run: %s is not set on this runner, so' \
               "$NAME" "$var"
        printf ' that part of tier 2 is unverified for this commit.\n'
        row 'SKIPPED' "\`$(cell "$var")\` is not set"
        exit 0
    done
fi

start=$SECONDS
"$@"
rc=$?
wall=$((SECONDS - start))

# 77 is a skip code here whether or not the arm asked for one.  It is what the
# rest of the tree already means by "I reached my own verdict and there was
# nothing in it": tools/test-verdict.sh returns it for a guest that skipped its
# own work, tools/ci.sh returns it for a run where every stage skipped, and the
# four harnesses that skip their off-box assertion return it too.  A DEFAULT
# rather than a flag, because the failure this replaces was an arm that nobody
# remembered to annotate.
for skipcode in 77 "${SKIPCODES[@]+"${SKIPCODES[@]}"}"; do
    [ "$rc" = "$skipcode" ] || continue
    printf 'arm="%s" status=skipped rc=%s wall_s=%s reason="it tested nothing"\n' \
           "$NAME" "$rc" "$wall"
    printf '::warning::%s did NOT run: it exited %s, which this arm declares' \
           "$NAME" "$rc"
    printf ' as "did not test", so that part of tier 2 is unverified for this'
    printf ' commit.\n'
    row 'SKIPPED' "${NOTE:+$(cell "$NOTE"), }it tested nothing, rc=$rc, ${wall}s"
    exit 0
done

if [ "$rc" = 0 ]; then
    printf 'arm="%s" status=pass rc=%s wall_s=%s\n' "$NAME" "$rc" "$wall"
    row 'pass' "${NOTE:+$(cell "$NOTE"), }rc=$rc, ${wall}s"
    exit 0
fi

printf 'arm="%s" status=fail rc=%s wall_s=%s\n' "$NAME" "$rc" "$wall"
row '**FAIL**' "${NOTE:+$(cell "$NOTE"), }rc=$rc, ${wall}s"
exit "$rc"
