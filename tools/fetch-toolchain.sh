#!/usr/bin/env bash
#
# Acquire the pinned m68k-amigaos cross toolchain.
#
#   tools/fetch-toolchain.sh              # fetch (or reuse) and report the root
#   eval "$(tools/fetch-toolchain.sh --export)"   # ... and export it
#   tools/fetch-toolchain.sh --print-root # just say where it would be
#   tools/fetch-toolchain.sh --print-sha  # this platform's asset sha256
#   tools/fetch-toolchain.sh --check-inlines  # NDK inline register ABI
#   tools/fetch-toolchain.sh --force      # re-fetch even if cached
#
# WHAT IS PINNED, AND WHY THIS PARTICULAR THING
#
#   GCC 16.2.0b for m68k-amigaos, binutils 2.39.0, NDK 3.9 headers, built from
#   bebbo's amiga-gcc.  tools/build-toolchain.sh is that build, pinned to exact
#   commits; this script downloads what it produced.
#
#   BINUTILS 2.39 IS NOT AN OVERSIGHT.  2.46 cannot assemble GCC 16.2's own
#   output -- it forces the MIT-syntax pseudo-branches to a byte displacement
#   and rejects thousands of them -- and its `size` does not recognise some of
#   our objects, which costs three targets their strip step.  The pairing is
#   deliberate.
#
#   NDK 3.9, NOT NDK 3.2.  Confusingly, 3.2 is the NEWER of the two: it is
#   Hyperion's 2021 SDK, and it renamed some long-standing types (`struct
#   timerequest` -> `struct TimeRequest`, and so on) while keeping the old
#   names as aliases.  The sources are written to the spellings both NDKs
#   accept, which costs nothing and lets anyone build the tree with whichever
#   NDK their toolchain shipped.
#
# ONE SOURCE.  THIS IS A REAL CHANGE, AND NOT A PAPERED-OVER ONE.
#
#   Until GCC 15.2 this script had a second route: the AmigaPorts image
#   docker.io/amigadev/crosstools:m68k-amigaos-gcc10, whose /opt/m68k-amigaos
#   layer WAS the toolchain, fetched by content digest if our release was
#   unreachable.  That fallback is gone, because it was never a fallback for
#   this toolchain -- it published GCC 15.2 with a different binutils, and no
#   registry, distro, PPA, Homebrew tap or nixpkgs derivation publishes GCC
#   16.2 + binutils 2.39.0 for m68k-amigaos at all.  There is nothing to fall
#   back TO.
#
#   So the release asset below is now the single point of failure for a
#   download, and pretending otherwise by leaving dead code in would be worse
#   than saying it.  The mitigation is not a mirror, it is that the build is
#   reproducible: if this release goes away, tools/build-toolchain.sh rebuilds
#   the same toolchain from pinned commits on either supported host.
#
#   github.com/tinic/AmiNetXDuo/releases/tag/toolchain-m68k-amigaos-gcc-16.2.0
#
#   A BAD HASH IS NOT A REASON TO CARRY ON.  Unreachable release -> a network
#   error, say so.  Release reachable but the bytes are wrong -> stop, loudly,
#   and write nothing.  The first is weather; the second means something is
#   serving us a toolchain we did not ask for.
#
# ONE ASSET PER PLATFORM
#
#   The binaries are native host executables, so there is one asset per
#   host platform and the pin is per platform too.  Covered:
#
#       Linux x86_64      built on Debian 13.5, glibc 2.41
#       macOS arm64       built on macOS 26.5, Apple Silicon
#
#   NOT covered, and there is no asset to serve them: macOS x86_64 and Linux
#   aarch64.  Those hosts get an error naming the platform and pointing at
#   tools/build-toolchain.sh, which builds on either of them; what they must
#   not get is a silent download of binaries that cannot execute.
#
# WHERE IT LANDS
#
#   $AMINETXDUO_TOOLCHAIN_CACHE, or ~/.cache/aminetxduo/toolchain by default:
#
#       <cache>/<sha12>/           the extracted tree
#       <cache>/current            symlink to it
#
#   <sha12> is the first 12 hex of THIS PLATFORM's asset hash, so two
#   platforms sharing a home directory over NFS do not fight over one path,
#   and a pin bump lands in a new directory rather than half-overwriting the
#   old one.
#
#   cmake/toolchain-m68k-amigaos.cmake and tools/amiga-toolchain.sh both look
#   at <cache>/current, so nothing else has to be told about it.
#
# ENVIRONMENT
#
#   AMINETXDUO_TOOLCHAIN_CACHE   where to unpack (default ~/.cache/...)
#   AMINETXDUO_TOOLCHAIN_URL     fetch the tarball from here instead of the
#                                release (an internal mirror, or a file:// URL
#                                for testing).  Still hash-checked against this
#                                platform's pin.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------- pin ----

