#!/usr/bin/env bash
#
# Build the toolchain tarball that tools/fetch-toolchain.sh downloads from our
# GitHub release, and print the sha256 to pin it by.
#
#   tools/package-toolchain-mirror.sh --from <root>
#   tools/package-toolchain-mirror.sh --from <root> --out /tmp/x.tar.xz
#
#   tools/build-toolchain.sh --package <file> calls this for you, which is the
#   normal way to get here.
#
# WHY THIS EXISTS
#
#   CI needs a working m68k-amigaos toolchain and there is no upstream that
#   publishes this one: GCC 16.2 paired with binutils 2.39.0 exists as no
#   image, package or tarball anywhere, so we build it (tools/build-toolchain.sh)
#   and host the result.  Hosting a binary is cheap; hosting it without a
#   written-down way to REMAKE it is how a mirror turns into a mystery blob
#   nobody dares touch.  That way is the build script; this script is only the
#   wrapping, and it is separate so that re-cutting an asset from a tree you
#   already have does not mean rebuilding for an hour.
#
#   PACKAGE WHAT YOU BUILT, NEVER WHAT YOU DOWNLOADED.  `--from` is required
#   for exactly that reason: pointing it at the fetch cache would copy from the
#   last copy, and any drift would become permanent.
#
# THE OUTPUT IS NOT BIT-REPRODUCIBLE
#
#   tar member order comes from readdir, and xz output varies by version, so
#   two runs on two machines will not produce identical bytes.  They produce
#   identical TREES, which is what matters and what this script checks.  The
#   sha256 printed at the end is therefore a fact about the artifact you just
#   made: paste it into the right TC_SHA256_* in tools/fetch-toolchain.sh
#   whenever you upload a new one.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GCC_VERSION="16.2.0"
PREFIX="opt/m68k-amigaos"

# The platform is part of the asset name because there is now more than one,
# and a tarball of ELF binaries whose name does not say so is a support ticket.
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64|Linux/amd64) PLATFORM="linux-x86_64" ;;
    Darwin/arm64)             PLATFORM="darwin-arm64" ;;
    Darwin/x86_64)            PLATFORM="darwin-x86_64" ;;
    Linux/aarch64|Linux/arm64) PLATFORM="linux-aarch64" ;;
    *) PLATFORM="$(uname -s | tr 'A-Z' 'a-z')-$(uname -m)" ;;
esac

FROM=""
OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --from) FROM="$2"; shift ;;
        --out)  OUT="$2"; shift ;;
        --platform) PLATFORM="$2"; shift ;;
        -h|--help) sed -n '2,37p' "$0"; exit 0 ;;
        *) echo "usage: $0 --from <toolchain root> [--out <file.tar.xz>] [--platform <name>]" >&2; exit 2 ;;
    esac
    shift
done

ASSET="m68k-amigaos-gcc-${GCC_VERSION}-ndk3.9-${PLATFORM}.tar.xz"
[ -n "$OUT" ] || OUT="${TMPDIR:-/tmp}/$ASSET"

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

[ -n "$FROM" ] || {
    echo "!! --from <toolchain root> is required." >&2
    echo "   Build one with tools/build-toolchain.sh; do not package the" >&2
    echo "   fetch cache, that would be copying from the last copy." >&2
    exit 2
}

[ -x "$FROM/bin/m68k-amigaos-gcc" ] || {
    echo "!! $FROM has no bin/m68k-amigaos-gcc" >&2; exit 1; }
[ -f "$FROM/m68k-amigaos/ndk-include/exec/types.h" ] || {
    echo "!! $FROM has no NDK headers" >&2; exit 1; }

echo "==> packaging $FROM"
echo "    -> $OUT"

# ------------------------------------------------------------------- pack ----

