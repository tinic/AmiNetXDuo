#!/usr/bin/env bash
#
# Build the pinned m68k-amigaos cross toolchain from source.  The slow path:
# tools/fetch-toolchain.sh downloads a prebuilt copy of exactly this.
#
#   tools/build-toolchain.sh                     # build into ./build/toolchain
#   tools/build-toolchain.sh --package out.tar.xz
#   tools/build-toolchain.sh --fetch-only        # just clone the sources
#   tools/build-toolchain.sh --print-pins        # what this would build
#
# Build the pinned codewiz/AmigaPorts baseline, including its runtime
# libraries and NDK 3.2. Package identity is the upstream GCC version plus
# our revision from toolchain-version.sh. Every source and archive is pinned.
# macOS requires GNU rsync, coreutils, sed and make.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------ pins ----

# The amiga-gcc build system itself.
PIN_AMIGA_GCC_URL="https://github.com/AmigaPorts/m68k-amigaos-gcc"
PIN_AMIGA_GCC_SHA="de370a15e783261e82d0ba0ab809eff128356de1"

# projects/<name>|<remote that serves this commit>|<commit>|<branch it was on>
#
# The branch is recorded only so a human can find the commit again; nothing
# reads it as a pin.
PINS="
amiga-netinclude|https://github.com/AmigaPorts/amiga-netinclude|7d155ca3a3090afe3466f819dd178f2af6f5c176|main
binutils|https://github.com/AmigaPorts/binutils-gdb|15f4b0aa860f74022c6aff7ace15bc0a307d5510|amiga-2.46
clib2|https://github.com/AmigaPorts/clib2|78e05e255495382bbf880d0d37d20763d129f01e|master
fd2pragma|https://github.com/AmigaPorts/fd2pragma|bcffac5c89295baf1db033addc984312b4b4a722|master
fd2sfd|https://github.com/AmigaPorts/fd2sfd|a0ae60d140fdfd30f889810faea07cc6968d1fd0|master
gcc|https://github.com/tinic/gcc|c99b02bd168e1272c98de83e765c438ec28eca0b|amiga16.2
ira|https://github.com/AmigaPorts/ira|bb7c11384a32c409341fdb0d16d715b7959a0290|master
ixemul|https://github.com/AmigaPorts/ixemul|f0dd997994263c6ac783b563222a9f53a00c013c|master
lha|https://github.com/AmigaPorts/lha|979815dc8699a9c5018de067946737c83ec6f697|master
libdebug|https://github.com/AmigaPorts/libdebug|76c7dbfeaac79516117dc810e3ba9b34d5f54a69|master
libnix|https://github.com/AmigaPorts/libnix|df91c92564490bae1a5abd9e302f466aa7684bac|master
newlib-cygwin|https://github.com/AmigaPorts/newlib-cygwin|e07bea74897b6ee7b645602d66fcff6c6792d1a9|amiga
sfdc|https://github.com/AmigaPorts/sfdc|4e7afc044c1ff129574fcf3b839c3351edcfe2fb|master
vasm|https://github.com/AmigaPorts/vasm|de93f7dd067001532cff663730133601edb274cb|master
vbcc|https://github.com/AmigaPorts/vbcc|55a487d500dccccad806cf3916e90bcd95189ec9|master
vlink|https://github.com/AmigaPorts/vlink|ce46ccfda0e30eda5b12b9a5d0259d8eaefdefe9|master
aros-stuff|https://github.com/AmigaPorts/aros-stuff|9e09fb0f9f5150058c788fb42787927907b28a46|master
"

# Shared upstream compiler objects can also be fetched from the fork network.
# The optimization merge itself is served by tinic/gcc.
# shellcheck disable=SC2034 # Resolved dynamically as MIRROR_<module>.
MIRROR_gcc="https://github.com/AmigaPorts/gcc"

# NDK 3.2: the exact archive in codewiz's pinned default-archives table.
PIN_NDK_URL="https://aminet.net/dev/misc/NDK3.2.lha"
PIN_NDK_SHA256="96cabd4ad683dced632e147bf86dee0f50dcb1254386216c25c362916a6409bb"