TC_GCC_VERSION="16.2.0b"
TC_ASSET_VERSION="16.2.0"
TC_PREFIX_IN_TAR="opt/m68k-amigaos"

TC_MIRROR_REPO="tinic/AmiNetXDuo"
TC_MIRROR_TAG="toolchain-m68k-amigaos-gcc-${TC_ASSET_VERSION}"

# --------------------------------------------------------------- platform ---

# Resolved before anything else because it selects the pin, which names the
# cache directory, which --print-root has to answer without touching the net.
OS=$(uname -s)
ARCH=$(uname -m)

case "$OS/$ARCH" in
    Linux/x86_64|Linux/amd64)
        TC_PLATFORM="linux-x86_64"
        TC_SHA256="eabb6789378f954f487a3db3dd2648b52b3932cc6fc69a858f105bb3b6761462"
        ;;
    Darwin/arm64|Darwin/aarch64)
        TC_PLATFORM="darwin-arm64"
        TC_SHA256="0549bac96463155a8f46fff3e4f7d54a1f6a473652317fd11fcc99f6652b24d1"
        ;;
    *)
        TC_PLATFORM=""
        TC_SHA256=""
        ;;
esac

# Deferred rather than raised here, because --check-crt0 and --check-inlines
# ask about a toolchain the caller already has, by path.  Someone on an
# uncovered host with a hand-built tree has every right to run those.
no_asset() {
    echo "!! no prebuilt m68k-amigaos toolchain for $OS/$ARCH." >&2
    echo "" >&2
    echo "   Published assets cover Linux x86_64 and macOS arm64 only." >&2
    echo "   For $OS/$ARCH, build it:" >&2
    echo "" >&2
    echo "       tools/build-toolchain.sh --package /tmp/tc.tar.xz" >&2
    echo "" >&2
    echo "   then point AMIGA_TOOLCHAIN_ROOT at the result, or set" >&2
    echo "   AMINETXDUO_TOOLCHAIN_URL to a copy you host and add its" >&2
    echo "   sha256 to the table at the top of this script." >&2
    exit 2
}

TC_MIRROR_ASSET="m68k-amigaos-gcc-${TC_ASSET_VERSION}-ndk3.9-${TC_PLATFORM}.tar.xz"
TC_MIRROR_URL="${AMINETXDUO_TOOLCHAIN_URL:-https://github.com/${TC_MIRROR_REPO}/releases/download/${TC_MIRROR_TAG}/${TC_MIRROR_ASSET}}"

# ---------------------------------------------------------------- options ----

MODE="fetch"
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --export)     MODE="export" ;;
        --print-root) MODE="print" ;;
        --print-sha)  MODE="printsha" ;;
        --check-crt0) MODE="checkcrt0" ;;
        --check-inlines) MODE="checkinlines" ;;
        --force)      FORCE=1 ;;
        -h|--help)    sed -n '2,89p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--export|--print-root|--print-sha|--check-crt0|--check-inlines] [--force]" >&2; exit 2 ;;
    esac
    shift
done

# Everything informational goes to stderr, so `--export` can be eval'd.
say() { [ "$MODE" = "export" ] && echo "$*" >&2 || echo "$*"; }

CACHE="${AMINETXDUO_TOOLCHAIN_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aminetxduo/toolchain}"
DIGEST12=$(printf '%s' "$TC_SHA256" | cut -c1-12)
ROOT="$CACHE/$DIGEST12"

case "$MODE" in
    checkcrt0|checkinlines) ;;
    *) [ -n "$TC_PLATFORM" ] || no_asset ;;
esac

if [ "$MODE" = "print" ]; then
    printf '%s\n' "$ROOT"
    exit 0
fi

