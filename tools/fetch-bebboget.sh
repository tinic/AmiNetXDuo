#!/usr/bin/env bash
#
# Fetch bebboget, a second third-party client of this library.
#
#   tools/fetch-bebboget.sh              # fetch, print the directory
#   tools/fetch-bebboget.sh --print-dir  # just say where it would go
#   tools/fetch-bebboget.sh --print-sha  # the pins, for a CI cache key
#   tools/fetch-bebboget.sh --check      # verify the cache, no network
#   tools/fetch-bebboget.sh --force      # re-fetch even if cached
#
# WHY THIS ONE AS WELL AS BebboSSH
#
#   bebboget is an HTTPS downloader by the same author, and it carries its OWN
#   TLS -- SSL 3.0 through TLS 1.3, ChaCha20-Poly1305 and the AES suites, with
#   the hot loops in 68k assembly.  It depends on nothing but
#   bsdsocket.library.
#
#   That makes it the fairest comparison available for src/tools/fetch, which
#   does the same job through our tls.library: same URL, same bytes, same
#   emulated CPU, two independent TLS stacks over one transport.  It is also a
#   different shape of load from BebboSSH -- one long unidirectional read
#   instead of a request/response ping-pong -- so it leans on the receive path
#   the way nothing else here does.
#
#   The author's own note is "not too much testing yet".  That is a reason to
#   run it, not a reason to skip it: an under-exercised client is exactly the
#   kind that trips over an ABI assumption, and when it does the first question
#   is still whether the fault is ours.
#
# WHAT IS NOT DONE HERE, DELIBERATELY
#
#   Nothing is vendored and nothing is linked.  bebboget is GPLv3+ (with some
#   SUPERCOP-derived maths in the public domain); this tree is MIT.  It is
#   fetched on the machine that needs it, into a cache outside the working
#   tree, and no byte of it is committed here.  lib/libbebboget.a is in the
#   archive and is NOT extracted: it is the one file somebody might be tempted
#   to link, and linking it would put this library under the GPL.
#
# TWO BINARIES, AND THE 68020 ONE IS NOT THE DEFAULT NAME
#
#   `bebboget` is the 68020+ build and `bebboget00` the 68000 one -- the
#   opposite of BebboSSH's naming, where the plain name is the 68000 build.
#   On the author's figures the difference is 28.9 against 51.5 KB/s on a
#   68020, so staging the wrong one measures the wrong thing.  Both are kept
#   and named as the archive names them.
#
# A BAD HASH IS NOT A REASON TO PROCEED
#
#   The archive and every extracted file are pinned by sha256.  A mismatch
#   stops the script and writes nothing.
#
# WHERE IT LANDS
#
#   $AMINETXDUO_BEBBOGET_CACHE, or ~/.cache/aminetxduo/bebboget by default.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------- pins ----

BEBBOGET_VERSION="1.11"
BEBBOGET_URL="https://aminet.net/comm/net/bebboget.lha"
BEBBOGET_SHA="b76e085ca05ddcff2a4b74b7d9ce0d2982edac8ef9ec7f0bf42ab6d57a2469dc"

# member in archive | name in the cache | sha256
BEBBOGET_FILES=(
  "bebboget/bebboget|bebboget|2fe745b4efde1f6eb887455fb71beba4b3c1570bda13f7283d7a5f57c9f54918"
  "bebboget/bebboget00|bebboget00|5d2e1720b5bfd072553e59e36b790072f7e14df622e3fd916ce9e48e7f6acb85"
  "bebboget/installcerts|installcerts|3e549b2a9b8507cf019aa8cd4de1433efcb60967d451c48643708ce08eeb1a34"
)

# ---------------------------------------------------------------- options ----

MODE="fetch"
FORCE=0
WANT_NAME=""

while [ $# -gt 0 ]; do
    case "$1" in
        --print-dir)     MODE="printdir" ;;
        --print-sha)     MODE="printsha" ;;
        --print-version) MODE="printversion" ;;
        --check)         MODE="check" ;;
        --print-path)    MODE="printpath"; WANT_NAME="${2:-}"; shift ;;
        --force)         FORCE=1 ;;
        -h|--help)       sed -n '2,58p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--print-dir|--print-sha|--print-version|--check|--print-path NAME] [--force]" >&2; exit 2 ;;
    esac
    shift
done

say() { [ "$MODE" = "printpath" ] && echo "$*" >&2 || echo "$*"; }

CACHE="${AMINETXDUO_BEBBOGET_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aminetxduo/bebboget}"

field() { printf '%s' "$1" | cut -d'|' -f"$2"; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

PIN12=$(printf '%s' "$BEBBOGET_SHA" | cut -c1-12)
DIR="$CACHE/$PIN12"

# ------------------------------------------------------------- read-only ----

case "$MODE" in
printdir)     printf '%s\n' "$DIR"; exit 0 ;;
printversion) printf '%s\n' "$BEBBOGET_VERSION"; exit 0 ;;
printsha)
    printf 'archive %s\n' "$BEBBOGET_SHA"
    for f in "${BEBBOGET_FILES[@]}"; do
        printf '%s %s\n' "$(field "$f" 2)" "$(field "$f" 3)"
    done
    exit 0 ;;
