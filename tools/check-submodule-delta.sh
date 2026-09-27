#!/usr/bin/env bash
# Every submodule delta between two releases, reviewed commit by commit.
# A gitlink bump carries upstream commits nobody read; a bump onto a branch
# that is not a descendant also drops commits.  Both directions are listed.
#
#   tools/check-submodule-delta.sh [--tag NAME] [--manifest FILE] [PREV] NEW [NAME]
#
#   PREV      previous release; default the newest v* tag reachable from NEW
#             whose commit is not NEW's
#   NEW       the release commit (tag, SHA or branch)
#   NAME      release tag naming the manifest; default NEW when NEW is a v*
#             tag, else tools/version.sh --tag
#   FILE      manifest to read; default NEW:tools/release/submodules-NAME.txt
#
# Manifest (plain text, # comments and blank lines ignored), per moved gitlink:
#
#   third_party/wifipi <old-sha>..<new-sha>
#   +<sha> reviewer=<nick> <compatibility statement>    per added commit
#   -<sha> reviewer=<nick> <compatibility statement>    per removed commit
#
# A gitlink added or removed outright uses 40 zeros for the missing side and
# lists the one pinned commit: +<new> or -<old>.
#
# Objects come from the submodule checkout, else .git/modules/<name>, else a
# blob-less clone of the .gitmodules URL under $TMPDIR.
#
# Output key=value lines, last submodule_delta=PASS|FAIL.  On a manifest
# failure manifest_template= lines carry the lines the manifest needs.
# Exit 0 PASS, 1 manifest incomplete or wrong, 2 usage, 3 objects unobtainable.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C              # sort, join and comm agree on one order

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
Z=0000000000000000000000000000000000000000

usage() { echo "submodule_delta=FAIL reason=usage detail=$*"; exit 2; }

tag="" manifest_file=""
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --tag)      [ $# -ge 2 ] || usage "--tag_needs_a_value"; tag=$2; shift 2 ;;
        --manifest) [ $# -ge 2 ] || usage "--manifest_needs_a_value"; manifest_file=$2; shift 2 ;;
        -h|--help)  sed -n '2,30p' "$0" | sed 's/^#\{0,1\} \{0,1\}//'; exit 0 ;;
        -*)         usage "unknown_option_$1" ;;
        *)          args+=("$1"); shift ;;
    esac
done
case ${#args[@]} in
    1) prev="" new=${args[0]} ;;
    2) prev=${args[0]} new=${args[1]} ;;
    3) prev=${args[0]} new=${args[1]}; [ -n "$tag" ] || tag=${args[2]} ;;
    *) usage "want_[PREV]_NEW_[NAME]" ;;
esac

top=$(git rev-parse --show-toplevel 2>/dev/null) || usage "not_in_a_git_repository"
cd "$top"
common=$(cd "$(git rev-parse --git-common-dir)" && pwd)

new_sha=$(git rev-parse -q --verify "$new^{commit}") || usage "unknown_ref_$new"

if [ -z "$prev" ]; then
    # -suffix=- puts v1.0.0-beta6 before v1.0.0, as a release order must.
    prev=$(git -c versionsort.suffix=- tag --merged "$new_sha" --list 'v*' \
               --sort=-version:refname |
           while read -r t; do
               [ "$(git rev-parse "$t^{commit}")" != "$new_sha" ] && { echo "$t"; break; }
           done)
    [ -n "$prev" ] || usage "no_previous_v_tag_reachable_from_$new"
fi
prev_sha=$(git rev-parse -q --verify "$prev^{commit}") || usage "unknown_ref_$prev"

if [ -z "$tag" ]; then
    case "$new" in
        v*) git rev-parse -q --verify "refs/tags/$new" >/dev/null && tag=$new ;;
    esac
    [ -n "$tag" ] || tag=$("$here/version.sh" --tag)
fi
manifest_name="tools/release/submodules-$tag.txt"

work=$(mktemp -d "${TMPDIR:-/tmp}/subdelta.XXXXXX")
trap 'rm -rf "$work"' EXIT

echo "submodule_delta_range prev=$prev prev_sha=$prev_sha new=$new new_sha=$new_sha tag=$tag"

gitlinks() {                    # ref -> "path sha" per gitlink, sorted by path
    git ls-tree -r --full-tree "$1" | awk -F'\t' '
        { split($1, m, " "); if (m[1] == "160000") print $2, m[3] }' | sort
}
gitlinks "$prev_sha" > "$work/old"
gitlinks "$new_sha" > "$work/new"

# path old new, one per gitlink present on either side.
join -a1 -a2 -e "$Z" -o 0,1.2,2.2 "$work/old" "$work/new" > "$work/pairs"

