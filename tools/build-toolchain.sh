#!/usr/bin/env bash
#
# Build the pinned m68k-amigaos cross toolchain from source.  The slow path:
# tools/fetch-toolchain.sh downloads a prebuilt copy of exactly this.
#
#   tools/build-toolchain.sh                     # build into ./build-toolchain
#   tools/build-toolchain.sh --package out.tar.xz
#   tools/build-toolchain.sh --fetch-only        # just clone the sources
#   tools/build-toolchain.sh --print-pins        # what this would build
#
# GCC 16.2.0b + binutils 2.39.0 + NDK 3.9, PINNED BY COMMIT, not by branch.
# The binutils pairing is deliberate.  2.46 works: with the two diffs in
# tools/patches/binutils-2.46/ it builds this tree at -DAMINETXDUO_LTO=ON and
# emits the four shipped images BYTE FOR BYTE as 2.39 does.  That is the whole
# reason the pin stays where it is -- identical output is no reason to
# republish a prebuilt toolchain tools/fetch-toolchain.sh pins by sha256 on two
# platforms.  Moving it is the one PINS line below; the patch set it selects is
# already written.  Each entry records the remote verified to serve its pin:
# bebbo's GitHub repos are gone and Codeberg does not carry every branch.
#
# On macOS GNU rsync is REQUIRED: openrsync mishandles libnix's --exclude
# patterns and silently installs the wrong set of objects.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------ pins ----

# The amiga-gcc build system itself.
PIN_AMIGA_GCC_URL="https://codeberg.org/bebbo/amiga-gcc"
PIN_AMIGA_GCC_SHA="86f8ba62f7a5035e309600c86962681e1cbacccb"

# projects/<name>|<remote that serves this commit>|<commit>|<branch it was on>
#
# The branch is recorded only so a human can find the commit again; nothing
# reads it as a pin.  `binutils` has no mirror: only franke.ms carries 2.39.0.
PINS="
binutils|https://franke.ms/git/bebbo/binutils-gdb|ab4e5183f56fd83165356a03c890bf0b681d7535|amiga-2.39.0
gcc|https://franke.ms/git/bebbo/gcc|60f21496319754a0e35b1a8e52df9abbac188065|amiga16.2
libnix|https://franke.ms/git/bebbo/libnix|b7268e35510b8b7b4ccdad67fbcbb25e73189aef|master
sfdc|https://franke.ms/git/bebbo/sfdc|5d4efca359e949547553463f5873778bd85e5506|master
fd2sfd|https://franke.ms/git/bebbo/fd2sfd|7f14d7f15aac2b8426f577f838069e53bf6008ea|master
amiga-netinclude|https://franke.ms/git/bebbo/amiga-netinclude|b9a8d2cdd410dbb896a93bc9c64b253be436dd89|master
aros-stuff|https://franke.ms/git/bebbo/aros-stuff|c0a06b4ccd13f52d1518540aa88a450d70581458|master
fd2pragma|https://github.com/adtools/fd2pragma|8c0f352c348a3252f84170eab737919372562e82|master
vasm|https://github.com/mheyer32/vasm|bb048d9d3cf54d5e38c643182a0ff55b552f65be|master
"

# A second remote to try, keyed by project name.  Only gcc has one that was
# confirmed to serve the pinned object.  Read by name construction below, which
# is why shellcheck cannot see the use.
# shellcheck disable=SC2034
MIRROR_gcc="https://codeberg.org/bebbo/gcc"

# NDK 3.9.  A single unmirrored third-party host, which is why the built
# toolchain is worth publishing at all.  Pinned by content.
PIN_NDK_URL="http://hp.alinea-computer.de/AmigaOS/NDK39.lha"
PIN_NDK_SHA256="ca5d8f923158d69a9c15b59d6e1580555ca6c0a48be21c5226c71f90fc927ca6"

