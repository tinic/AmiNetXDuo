#!/usr/bin/env bash
# check-gitlinks.sh's backward-move rule against throwaway repos: a gitlink
# that moves to a non-descendant fails unless a release manifest at that commit
# names the exact move and gives every dropped commit a reviewer.
#
#   tools/check-gitlinks-selftest.sh
#
# Prints check_gitlinks_selftest=PASS|FAIL cases=N failures=M; exit 0/1.
# SPDX-License-Identifier: MIT

set -u

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/gitlinks-selftest.XXXXXX")
trap 'rm -rf "$work"' EXIT

export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
g() { git -c init.defaultBranch=main -c protocol.file.allow=always "$@"; }

# Submodule:  b0 - b1 - b2 - b3 - b4   main
#              \- m1 - m2              maint (drops b1 b2 b3 from a b3 pin)
#              \- o1                   other
sub="$work/sub"
g init -q "$sub"
c() { echo "$1" > "$sub/f"; g -C "$sub" add f; g -C "$sub" commit -q -m "$1"; g -C "$sub" rev-parse HEAD; }
b0=$(c b0); b1=$(c b1); b2=$(c b2); b3=$(c b3); b4=$(c b4)
g -C "$sub" checkout -q -b maint "$b0"; m1=$(c m1); m2=$(c m2)
g -C "$sub" checkout -q -b other "$b0"; o1=$(c o1)
g -C "$sub" checkout -q main

# Superproject on main pins b3.  The script is copied in: it checks the repo
# it lives in.  lib is a real clone, so every submodule object is there.
sup="$work/sup"
g init -q "$sup"
mkdir -p "$sup/tools"
cp "$here/check-gitlinks.sh" "$sup/tools/"
gm() { printf '[submodule "lib"]\n\tpath = lib\n\turl = %s\n\tbranch = %s\n' "$sub" "$1" > "$sup/.gitmodules"; }
gm main
g -C "$sup" add .gitmodules
g -C "$sup" update-index --add --cacheinfo "160000,$b3,lib"
g -C "$sup" commit -q -m base
base=$(g -C "$sup" rev-parse HEAD)
g -C "$sup" clone -q "$sub" lib 2>/dev/null
g -C "$sup/lib" checkout -q "$b3"

# One commit on base: pin, .gitmodules branch, and a manifest body (or -).
move() {                        # pin branch manifest-file|-  -> commit sha
    g -C "$sup" checkout -q --detach "$base"
    gm "$2"; g -C "$sup" add .gitmodules
    g -C "$sup" update-index --cacheinfo "160000,$1,lib"
    if [ "$3" != - ]; then
        mkdir -p "$sup/tools/release"
        cp "$3" "$sup/tools/release/submodules-v9.txt"
        g -C "$sup" add tools/release/submodules-v9.txt
    fi
    g -C "$sup" commit -q -m move
    g -C "$sup" rev-parse HEAD
    g -C "$sup" rm -q --cached -r tools/release 2>/dev/null
    rm -rf "$sup/tools/release"
}
m() { local f; f=$(mktemp "$work/m.XXXXXX"); cat > "$f"; echo "$f"; }

cases=0 failures=0
run_case() {                    # name want-rc want-text commit [script]
    local out rc s=${5:-tools/check-gitlinks.sh}
    out=$(cd "$sup" && "$s" "$4" 2>&1)
    rc=$?
    cases=$((cases + 1))
    if [ "$rc" != "$2" ] || ! printf '%s' "$out" | grep -q -- "$3"; then
        failures=$((failures + 1))
        echo "  FAIL $1: rc=$rc (want $2)"
        printf '%s\n' "$out" | sed 's/^/    /'
    fi
}

full=$(m <<EOF
# reviewed replacement
lib $b3..$m2
-$b3 reviewer=alice superseded by m2
-$b2 reviewer=alice superseded by m1
-$b1 reviewer=bob superseded by m1
+$m1 reviewer=bob
+$m2 reviewer=bob
EOF
)

