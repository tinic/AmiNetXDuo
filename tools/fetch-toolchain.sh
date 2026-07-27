#!/usr/bin/env bash
#
# Acquire the pinned m68k-amigaos cross toolchain.
#
#   tools/fetch-toolchain.sh              # fetch (or reuse) and report the root
#   eval "$(tools/fetch-toolchain.sh --export)"   # ... and export it
#   tools/fetch-toolchain.sh --print-root # just say where it would be
#   tools/fetch-toolchain.sh --print-sha  # the mirror asset's sha256
#   tools/fetch-toolchain.sh --force      # re-fetch even if cached
#
# WHAT IS PINNED, AND WHY THIS PARTICULAR THING
#
#   The toolchain is GCC 15.2.0 for m68k-amigaos with the NDK 3.9 headers.
#   Upstream publishes it only as a Docker image, by AmigaPorts, as
#
#       docker.io/amigadev/crosstools:m68k-amigaos-gcc10
#
#   That tag is misnamed: it says gcc10 and contains GCC 15.2.0.  The
#   AmigaPorts build switched its `gcc10` branch to bebbo's amiga15.2 gcc in
#   October 2025 and never renamed the tag; the plain `:m68k-amigaos` tag is
#   still GCC 6.5, so the obvious-looking one is the wrong one.  Everything
#   here is therefore named for what it IS, not for what upstream calls it.
#
#   Alternatives considered and rejected: there is no upstream GitHub Release,
#   PPA, Homebrew formula, nixpkgs derivation or plain tarball of a modern
#   m68k-amigaos GCC (bebbo's GitHub repo is gone; only the Codeberg and
#   AmigaPorts remotes survive).  Building from source works but takes the
#   better part of an hour and pulls NDK 3.9 from a single unmirrored
#   third-party host, which is a worse dependency than either of these.
#
#   NDK 3.9, NOT NDK 3.2.  Confusingly, 3.2 is the NEWER of the two: it is
#   Hyperion's 2021 SDK, and it renamed some long-standing types (`struct
#   timerequest` -> `struct TimeRequest`, and so on) while keeping the old
#   names as aliases.  Neither published amigadev/crosstools m68k tag carries
#   it -- the build selects the NDK with `make NDK=3.9` -- so pinning a 3.2
#   toolchain would mean building and hosting one ourselves.  The sources are
#   written to the spellings both NDKs accept instead, which costs nothing and
#   lets anyone build the tree with whichever NDK their toolchain shipped.
#
# TWO SOURCES, IN ORDER
#
#   1. OUR MIRROR -- a release asset on this repository:
#
#        github.com/tinic/AmiNetXDuo/releases/tag/toolchain-m68k-amigaos-gcc-15.2.0
#
#      The same tree, repackaged as .tar.xz (35 MB, versus the 93 MB gzipped
#      layer) and pinned by the sha256 of the asset.  Preferred because it is
#      infrastructure we control: no third-party registry, no auth dance, no
#      dependence on Docker Hub continuing to serve one blob forever.
#
#      We are a redistributor of GPLv3 GCC binaries the moment we host them,
#      so the corresponding-source directions required by GPLv3 6(d) are in
#      the release body.  If the asset is ever re-cut, they go with it.
#
#   2. THE UPSTREAM REGISTRY -- the original image layer, if the mirror
#      cannot be reached.  We do not pull the image; the whole thing is
#      1.2 GB, of which the toolchain is one layer (the `COPY
#      /opt/m68k-amigaos` step).  That layer is fetched straight from the
#      registry over HTTPS and pinned by its CONTENT DIGEST, so:
#
#        * no Docker daemon is needed, on any host;
#        * the pin cannot drift.  A registry blob digest is the sha256 of the
#          bytes, so the integrity check and the version pin are the same
#          string.  If AmigaPorts re-pushes the tag, this keeps fetching what
#          it always fetched.
#
#   Both sources yield a byte-identical tree under the same `opt/m68k-amigaos`
#   prefix, so they share one extraction path and one cache directory.
#
#   A BAD HASH IS NOT A REASON TO FALL BACK.  Unreachable mirror -> try the
#   registry.  Mirror reachable but the bytes are wrong -> stop, loudly.  The
#   first is weather; the second means something is serving us a toolchain we
#   did not ask for, and quietly routing around it is how you end up not
#   noticing.
#
# WHERE IT LANDS
#
#   $AMINETXDUO_TOOLCHAIN_CACHE, or ~/.cache/aminetxduo/toolchain by default:
#
#       <cache>/<digest12>/        the extracted tree
#       <cache>/current            symlink to it
#
#   <digest12> is the upstream layer digest either way -- it names the
#   toolchain, not the route we took to get it.
#
#   cmake/toolchain-m68k-amigaos.cmake and tools/amiga-toolchain.sh both look
#   at <cache>/current, so nothing else has to be told about it.
#
# ENVIRONMENT
#
#   AMINETXDUO_TOOLCHAIN_CACHE   where to unpack (default ~/.cache/...)
#   AMINETXDUO_TOOLCHAIN_URL     fetch the tarball from here instead of the
#                                release (an internal mirror, or a file:// URL
#                                for testing).  Still hash-checked.
#   AMINETXDUO_TOOLCHAIN_SOURCE  auto (default) | mirror | registry.
#                                `registry` skips the mirror entirely; `mirror`
#                                refuses to fall back.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------- pin ----