# .gitmodules value for a path: key (url or name) from NEW, then PREV.
module_name() {
    local ref
    for ref in "$new_sha" "$prev_sha"; do
        git config --blob "$ref:.gitmodules" --get-regexp '^submodule\..*\.path$' 2>/dev/null |
            awk -v p="$1" '$2 == p { n = $1; sub(/^submodule\./, "", n); sub(/\.path$/, "", n); print n; exit }' |
            grep . && return 0
    done
    return 1
}
module_url() {
    local ref
    for ref in "$new_sha" "$prev_sha"; do
        git config --blob "$ref:.gitmodules" "submodule.$1.url" 2>/dev/null && return 0
    done
    return 1
}

# Echo a git dir holding both commits and their full history, or fail.
has_both() {                    # gitdir old new
    [ "$(git --git-dir="$1" rev-parse --is-shallow-repository 2>/dev/null)" = false ] &&
    git --git-dir="$1" cat-file -e "$2^{commit}" 2>/dev/null &&
    git --git-dir="$1" cat-file -e "$3^{commit}" 2>/dev/null
}
objects_for() {                 # path old new
    local path=$1 old=$2 newc=$3 name url gd clone
    if [ -e "$top/$path/.git" ]; then
        gd=$(git -C "$top/$path" rev-parse --absolute-git-dir 2>/dev/null) &&
            has_both "$gd" "$old" "$newc" && { echo "checkout $gd"; return 0; }
    fi
    name=$(module_name "$path") || name=$path
    gd="$common/modules/$name"
    [ -d "$gd" ] && has_both "$gd" "$old" "$newc" && { echo "modules $gd"; return 0; }
    url=$(module_url "$name") || return 1
    case "$url" in ./*|../*) return 1 ;; esac
    clone="$work/clone-$(printf '%s' "$path" | tr '/' '_')"
    git clone -q --bare --filter=blob:none "$url" "$clone" >/dev/null 2>&1 || return 1
    has_both "$clone" "$old" "$newc" && { echo "fetched $clone"; return 0; }
    # Commits off every branch (a force-pushed pin) may still be served by id.
    git --git-dir="$clone" fetch -q origin "$old" "$newc" >/dev/null 2>&1 || true
    has_both "$clone" "$old" "$newc" && { echo "fetched $clone"; return 0; }
    return 1
}

: > "$work/expected"
: > "$work/template"
: > "$work/subjects"
unobtainable=0 moved=0
while read -r path old newc; do
    if [ "$old" = "$newc" ]; then
        echo "submodule_delta=ok path=$path sha=$newc added=0 removed=0"
        continue
    fi
    moved=$((moved + 1))
    printf '%s H %s..%s\n' "$path" "$old" "$newc" >> "$work/expected"
    printf 'manifest_template=%s %s..%s\n' "$path" "$old" "$newc" >> "$work/template"
    if [ "$old" = "$Z" ]; then
        printf '%s\n' "$newc" > "$work/add"; : > "$work/rem"; state=gitlink_added
    elif [ "$newc" = "$Z" ]; then
        : > "$work/add"; printf '%s\n' "$old" > "$work/rem"; state=gitlink_removed
    else
        if ! src=$(objects_for "$path" "$old" "$newc"); then
            unobtainable=$((unobtainable + 1))
            echo "submodule_delta=unobtainable path=$path old=$old new=$newc url=$(module_url "$(module_name "$path" || echo "$path")" || echo none)"
            continue
        fi
        gd=${src#* }
        echo "submodule_delta_objects path=$path source=${src%% *}"
        added=$(git --git-dir="$gd" rev-list --reverse "$old..$newc")
        removed=$(git --git-dir="$gd" rev-list --reverse "$newc..$old")
        printf '%s' "$added${added:+$'\n'}" > "$work/add"
        printf '%s' "$removed${removed:+$'\n'}" > "$work/rem"
        state=moved
        while read -r c; do
            printf '%s + %s %s\n' "$path" "$c" "$(git --git-dir="$gd" log -1 --format=%s "$c")"
        done < "$work/add" >> "$work/subjects"
        while read -r c; do
            printf '%s - %s %s\n' "$path" "$c" "$(git --git-dir="$gd" log -1 --format=%s "$c")"
        done < "$work/rem" >> "$work/subjects"
    fi
    echo "submodule_delta=$state path=$path old=$old new=$newc added=$(wc -l < "$work/add" | tr -d ' ') removed=$(wc -l < "$work/rem" | tr -d ' ')"
    sed "s|^|$path + |" "$work/add" >> "$work/expected"
    sed "s|^|$path - |" "$work/rem" >> "$work/expected"
    sed 's|^|manifest_template=+|; s|$| reviewer= |' "$work/add" >> "$work/template"
    sed 's|^|manifest_template=-|; s|$| reviewer= |' "$work/rem" >> "$work/template"
done < "$work/pairs"

while read -r path sign c subject; do
    echo "submodule_delta_commit path=$path dir=$([ "$sign" = + ] && echo added || echo removed) sha=$c subject=$subject"
done < "$work/subjects"

if [ "$unobtainable" -gt 0 ]; then
    echo "submodule_delta=FAIL reason=objects_unobtainable paths=$unobtainable"
    exit 3
fi

# ------------------------------------------------------------ manifest -----

have_manifest=0
if [ -n "$manifest_file" ]; then
    [ -f "$manifest_file" ] || usage "no_such_manifest_$manifest_file"
    cp "$manifest_file" "$work/manifest"; have_manifest=1
    echo "submodule_delta_manifest source=$manifest_file"
elif git cat-file -e "$new_sha:$manifest_name" 2>/dev/null; then
    git show "$new_sha:$manifest_name" > "$work/manifest"; have_manifest=1
    echo "submodule_delta_manifest source=$new:$manifest_name"
fi

if [ "$have_manifest" = 0 ]; then
    if [ "$moved" = 0 ]; then
        echo "submodule_delta=PASS moved=0 manifest=none"
        exit 0
    fi
    echo "submodule_delta_error reason=no_manifest want=$new:$manifest_name"
    cat "$work/template"
    echo "submodule_delta=FAIL reason=no_manifest moved=$moved"
    exit 1
fi

# Normalise to "path H old..new" / "path +|- sha"; malformed lines as ERR.
awk -v Z="$Z" '
    function err(r) { printf "ERR reason=%s line=%d text=%s\n", r, NR, $0 }
    /^[[:space:]]*(#|$)/ { next }
    /^[+-]/ {
        if (path == "") { err("commit_before_header"); next }
        s = substr($1, 2)
        if (s !~ /^[0-9a-f]+$/ || length(s) != 40) { err("sha_not_40_hex"); next }
        if ($2 !~ /^reviewer=/) { err("no_reviewer_field"); next }
        if ($2 == "reviewer=") { err("empty_reviewer"); next }
        if (NF < 3) { err("empty_statement"); next }
        print path, substr($1, 1, 1), s
        next
    }
    NF == 2 && $2 ~ /^[0-9a-f]+\.\.[0-9a-f]+$/ { path = $1; print path, "H", $2; next }
    { err("malformed") }
' "$work/manifest" > "$work/parsed"

fail=0
while read -r _ reason line text; do
    echo "submodule_delta_error $reason $line $text"; fail=1
done < <(grep '^ERR ' "$work/parsed" || true)

grep -v '^ERR ' "$work/parsed" | sort -u > "$work/got"
awk '{ print $1, $2, $3 }' "$work/expected" | sort -u > "$work/want"

# Header mismatch shows up as a missing and an extra header of one path.
comm -23 "$work/want" "$work/got" | while read -r path kind v; do
    case "$kind" in
        H) if awk -v p="$path" '$1 == p && $2 == "H" { f = 1 } END { exit !f }' "$work/got"; then
               echo "submodule_delta_error reason=header_mismatch path=$path want=$v"
           else
               echo "submodule_delta_error reason=missing_header path=$path want=$v"
           fi ;;
        +) echo "submodule_delta_error reason=missing_added path=$path sha=$v" ;;
        -) echo "submodule_delta_error reason=missing_removed path=$path sha=$v" ;;
    esac
done > "$work/missing"
comm -13 "$work/want" "$work/got" | while read -r path kind v; do
    case "$kind" in
        H) awk -v p="$path" '$1 == p && $2 == "H" { f = 1 } END { exit !f }' "$work/want" ||
               echo "submodule_delta_error reason=unmoved_path path=$path got=$v"
           awk -v p="$path" '$1 == p && $2 == "H" { f = 1 } END { exit !f }' "$work/want" &&
               echo "submodule_delta_error reason=header_mismatch path=$path got=$v" ;;
        +|-) echo "submodule_delta_error reason=unknown_commit path=$path dir=$kind sha=$v" ;;
    esac
done >> "$work/missing"

if [ -s "$work/missing" ]; then
    cat "$work/missing"; fail=1
fi

if [ "$fail" = 0 ]; then
    echo "submodule_delta=PASS moved=$moved manifest=$manifest_name"
    exit 0
fi
cat "$work/template"
echo "submodule_delta=FAIL reason=manifest moved=$moved manifest=$manifest_name"
exit 1