# 1. The exact reviewed move passes.
c1=$(move "$m2" maint "$full")
run_case "reviewed move" 0 "gitlink_lib=ok_reviewed_move from=$b3 to=$m2" "$c1"

# 1b. The same move reached through a merge, as CI checks a PR: COMMIT^ is
#     the first parent, the base branch.
g -C "$sup" checkout -q --detach "$base"
g -C "$sup" merge -q --no-ff -m merge "$c1" 2>/dev/null
c1m=$(g -C "$sup" rev-parse HEAD)
run_case "reviewed move via merge" 0 "ok_reviewed_move from=$b3 to=$m2" "$c1m"

# 2. A dropped commit with no "-" line.
c2=$(move "$m2" maint "$(grep -v "^-$b2" "$full" | m)")
run_case "dropped commit unlisted" 1 "gitlink_lib=MOVED_BACK from=$b3 to=$m2" "$c2"

# 3. A "-" line with an empty reviewer.
c3=$(move "$m2" maint "$(sed "s/^-$b2 reviewer=alice/-$b2 reviewer=/" "$full" | m)")
run_case "empty reviewer" 1 "gitlink_lib=MOVED_BACK" "$c3"

# 4. Header with the wrong old OID.
c4=$(move "$m2" maint "$(sed "s/^lib $b3\.\./lib $b1../" "$full" | m)")
run_case "header wrong from" 1 "gitlink_lib=MOVED_BACK" "$c4"

# 5. Header with the wrong new OID.
c5=$(move "$m2" maint "$(sed "s/\.\.$m2\$/..$m1/" "$full" | m)")
run_case "header wrong to" 1 "gitlink_lib=MOVED_BACK" "$c5"

# 6. Header naming another path.
c6=$(move "$m2" maint "$(sed "s/^lib $b3/other $b3/" "$full" | m)")
run_case "header wrong path" 1 "gitlink_lib=MOVED_BACK" "$c6"

# 7. No manifest at all.
c7=$(move "$m2" maint -)
run_case "no manifest" 1 "gitlink_lib=MOVED_BACK from=$b3 to=$m2" "$c7"

# 8. An unrelated backward move with only the reviewed move's manifest.
c8=$(move "$o1" other "$full")
run_case "unrelated backward move" 1 "gitlink_lib=MOVED_BACK from=$b3 to=$o1" "$c8"

# 9. The "-" lines under another header do not count for this one.
c9=$(move "$m2" maint "$( { echo "lib $b3..$m2"; echo "+$m1 reviewer=bob"; echo "zz $b0..$b0"; grep '^-' "$full"; } | m)")
run_case "dropped lines in another section" 1 "gitlink_lib=MOVED_BACK" "$c9"

# 10. A forward move needs no manifest.
c10=$(move "$b4" main -)
run_case "forward move" 0 "gitlink_lib=ok pin=origin/main" "$c10"

# Discrimination: a script that skips the dropped-commit coverage must pass
# case 2, or case 2 proves nothing.
mutant="$sup/tools/mutant.sh"
sed 's|if \[ -z "$(sort -u "$WORK/dropped" \| comm -23 - "$WORK/reviewed")" \]; then|if true; then|' \
    "$sup/tools/check-gitlinks.sh" > "$mutant"
chmod +x "$mutant"
if cmp -s "$sup/tools/check-gitlinks.sh" "$mutant"; then
    cases=$((cases + 1)); failures=$((failures + 1))
    echo "  FAIL mutant: the coverage check was not found to disable"
else
    run_case "mutant (no coverage) passes an unlisted drop" 0 "ok_reviewed_move" "$c2" "$mutant"
fi

if [ "$failures" = 0 ]; then
    echo "check_gitlinks_selftest=PASS cases=$cases failures=0"
    exit 0
fi
echo "check_gitlinks_selftest=FAIL cases=$cases failures=$failures"
exit 1
