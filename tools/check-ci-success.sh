#!/usr/bin/env bash
# Require a successful main-push CI run for one exact commit.  A release may
# package that commit; it may not re-run a convenient subset and call that the
# same verdict.
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
while :; do
    runs=$(gh run list --workflow CI --commit "$sha" --event push --limit 20 \
        --json databaseId,headSha,status,conclusion,url)

    latest=$(printf '%s' "$runs" | jq -c --arg sha "$sha" '
        [.[] | select(.headSha == $sha)][0] // empty')
    if [ -n "$latest" ] &&
       [ "$(printf '%s' "$latest" | jq -r .status)" = completed ]; then
        conclusion=$(printf '%s' "$latest" | jq -r .conclusion)
        if [ "$conclusion" != success ]; then
            printf '%s\n' "$latest" | jq -r '
                "ci_success=FAIL run=\(.databaseId) sha=\(.headSha) conclusion=\(.conclusion) url=\(.url)"' >&2
            exit 1
        fi
        printf '%s\n' "$latest" | jq -r '
            "ci_success=PASS run=\(.databaseId) sha=\(.headSha) url=\(.url)"'
        exit 0
    fi

    active=0
    [ -z "$latest" ] || active=1

    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "ci_success=FAIL sha=$sha active=$active reason=no_successful_latest_run" >&2
        exit 1
    fi

    echo "ci_success=WAIT sha=$sha active=$active"
    sleep 15
done