TC_GCC_VERSION="15.2.0"
TC_PREFIX_IN_TAR="opt/m68k-amigaos"

# Our mirror.  Repackaged from the layer below without recompiling anything;
# see the release body for provenance and the GPL source offer.
TC_MIRROR_REPO="tinic/AmiNetXDuo"
TC_MIRROR_TAG="toolchain-m68k-amigaos-gcc-15.2.0"
TC_MIRROR_ASSET="m68k-amigaos-gcc-${TC_GCC_VERSION}-ndk3.9-linux-x86_64.tar.xz"
TC_MIRROR_SHA256="fca243597aac34400251c0cc9cede9d74434e43dca1ed5c18d3a82b7bd1ab977"

# amigadev/crosstools:m68k-amigaos-gcc10, linux/amd64, the /opt/m68k-amigaos
# layer.  GCC 15.2.0, binutils, NDK 3.9 + Roadshow bsdsocket/SANA-II headers.
# Recorded here as the single source of truth for the version, and it is what
# names the cache directory.
TC_REGISTRY="registry-1.docker.io"
TC_REPO="amigadev/crosstools"
TC_BLOB_AMD64="sha256:c63033fd447383b09ab739299075f11d482c79182ff99b55959dd7b970f7b12d"

TC_MIRROR_URL="${AMINETXDUO_TOOLCHAIN_URL:-https://github.com/${TC_MIRROR_REPO}/releases/download/${TC_MIRROR_TAG}/${TC_MIRROR_ASSET}}"
TC_SOURCE="${AMINETXDUO_TOOLCHAIN_SOURCE:-auto}"

case "$TC_SOURCE" in
    auto|mirror|registry) ;;
    *) echo "AMINETXDUO_TOOLCHAIN_SOURCE must be auto, mirror or registry" >&2; exit 2 ;;
esac

# ---------------------------------------------------------------- options ----

MODE="fetch"
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --export)     MODE="export" ;;
        --print-root) MODE="print" ;;
        --print-sha)  MODE="printsha" ;;
        --check-crt0) MODE="checkcrt0" ;;
        --force)      FORCE=1 ;;
        -h|--help)    sed -n '2,97p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--export|--print-root|--print-sha|--check-crt0] [--force]" >&2; exit 2 ;;
    esac
    shift
done