# What the result must report, checked at the end.
. "$(dirname "$0")/toolchain-version.sh"
EXPECT_GCC_VERSION="$TC_GCC_VERSION"

# Use codewiz's dependency graph and the same complete toolchain target.
MAKE_TARGETS="all"

# --------------------------------------------------------------- options ----

HERE=$(cd "$(dirname "$0")" && pwd)
WORK="$PWD/build/toolchain"
PREFIX=""
PACKAGE=""
JOBS=""
FETCH_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --work)     WORK="$2"; shift ;;
        --prefix)   PREFIX="$2"; shift ;;
        --package)  PACKAGE="$2"; shift ;;
        -j)         JOBS="$2"; shift ;;
        -j*)        JOBS="${1#-j}" ;;
        --fetch-only) FETCH_ONLY=1 ;;
        --print-pins)
            printf 'amiga-gcc %s %s\n' "$PIN_AMIGA_GCC_SHA" "$PIN_AMIGA_GCC_URL"
            printf '%s\n' "$PINS" | while IFS='|' read -r n u s b; do
                [ -n "$n" ] || continue
                printf '%-17s %s %s (%s)\n' "$n" "$s" "$u" "$b"
            done
            printf 'NDK3.2.lha        sha256:%s %s\n' "$PIN_NDK_SHA256" "$PIN_NDK_URL"
            exit 0 ;;
        -h|--help)  sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--work DIR] [--prefix DIR] [--package FILE.tar.xz] [-j N] [--fetch-only]" >&2; exit 2 ;;
    esac
    shift
done

# The tarball is extracted so that <root>/bin/m68k-amigaos-gcc lands where
# fetch-toolchain.sh expects, which means the build has to happen under a
# directory literally named opt/m68k-amigaos.
[ -n "$PREFIX" ] || PREFIX="$WORK/opt/m68k-amigaos"

# ---------------------------------------------------------------- host ------

OS=$(uname -s)
ARCH=$(uname -m)

need() { command -v "$1" >/dev/null 2>&1 || { echo "!! missing required tool: $1" >&2; exit 2; }; }
for t in git curl make python3 tar; do need "$t"; done

if [ -z "$JOBS" ]; then
    if command -v nproc >/dev/null 2>&1; then JOBS=$(nproc)
    elif command -v sysctl >/dev/null 2>&1; then JOBS=$(sysctl -n hw.ncpu)
    else JOBS=4; fi
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

CC_PROBE="${CC:-cc}"

case "$OS" in
Darwin)
    BREW=$(command -v brew >/dev/null 2>&1 && brew --prefix || echo /opt/homebrew)
    [ -d "$BREW" ] || { echo "!! Homebrew not found; see HOST REQUIREMENTS" >&2; exit 2; }

    # configure looks for gmp/mpfr in the default search path and Homebrew is
    # not on it.
    export CPPFLAGS="-I$BREW/include ${CPPFLAGS:-}"
    export LDFLAGS="-L$BREW/lib ${LDFLAGS:-}"
    export PATH="$BREW/bin:$BREW/opt/coreutils/libexec/gnubin:$BREW/opt/gnu-sed/libexec/gnubin:$BREW/opt/texinfo/bin:$PATH"

    # libnix installs with `rsync --exclude`, and macOS's openrsync gets those
    # patterns wrong: the build succeeds and the library is short some objects.
    # Capture the banner, then match it.  `rsync --version | head -1 | grep -q`
    # is a race: grep -q exits on the match, head exits with it, and rsync dies
    # of SIGPIPE before it has finished writing -- which under `set -o pipefail`
    # reads as "not GNU rsync".  It fires perhaps one run in three with GNU
    # rsync 3.5.0 installed and first on PATH, and it is the only thing standing
    # between a Mac and a toolchain build.
    RSYNC_BANNER=$( { rsync --version 2>/dev/null || true; } | sed -n 1p )
    case "$RSYNC_BANNER" in
        *rsync*version*protocol*) ;;
        *)
            echo "!! rsync on PATH is not GNU rsync (macOS ships openrsync)." >&2
            echo "   brew install rsync, and make sure $BREW/bin comes first." >&2
            exit 2 ;;
    esac
    for t in lha makeinfo bison flex m4; do need "$t"; done
    ;;
