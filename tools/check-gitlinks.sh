#!/usr/bin/env bash
#
# Every recorded submodule commit must be an object a fresh clone can fetch.
#
#   tools/check-gitlinks.sh [commit]
#
# Nothing validates a gitlink at the moment it is written, so five things can be
# wrong and all five are checked: the working tree is stale (`git submodule
# status` prints `+`); no such object; the object is reachable from no ref, so a
# fresh clone cannot fetch it; the pin is not on the first-parent chain of the
# branch .gitmodules names nor on a tag (asked only of a submodule with history
# to answer with); and the bump went backwards, so that pin silently reverts
# every fix merged in between.
#
# Existence and reachability are also swept over every gitlink ever recorded,
# because a pin can rot after it is written: deleting a topic branch on the
# remote takes its object with it.
# Output is key=value; exit 0 clean, 1 a pin is wrong, 2 nothing to check.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

COMMIT="${1:-HEAD}"

# Pins that are already published and cannot be repaired without rewriting
# tags v0.25.0, v0.25.1 and v0.25.2.  Each line is
#   <path> <recorded oid> <what to check out instead> <why>
# so a bisect landing on one of them is told where to go.  Without this the
# whole symptom is git refusing an object id that appears nowhere, in a
# submodule nobody has touched.
KNOWN_BAD='
third_party/netxduo a5366b1d47d5f00b7b35e7905ffad4d9cb32448c a5366b1d73b91c30a23f59e8ced005c7281dc54d fabricated_tail_of_the_autoip_merge
third_party/netxduo 9e5d3226230924ba0f4525db08d45c38622a5f2e 6b586f936cf8df160bd490f1029b833230e43ad8 topic_tip_revised_before_it_was_merged
'

known_bad_use()    { printf '%s\n' "$KNOWN_BAD" | awk -v p="$1" -v o="$2" '$1==p && $2==o {print $3}'; }
known_bad_reason() { printf '%s\n' "$KNOWN_BAD" | awk -v p="$1" -v o="$2" '$1==p && $2==o {print $4}'; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# .gitmodules as of the commit under test, so this works under bisect.
if ! git show "$COMMIT:.gitmodules" > "$WORK/gitmodules" 2>/dev/null; then
    echo "gitlinks=skipped reason=no_gitmodules commit=$COMMIT"
    exit 2
fi

# Where a submodule's objects live, whether or not it is checked out.  Prints
# nothing and still succeeds when there is nowhere: a clone made without
# --recursive has no objects to check and is not a failure.
sub_gitdir() {
    if [ -e "$1/.git" ] && git -C "$1" rev-parse --absolute-git-dir 2>/dev/null; then
        return 0
    fi
    d="$ROOT/$(git rev-parse --git-path "modules/$2")"
    [ -d "$d" ] && printf '%s\n' "$d"
    return 0
}

# One rev-list per submodule, not one per pin: everything a fresh clone would
# be able to fetch, which is what "reachable" has to mean here.
reachable_file() {
    f="$WORK/reach.$(printf '%s' "$1" | tr / _)"
    [ -f "$f" ] || git --git-dir="$1" rev-list --remotes --tags 2>/dev/null | sort > "$f"
    printf '%s\n' "$f"
}

rc=0
checked=0
uninitialised=0

# ------------------------------------------------ the pins at $COMMIT -------

names="$(git config -f "$WORK/gitmodules" --name-only --get-regexp '^submodule\..*\.path$' |
         sed 's/^submodule\.//; s/\.path$//')"

