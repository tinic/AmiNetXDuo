#!/usr/bin/env bash
#
# Build the toolchain tarball that tools/fetch-toolchain.sh downloads from our
# GitHub release, and print the sha256 to pin it by.
#
#   tools/package-toolchain-mirror.sh                 # package + verify
#   tools/package-toolchain-mirror.sh --out /tmp/x.tar.xz
#   tools/package-toolchain-mirror.sh --from <root>   # package a tree you have
#
# WHY THIS EXISTS
#
#   We host a copy of a toolchain someone else built, because CI needs it and
#   the only upstream distribution is one layer of a Docker image.  Hosting it
#   is cheap; hosting it without a written-down way to REMAKE it is how a
#   mirror turns into a mystery binary that nobody dares to touch.  This is
#   that way.
#
#   It also fixes the source of the copy.  Packaging is always done from the
#   UPSTREAM REGISTRY, never from our own mirror, otherwise re-cutting the
#   asset would copy from the last copy, and any drift becomes permanent.
#
# THE OUTPUT IS NOT BIT-REPRODUCIBLE
#
#   tar member order comes from readdir, and xz output varies by version, so
#   two runs on two machines will not produce identical bytes.  They produce
#   identical TREES, which is what matters and what this script checks.  The
#   sha256 printed at the end is therefore a fact about the artifact you just
#   made: paste it into TC_MIRROR_SHA256 whenever you upload a new one.
#
# WHAT TO DO WITH THE RESULT
#
#   The release body carries the provenance and the GPLv3 6(d) corresponding
#   source directions, we redistribute GCC binaries, so that is not
#   optional.  It is kept in tools/toolchain-mirror-release-notes.md rather
#   than only on the release page, because an obligation that exists solely in
#   a text box on github.com is one nobody can review, diff, or reinstate.
#   Update the digests and commit hashes in it whenever the pin moves.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GCC_VERSION="15.2.0"
ASSET="m68k-amigaos-gcc-${GCC_VERSION}-ndk3.9-linux-x86_64.tar.xz"
PREFIX="opt/m68k-amigaos"

FROM=""
OUT="${TMPDIR:-/tmp}/$ASSET"

while [ $# -gt 0 ]; do
    case "$1" in
        --from) FROM="$2"; shift ;;
        --out)  OUT="$2"; shift ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--from <toolchain root>] [--out <file.tar.xz>]" >&2; exit 2 ;;
    esac
    shift
done

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing required tool: $1" >&2; exit 2; }; }
need tar
need xz

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# ------------------------------------------------------------------ source ---

if [ -z "$FROM" ]; then
    FROM=$("$ROOT_DIR/tools/fetch-toolchain.sh" --print-root)
    if [ ! -x "$FROM/bin/m68k-amigaos-gcc" ]; then
        echo "==> no cached toolchain; fetching from upstream (not from our mirror)"
        AMINETXDUO_TOOLCHAIN_SOURCE=registry "$ROOT_DIR/tools/fetch-toolchain.sh" >/dev/null
    fi
fi

[ -x "$FROM/bin/m68k-amigaos-gcc" ] || {
    echo "!! $FROM has no bin/m68k-amigaos-gcc" >&2; exit 1; }
[ -f "$FROM/m68k-amigaos/ndk-include/exec/types.h" ] || {
    echo "!! $FROM has no NDK headers" >&2; exit 1; }

echo "==> packaging $FROM"
echo "    -> $OUT"

# ------------------------------------------------------------------- pack ----

# The archive keeps the upstream layer's `opt/m68k-amigaos/` prefix so that
# fetch-toolchain.sh extracts either source with one code path.  That means
# renaming the top-level directory on the way in, and the two tars spell that
# differently.
SRC_PARENT=$(cd "$FROM/.." && pwd)
SRC_BASE=$(basename "$FROM")

TAR_FLAGS=(--uid 0 --gid 0 --uname root --gname root)
if tar --version 2>/dev/null | head -1 | grep -qi bsdtar; then
    TAR_FLAGS+=(--no-mac-metadata --no-xattrs -s "|^${SRC_BASE}|${PREFIX}|")
else
    # --sort=name buys GNU tar a stable member order; bsdtar has no equivalent.
    TAR_FLAGS+=(--sort=name --transform "s|^${SRC_BASE}|${PREFIX}|")
fi

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
# Hard links matter here: 46 of the binaries are links to each other, and
# storing them as copies would add ~100 MB of duplicates.
COPYFILE_DISABLE=1 tar "${TAR_FLAGS[@]}" -cf - -C "$SRC_PARENT" "$SRC_BASE" \
    | xz -9 -T0 -c > "$OUT"

# ----------------------------------------------------------------- verify ----

# A mirror that unpacks to something subtly different from what it mirrors is
# worse than no mirror, so this is checked rather than assumed.
echo "==> verifying the round trip"
VTMP=$(mktemp -d "${TMPDIR:-/tmp}/aminetxduo-pkg.XXXXXX")
trap 'rm -rf "$VTMP"' EXIT
tar xf "$OUT" -C "$VTMP"

if ! diff -rq --no-dereference "$FROM" "$VTMP/$PREFIX" >/dev/null; then
    echo "!! the repackaged tree differs from the source tree" >&2
    diff -rq --no-dereference "$FROM" "$VTMP/$PREFIX" | head -20 >&2
    exit 1
fi

SRC_N=$(find "$FROM" | wc -l | tr -d ' ')
OUT_N=$(find "$VTMP/$PREFIX" | wc -l | tr -d ' ')
LINKS=$(find "$VTMP/$PREFIX" -type f -links +1 | wc -l | tr -d ' ')
SYMS=$(find "$VTMP/$PREFIX" -type l | wc -l | tr -d ' ')
[ "$SRC_N" = "$OUT_N" ] || { echo "!! entry count $OUT_N != $SRC_N" >&2; exit 1; }

SHA=$(sha256_of "$OUT")
SIZE=$(wc -c < "$OUT" | tr -d ' ')

echo "    $OUT_N entries, $LINKS hard links, $SYMS symlinks -- identical to the source"
echo
echo "==> $ASSET"
echo "    size    $SIZE"
echo "    sha256  $SHA"
echo
echo "Pin it, then publish it:"
echo
echo "  1. set TC_MIRROR_SHA256 in tools/fetch-toolchain.sh to"
echo "     $SHA"
echo
echo "  2. gh release create toolchain-m68k-amigaos-gcc-$GCC_VERSION \\"
echo "       --title 'Toolchain mirror - m68k-amigaos GCC $GCC_VERSION (build artifact, not an AmiNetXDuo release)' \\"
echo "       --notes-file tools/toolchain-mirror-release-notes.md \\"
echo "       --prerelease \\"
echo "       '$OUT'"
echo
echo "     (that body carries the provenance and the GPLv3 6(d) corresponding"
echo "      source directions -- publishing GCC binaries without them is not"
echo "      an option.  Check its digests still match before you upload.)"
