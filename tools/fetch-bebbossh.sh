#!/usr/bin/env bash
#
# Fetch BebboSSH, so there is a real third-party SSH client to point at this
# stack.
#
#   tools/fetch-bebbossh.sh              # fetch, print the directory
#   tools/fetch-bebbossh.sh --print-dir  # just say where it would go
#   tools/fetch-bebbossh.sh --print-sha  # the pins, for a CI cache key
#   tools/fetch-bebbossh.sh --check      # verify the cache, no network
#   tools/fetch-bebbossh.sh --force      # re-fetch even if cached
#
# WHY THIS EXISTS
#
#   BebboSSH is an independent SSH2 suite for AmigaOS -- not Dropbear, not
#   OpenSSH, not libssh -- and it opens bsdsocket.library version 4.  That
#   makes it a CLIENT OF THIS LIBRARY written by somebody who has never seen
#   our source, which is the only kind of test that can find the mistakes our
#   own tests share an author with.  Two of this week's released defects came
#   from exactly that class of evidence.
#
#   It is also the closest thing to a peer: its crypto is hand-optimised 68020
#   assembly in libcryptossh.library, and so is ours, so a transfer rate
#   measured against it is a comparison rather than a handicap.
#
# WHAT IS NOT DONE HERE, DELIBERATELY
#
#   Nothing is vendored and nothing is linked.  BebboSSH is GPLv3+ (with some
#   SUPERCOP-derived maths in the public domain); this tree is MIT, and the
#   clean arrangement for a GPL program that talks to an MIT library is that it
#   stays a separate program we install and run.  So it is fetched on the
#   machine that needs it, it lands in a cache outside the working tree, and no
#   byte of it is ever committed here.
#
#   The archive is deleted after extraction.  Only the executables and the
#   three crypto libraries are kept.
#
# THE 68020 LIBRARY IS NOT THE DEFAULT, AND THAT MATTERS FOR TIMINGS
#
#   libcryptossh.library is the 68000 build.  The archive also ships a 68020
#   and a 68060 build under different names, and the ReadMe's instruction is to
#   RENAME the one that suits the CPU.  On the A1200 profile -- a 68EC020 --
#   the 68020 build is roughly twice as fast at ChaCha20-Poly1305 by the
#   author's own figures, so a run that stages the wrong file measures the
#   wrong thing.  --print-path takes the variant name, and the harness asks for
#   the 020 one by name rather than taking whatever "libcryptossh.library" is.
#
# A BAD HASH IS NOT A REASON TO PROCEED
#
#   The archive and every extracted file are pinned by sha256.  A mismatch
#   stops the script and writes nothing.  Fetching an SSH implementation over
#   an unverified path is how a test harness ends up being the attack.
#
# BEING A POLITE CLIENT
#
#   Aminet is volunteer-run.  One download, no retry, identified by User-Agent.
#
# WHERE IT LANDS
#
#   $AMINETXDUO_BEBBOSSH_CACHE, or ~/.cache/aminetxduo/bebbossh by default:
#
#       <cache>/<pin12>/<file>    the binaries
#       <cache>/current           symlink to it
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------- pins ----

# Aminet carries 1.45, uploaded by the author.  https://franke.ms/git/bebbo/
# bebbossh is the source and is at 1.46; it ships no binaries, and a harness
# that built the client itself would be testing our build of his code rather
# than the thing his users run.  So the pin is the release.
BEBBOSSH_VERSION="1.45"
BEBBOSSH_URL="https://aminet.net/comm/net/bebbossh.lha"
BEBBOSSH_SHA="b959b8431dd608b46a5f93ab59957bff238940d14667572143dfe47206df7e7c"

