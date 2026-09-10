#!/usr/bin/env bash
#
# A change to what ships must say what it means, IN THE COMMIT THAT MAKES IT.
#
#   tools/check-changelog-entry.sh            # the staged change, for the hook
#
# 0.26.6 was written by reading 201 commits after the fact.  CHANGELOG.md says
# at the top that entries go under `Unreleased` and nowhere else; two entries
# were there for the whole release.  Reconstructing them found an installer
# still offering micro as "static IP only" three weeks after DHCP went back on,
# because the commit that changed the behaviour changed no user-facing words
# and nothing asked it to.
#
# The rule is not "every commit needs an entry".  It is "every commit that
# touches the shipping surface has DECIDED whether it needs one", and the
# decision is recorded either way: stage CHANGELOG.md, or say why not.
#
#   AMINETXDUO_NO_CHANGELOG="refactor, identical objects" git commit ...
#   AMINETXDUO_RELEASE_COMMIT=1                            (release bookkeeping)
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

if [ "${AMINETXDUO_RELEASE_COMMIT:-}" = "1" ]; then
    echo "changelog_entry=skipped reason=release_commit"
    exit 0
fi

staged=$(git diff --cached --name-only --diff-filter=ACMR || true)
[ -n "$staged" ] || { echo "changelog_entry=skipped reason=nothing_staged"; exit 0; }

# The shipping surface: what a user receives or is told.  Tests, harnesses,
# gates and tools are not on it -- they are how the work is checked, and the
# git log is where they belong.
surface=$(printf '%s\n' "$staged" \
          | grep -E '^(src/|include/|port/|install/|dist/|clients/)' \
          | grep -v '/test/' | grep -v '_test\.' || true)

if [ -z "$surface" ]; then
    echo "changelog_entry=ok reason=nothing_on_the_shipping_surface"
    exit 0
fi

if printf '%s\n' "$staged" | grep -qx 'CHANGELOG.md'; then
    echo "changelog_entry=ok reason=changelog_staged files=$(printf '%s\n' "$surface" | wc -l)"
    exit 0
fi

if [ -n "${AMINETXDUO_NO_CHANGELOG:-}" ]; then
    echo "changelog_entry=ok reason=declared_none note=${AMINETXDUO_NO_CHANGELOG}"
    exit 0
fi

echo "changelog_entry=FAIL files=$(printf '%s\n' "$surface" | wc -l)" >&2
printf '%s\n' "$surface" | sed 's/^/  /' >&2
exit 1
