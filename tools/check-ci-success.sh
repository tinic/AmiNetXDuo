#!/usr/bin/env bash
# Require a successful CI run for one exact commit that uploaded its release
# candidate.  A release may package that commit; it may not re-run a
# convenient subset and call that the same verdict.
#
# A main push or a workflow_dispatch on it both count, but only a run that
# uploaded release-candidate-<SHA>: a docs-only push builds no candidate, so
# a CHANGELOG-only release commit is released from the full run dispatched
# on the same SHA.  The newest completed run with the artifact is the one.
#
#   tools/check-ci-success.sh SHA [WAIT_SECONDS]
#
# Needs an authenticated gh CLI.  SPDX-License-Identifier: MIT

set -euo pipefail

sha="${1:-}"
wait_seconds="${2:-0}"

if [ "${#sha}" -ne 40 ]; then
    echo "ci_success=FAIL reason=sha_must_be_40_hex value=$sha" >&2
    exit 2
fi
case "$sha" in
    *[!0-9a-fA-F]*) echo "ci_success=FAIL invalid_sha=$sha" >&2; exit 2 ;;
esac
case "$wait_seconds" in *[!0-9]*|'')
    echo "ci_success=FAIL invalid_wait=$wait_seconds" >&2; exit 2 ;; esac

command -v gh >/dev/null 2>&1 || {
    echo "ci_success=FAIL reason=no_gh_cli" >&2
    exit 2
}

deadline=$((SECONDS + wait_seconds))
artifact="release-candidate-$sha"
while :; do
    runs=$(gh run list --workflow CI --commit "$sha" --limit 20 \
        --json databaseId,headSha,status,conclusion,url,event)
    runs=$(printf '%s' "$runs" | jq -c --arg sha "$sha" '
        [.[] | select(.headSha == $sha and
                      (.event == "push" or .event == "workflow_dispatch"))]')

    # One walk, newest first, as gh lists them.  A completed success without
    # the candidate (a docs-only push) is passed over; anything else decides:
    # a run still going is waited for, a failed run is the verdict, and a
    # success holding an unexpired candidate is the one.  No run is stepped
    # over to reach an older candidate except a candidate-less success.
    state=none
    count=$(printf '%s' "$runs" | jq 'length')
    i=0
    while [ "$i" -lt "$count" ]; do
        run=$(printf '%s' "$runs" | jq -c ".[$i]")
        i=$((i + 1))
        if [ "$(printf '%s' "$run" | jq -r .status)" != completed ]; then
            state=active
            break
        fi
        if [ "$(printf '%s' "$run" | jq -r .conclusion)" != success ]; then
            printf '%s\n' "$run" | jq -r '
                "ci_success=FAIL run=\(.databaseId) sha=\(.headSha) conclusion=\(.conclusion) url=\(.url)"' >&2
            exit 1
        fi
        id=$(printf '%s' "$run" | jq -r .databaseId)
        if gh api "repos/{owner}/{repo}/actions/runs/$id/artifacts" 2>/dev/null |
           jq -r '.artifacts[] | select(.expired == false) | .name' |
           grep -qx "$artifact"; then
            printf '%s\n' "$run" | jq -r '
                "ci_success=PASS run=\(.databaseId) sha=\(.headSha) event=\(.event) url=\(.url)"'
            exit 0
        fi
    done

    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "ci_success=FAIL sha=$sha state=$state reason=no_successful_run_with_candidate" >&2
        exit 1
    fi

    echo "ci_success=WAIT sha=$sha state=$state"
    sleep 15
done