Linux)
    # The system GMP/MPFR/MPC are where configure looks; rsync is GNU rsync.
    #
    # bison, flex and m4 are checked HERE and not left to the build.  binutils
    # generates its parsers from .y and .l, and a missing m4 does not merely
    # fail: binutils' configure records `missing m4` in the Makefile, so
    # installing m4 afterwards fixes nothing until the whole binutils build
    # directory is deleted.  The diagnostic at that point is
    # "bison: m4 subprocess failed", three levels down a make log.
    for t in makeinfo bison flex m4; do need "$t"; done
    command -v lha >/dev/null 2>&1 || command -v lhasa >/dev/null 2>&1 || {
        echo "!! need lha or lhasa to unpack the NDK" >&2; exit 2; }
    ;;
*)
    echo "!! unsupported build host: $OS.  Linux and macOS only." >&2
    exit 2 ;;
esac

# GCC needs GMP, MPFR and MPC HEADERS, and a host that has one does not
# necessarily have the other two: Debian 13 keeps gmp.h under
# /usr/include/<triple> and carries libmpfr.so.6 with no mpfr.h at all.  The
# diagnostic without this probe is `configure: error: Building GCC requires GMP
# 4.2+` an hour in, AFTER binutils has built and installed.  Ask the compiler
# rather than looking for files: gmp.h sits on a search path no `ls` guesses.
# After the host block, which puts Homebrew on CPPFLAGS on macOS.
MPX_MISSING=""
# shellcheck disable=SC2086  # CPPFLAGS is a flag list and must split
for h in gmp mpfr mpc; do
    printf '#include <%s.h>\nint main(void){return 0;}\n' "$h" |
        "$CC_PROBE" ${CPPFLAGS:-} -fsyntax-only -x c - >/dev/null 2>&1 ||
        MPX_MISSING="$MPX_MISSING $h.h"
done
if [ -n "$MPX_MISSING" ]; then
    echo "!! GCC cannot be configured without$MPX_MISSING" >&2
    echo "   Debian/Ubuntu: libgmp-dev libmpfr-dev libmpc-dev" >&2
    echo "   macOS:         brew install gmp mpfr libmpc" >&2
    echo "   No packages: build them into a prefix, then export" >&2
    echo "   CPPFLAGS=-I<prefix>/include and LDFLAGS=-L<prefix>/lib." >&2
    exit 2
fi

# newlib archives its objects in shell-glob order, and glob order follows the
# collation of the locale.  Under en_US.UTF-8 on Linux `memcpy` sorts before
# `__udivsi3`; under C, and on macOS in any locale, it sorts after.  ld pulls
# members in archive order, so the same source linked against the two libc.a
# files places them differently and every shipped image changes bytes.  C
# everywhere gives one order, the one every earlier asset has.
export LC_ALL=C

export CFLAGS="-O2 ${CFLAGS:-}"
export CXXFLAGS="$CFLAGS"

echo "==> host $OS/$ARCH, -j$JOBS"
echo "==> work   $WORK"
echo "==> prefix $PREFIX"

# --------------------------------------------------------------- sources ----

mkdir -p "$WORK"
SRC="$WORK/amiga-gcc"

# Fetch one commit by name.  --depth 1 of a named object is the whole point:
# no branch is consulted, so nothing can drift, and gcc's history stays
# unfetched.  Falls back to a deepening branch fetch for a server that refuses
# uploadpack.allowReachableSHA1InWant.
fetch_pinned() {
    local dir="$1" url="$2" sha="$3" branch="$4" mirror="${5:-}"

    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$sha" ]; then
        echo "    $(basename "$dir") already at ${sha:0:12}"
        return 0
    fi

    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    [ -n "$mirror" ] && git -C "$dir" remote add mirror "$mirror"

    local remote
    for remote in origin ${mirror:+mirror}; do
        echo "    $(basename "$dir") <- $remote ${sha:0:12}"
        if git -C "$dir" fetch -q --depth 1 "$remote" "$sha" 2>/dev/null; then
            git -C "$dir" checkout -q FETCH_HEAD
            return 0
        fi
        local depth=32
        while [ "$depth" -le 4096 ]; do
            git -C "$dir" fetch -q --depth "$depth" "$remote" "$branch" 2>/dev/null || break
            if git -C "$dir" cat-file -e "$sha^{commit}" 2>/dev/null; then
                git -C "$dir" checkout -q "$sha"
                return 0
            fi
            depth=$((depth * 2))
        done
    done

    echo "!! could not fetch $sha for $(basename "$dir") from $url" >&2
    return 1
}