printpath)
    [ -n "$WANT_NAME" ] || { echo "--print-path needs a file name" >&2; exit 2; }
    for f in "${BEBBOGET_FILES[@]}"; do
        [ "$(field "$f" 2)" = "$WANT_NAME" ] || continue
        [ -f "$DIR/$WANT_NAME" ] || {
            echo "!! $WANT_NAME is not fetched yet; run tools/fetch-bebboget.sh" >&2
            exit 1
        }
        printf '%s\n' "$DIR/$WANT_NAME"
        exit 0
    done
    echo "!! $WANT_NAME is not one of the files this script keeps." >&2
    echo "   tools/fetch-bebboget.sh --print-sha lists them." >&2
    exit 2 ;;
check)
    rc=0
    for f in "${BEBBOGET_FILES[@]}"; do
        name=$(field "$f" 2); want=$(field "$f" 3)
        if [ ! -f "$DIR/$name" ]; then
            printf '  %-20s not fetched\n' "$name"; continue
        fi
        got=$(sha256_of "$DIR/$name")
        if [ "$got" = "$want" ]; then
            printf '  %-20s ok\n' "$name"
        else
            printf '  %-20s MISMATCH (want %s, got %s)\n' "$name" "$want" "$got"
            rc=1
        fi
    done
    exit "$rc" ;;
esac

# ------------------------------------------------------------------ fetch ----

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing required tool: $1" >&2; exit 2; }; }
need curl

EXTRACTOR=""
for cand in lha bsdtar 7z 7zz; do
    command -v "$cand" >/dev/null 2>&1 && { EXTRACTOR="$cand"; break; }
done
[ -n "$EXTRACTOR" ] || {
    echo "!! no LHA extractor found.  Install lha, bsdtar or 7z." >&2
    exit 2
}

ALL_CACHED=1
for f in "${BEBBOGET_FILES[@]}"; do
    name=$(field "$f" 2); want=$(field "$f" 3)
    [ -f "$DIR/$name" ] && [ "$(sha256_of "$DIR/$name")" = "$want" ] || ALL_CACHED=0
done
if [ "$FORCE" = "0" ] && [ "$ALL_CACHED" = "1" ]; then
    say "==> bebboget $BEBBOGET_VERSION already at $DIR"
    printf '%s\n' "$DIR"
    exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/aminetxduo-bebboget.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

UA="AmiNetXDuo/1.0 (+https://github.com/tinic/AmiNetXDuo) bebboget-fetch"
ARCHIVE="$TMP/bebboget.lha"

say "==> fetching bebboget $BEBBOGET_VERSION"
say "    $BEBBOGET_URL"

if ! curl -fsSL -A "$UA" --connect-timeout 20 --max-time 300 \
          "$BEBBOGET_URL" -o "$ARCHIVE" 2>"$TMP/curl.err"; then
    echo "!! could not fetch $BEBBOGET_URL" >&2
    echo "   $(tail -1 "$TMP/curl.err" 2>/dev/null)" >&2
    exit 1
fi

got=$(sha256_of "$ARCHIVE")
if [ "$got" != "$BEBBOGET_SHA" ]; then
    echo "" >&2
    echo "!! the bebboget archive does not match its pin -- refusing to install it." >&2
    echo "   want $BEBBOGET_SHA" >&2
    echo "   got  $got" >&2
    echo "   Aminet may be carrying a newer release.  Check the version, read the" >&2
    echo "   changes, then update the pins here -- do not delete the check." >&2
    echo "   Nothing was written to $CACHE." >&2
    exit 1
fi
say "    archive sha256 verified"

mkdir -p "$TMP/x"
case "$EXTRACTOR" in
    lha)    ( cd "$TMP/x" && lha xq2 "$ARCHIVE" ) >/dev/null 2>&1 || true ;;
    bsdtar) bsdtar xf "$ARCHIVE" -C "$TMP/x" >/dev/null 2>&1 || true ;;
    7z|7zz) "$EXTRACTOR" x -y -o"$TMP/x" "$ARCHIVE" >/dev/null 2>&1 || true ;;
esac

for f in "${BEBBOGET_FILES[@]}"; do
    member=$(field "$f" 1); name=$(field "$f" 2); want=$(field "$f" 3)
    [ -f "$TMP/x/$member" ] || { echo "!! $EXTRACTOR did not produce $member" >&2; exit 1; }
    got=$(sha256_of "$TMP/x/$member")
    if [ "$got" != "$want" ]; then
        echo "!! $name does not match its pin (want $want, got $got)" >&2
        exit 1
    fi
done
say "    all file sha256 verified"

mkdir -p "$DIR"
for f in "${BEBBOGET_FILES[@]}"; do
    member=$(field "$f" 1); name=$(field "$f" 2)
    mv "$TMP/x/$member" "$DIR/$name.tmp"
    chmod 0755 "$DIR/$name.tmp"
    mv "$DIR/$name.tmp" "$DIR/$name"
done

ln -sfn "$DIR" "$CACHE/current.tmp"
mv -f "$CACHE/current.tmp" "$CACHE/current"

say "==> bebboget $BEBBOGET_VERSION at $DIR"
printf '%s\n' "$DIR"
