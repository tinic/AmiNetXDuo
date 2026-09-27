#!/usr/bin/env bash
# check-submodule-delta.sh against throwaway repos: a superproject whose one
# gitlink moves forward, sideways (off a non-descendant branch), or not at all.
#
#   tools/check-submodule-delta-selftest.sh
#
# Prints check_submodule_delta_selftest=PASS|FAIL cases=N failures=M; exit 0/1.
# SPDX-License-Identifier: MIT

set -u

here=$(cd "$(dirname "$0")" && pwd)
script="$here/check-submodule-delta.sh"
work=$(mktemp -d "${TMPDIR:-/tmp}/subdelta-selftest.XXXXXX")
trap 'rm -rf "$work"' EXIT

export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
g() { git -c init.defaultBranch=main -c protocol.file.allow=always "$@"; }

# Submodule history:  b0 - b1 (release A pin) - b2 - b3 (release B pin)
#                      \- m1 - m2 (maint, release C pin: drops b1 b2 b3, adds m1 m2)
sub="$work/sub"
g init -q "$sub"
c() { echo "$1" > "$sub/f"; g -C "$sub" add f; g -C "$sub" commit -q -m "$1"; g -C "$sub" rev-parse HEAD; }
b0=$(c b0); b1=$(c b1); b2=$(c b2); b3=$(c b3)
g -C "$sub" checkout -q -b maint "$b0"
m1=$(c m1); m2=$(c m2)
g -C "$sub" checkout -q main

# Superproject: vA pins b1, vB pins b3, vC pins m2, vD pins b3 again (no move
# from vB is simulated by tagging vB twice).  The gitlink is written with
# update-index, so no submodule checkout exists: objects come by clone.
sup="$work/sup"
g init -q "$sup"
printf '[submodule "lib"]\n\tpath = lib\n\turl = %s\n' "$sub" > "$sup/.gitmodules"
g -C "$sup" add .gitmodules
pin() {                         # sha tag
    g -C "$sup" update-index --add --cacheinfo "160000,$1,lib"
    g -C "$sup" commit -q --allow-empty -m "$2"
    g -C "$sup" tag "$2"
}
pin "$b1" vA
pin "$b3" vB
pin "$b3" vB2
pin "$m2" vC

cases=0 failures=0
run_case() {                    # name want-rc want-text manifest-file|- script args...
    local name=$1 want_rc=$2 want_text=$3 man=$4; shift 4
    local out rc s=${SCRIPT:-$script} extra=()
    [ "$man" = - ] || extra=(--manifest "$man")
    out=$(cd "$sup" && "$s" ${extra[@]+"${extra[@]}"} "$@" 2>&1)
    rc=$?
    cases=$((cases + 1))
    if [ "$rc" != "$want_rc" ] ||
       { [ -n "$want_text" ] && ! printf '%s' "$out" | grep -q -- "$want_text"; }; then
        failures=$((failures + 1))
        echo "  FAIL $name: rc=$rc (want $want_rc)"
        printf '%s\n' "$out" | sed 's/^/    /'
    fi
}
m() { local f; f=$(mktemp "$work/m.XXXXXX"); cat > "$f"; echo "$f"; }

# 1. No move: PASS with no manifest at all.
run_case "no move, no manifest" 0 "submodule_delta=PASS moved=0 manifest=none" - vB vB2 vB2

# 2. Forward move with a complete manifest.
fwd=$(m <<EOF
# reviewed
lib $b1..$b3
+$b2 reviewer=alice no API change
+$b3 reviewer=bob no API change
EOF
)
run_case "forward move, complete" 0 "submodule_delta=PASS moved=1" "$fwd" vA vB vB

# 2b. Default PREV: the newest v* tag before vB on a different commit is vA.
run_case "default previous tag" 0 "prev=vA " "$fwd" vB

# 2c. The same manifest read from the tagged tree, not a file.
g -C "$sup" checkout -q vB 2>/dev/null
mkdir -p "$sup/tools/release"
cp "$fwd" "$sup/tools/release/submodules-vB3.txt"
g -C "$sup" add tools/release/submodules-vB3.txt
g -C "$sup" commit -q -m manifest
g -C "$sup" tag vB3
run_case "manifest from the tree" 0 "submodule_delta=PASS moved=1" - vA vB3 vB3
run_case "no manifest in the tree" 1 "reason=no_manifest" - vA vB vB

# 2d. The failure prints the lines to fill in, reviewer blank.
run_case "template printed" 1 "manifest_template=+$b2 reviewer= " - vA vB vB

# 3. An added commit left out.
run_case "missing added commit" 1 "reason=missing_added path=lib sha=$b2" "$(m <<EOF
lib $b1..$b3
+$b3 reviewer=bob no API change
EOF
)" vA vB vB

# 4. Sideways move vB -> vC: b1 b2 b3 removed, m1 m2 added; the removed ones
# left out, as a manifest written from old..new alone would leave them.
side_part=$(m <<EOF
lib $b3..$m2
+$m1 reviewer=alice fine
+$m2 reviewer=alice fine
EOF
)
run_case "missing removed commit (non-ancestor)" 1 "reason=missing_removed path=lib sha=$b3" \
    "$side_part" vB vC vC
side_full=$(m <<EOF
lib $b3..$m2
+$m1 reviewer=alice fine
+$m2 reviewer=alice fine
-$b1 reviewer=carol dropped by the maint branch
-$b2 reviewer=carol reverted upstream, nothing of ours used it
-$b3 reviewer=carol same
EOF
)
run_case "non-ancestor move, complete" 0 "added=2 removed=3" "$side_full" vB vC vC

