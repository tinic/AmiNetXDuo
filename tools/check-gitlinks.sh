#!/usr/bin/env bash
#
# Every recorded submodule commit must be an object a fresh clone can fetch.
#
#   tools/check-gitlinks.sh [commit]
#
# A gitlink is forty characters in a tree, and nothing validates it at the
# moment it is written: `git commit` will record an object id no submodule has
# ever produced, the superproject stays green, and the failure surfaces on
# somebody else's machine either as `git submodule update` refusing to run or,
# worse, as a stale submodule that still compiles.  This session lost time to
# the second one -- a checkout arrived with third_party/netxduo at the wrong
# commit and the build said `implicit declaration of nx_mld_enable`, which
# reads exactly like a defect of ours.
#
# Five things can be wrong, and this checks all five.
#
#   THE WORKING TREE IS STALE.  `git submodule status --recursive` prints `+`
#   when the checked-out commit is not the recorded one.  Cheapest of the four
#   and the one that pays for this script every time it fires.
#
#   NO SUCH OBJECT.  3cb2e52e recorded netxduo at a5366b1d47d5f00b... when the
#   merge it meant was a5366b1d73b91c30...: the first eight characters are
#   right and the remaining thirty-two were invented, so it is not a truncation
#   anybody can widen back out.  Seven commits carry it and `git submodule
#   update` cannot work at any of them.
#
#   NOT REACHABLE FROM ANY REF.  1787574d pinned netxduo at 9e5d3226, the tip
#   of a topic branch revised before it was merged.  That object still exists
#   in the clone that wrote it and nowhere else: a fresh clone fetches refs,
#   and no ref reaches that commit any more.  This is what pinning a tip costs,
#   and it is invisible from the machine that did it.
#
#   NOT ON THE TRACKED BRANCH.  A pin belongs on the first-parent chain of the
#   branch .gitmodules names, or on a tag.  Merges and tags do not move; a
#   topic tip does, and is deleted the moment it lands.
#
#   THE BUMP WENT BACKWARDS.  1d8b8a15 moved netxduo from merge 396dc632 back
#   to tip 7206b214, which is not a descendant of it, so that commit's tree
#   silently reverts every fork fix merged in between.  Both ids are real and
#   both check out: the only thing that separates a bump from a revert is
#   ancestry, and nothing was asking.
#
# Object existence and reachability are also swept over every gitlink recorded,
# which is cheap because a raw log names each change once: about 140 distinct
# ids over 3000 commits.  A pin can rot after it is written -- deleting a topic
# branch on the remote takes its object with it -- so the sweep is the only
# thing that notices.
#
# Output is key=value and an exit code: 0 clean, 1 a pin is wrong, 2 nothing
# to check (no submodule initialised).
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

# Where a submodule's objects live, whether or not it is checked out.
sub_gitdir() {
    if [ -e "$1/.git" ] && git -C "$1" rev-parse --absolute-git-dir 2>/dev/null; then
        return 0
    fi
    d="$ROOT/$(git rev-parse --git-path "modules/$2")"
    [ -d "$d" ] && printf '%s\n' "$d"
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

    if [ "$(git --git-dir="$gitdir" rev-parse --is-shallow-repository)" = "false" ] &&
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
