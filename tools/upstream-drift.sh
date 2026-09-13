#!/usr/bin/env bash
#
# How far the vendored forks have drifted from eclipse-threadx.
#
#   tools/upstream-drift.sh            # the numbers docs/UPSTREAMING.md quotes
#   tools/upstream-drift.sh --fetch    # add/refresh the upstream remotes first
#
# docs/UPSTREAMING.md is a survey, and a survey goes stale the moment either
# side moves.  This is how its numbers were produced, so the next person can
# reproduce them in one command instead of rediscovering the incantation --
# including the part that is easy to get wrong: UPSTREAM'S WORK IS ON `dev',
# NOT `master'.  Both forks already contain master's tip (2026-06-30, a
# SECURITY.md edit), so a comparison against master reports no drift at all
# and no work to do, which is false.
#
# .gitmodules tracks `master' on our own forks, so `git submodule update
# --remote' never sees any of this either.
#
# key=value on stdout, like every other tool here.  Exit 2 when the upstream
# remote is absent and --fetch was not asked for: a number this cannot compute
# is not reported as zero.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

FETCH=0
case "${1:-}" in
    --fetch) FETCH=1 ;;
    "")      ;;
    *) echo "usage: $0 [--fetch]" >&2; exit 2 ;;
esac

# path:upstream URL.  bsdsocktest and dropbear are tracked at a tag and have no
# fork of ours, so there is nothing to compare.
MODULES=(
    "third_party/threadx:https://github.com/eclipse-threadx/threadx.git"
    "third_party/netxduo:https://github.com/eclipse-threadx/netxduo.git"
)

rc=0
missing=0

for entry in "${MODULES[@]}"; do
    path="${entry%%:*}"
    url="${entry#*:}"
    name="${path##*/}"

    if [ ! -d "$path/.git" ] && [ ! -f "$path/.git" ]; then
        echo "upstream_drift_$name=skipped reason=not_checked_out"
        continue
    fi

    if [ "$FETCH" = 1 ]; then
        git -C "$path" remote get-url upstream >/dev/null 2>&1 ||
            git -C "$path" remote add upstream "$url"
        git -C "$path" remote set-url upstream "$url"
        git -C "$path" fetch -q upstream 2>/dev/null || {
            echo "upstream_drift_$name=skipped reason=fetch_failed"
            rc=2
            continue
        }
    fi

    if ! git -C "$path" rev-parse --verify -q upstream/dev >/dev/null 2>&1; then
        echo "upstream_drift_$name=skipped reason=no_upstream_remote" \
             "hint=$0 --fetch"
        missing=1
        continue
    fi

    head=$(git -C "$path" rev-parse --short HEAD)
    up=$(git -C "$path" rev-parse --short upstream/dev)

    # Ours that upstream has not got, and upstream's we have not got.  Both
    # matter: the second is the merge that is owed, the first is the survey.
    ours=$(git -C "$path" rev-list --count --no-merges upstream/dev..HEAD)
    theirs=$(git -C "$path" rev-list --count --no-merges HEAD..upstream/dev)

    stat=$(git -C "$path" diff --shortstat upstream/dev..HEAD)
    files=$(echo "$stat" | grep -o '[0-9]* file' | grep -o '[0-9]*')
    ins=$(echo "$stat" | grep -o '[0-9]* insertion' | grep -o '[0-9]*')
    del=$(echo "$stat" | grep -o '[0-9]* deletion' | grep -o '[0-9]*')

    echo "upstream_drift_$name=ok head=$head upstream_dev=$up" \
         "ours=$ours theirs=$theirs" \
         "files=${files:-0} added=${ins:-0} removed=${del:-0}"
done

if [ "$missing" = 1 ]; then
    echo "upstream_drift=incomplete reason=no_upstream_remote"
    exit 2
fi

[ "$rc" = 0 ] && echo "upstream_drift=ok modules=${#MODULES[@]}"
exit "$rc"
