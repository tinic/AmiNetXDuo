#!/usr/bin/env bash
#
# CHANGELOG.md carries facts. It is not a place to explain anything.
#
#   tools/check-changelog-prose.sh
#
# The release workflow builds the published notes out of this file, so a
# paragraph written here is a paragraph on the release page. Entries are
# tables or short lines: what changed, and the number.
#
# Two rules, both mechanical:
#   1. No line over MAXLEN characters.
#   2. No narrative connective. A sentence that needs "because" or "so that"
#      is explaining, and the explanation belongs in the commit message.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

FILE=CHANGELOG.md
MAXLEN=${AMINETXDUO_CHANGELOG_MAXLEN:-200}

# Checked only from `## Unreleased` down to the first shipped heading below the
# newest one: history is history and is not rewritten to satisfy a rule added
# after it. Two versions of runway is enough to catch what is being written now.
START=$(grep -n '^## Unreleased' "$FILE" | head -1 | cut -d: -f1)
[ -n "$START" ] || { echo "changelog_prose=error reason=no_unreleased_heading" >&2; exit 1; }
END=$(awk -v s="$START" 'NR>s && /^## /{n++; if(n==3){print NR; exit}}' "$FILE")
[ -n "$END" ] || END=$(wc -l < "$FILE")

BANNED='because|so that|which is|which was|rather than|it used to|the whole of|turns out|the reason|in other words|what happened|this means'

rc=0
long=0
narrative=0

while IFS= read -r entry; do
    n=${entry%%:*}
    line=${entry#*:}
    case "$line" in ''|'#'*) continue ;; esac

    if [ "${#line}" -gt "$MAXLEN" ]; then
        echo "changelog_prose=LONG line=$n chars=${#line} max=$MAXLEN"
        echo "  ${line:0:90}..."
        long=$((long + 1))
        rc=1
    fi

    if printf '%s' "$line" | grep -qiE "$BANNED"; then
        echo "changelog_prose=NARRATIVE line=$n"
        echo "  ${line:0:90}..."
        echo "  A changelog entry states what changed and the number. The"
        echo "  explanation belongs in the commit message."
        narrative=$((narrative + 1))
        rc=1
    fi
done < <(sed -n "${START},${END}p" "$FILE" | grep -n '' |
         awk -v off="$((START - 1))" -F: '{printf "%d:%s\n", $1 + off, substr($0, index($0,":")+1)}')

if [ "$rc" = 0 ]; then
    echo "changelog_prose=PASS range=${START}-${END} maxlen=$MAXLEN"
else
    echo "changelog_prose=FAIL long=$long narrative=$narrative"
fi
exit "$rc"
