#!/usr/bin/env bash
# The GitHub release body for a tag.  The download link comes first, then the
# tag's own paragraph, the links a user needs from elsewhere
# (dist/release-links.tsv), the tree's own pages at the tag, and the CHANGELOG
# section folded away: v1.0.0-beta1 put 150 lines of changelog between the
# title and the archive, and the archive is what a visitor to the page came
# for.  release.yml runs this; so does a hand
# `gh release edit <tag> --notes-file <(tools/release-notes.sh <tag>)`.
#
#   tools/release-notes.sh [vX.Y.Z]      defaults to the tree's own tag
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

tag="${1:-$(tools/version.sh --tag)}"
repo="${GITHUB_REPOSITORY:-tinic/AmiNetXDuo}"
ver=$(tools/version.sh --product)
[ "v$ver" = "$tag" ] || {
    echo "release-notes: tag $tag does not match the tree, which says v$ver" >&2
    exit 1
}

# The tag's section and, below it, every section of the same line (0.28.9's
# notes carried 0.28.8 downwards); a heading outside the line ends it.
section=$(awk -v v="## $ver" -v pfx="## ${ver%.*}." '
    $0 == v {found = 1; next}
    found && /^## / && index($0, pfx) != 1 {exit}
    found {print}
' CHANGELOG.md)
[ -n "$section" ] || {
    echo "release-notes: CHANGELOG.md has no '## $ver' section" >&2
    exit 1
}
entries=$(printf '%s\n' "$section" | grep -c '^- ' || true)

# The annotated tag's body, the paragraph written when the release was cut.
# A lightweight tag has none, and the notes do without.
summary=$(git tag -l --format='%(contents:body)' "$tag" 2>/dev/null |
          sed '/^-----BEGIN PGP/,$d' | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}')

# name<TAB>url<TAB>why; a malformed line fails the render rather than
# publishing a broken bullet.
links=""
lineno=0
while IFS=$'\t' read -r name url why || [ -n "$name" ]; do
    lineno=$((lineno + 1))
    case "$name" in ''|'#'*) continue ;; esac
    case "$url" in https://*) ;; *)
        echo "release-notes: dist/release-links.tsv:$lineno: url \"$url\" is not https://" >&2
        exit 1 ;;
    esac
    [ -n "$why" ] || {
        echo "release-notes: dist/release-links.tsv:$lineno: no third field" >&2
        exit 1
    }
    links="$links- [$name]($url) -- $why"$'\n'
done < dist/release-links.tsv
[ -n "$links" ] || { echo "release-notes: dist/release-links.tsv has no links" >&2; exit 1; }

asset="AmiNetXDuo-$ver.lha"
tree="https://github.com/$repo/blob/$tag"
printf '**Download:** [%s](https://github.com/%s/releases/download/%s/%s)' \
       "$asset" "$repo" "$tag" "$asset"
printf ' -- unpack it, run `Install-AmiNetXDuo`.\n\n'
if [ -n "${summary//[[:space:]]/}" ]; then
    printf '%s\n\n' "$summary"
fi
printf '**Also needed:**\n\n%s\n' "$links"
printf '[README](%s/README.md) · [What is missing](%s/docs/GAPS.md) · [CHANGELOG](%s/CHANGELOG.md) · [Issues](https://github.com/%s/issues)\n\n' \
       "$tree" "$tree" "$tree" "$repo"
printf '<details>\n<summary>%s changes, from CHANGELOG.md</summary>\n\n%s\n\n</details>\n' \
       "$entries" "$section"