# The archive carries the `opt/m68k-amigaos/` prefix, because that is the one
# path fetch-toolchain.sh strips.
#
# PREFER NOT RENAMING.  tools/build-toolchain.sh already builds into
# .../opt/m68k-amigaos, and a tree that is already in the right shape is
# archived with no path rewriting at all.  Renaming is the fallback, for a tree
# somewhere else, and it is the risky path: BOTH tars rewrite SYMLINK TARGETS
# as well as member names by default, so a source directory named
# `m68k-amigaos` silently turns every `m68k-amigaos-*` symlink target into
# `opt/m68k-amigaos-*` and ships a tarball of dangling links.  Measured, not
# assumed: bsdtar needs the `S` modifier and GNU tar needs `flags=r` to stop.
SRC_PARENT=$(cd "$FROM/.." && pwd)
SRC_BASE=$(basename "$FROM")

# Owner normalisation, so the archive does not carry whoever happened to build
# it.  The two tars share no spelling of this at all: --uid/--gid/--uname/--gname
# are bsdtar's and GNU tar rejects them outright, which is how the first Linux
# packaging run died after a 40-minute build.
IS_BSDTAR=0
tar --version 2>/dev/null | head -1 | grep -qi bsdtar && IS_BSDTAR=1

if [ "$IS_BSDTAR" = "1" ]; then
    TAR_FLAGS=(--uid 0 --gid 0 --uname root --gname root)
else
    TAR_FLAGS=(--owner=root:0 --group=root:0)
fi

if [ "$SRC_PARENT/$SRC_BASE" = "${SRC_PARENT%/opt}/opt/m68k-amigaos" ]; then
    TAR_MEMBER="$PREFIX"
    TAR_ROOT=$(cd "$SRC_PARENT/.." && pwd)
else
    TAR_MEMBER="$SRC_BASE"
    TAR_ROOT="$SRC_PARENT"
    if [ "$IS_BSDTAR" = "1" ]; then
        TAR_FLAGS+=(-s "|^${SRC_BASE}|${PREFIX}|S")
    else
        TAR_FLAGS+=(--transform "flags=r;s|^${SRC_BASE}|${PREFIX}|")
    fi
fi

if [ "$IS_BSDTAR" = "1" ]; then
    TAR_FLAGS+=(--no-mac-metadata --no-xattrs)
else
    # --sort=name buys GNU tar a stable member order; bsdtar has no equivalent.
    TAR_FLAGS+=(--sort=name)
fi

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
# Hard links matter here: 46 of the binaries are links to each other, and
# storing them as copies would add ~100 MB of duplicates.
COPYFILE_DISABLE=1 tar "${TAR_FLAGS[@]}" -cf - -C "$TAR_ROOT" "$TAR_MEMBER" \
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

# A dangling link is the one defect that survives every other check here: the
# tarball has the right size, the right entry count and the right sha256, and
# the compiler driver fails to find its own cc1 the first time anyone runs it.
DANGLING=$(find "$VTMP/$PREFIX" -type l ! -exec test -e {} \; -print 2>/dev/null)
if [ -n "$DANGLING" ]; then
    echo "!! the packaged tree has dangling symlinks:" >&2
    printf '%s\n' "$DANGLING" | head -20 >&2
    exit 1
fi

SRC_N=$(find "$FROM" | wc -l | tr -d ' ')
OUT_N=$(find "$VTMP/$PREFIX" | wc -l | tr -d ' ')
LINKS=$(find "$VTMP/$PREFIX" -type f -links +1 | wc -l | tr -d ' ')
SYMS=$(find "$VTMP/$PREFIX" -type l | wc -l | tr -d ' ')
[ "$SRC_N" = "$OUT_N" ] || { echo "!! entry count $OUT_N != $SRC_N" >&2; exit 1; }

SHA=$(sha256_of "$OUT")
SIZE=$(wc -c < "$OUT" | tr -d ' ')

echo "    $OUT_N entries, $LINKS hard links, $SYMS symlinks, identical to the source"
echo
echo "==> $ASSET"
echo "    size    $SIZE"
echo "    sha256  $SHA"
echo
echo "Pin it, then publish it:"
echo
echo "  1. in tools/fetch-toolchain.sh, set the sha256 for $PLATFORM to"
echo "     $SHA"
echo
echo "  2. gh release upload toolchain-m68k-amigaos-gcc-$GCC_VERSION '$OUT'"
echo
echo "     (or gh release create, with a body naming every asset, its"
echo "      platform and its sha256.  tools/toolchain-mirror-release-notes.md"
echo "      is the model to follow.)"