# What the result must report, checked at the end.
EXPECT_GCC_VERSION="16.2.0b"

# amiga-gcc target list.  NOT `all`: that drags in gdb, which wants a host
# GMP/MPFR it does not need here, and libSDL12, which this project never uses.
MAKE_TARGETS="binutils gcc gprof fd2sfd fd2pragma sfdc vasm libnix libgcc libpthread ndk ndk13"

# --------------------------------------------------------------- options ----

HERE=$(cd "$(dirname "$0")" && pwd)
WORK="$PWD/build-toolchain"
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
            printf 'NDK3.9.lha        sha256:%s %s\n' "$PIN_NDK_SHA256" "$PIN_NDK_URL"
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

# binutils 2.39 has an old-style function-pointer mismatch that every current
# compiler rejects OUTRIGHT: objdump.c:4196 assigns dummy_fprintf, which
# ignores its arguments and returns int, to memory_error_func, which returns
# void.  clang 16+ and GCC 14+ both promoted that from a warning to an error,
# so --disable-werror does not reach it.  They spell the switch that demotes it
# differently, and neither accepts the other's spelling, so ask the compiler
# which one it has rather than deciding from $OS -- this failed on Linux/GCC 14
# exactly as it failed on macOS/clang, and a uname gate would have hidden that.
#
# The probe compiles the offending assignment, not an empty file: GCC accepts
# an unrecognised -Wno-anything in silence unless some other diagnostic fires,
# so "does the compiler take this flag" answers yes to both spellings and picks
# the wrong one.  "Does this flag make THIS code compile" has one right answer.
CC_PROBE="${CC:-cc}"
NO_PTR_WARN=""
PROBE_SRC='typedef void (*memerr)(int, unsigned long, void *);
int dummy_fprintf(void *s, const char *f, ...);
memerr p;
int main(void) { p = dummy_fprintf; return 0; }'
for f in "" -Wno-incompatible-function-pointer-types -Wno-incompatible-pointer-types; do
    if printf '%s\n' "$PROBE_SRC" | "$CC_PROBE" ${f:+"$f"} -c -x c - -o /dev/null >/dev/null 2>&1; then
        NO_PTR_WARN="$f"
        break
    fi
done
if [ -n "$NO_PTR_WARN" ]; then
    echo "==> binutils 2.39 objdump.c needs $NO_PTR_WARN"
elif printf '%s\n' "$PROBE_SRC" | "$CC_PROBE" -c -x c - -o /dev/null >/dev/null 2>&1; then
    : # this compiler still only warns
else
    echo "!! $CC_PROBE rejects binutils 2.39's objdump.c:4196 and neither" >&2
    echo "   -Wno-incompatible-function-pointer-types nor" >&2
    echo "   -Wno-incompatible-pointer-types demotes it." >&2
    exit 2
fi

# GCC needs GMP, MPFR and MPC HEADERS, and a host that has one does not
# necessarily have the other two: Debian 13 keeps gmp.h under
# /usr/include/<triple> and carries libmpfr.so.6 with no mpfr.h at all.  The
# diagnostic without this probe is `configure: error: Building GCC requires GMP
# 4.2+` an hour in, AFTER binutils has built and installed.  Ask the compiler
# rather than looking for files: gmp.h sits on a search path no `ls` guesses.
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

case "$OS" in
Darwin)
    BREW=$(command -v brew >/dev/null 2>&1 && brew --prefix || echo /opt/homebrew)
    [ -d "$BREW" ] || { echo "!! Homebrew not found; see HOST REQUIREMENTS" >&2; exit 2; }

    # configure looks for gmp/mpfr in the default search path and Homebrew is
    # not on it.
    export CPPFLAGS="-I$BREW/include ${CPPFLAGS:-}"
    export LDFLAGS="-L$BREW/lib ${LDFLAGS:-}"
    export PATH="$BREW/bin:$BREW/opt/texinfo/bin:$PATH"

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
    # Nothing beyond the shared objdump.c switch: the system zlib and gmp/mpfr
    # are where configure looks, and rsync is GNU rsync.
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