# The CI cache key wants this without having to parse the script.  It is per
# platform now, which is also what makes a Linux and a macOS runner stop
# sharing one cache entry.
if [ "$MODE" = "printsha" ]; then
    printf '%s\n' "$TC_SHA256"
    exit 0
fi

# Answers "does the toolchain I am about to build with still have the crt0 bug"
# for a tree this script did not install, a hand-built toolchain, or one from
# before the repair existed.  Exits non-zero if any crt0.o is still broken.
#
# A toolchain whose ____start and exit both keep no frame never had the bug --
# which is what bebbo/amiga-gcc issue #12 produced, the compiler honouring
# __entrypoint on amiga15.2, amiga13.4 and amiga16.1.  That reports "immune"
# and succeeds; it used to be reported as an error, which told a user with a
# sound toolchain that theirs was broken.
if [ "$MODE" = "checkcrt0" ]; then
    CHECK_ROOT="${AMIGA_TOOLCHAIN_ROOT:-$ROOT}"
    [ -d "$CHECK_ROOT" ] || { echo "no toolchain at $CHECK_ROOT" >&2; exit 2; }
    echo "==> $CHECK_ROOT"
    exec python3 "$(dirname "$0")/fix-toolchain-crt0.py" "$CHECK_ROOT" --check
fi

# Answers "does this toolchain still hand Write(), CopyMem() and forty-odd
# other inlines their argument in a register the library does not read".
# tools/fix-toolchain-inline-const.py says what the bug is.  Exits non-zero
# if any top-level const is left on an LPn parameter.
if [ "$MODE" = "checkinlines" ]; then
    CHECK_ROOT="${AMIGA_TOOLCHAIN_ROOT:-$ROOT}"
    [ -d "$CHECK_ROOT" ] || { echo "no toolchain at $CHECK_ROOT" >&2; exit 2; }
    echo "==> $CHECK_ROOT"
    exec python3 "$(dirname "$0")/fix-toolchain-inline-const.py" \
         "$CHECK_ROOT" --check
fi

emit_root() {
    if [ "$MODE" = "export" ]; then
        printf 'export AMIGA_TOOLCHAIN_ROOT=%s\n' "$ROOT"
    else
        printf '%s\n' "$ROOT"
    fi
}

# ------------------------------------------------------------ already here ---

if [ "$FORCE" = "0" ] && [ -x "$ROOT/bin/m68k-amigaos-gcc" ]; then
    say "==> toolchain already at $ROOT"
    emit_root
    exit 0
fi

# ----------------------------------------------------------------- host ------

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing required tool: $1" >&2; exit 2; }; }
need curl
need tar

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# Refuses rather than returns non-zero: a hash mismatch is never something to
# route around.
verify_sha256() {
    local file="$1" want="$2" what="$3" got
    got=$(sha256_of "$file")
    if [ "$got" != "$want" ]; then
        echo "" >&2
        echo "!! $what does not match its pin, refusing to install it." >&2
        echo "   want $want" >&2
        echo "   got  $got" >&2
        echo "   Nothing was written to $CACHE." >&2
        exit 1
    fi
}

# ---------------------------------------------------------------- fetch ------

say "==> fetching m68k-amigaos-gcc $TC_GCC_VERSION for $TC_PLATFORM"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/aminetxduo-tc.XXXXXX")
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

ARCHIVE="$TMP/toolchain.tar.compressed"

say "    $TC_MIRROR_URL"

# curl's own diagnostic is worth keeping, "404" and "connection refused" are
# different problems, but --retry repeats it, so keep the last line only.
if ! curl -fsSL --retry 3 --retry-connrefused --retry-delay 2 \
          --connect-timeout 20 "$TC_MIRROR_URL" -o "$ARCHIVE" 2>"$TMP/curl.err"; then
    echo "!! could not download the toolchain: $(tail -1 "$TMP/curl.err" 2>/dev/null)" >&2
    echo "   There is no second source for this pairing; see the header." >&2
    echo "   tools/build-toolchain.sh builds it from pinned commits." >&2
    exit 1
fi

verify_sha256 "$ARCHIVE" "$TC_SHA256" "the $TC_PLATFORM toolchain asset"
say "    sha256 verified"