# Everything informational goes to stderr, so `--export` can be eval'd.
say() { [ "$MODE" = "export" ] && echo "$*" >&2 || echo "$*"; }

CACHE="${AMINETXDUO_TOOLCHAIN_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aminetxduo/toolchain}"
DIGEST12=$(printf '%s' "${TC_BLOB_AMD64#sha256:}" | cut -c1-12)
ROOT="$CACHE/$DIGEST12"

if [ "$MODE" = "print" ]; then
    printf '%s\n' "$ROOT"
    exit 0
fi

# The CI cache key wants this without having to parse the script.
if [ "$MODE" = "printsha" ]; then
    printf '%s\n' "$TC_MIRROR_SHA256"
    exit 0
fi

# Answers "does the toolchain I am about to build with still have the crt0 bug"
# for a tree this script did not install -- a hand-built toolchain, or one from
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

# The binaries are linux/amd64 ELF.  Fetching them on anything else still works
# and is occasionally useful (inspecting the headers, checking the pin), but
# they will not run, so say so rather than let CMake fail later with something
# cryptic.
OS=$(uname -s)
ARCH=$(uname -m)
RUNNABLE=1
if [ "$OS" != "Linux" ] || { [ "$ARCH" != "x86_64" ] && [ "$ARCH" != "amd64" ]; }; then
    RUNNABLE=0
fi

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
        echo "!! $what does not match its pin -- refusing to install it." >&2
        echo "   want $want" >&2
        echo "   got  $got" >&2
        echo "   Nothing was written to $CACHE." >&2
        exit 1
    fi
}

# ---------------------------------------------------------------- fetch ------

say "==> fetching m68k-amigaos-gcc $TC_GCC_VERSION"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/aminetxduo-tc.XXXXXX")
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

ARCHIVE="$TMP/toolchain.tar.compressed"

# --- 1. our mirror --------------------------------------------------------

# Returns non-zero only for "could not get the bytes" or "could not unpack
# them here"; a wrong hash exits the script from verify_sha256.
fetch_mirror() {
    say "    mirror $TC_MIRROR_URL"

    # curl's own diagnostic is worth keeping -- "404" and "connection refused"
    # are different problems -- but --retry repeats it, so keep the last line
    # only and say once what we are going to do about it.
    if ! curl -fsSL --retry 3 --retry-connrefused --retry-delay 2 \
              --connect-timeout 20 "$TC_MIRROR_URL" -o "$ARCHIVE" 2>"$TMP/curl.err"; then
        say "    !! mirror unreachable: $(tail -1 "$TMP/curl.err" 2>/dev/null)"
        return 1
    fi

    verify_sha256 "$ARCHIVE" "$TC_MIRROR_SHA256" "the mirrored toolchain"
    say "    sha256 verified"

    # xz support is not universal: GNU tar shells out to the xz binary, bsdtar
    # has liblzma built in.  The hash already passed, so a failure here is this
    # machine's tooling, not the payload -- exactly the case where falling back
    # to the gzipped layer is the right answer.
    if ! extract_archive; then
        say "    !! cannot unpack .tar.xz here (no xz?)"
        return 1
    fi
    return 0
}

# --- 2. the upstream registry ---------------------------------------------