export CFLAGS="-Os $NO_PTR_WARN ${CFLAGS:-}"
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

NDK_LHA="$SRC/download/NDK3.9.lha"
if [ ! -f "$NDK_LHA" ] || [ "$(sha256_of "$NDK_LHA")" != "$PIN_NDK_SHA256" ]; then
    echo "    NDK3.9.lha <- $PIN_NDK_URL"
    curl -fsSL --retry 3 --retry-connrefused --retry-delay 2 --connect-timeout 30 \
         "$PIN_NDK_URL" -o "$NDK_LHA.tmp"
    got=$(sha256_of "$NDK_LHA.tmp")
    if [ "$got" != "$PIN_NDK_SHA256" ]; then
        echo "!! NDK3.9.lha does not match its pin" >&2
        echo "   want $PIN_NDK_SHA256" >&2
        echo "   got  $got" >&2
        rm -f "$NDK_LHA.tmp"
        exit 1
    fi
    mv "$NDK_LHA.tmp" "$NDK_LHA"
fi
echo "    NDK3.9.lha sha256 verified"

[ "$FETCH_ONLY" = "1" ] && { echo "==> --fetch-only, stopping"; exit 0; }

# --------------------------------------------------------------- patches ----

# binutils 2.39 bundles a zlib whose headers collide with a current macOS SDK's
# _stdio.h and the build dies in libz_a-zutil.o.  The system zlib is fine and
# is what everything else links anyway, on both hosts, so this is unconditional
# rather than one more thing that is only exercised on one platform.
if ! grep -q 'with-system-zlib' "$SRC/Makefile"; then
    python3 - "$SRC/Makefile" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
anchor = "CONFIG_BINUTILS =--prefix=$(PREFIX)"
i = s.index(anchor)
j = s.index("\n", i) + 1
s = s[:j] + "CONFIG_BINUTILS += --with-system-zlib\n" + s[j:]
open(p, "w").write(s)
PY
    echo "==> patched CONFIG_BINUTILS += --with-system-zlib"
fi

# MAKE_TARGETS deliberately leaves gdb out -- it wants a host GMP and MPFR it
# does not need here -- but the top-level configure still runs for it, and from
# binutils 2.46 that configure ERRORS on a host with gmp.h and no mpfr.h rather
# than dropping the directory.  Nothing this builds comes out of gdb, gprof
# included, so switch it off where the decision already was.
if ! grep -q 'disable-gdb' "$SRC/Makefile"; then
    python3 - "$SRC/Makefile" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
anchor = "CONFIG_BINUTILS =--prefix=$(PREFIX)"
i = s.index(anchor)
j = s.index("\n", i) + 1
s = s[:j] + "CONFIG_BINUTILS += --disable-gdb --disable-sim\n" + s[j:]
open(p, "w").write(s)
PY
    echo "==> patched CONFIG_BINUTILS += --disable-gdb --disable-sim"
fi

# -flto.  Two changes to binutils; GCC needs no patch, only the pin above,
# which moved from 6f4ca1b4 to 60f21496 -- the tip of amiga16.2, and the only
# one of the two that still exists on the branch.
#
# First, amiga-gcc configures binutils with --disable-plugins for every target
# but m68k-elf, so ld accepts -plugin and ignores it.  GCC's liblto_plugin.so
# is then never loaded, no IR is ever read, and an LTO link quietly produces a
# library with nothing in it.  Upstream amiga-gcc dropped that gate in 7e75d1c.
if ! grep -q 'enable-plugins' "$SRC/Makefile"; then
    python3 - "$SRC/Makefile" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = """ifneq (m68k-elf,$(TARGET))
CONFIG_BINUTILS += --disable-plugins
endif
"""
i = s.index(old)
s = s[:i] + "CONFIG_BINUTILS += --enable-plugins\n" + s[i + len(old):]
open(p, "w").write(s)
PY
    echo "==> patched CONFIG_BINUTILS += --enable-plugins"
