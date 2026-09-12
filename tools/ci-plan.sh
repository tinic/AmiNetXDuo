#!/usr/bin/env bash
# Classify a Git range for CI.  A documentation-only change still gets its
# own gates, but it cannot affect a compiled image and must not consume the
# cross-build matrix.
#
#   tools/ci-plan.sh BASE HEAD
#
# Output is suitable for appending to $GITHUB_OUTPUT.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

base="${1:-}"
head="${2:-HEAD}"
zero=0000000000000000000000000000000000000000

mode=full
reason=unbounded_range
files=()

if [ -n "$base" ] && [ "$base" != "$zero" ] &&
   git cat-file -e "$base^{commit}" 2>/dev/null &&
   git cat-file -e "$head^{commit}" 2>/dev/null; then
    while IFS= read -r -d '' path; do files+=("$path"); done \
        < <(git diff --name-only -z "$base" "$head")

    if [ "${#files[@]}" -eq 0 ]; then
        mode=docs
        reason=no_changed_files
    else
        mode=docs
        reason=documentation_only
        for path in "${files[@]}"; do
            case "$path" in
                # These are generated inputs/outputs, not prose merely because
                # they live under docs/.  The survey gates must see them.
                docs/aminet-survey/*.tsv) mode=full; reason=derived_data; break ;;
                *.md|*.guide|*.info|docs/*|tests/HARNESSES|LICENSE) ;;
                *) mode=full; reason=build_input; break ;;
            esac
        done
    fi
fi

printf 'mode=%s\n' "$mode"
printf 'reason=%s\n' "$reason"
printf 'changed=%s\n' "${#files[@]}"