# Only the toolchain prefix is taken, and taking it by name is also what makes
# a tarball that packs some other layout fail here rather than three hundred
# "exec/types.h: No such file" errors later.
rm -rf "$TMP/x"
mkdir -p "$TMP/x"
if ! tar xf "$ARCHIVE" -C "$TMP/x" "$TC_PREFIX_IN_TAR" 2>"$TMP/tar.err"; then
    echo "!! could not unpack the asset: $(tail -1 "$TMP/tar.err" 2>/dev/null)" >&2
    echo "   (xz support: GNU tar shells out to the xz binary, bsdtar has" >&2
    echo "    liblzma built in.  The hash passed, so this is local tooling.)" >&2
    exit 1
fi
[ -x "$TMP/x/$TC_PREFIX_IN_TAR/bin/m68k-amigaos-gcc" ] || {
    echo "!! the asset has no $TC_PREFIX_IN_TAR/bin/m68k-amigaos-gcc" >&2
    exit 1
}

# ---------------------------------------------------------------- install ----

# The extract already proved bin/m68k-amigaos-gcc is there; the NDK headers
# are the other half of a usable tree, and a root without them fails a hundred
# "exec/types.h: No such file" errors deep instead of here.
[ -f "$TMP/x/$TC_PREFIX_IN_TAR/m68k-amigaos/ndk-include/exec/types.h" ] || {
    echo "!! extracted tree has no NDK headers" >&2
    exit 1
}

# The libnix crt0.o in some builds saves three registers at _start and restores
# four at ___exit, so every command built with it dies the moment it returns to
# the Shell.  It is repaired HERE, once, before the tree is installed, not in
# every link line downstream.  There are four affected crt0.o files, one per
# CPU multilib, which is a second reason a per-target workaround was the wrong
# shape.  tools/fix-toolchain-crt0.py says what the bug is and why this is the
# seam; it is idempotent and it leaves an already-correct toolchain alone, so a
# future image that has been fixed upstream needs no change here.
say "==> repairing the newlib crt0 frame skew"
if ! python3 "$(dirname "$0")/fix-toolchain-crt0.py" "$TMP/x/$TC_PREFIX_IN_TAR"; then
    echo "!! crt0 repair failed, refusing to install a toolchain that" >&2
    echo "!! builds commands which crash on return to the Shell." >&2
    exit 1
fi

# The NDK inline headers const-qualify 46 register parameters, and GCC drops
# the register binding on a const one whose value is a link-time constant, so
# Write() reads its buffer pointer out of d2 while the caller left the address
# in d0.  Repaired HERE for the same reason as the crt0 above: it is the
# toolchain's defect, no call site can work around it reliably, and there is
# no diagnostic.  Idempotent, and it leaves a clean tree alone.
say "==> repairing the NDK inline register ABI"
if ! python3 "$(dirname "$0")/fix-toolchain-inline-const.py" \
     "$TMP/x/$TC_PREFIX_IN_TAR"; then
    echo "!! inline repair failed, refusing to install a toolchain whose" >&2
    echo "!! library calls pass arguments in the wrong registers." >&2
    exit 1
fi

rm -rf "$ROOT" "$ROOT.tmp"
mkdir -p "$CACHE"
mv "$TMP/x/$TC_PREFIX_IN_TAR" "$ROOT.tmp"
mv "$ROOT.tmp" "$ROOT"

# `current` is what the CMake toolchain file and tools/amiga-toolchain.sh look
# for, so switching pins is one symlink rather than an edit everywhere.
ln -sfn "$ROOT" "$CACHE/current.tmp"
mv -f "$CACHE/current.tmp" "$CACHE/current"

say "==> installed $ROOT"

# ---------------------------------------------------------------- verify -----

# The asset was selected by this host's uname, so it is meant to run here, and
# "it does not" is a defect in the asset rather than an expected condition to
# be narrated -- which is what it used to be, back when there was one Linux
# tarball for every host.
VER=$("$ROOT/bin/m68k-amigaos-gcc" -dumpversion 2>/dev/null || echo "")
if [ "$VER" != "$TC_GCC_VERSION" ]; then
    echo "!! expected GCC $TC_GCC_VERSION, got '${VER:-nothing}'" >&2
    echo "   The $TC_PLATFORM asset does not run on this $OS/$ARCH host." >&2
    exit 1
fi
say "    m68k-amigaos-gcc $VER runs"

emit_root