# 5. Empty reviewer.
run_case "empty reviewer" 1 "reason=empty_reviewer" "$(m <<EOF
lib $b1..$b3
+$b2 reviewer= no API change
+$b3 reviewer=bob no API change
EOF
)" vA vB vB

# 6. A commit that is not in the delta.
run_case "unknown commit" 1 "reason=unknown_commit path=lib dir=+ sha=$m1" "$(m <<EOF
lib $b1..$b3
+$b2 reviewer=alice ok
+$b3 reviewer=bob ok
+$m1 reviewer=bob ok
EOF
)" vA vB vB
run_case "removed listed as added" 1 "reason=unknown_commit" "$(m <<EOF
lib $b3..$m2
+$m1 reviewer=alice fine
+$m2 reviewer=alice fine
+$b2 reviewer=carol x
-$b3 reviewer=carol x
EOF
)" vB vC vC

# 7. Header naming other refs.
run_case "header mismatch" 1 "reason=header_mismatch path=lib" "$(m <<EOF
lib $b0..$b3
+$b2 reviewer=alice ok
+$b3 reviewer=bob ok
EOF
)" vA vB vB

# 7b. A gitlink added outright (vC -> vH adds lib2 at b1) and deleted
# outright (vH -> vI): the zero side in the header, the one pin listed.
g -C "$sup" checkout -q vC 2>/dev/null
g -C "$sup" update-index --add --cacheinfo "160000,$b1,lib2"
g -C "$sup" commit -q -m "add lib2"
g -C "$sup" tag vH
g -C "$sup" update-index --force-remove lib2
g -C "$sup" commit -q -m "drop lib2"
g -C "$sup" tag vI
Z=0000000000000000000000000000000000000000
run_case "gitlink added, no manifest" 1 "reason=no_manifest" - vC vH vH
run_case "gitlink added, template" 1 "manifest_template=lib2 $Z..$b1" - vC vH vH
run_case "gitlink added, complete" 0 "submodule_delta=gitlink_added path=lib2" "$(m <<EOF
lib2 $Z..$b1
+$b1 reviewer=alice new dependency, reviewed whole
EOF
)" vC vH vH
run_case "gitlink added, listed as removed" 1 "reason=missing_added" "$(m <<EOF
lib2 $Z..$b1
-$b1 reviewer=alice wrong side
EOF
)" vC vH vH
run_case "gitlink removed, no manifest" 1 "reason=no_manifest" - vH vI vI
run_case "gitlink removed, complete" 0 "submodule_delta=gitlink_removed path=lib2" "$(m <<EOF
lib2 $b1..$Z
-$b1 reviewer=alice nothing of ours links it any more
EOF
)" vH vI vI

# 8. Objects that cannot be had: the URL points nowhere.  Distinct code 3.
g -C "$sup" checkout -q vC 2>/dev/null
printf '[submodule "lib"]\n\tpath = lib\n\turl = %s\n' "$work/gone" > "$sup/.gitmodules"
g -C "$sup" commit -q -am "url gone"
g -C "$sup" tag vE
pin "$b1" vF
run_case "unobtainable objects" 3 "submodule_delta=FAIL reason=objects_unobtainable" "$fwd" vE vF vF
# ... and a pin the server does not have, at a URL that works.
g -C "$sup" checkout -q vC 2>/dev/null
pin 1111111111111111111111111111111111111111 vG
run_case "pin missing upstream" 3 "reason=objects_unobtainable" "$fwd" vC vG vG

# Discrimination: a script that computes old..new only must pass case 4's
# incomplete manifest, or case 4 proves nothing.
mutant="$work/mutant.sh"
sed 's|removed=$(git --git-dir="$gd" rev-list --reverse "$newc..$old")|removed=""|' \
    "$script" > "$mutant"
chmod +x "$mutant"
if cmp -s "$script" "$mutant"; then
    cases=$((cases + 1)); failures=$((failures + 1))
    echo "  FAIL mutant: the removed-commit line was not found to disable"
else
    SCRIPT=$mutant run_case "mutant (old..new only) passes the non-ancestor gap" 0 \
        "submodule_delta=PASS" "$side_part" vB vC vC
fi

# ... and a script that pairs only gitlinks present on both sides must pass
# an unlisted add and removal, or cases 7b prove nothing.
mutant2="$work/mutant2.sh"
sed 's|join -a1 -a2 -e|join -e|' "$script" > "$mutant2"
chmod +x "$mutant2"
if cmp -s "$script" "$mutant2"; then
    cases=$((cases + 1)); failures=$((failures + 1))
    echo "  FAIL mutant2: the outer join was not found to disable"
else
    SCRIPT=$mutant2 run_case "mutant (both sides only) passes an unlisted add" 0 \
        "submodule_delta=PASS moved=0 manifest=none" - vC vH vH
    SCRIPT=$mutant2 run_case "mutant (both sides only) passes an unlisted removal" 0 \
        "submodule_delta=PASS moved=0 manifest=none" - vH vI vI
fi

if [ "$failures" = 0 ]; then
    echo "check_submodule_delta_selftest=PASS cases=$cases failures=0"
    exit 0
fi
echo "check_submodule_delta_selftest=FAIL cases=$cases failures=$failures"
exit 1