fetch_registry() {
    say "    registry $TC_REPO blob $TC_BLOB_AMD64"

    # Anonymous pull token.  No credentials, no rate-limit surprises for a
    # single blob, and nothing here needs an account.
    local token
    token=$(curl -fsSL --retry 3 --retry-connrefused --retry-delay 2 \
        "https://auth.docker.io/token?service=registry.docker.io&scope=repository:${TC_REPO}:pull" \
        | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
    [ -n "$token" ] || { echo "could not get a registry token" >&2; return 1; }

    # -L matters: the registry answers blob requests with a 307 to object
    # storage.
    curl -fsSL --retry 3 --retry-connrefused --retry-delay 2 \
        -H "Authorization: Bearer $token" \
        "https://${TC_REGISTRY}/v2/${TC_REPO}/blobs/${TC_BLOB_AMD64}" \
        -o "$ARCHIVE" || return 1

    verify_sha256 "$ARCHIVE" "${TC_BLOB_AMD64#sha256:}" "the upstream layer"
    say "    digest verified"

    extract_archive
}

# --------------------------------------------------------------- extract -----

# Both sources carry the same tree under the same prefix, differing only in
# compression, and tar sniffs that for itself.  Only the toolchain prefix is
# taken: the upstream layer also carries a stray etc/ from the image build.
extract_archive() {
    rm -rf "$TMP/x"
    mkdir -p "$TMP/x"
    tar xf "$ARCHIVE" -C "$TMP/x" "$TC_PREFIX_IN_TAR" 2>/dev/null || return 1
    [ -x "$TMP/x/$TC_PREFIX_IN_TAR/bin/m68k-amigaos-gcc" ] || return 1
    return 0
}

# --------------------------------------------------------------- dispatch ----

case "$TC_SOURCE" in
    registry)
        fetch_registry || { echo "!! could not fetch the toolchain" >&2; exit 1; }
        ;;
    mirror)
        fetch_mirror || { echo "!! mirror failed and AMINETXDUO_TOOLCHAIN_SOURCE=mirror forbids the fallback" >&2; exit 1; }
        ;;
    auto)
        if ! fetch_mirror; then
            say "    falling back to the upstream registry"
            fetch_registry || { echo "!! neither the mirror nor the registry could be reached" >&2; exit 1; }
        fi
        ;;
esac

# ---------------------------------------------------------------- install ----

# extract_archive already proved bin/m68k-amigaos-gcc is there; the NDK headers
# are the other half of a usable tree, and a root without them fails a hundred
# "exec/types.h: No such file" errors deep instead of here.
[ -f "$TMP/x/$TC_PREFIX_IN_TAR/m68k-amigaos/ndk-include/exec/types.h" ] || {
    echo "!! extracted tree has no NDK headers" >&2
    exit 1
}

# The libnix crt0.o in this image saves three registers at _start and restores
# four at ___exit, so every command built with it dies the moment it returns to
# the Shell.  It is repaired HERE, once, before the tree is installed -- not in
# every link line downstream.  There are four affected crt0.o files, one per
# CPU multilib, which is a second reason a per-target workaround was the wrong
# shape.  tools/fix-toolchain-crt0.py says what the bug is and why this is the
# seam; it is idempotent and it leaves an already-correct toolchain alone, so a
# future image that has been fixed upstream needs no change here.
say "==> repairing the newlib crt0 frame skew"
if ! python3 "$(dirname "$0")/fix-toolchain-crt0.py" "$TMP/x/$TC_PREFIX_IN_TAR"; then
    echo "!! crt0 repair failed -- refusing to install a toolchain that" >&2
    echo "!! builds commands which crash on return to the Shell." >&2
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

if [ "$RUNNABLE" = "1" ]; then
    VER=$("$ROOT/bin/m68k-amigaos-gcc" -dumpversion 2>/dev/null || echo "")
    if [ "$VER" != "$TC_GCC_VERSION" ]; then
        echo "!! expected GCC $TC_GCC_VERSION, got '${VER:-nothing}'" >&2
        exit 1
    fi
    say "    m68k-amigaos-gcc $VER runs"
else
    say ""
    say "!! These binaries are linux/amd64 ELF and this is $OS/$ARCH."
    say "   The tree is downloaded and correct, but nothing in it will run"
    say "   here.  Build or install a native toolchain and point"
    say "   AMIGA_TOOLCHAIN_ROOT at it instead; the layout to match is"
    say "   <root>/bin/m68k-amigaos-gcc + <root>/m68k-amigaos/ndk-include."
fi

emit_root