# member in archive | name in the cache | sha256
BEBBOSSH_FILES=(
  "bebbossh/bebbossh|bebbossh|643fc3f16455fb75c5546fa02d4e4992c0ca65b39ea73e6e42c2958666f4362c"
  "bebbossh/bebbosshd|bebbosshd|35ca8052014f4ea7aa77c6b3cc54e81593f142ef32689f86d0dcb8d0e31949d0"
  "bebbossh/bebboscp|bebboscp|d036f31180a1204c0823a7490960d97e361c9bf765ba932f747516799da14f46"
  "bebbossh/bebbosshkeygen|bebbosshkeygen|b515c4633194195c8c491f929f1e1f866f9008ad69efa845c8bf575f15c71c34"
  "bebbossh/libcryptossh.library|libcryptossh.library|aeade147a244f4269c115d108cfed16245f6fbc758c437695001b5c2ba1a6b75"
  "bebbossh/libcryptossh.library020|libcryptossh.library020|106844e95629d630434046393ccf09a1ce1580526a70200b33df1dcb59f38970"
  "bebbossh/libcryptossh.library060|libcryptossh.library060|bfc9db19c2c8d88fca2da942d9a9b725c87271e72e15d2e0a3a6132def41e9b2"
  # The terminfo source for the terminal bebbossh names in pty-req.  A host
  # that does not have it answers `tput cols` with "unknown terminal type",
  # which looks like a BebboSSH bug and is not one -- so the interactive
  # harness compiles this with tic and points the remote end at it.
  "bebbossh/xterm-amiga.src|xterm-amiga.src|ca16d1465718e6109a3d4077e590d227ce6e7a313f353f17fc770272eaeacad7"
)

# ---------------------------------------------------------------- options ----

MODE="fetch"
FORCE=0
WANT_NAME=""

while [ $# -gt 0 ]; do
    case "$1" in
        --print-dir)  MODE="printdir" ;;
        --print-sha)  MODE="printsha" ;;
        --print-version) MODE="printversion" ;;
        --check)      MODE="check" ;;
        --print-path) MODE="printpath"; WANT_NAME="${2:-}"; shift ;;
        --force)      FORCE=1 ;;
        -h|--help)    sed -n '2,66p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--print-dir|--print-sha|--print-version|--check|--print-path NAME] [--force]" >&2; exit 2 ;;
    esac
    shift
done

# --print-path is the machine-readable one, so its diagnostics go to stderr and
# stdout carries a path or nothing.
say() { [ "$MODE" = "printpath" ] && echo "$*" >&2 || echo "$*"; }

CACHE="${AMINETXDUO_BEBBOSSH_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aminetxduo/bebbossh}"

field() { printf '%s' "$1" | cut -d'|' -f"$2"; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

PIN12=$(printf '%s' "$BEBBOSSH_SHA" | cut -c1-12)
DIR="$CACHE/$PIN12"

# ------------------------------------------------------------- read-only ----

case "$MODE" in
printdir)     printf '%s\n' "$DIR"; exit 0 ;;
printversion) printf '%s\n' "$BEBBOSSH_VERSION"; exit 0 ;;
printsha)
    printf 'archive %s\n' "$BEBBOSSH_SHA"
    for f in "${BEBBOSSH_FILES[@]}"; do
        printf '%s %s\n' "$(field "$f" 2)" "$(field "$f" 3)"
    done
    exit 0 ;;
printpath)
    [ -n "$WANT_NAME" ] || { echo "--print-path needs a file name" >&2; exit 2; }
    for f in "${BEBBOSSH_FILES[@]}"; do
        [ "$(field "$f" 2)" = "$WANT_NAME" ] || continue
        [ -f "$DIR/$WANT_NAME" ] || {
            echo "!! $WANT_NAME is not fetched yet; run tools/fetch-bebbossh.sh" >&2
            exit 1
        }
        printf '%s\n' "$DIR/$WANT_NAME"
        exit 0
    done
    echo "!! $WANT_NAME is not part of the BebboSSH archive." >&2
    echo "   tools/fetch-bebbossh.sh --print-sha lists what is." >&2
    exit 2 ;;
