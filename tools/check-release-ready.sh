#!/usr/bin/env bash
#
# Run what .github/workflows/release.yml runs, before the tag is pushed.
#
#   tools/check-release-ready.sh --list   print the job set, build nothing
#   tools/check-release-ready.sh          run all of it
#   tools/check-release-ready.sh --no-arms   skip the twelve cross arms
#
# WHY THIS EXISTS
#
# The Release workflow was red on v0.25.3 (twice) and v0.25.4 and all three
# archives were built and uploaded by hand instead. Nothing said so: the job
# that packs the archive stayed green, and the job beside it that validates the
# tree -- host, host32 and the analyser -- is the one that failed. Three
# releases shipped off a path nobody could reproduce.
#
# The job set is DERIVED from release.yml rather than repeated here, so a stage
# added to the workflow is a stage this runs. --list is cheap and is asserted
# by the conformance stage, so the derivation cannot go blind without CI
# saying so on the next push.
#
# SPDX-License-Identifier: MIT

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

WF=".github/workflows/release.yml"
[ -r "$WF" ] || { echo "check-release-ready: no $WF" >&2; exit 1; }

MODE=run
ARMS=1
for a in "$@"; do
    case "$a" in
        --list) MODE=list ;;
        --no-arms) ARMS=0 ;;
        *) echo "check-release-ready: unknown argument $a" >&2; exit 1 ;;
    esac
done

# The stage names tools/ci.sh actually defines. Anything else after
# `tools/ci.sh` on a run: line is a flag or a shell word, not a stage.
KNOWN=()
while IFS= read -r stage; do
    KNOWN+=("$stage")
done < <(grep -o '^stage_[a-z0-9_]*' tools/ci.sh |
         sed 's/^stage_//' | sort -u)
is_stage() { local s; for s in "${KNOWN[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

# The workflow is YAML with folded (>-) run: blocks, so a `tools/ci.sh`
# invocation is not always one line. Flatten first and read tokens.
TOK=()
while IFS= read -r token; do
    TOK+=("$token")
done < <(tr '\n' ' ' < "$WF" | tr -s ' ' | tr ' ' '\n')

INVOKE=()          # one entry per tools/ci.sh call: "ENV... -- stage stage"
env_carry=""
for ((i = 0; i < ${#TOK[@]}; i++)); do
    t="${TOK[i]}"
    case "$t" in
        AMINETXDUO_*=*) env_carry="$env_carry ${t//\"/}" ;;
        */ci.sh)
            stages=""
            for ((j = i + 1; j < ${#TOK[@]}; j++)); do
                is_stage "${TOK[j]}" || break
                stages="$stages ${TOK[j]}"
            done
            # A ${{ matrix.* }} invocation is one call per arm; the arm loop
            # below runs those, so it must not also land here unexpanded.
            case "$env_carry" in *'${{'*) stages="" ;; esac
            [ -n "$stages" ] && INVOKE+=("${env_carry# } --${stages}")
            env_carry=""
            ;;
    esac
done

# The cross matrix: the `config:` list under the options job.
ARMLIST=$(tr '\n' ' ' < "$WF" | tr -s ' ' |
          sed -n 's/.*config: \[\([^]]*\)\].*/\1/p' | tr -d ',' )

if [ "${#INVOKE[@]}" -lt 2 ] || [ -z "$ARMLIST" ]; then
    echo "check-release-ready: parsed ${#INVOKE[@]} ci.sh calls and arms" \
         "'$ARMLIST' out of $WF -- the workflow changed shape and this" \
         "script no longer sees its job set" >&2
    exit 1
fi

for inv in "${INVOKE[@]}"; do
    echo "release-job: ${inv%% --*} tools/ci.sh ${inv#*-- }"
done
echo "release-arms: $ARMLIST"
[ "$MODE" = list ] && exit 0

status=0
for inv in "${INVOKE[@]}"; do
    envs="${inv%% --*}"
    stages="${inv#*-- }"
    echo "== ${envs} tools/ci.sh ${stages}"
    # shellcheck disable=SC2086
    if ! env $envs tools/ci.sh $stages; then
        echo "check-release-ready: FAILED: tools/ci.sh $stages" >&2
        status=1
    fi
done

if [ "$ARMS" = 1 ]; then
    for arm in $ARMLIST; do
        echo "== AMINETXDUO_CI_CROSS=$arm tools/ci.sh cross"
        AMINETXDUO_CI_CROSS="$arm" tools/ci.sh cross || {
            echo "check-release-ready: FAILED: cross arm $arm" >&2; status=1; }
    done
fi

[ "$status" = 0 ] && echo "check-release-ready: the release job set is green"
exit "$status"
