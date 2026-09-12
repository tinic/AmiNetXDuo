#!/usr/bin/env bash
#
# A hash of the tracked CONTENT of this working tree.
#
#   tools/tree-stamp.sh
#
# tools/ci.sh writes it after the host stage passes and .githooks/pre-push
# compares it, which is how a push of unverified code is refused.  ONE script
# for both, because two copies of this would drift and the failure mode is a
# hook that refuses everything or a stamp nobody can match.
#
# CONTENT, not the commit id: running the host stage and then committing that
# same content has to still count, or the rule would be about the order things
# were done in rather than about what was checked.
#
# SUBMODULE GITLINKS ARE NOT FILES.  `git ls-files' lists third_party/netxduo
# and the three beside it as paths, sha256sum refuses a directory, and under
# tools/ci.sh's `set -o pipefail' that failed the whole pipeline -- so the
# stamp was never written and the hook refused a push of a tree that HAD
# passed.  The gitlink commit ids go in separately, so bumping a submodule
# still changes the stamp.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

{
    # TRACKED **AND** UNTRACKED-BUT-NOT-IGNORED.  `git ls-files' alone lists
    # only what is already tracked, so a gate script added in this session was
    # invisible to the stamp until it was committed: the host stage verified a
    # tree the stamp did not describe, and the hash moved at `git add' time for
    # no change in content.  --others --exclude-standard adds exactly the files
    # a commit would pick up, and nothing that .gitignore covers.
    #
    # -f: the regular files.  Drops the gitlinks, and drops a symlink whose
    # target is missing rather than failing on it.
    git ls-files -z --cached --others --exclude-standard |
    while IFS= read -r -d '' f; do
        [ -f "$f" ] && printf '%s\0' "$f"
    done | sort -z | xargs -0 sha256sum

    # ...and the submodules, by the commit each is pinned at.
    git ls-files -s | awk '$1 == "160000" { print $2, $4 }'
} | sha256sum | cut -d' ' -f1