fi

# Second, bfd.  With the plugin loaded, five defects in the amigaos back end
# and in the generic archive scan lose the plugin's symbols: slim LTO archive
# members are never pulled in, the IR dummy BFD's definitions and references
# are dropped when ld round-trips it, --gc-sections purges the link, and the
# IR hunk is written without the length header libiberty reads back.  The
# diffs are against the pinned binutils commit; each is skipped if it is
# already in the tree, so a rebuild in a populated work directory is a no-op.
#
# WHICH set applies is decided by the branch recorded in PINS above, not by a
# fixed directory name.  The two diffs above are written against 2.39's bfd and
# do not apply to 2.46, which carries LTO support for the HUNK target of its
# own and needs two different repairs instead: `strip` there drops every
# HUNK_RELOC32, and `ar` there writes a native Amiga library, which has no
# symbol index a slim LTO member can appear in.  A pin that moves without its
# patch set is exactly the failure this table exists to make impossible, so an
# unrecorded branch stops the build rather than quietly producing an unpatched
# binutils.
BU_BRANCH=$(printf '%s\n' "$PINS" | awk -F'|' '$1 == "binutils" { print $4 }')
case "$BU_BRANCH" in
    amiga-2.39.0) BU_PATCH_SET="binutils" ;;
    amiga-2.46)   BU_PATCH_SET="binutils-2.46" ;;
    *)
        echo "!! no patch set under tools/patches/ for binutils $BU_BRANCH" >&2
        echo "   Write one and name it here.  An unpatched binutils links" >&2
        echo "   images that LoadSeg() will not relocate." >&2
        exit 2 ;;
esac

echo "==> binutils patches: tools/patches/$BU_PATCH_SET ($BU_BRANCH)"
BU_PATCHES="$HERE/patches/$BU_PATCH_SET"
if [ ! -d "$BU_PATCHES" ]; then
    echo "!! tools/patches/$BU_PATCH_SET is missing" >&2
    exit 2
fi
for patch in "$BU_PATCHES"/*.diff; do
    [ -f "$patch" ] || continue
    if git -C "$SRC/projects/binutils" apply --reverse --check "$patch" 2>/dev/null; then
        echo "    $(basename "$patch") already applied"
    else
        echo "    $(basename "$patch")"
        git -C "$SRC/projects/binutils" apply "$patch"
    fi
done

# ----------------------------------------------------------------- build ----

mkdir -p "$PREFIX"
echo "==> make $MAKE_TARGETS"
( cd "$SRC" && make NDK=3.9 PREFIX="$PREFIX" -j"$JOBS" $MAKE_TARGETS )

# ------------------------------------------------------------- post-build ----

# The same two repairs fetch-toolchain.sh applies to a downloaded tree, applied
# here so a locally built one is equally sound before anything links against
# it.  Both are idempotent and leave a clean tree alone, so applying them twice
# (here, then again on install) costs nothing.
if [ -f "$HERE/fix-toolchain-crt0.py" ]; then
    echo "==> crt0 frame skew"
    python3 "$HERE/fix-toolchain-crt0.py" "$PREFIX"
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

VER=$("$PREFIX/bin/m68k-amigaos-gcc" -dumpversion 2>/dev/null || echo "")
if [ "$VER" != "$EXPECT_GCC_VERSION" ]; then
    echo "!! expected GCC $EXPECT_GCC_VERSION, got '${VER:-nothing}'" >&2
    exit 1
fi
[ -f "$PREFIX/m68k-amigaos/ndk-include/exec/types.h" ] || {
    echo "!! no NDK headers under $PREFIX/m68k-amigaos/ndk-include" >&2; exit 1; }

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