for name in $names; do
    path="$(git config -f "$WORK/gitmodules" "submodule.$name.path")"
    branch="$(git config -f "$WORK/gitmodules" "submodule.$name.branch" || true)"
    oid="$(git ls-tree "$COMMIT" "$path" | awk '$2 == "commit" { print $3 }')"
    [ -n "$oid" ] || continue

    printf '%s %s\n' "$path" "$name" >> "$WORK/names"

    gitdir="$(sub_gitdir "$path" "$name")"
    if [ -z "$gitdir" ]; then
        uninitialised=$((uninitialised + 1))
        continue
    fi
    checked=$((checked + 1))

    if ! git --git-dir="$gitdir" cat-file -e "$oid^{commit}" 2>/dev/null; then
        use="$(known_bad_use "$path" "$oid")"
        echo "gitlink_$path=NO_SUCH_OBJECT recorded=$oid${use:+ use=$use reason=$(known_bad_reason "$path" "$oid")}"
        rc=1
        continue
    fi

    shallow="$(git --git-dir="$gitdir" rev-parse --is-shallow-repository)"

    if [ "$shallow" = "false" ] &&
       ! grep -qx "$oid" "$(reachable_file "$gitdir")"; then
        use="$(known_bad_use "$path" "$oid")"
        echo "gitlink_$path=UNREACHABLE recorded=$oid${use:+ use=$use}"
        echo "  no ref reaches it, so a fresh clone will not have it" >&2
        rc=1
        continue
    fi

    ref=""
    if [ -n "$branch" ] &&
       git --git-dir="$gitdir" rev-parse --verify -q "refs/remotes/origin/$branch" > /dev/null; then
        ref="refs/remotes/origin/$branch"
    elif git --git-dir="$gitdir" symbolic-ref -q refs/remotes/origin/HEAD > /dev/null; then
        ref="$(git --git-dir="$gitdir" symbolic-ref refs/remotes/origin/HEAD)"
    fi

    tag="$(git --git-dir="$gitdir" tag --points-at "$oid" | head -1)"
    if [ -n "$tag" ]; then
        echo "gitlink_$path=ok pin=tag:$tag oid=$oid"
    elif [ -z "$ref" ]; then
        echo "gitlink_$path=ok pin=no_tracking_ref oid=$oid"
    elif [ "$shallow" = "true" ]; then
        # A SHALLOW SUBMODULE HAS NO CHAIN TO BE ON.  actions/checkout takes
        # every submodule with `--depth=1 --no-tags`, which leaves one commit
        # of history and no tag objects at all: `rev-list --first-parent
        # origin/<branch>` then answers with the branch tip alone, and any pin
        # that is not the tip -- which is every pin the moment upstream moves
        # on -- reads as a topic tip that was never on the branch.  It failed
        # exactly that way from the day this gate landed: third_party/dropbear
        # is DROPBEAR_2026.94, one commit behind mkj/dropbear's main and
        # carrying the tag that would have answered the question, and every
        # job in every workflow died on it before compiling a line.
        #
        # The history is not there to be checked, so this says so rather than
        # convicting on it.  A full clone -- a developer's, and the sweep
        # below -- still does the real check.
        echo "gitlink_$path=ok pin=shallow_unchecked oid=$oid"
    elif git --git-dir="$gitdir" rev-list --first-parent "$ref" | grep -qx "$oid"; then
        echo "gitlink_$path=ok pin=${ref#refs/remotes/} oid=$oid"
    else
        echo "gitlink_$path=TOPIC_TIP recorded=$oid ref=${ref#refs/remotes/}"
        echo "  not on the first-parent chain of ${ref#refs/remotes/}; pin the merge that landed it" >&2
        rc=1
    fi

    # 4. A bump goes forwards.  `1d8b8a15` moved netxduo from a merge back to a
    #    topic tip that was not a descendant of it, which silently reverted
    #    every fork fix merged in between, and nothing said so: both ids are
    #    real objects and both check out.  The only thing that distinguishes a
    #    bump from a revert is ancestry.
    was="$(git ls-tree "$COMMIT^" "$path" 2>/dev/null | awk '$2 == "commit" { print $3 }')"
    if [ -n "$was" ] && [ "$was" != "$oid" ] &&
       git --git-dir="$gitdir" cat-file -e "$was^{commit}" 2>/dev/null &&
       ! git --git-dir="$gitdir" merge-base --is-ancestor "$was" "$oid"; then
        echo "gitlink_$path=MOVED_BACK from=$was to=$oid"
        echo "  the new pin is not a descendant of the old one: this reverts\
 whatever landed in between" >&2
        rc=1
    fi
done

if [ "$checked" = 0 ]; then
    echo "gitlinks=skipped reason=no_submodule_initialised commit=$COMMIT"
    exit 2
fi
[ "$uninitialised" = 0 ] || echo "gitlinks_uninitialised=$uninitialised"

# ------------------------------------------------ the working tree ----------

if [ "$COMMIT" = HEAD ]; then
    git submodule status --recursive > "$WORK/status" || true
    if grep -q '^[+U]' "$WORK/status"; then
        while read -r sha path _; do
            echo "worktree_$path=STALE checked_out=${sha#?}"
        done < <(grep '^[+U]' "$WORK/status")
        echo "gitlinks_worktree=FAILED"
        echo "  run: git submodule update --init --recursive" >&2
        rc=1
    else
        echo "gitlinks_worktree=clean entries=$(wc -l < "$WORK/status" | tr -d ' ')"
    fi
fi

# ------------------------------------------------ every pin ever ------------

if [ "$COMMIT" = HEAD ]; then
    oids=0; unfetchable=0; tolerated=0
    git log --full-history -m --format='C %h' --raw --no-abbrev HEAD -- third_party |
        awk -F'\t' '
            /^C / { c = $0; sub(/^C /, "", c); next }
            /^:/  { split($1, a, " "); if (a[2] == "160000") print $2, a[4], c }
        ' | sort -u > "$WORK/history_full"
    awk '{ print $1, $2 }' "$WORK/history_full" | sort -u > "$WORK/history"

    while read -r path oid; do
        name="$(awk -v p="$path" '$1 == p { print $2 }' "$WORK/names" 2>/dev/null | head -1)"
        [ -n "$name" ] || continue          # a submodule the tree no longer has
        gitdir="$(sub_gitdir "$path" "$name")"
        [ -n "$gitdir" ] || continue
        [ "$(git --git-dir="$gitdir" rev-parse --is-shallow-repository)" = "false" ] || continue

        oids=$((oids + 1))
        grep -qx "$oid" "$(reachable_file "$gitdir")" && continue

        if [ -n "$(known_bad_use "$path" "$oid")" ]; then
            tolerated=$((tolerated + 1))
            continue
        fi
        unfetchable=$((unfetchable + 1))
        echo "history_$path=UNFETCHABLE oid=$oid recorded_by=$(
            awk -v p="$path" -v o="$oid" '$1 == p && $2 == o { printf "%s,", $3 }' \
                "$WORK/history_full" | sed 's/,$//')"
        rc=1
    done < "$WORK/history"

    echo "gitlinks_history=$([ "$unfetchable" = 0 ] && echo clean || echo FAILED)\
 oids=$oids unfetchable=$unfetchable known_bad=$tolerated"
fi

[ "$rc" = 0 ] && echo "gitlinks=clean submodules=$checked commit=$(git rev-parse --short "$COMMIT")"
exit "$rc"