check)
    rc=0
    for f in "${BEBBOSSH_FILES[@]}"; do
        name=$(field "$f" 2); want=$(field "$f" 3)
        if [ ! -f "$DIR/$name" ]; then
            printf '  %-26s not fetched\n' "$name"; continue
        fi
        got=$(sha256_of "$DIR/$name")
        if [ "$got" = "$want" ]; then
            printf '  %-26s ok\n' "$name"
        else
            printf '  %-26s MISMATCH (want %s, got %s)\n' "$name" "$want" "$got"
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
    echo "!! no LHA extractor found.  Install one of:" >&2
    echo "     lha        Debian/Ubuntu: lhasa      macOS: brew install lha" >&2
    echo "     bsdtar     Debian/Ubuntu: libarchive-tools" >&2
    echo "     7z         Debian/Ubuntu: p7zip-full" >&2
    exit 2
}

ALL_CACHED=1
for f in "${BEBBOSSH_FILES[@]}"; do
    name=$(field "$f" 2); want=$(field "$f" 3)
    [ -f "$DIR/$name" ] && [ "$(sha256_of "$DIR/$name")" = "$want" ] || ALL_CACHED=0
done
if [ "$FORCE" = "0" ] && [ "$ALL_CACHED" = "1" ]; then
    say "==> BebboSSH $BEBBOSSH_VERSION already at $DIR"
    printf '%s\n' "$DIR"
    exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/aminetxduo-bebbossh.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

UA="AmiNetXDuo/1.0 (+https://github.com/tinic/AmiNetXDuo) bebbossh-fetch"
ARCHIVE="$TMP/bebbossh.lha"

say "==> fetching BebboSSH $BEBBOSSH_VERSION"
say "    $BEBBOSSH_URL"

# No --retry: a volunteer archive having a bad day is worth reporting, not
# hammering.
if ! curl -fsSL -A "$UA" --connect-timeout 20 --max-time 300 \
          "$BEBBOSSH_URL" -o "$ARCHIVE" 2>"$TMP/curl.err"; then
    echo "!! could not fetch $BEBBOSSH_URL" >&2
    echo "   $(tail -1 "$TMP/curl.err" 2>/dev/null)" >&2
    exit 1
fi

got=$(sha256_of "$ARCHIVE")
if [ "$got" != "$BEBBOSSH_SHA" ]; then
    echo "" >&2
    echo "!! the BebboSSH archive does not match its pin -- refusing to install it." >&2
    echo "   want $BEBBOSSH_SHA" >&2
    echo "   got  $got" >&2
    echo "   Aminet may be carrying a newer release.  Check the version, read the" >&2
    echo "   changes, then update the pins in this file -- do not delete the check." >&2
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

for f in "${BEBBOSSH_FILES[@]}"; do
    member=$(field "$f" 1); name=$(field "$f" 2); want=$(field "$f" 3)
    [ -f "$TMP/x/$member" ] || {
        echo "!! $EXTRACTOR did not produce $member" >&2
        exit 1
    }
    # The archive hash already passed, so a wrong file here would mean the
    # extractor mangled it -- which is what this second pin catches.
    got=$(sha256_of "$TMP/x/$member")
    if [ "$got" != "$want" ]; then
        echo "!! $name does not match its pin (want $want, got $got)" >&2
        exit 1
    fi
done
say "    all file sha256 verified"

# Created only once everything has passed, so a failed run leaves no directory
# behind to make "nothing was written" a lie.
mkdir -p "$DIR"
for f in "${BEBBOSSH_FILES[@]}"; do
    member=$(field "$f" 1); name=$(field "$f" 2)
    mv "$TMP/x/$member" "$DIR/$name.tmp"
    chmod 0755 "$DIR/$name.tmp"
    mv "$DIR/$name.tmp" "$DIR/$name"
done

ln -sfn "$DIR" "$CACHE/current.tmp"
mv -f "$CACHE/current.tmp" "$CACHE/current"

say "==> BebboSSH $BEBBOSSH_VERSION at $DIR"
printf '%s\n' "$DIR"
