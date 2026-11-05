#!/usr/bin/env bash
# Require a successful CI run for one exact commit that uploaded its release
# candidate.  A release may package that commit; it may not re-run a
# convenient subset and call that the same verdict.
#
# A main push, full manual dispatch or weekly full run counts only with BOTH
# release-candidate-<SHA> and ci-full-<SHA>. Shipping-only and reused-matrix
# success is not full pre-release coverage. Dispatch CI on the exact release
# SHA if it does not already have a full run (including prose-only releases).
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

# A run whose only non-success job is the hosted macOS/clang job that GitHub
# never gave a runner (cancelled with no runner and no steps), while the
# tier 1 report passed, is a success: the gate already counted it as not run.
macos_unacquired_only() {        # run-id
    gh api "repos/{owner}/{repo}/actions/runs/$1/jobs?per_page=100" 2>/dev/null |
    jq -e '
      ([.jobs[] | select(.name == "tier 1 report") | .conclusion] == ["success"]) and
      ([.jobs[] | select(.conclusion != "success" and .conclusion != "skipped")] as $bad |
        ($bad | length) > 0 and
        ($bad | all(.name == "host tests (macOS / clang)" and
                    .conclusion == "cancelled" and
                    (.runner_name // "") == "" and
                    ((.steps // []) | length) == 0)))' >/dev/null
}

deadline=$((SECONDS + wait_seconds))
artifact="release-candidate-$sha"
proof="ci-full-$sha"
while :; do
    runs=$(gh run list --workflow CI --commit "$sha" --limit 20 \
        --json databaseId,headSha,status,conclusion,url,event)
    runs=$(printf '%s' "$runs" | jq -c --arg sha "$sha" '
        [.[] | select(.headSha == $sha and
                      (.event == "push" or .event == "workflow_dispatch" or .event == "schedule"))]')

    # One walk, newest first, as gh lists them.  A completed success without
    # full proof/candidate (docs, shipping or reuse) is passed over; otherwise:
    # a run still going is waited for, a failed run is the verdict, and a
    # success holding both unexpired artifacts is the one. No failed or
    # pending run is stepped over to reach an older full success.
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
        id=$(printf '%s' "$run" | jq -r .databaseId)
        if [ "$(printf '%s' "$run" | jq -r .conclusion)" != success ] &&
           ! macos_unacquired_only "$id"; then
            printf '%s\n' "$run" | jq -r '
                "ci_success=FAIL run=\(.databaseId) sha=\(.headSha) conclusion=\(.conclusion) url=\(.url)"' >&2
            exit 1
        fi
        if gh api "repos/{owner}/{repo}/actions/runs/$id/artifacts" 2>/dev/null |
           jq -e --arg candidate "$artifact" --arg proof "$proof" '
             [.artifacts[] | select(.expired == false) | .name] as $names |
             ($names | index($candidate)) != null and ($names | index($proof)) != null' >/dev/null; then
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