echo "==> sources"
fetch_pinned "$SRC" "$PIN_AMIGA_GCC_URL" "$PIN_AMIGA_GCC_SHA" master

mkdir -p "$SRC/projects" "$SRC/download"
printf '%s\n' "$PINS" | while IFS='|' read -r name url sha branch; do
    [ -n "$name" ] || continue
    eval "mirror=\${MIRROR_$(printf '%s' "$name" | tr -c 'A-Za-z0-9' '_'):-}"
    fetch_pinned "$SRC/projects/$name" "$url" "$sha" "$branch" "$mirror"
done

# amiga-gcc consults .repos for anything not already cloned.  Everything the
# target list needs is pinned above, so this only exists so a stray `make
# update` or an added target reaches a remote that is still alive rather than
# the dead GitHub ones in the shipped default.
{
    printf '%s\n' "$PINS" | while IFS='|' read -r name url sha branch; do
        [ -n "$name" ] || continue
        printf '%-18s %s\t%s\n' "$name" "$url" "$branch"
    done
} > "$SRC/.repos"

# --- NDK ---------------------------------------------------------------------

NDK_LHA="$SRC/download/NDK3.2.lha"
if [ ! -f "$NDK_LHA" ] || [ "$(sha256_of "$NDK_LHA")" != "$PIN_NDK_SHA256" ]; then
    echo "    NDK3.2.lha <- $PIN_NDK_URL"
    curl -fsSL --retry 3 --retry-connrefused --retry-delay 2 --connect-timeout 30 \
         "$PIN_NDK_URL" -o "$NDK_LHA.tmp"
    got=$(sha256_of "$NDK_LHA.tmp")
    if [ "$got" != "$PIN_NDK_SHA256" ]; then
        echo "!! NDK3.2.lha does not match its pin" >&2
        echo "   want $PIN_NDK_SHA256" >&2
        echo "   got  $got" >&2
        rm -f "$NDK_LHA.tmp"
        exit 1
    fi
    mv "$NDK_LHA.tmp" "$NDK_LHA"
fi
echo "    NDK3.2.lha sha256 verified"

[ "$FETCH_ONLY" = "1" ] && { echo "==> --fetch-only, stopping"; exit 0; }

# The pinned AmigaPorts build system carries the HUNK/LTO, startup, header
# and pthread fixes. Build those reviewed sources directly; do not apply
# the historical patches for the older bebbo build system.

# ----------------------------------------------------------------- build ----

mkdir -p "$PREFIX"
MAKE_TOOL=$(command -v gmake || command -v make)
echo "==> make $MAKE_TARGETS"
( cd "$SRC" && "$MAKE_TOOL" PREFIX="$PREFIX" -j"$JOBS" $MAKE_TARGETS )
# These are the remaining package steps in codewiz's toolchain.yml.
( cd "$SRC" && "$MAKE_TOOL" PREFIX="$PREFIX" -j4 all-sdk )
( cd "$SRC" && "$MAKE_TOOL" PREFIX="$PREFIX" -j"$JOBS" vbcc vlink )
# zlib otherwise runs the host ranlib, which silently empties HUNK archives
# on macOS. Override it for these target libraries and their recursive makes.
( cd "$SRC" && "$MAKE_TOOL" PREFIX="$PREFIX" -j"$JOBS" zlib libpng RANLIB=m68k-amigaos-ranlib )

