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
# THREE STATES, NOT TWO.  An arm either ran and passed, or did not run, or
# failed, and the first two are not the same fact.  A missing variable was
# already the second; an exit code was not.  `-x CODE` used to name a code that
# "counts as a pass", so a harness whose whole point was to say I COULD NOT
# OBSERVE THIS got a green row that read as a measurement -- and `tools/ci.sh
# cards` with no peer, which measures nothing at all, exited 0 and was reported
# as nine cards proved.  `-s CODE` replaces it: the same exit code, and the row
# says the arm did not run.  Both still exit 0 from here.
#
# OUTPUT is key=value plus an exit code, and nothing about the verdict is left
# in prose:
#
#   arm="TLS 1.3 handshake, 68020" status=pass rc=0 wall_s=41
#   arm="Mount an SMB share" status=skipped reason="AMINETXDUO_PEER is not set"
#   arm="Every network card" status=skipped rc=77 wall_s=3 reason="it tested nothing"
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
