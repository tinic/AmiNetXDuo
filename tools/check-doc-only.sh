#!/usr/bin/env bash
#
# A commit that only edits documentation is not work, it is bookkeeping.
#
#   tools/check-doc-only.sh              # the staged change, for the hook
#   tools/check-doc-only.sh --range A..B # every commit in a range
#
# Sixty commits went into 0.26.3 and roughly half of them moved no shipped
# byte: harness rows restated, backlog rows corrected, a changelog rewritten,
# ledger prose adjusted.  Each was defensible on its own and the total was
# churn.  A note worth keeping is worth keeping BESIDE THE CHANGE IT DESCRIBES,
# in the same commit, where a reader finds it with `git log -p` on the code.
#
# So: a commit whose whole diff is documentation is refused.  Put the note in
# the commit that changes the thing it is about.
#
# THE ONE EXEMPTION IS THE RELEASE COMMIT.  `release: 0.26.2` (6fe8d4df) edits
# CHANGELOG.md and nothing else, by design -- it renames the Unreleased
# heading.  Set AMINETXDUO_RELEASE_COMMIT=1 for that, and for nothing else.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

# Documentation, for this purpose: prose a build never reads.  tests/HARNESSES
# is here because it is a table of descriptions; the harnesses themselves are
# code and are not.
is_doc() {
    case "$1" in
        *.md|docs/*|tests/HARNESSES|LICENSE) return 0 ;;
        *) return 1 ;;
    esac
}

verdict() {                       # $1 = label, rest = paths
    local label="$1"; shift
    local total=0 docs=0 p
    for p in "$@"; do
        [ -n "$p" ] || continue
        total=$((total + 1))
        if is_doc "$p"; then docs=$((docs + 1)); fi
    done

    [ "$total" -gt 0 ] || { echo "doc_only=skipped $label reason=no_files"; return 0; }

    if [ "$docs" -eq "$total" ]; then
        echo "doc_only=REFUSED $label files=$total"
        printf '  %s\n' "$@"
        echo "  Every file here is documentation.  Put the note in the commit"
        echo "  that changes what it describes, so `git log -p` on the code"
        echo "  shows both.  A release commit sets AMINETXDUO_RELEASE_COMMIT=1."
        return 1
    fi

    echo "doc_only=ok $label files=$total docs=$docs code=$((total - docs))"
    return 0
}

if [ "${AMINETXDUO_RELEASE_COMMIT:-0}" = 1 ]; then
    echo "doc_only=exempt reason=release_commit"
    exit 0
fi

if [ "${1:-}" = "--range" ]; then
    [ -n "${2:-}" ] || { sed -n '3,6p' "$0" >&2; exit 2; }
    rc=0
    for c in $(git rev-list --no-merges "$2"); do
        verdict "$(git log -1 --format=%h "$c")" \
            $(git show --name-only --format= "$c") || rc=1
    done
    exit "$rc"
fi

verdict staged $(git diff --cached --name-only)