# Catch the precise stale allocator that escaped the 16.2.2 packages even
# though their source checkout carried the fix.  This checks the *installed*
# libraries, not merely the pinned source or build logs.
python3 "$HERE/check-toolchain-malloc.py" "$PREFIX"
python3 "$HERE/toolchain-branch-selftest.py" "$PREFIX"

# ------------------------------------------------------------- post-build ----

# The same repairs fetch-toolchain.sh applies to a downloaded tree, applied
# here so a locally built one is equally sound before anything links against
# it. The pinned upstream argv fix should make the binary repair a no-op;
# the immediate --check is the gate proving that every compiled multilib has
# the reviewed shape. Both are idempotent, so an upstream-fixed tree needs no
# special branch here.
if [ -f "$HERE/fix-toolchain-crt0.py" ]; then
    echo "==> crt0 repair"
    python3 "$HERE/fix-toolchain-crt0.py" "$PREFIX"
    echo "==> crt0 verification"
    python3 "$HERE/fix-toolchain-crt0.py" "$PREFIX" --check
fi
if [ -f "$HERE/fix-toolchain-inline-const.py" ]; then
    echo "==> NDK inline register ABI"
    python3 "$HERE/fix-toolchain-inline-const.py" "$PREFIX"
fi

# Make plain ar, ranlib and nm plugin-aware.  binutils auto-loads whatever it
# finds in <prefix>/lib/bfd-plugins; GCC installs liblto_plugin.so under
# libexec/gcc/<target>/<version>/ and nothing links the two, so out of the box
# `ar` writes an archive index whose only entry is ___gnu_lto_slim and the LTO
# link fails on the first symbol that lives in a static library:
#
#   ld: ...ltrans0.ltrans.o: undefined reference to `ami_rt_cpu_select'
#
# Setting CMAKE_AR to gcc-ar fixes our CMake builds and nothing else; the
# conformance suite and the dropbear client call the bare tools.  Every
# distribution ships this symlink, and it is what makes the bare tools work.
LTO_PLUGIN=$(ls "$PREFIX"/libexec/gcc/m68k-amigaos/*/liblto_plugin.so 2>/dev/null | head -1)
if [ -z "$LTO_PLUGIN" ]; then
    echo "!! no liblto_plugin.so under $PREFIX/libexec; -flto cannot work" >&2
    exit 1
fi
LTO_PLUGIN_REL="../../libexec/gcc/m68k-amigaos/$(basename "$(dirname "$LTO_PLUGIN")")/liblto_plugin.so"
mkdir -p "$PREFIX/lib/bfd-plugins"
ln -sf "$LTO_PLUGIN_REL" "$PREFIX/lib/bfd-plugins/liblto_plugin.so"
echo "==> lib/bfd-plugins/liblto_plugin.so -> $LTO_PLUGIN_REL"

VER=$("$PREFIX/bin/m68k-amigaos-gcc" -dumpfullversion 2>/dev/null || echo "")
if [ "$VER" != "$EXPECT_GCC_VERSION" ]; then
    echo "!! expected GCC $EXPECT_GCC_VERSION, got '${VER:-nothing}'" >&2
    exit 1
fi
[ -f "$PREFIX/m68k-amigaos/ndk-include/exec/types.h" ] || {
    echo "!! no NDK headers under $PREFIX/m68k-amigaos/ndk-include" >&2; exit 1; }

( cd "$SRC" && scripts/manifest ) > "$PREFIX/.manifest"
printf '%s\n' "$TC_ASSET_VERSION" > "$PREFIX/.aminetxduo-toolchain-version"
echo "==> built GCC $VER at $PREFIX"

# --------------------------------------------------------------- package ----

if [ -n "$PACKAGE" ]; then
    # One packer for both routes.  It renames the top directory to
    # opt/m68k-amigaos, preserves the 46 hard links between the binaries
    # (storing them as copies costs ~100 MB), and unpacks the result to check
    # it against the tree it came from before printing a sha256 anyone will
    # pin.  None of that is worth writing twice.
    exec "$HERE/package-toolchain-mirror.sh" --from "$PREFIX" --out "$PACKAGE"
fi
