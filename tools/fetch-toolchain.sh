#!/usr/bin/env bash
#
# Acquire the pinned m68k-amigaos cross toolchain.
#
#   tools/fetch-toolchain.sh              # fetch (or reuse) and report the root
#   eval "$(tools/fetch-toolchain.sh --export)"   # ... and export it
#   tools/fetch-toolchain.sh --print-root # just say where it would be
#   tools/fetch-toolchain.sh --force      # re-fetch even if cached
#
# WHAT IS PINNED, AND WHY THIS PARTICULAR THING
#
#   The toolchain is GCC 15.2.0 for m68k-amigaos with the NDK 3.9 headers --
#   the same build the project is developed against.  It is published only as
#   a Docker image, by AmigaPorts, as
#
#       docker.io/amigadev/crosstools:m68k-amigaos-gcc10
#
#   That tag is misnamed: it says gcc10 and contains GCC 15.2.0.  The
#   AmigaPorts build switched its `gcc10` branch to bebbo's amiga15.2 gcc in
#   October 2025 and never renamed the tag; the plain `:m68k-amigaos` tag is
#   still GCC 6.5, so the obvious-looking one is the wrong one.
#
#   We do not pull the image.  The whole image is 1.2 GB, of which the
#   toolchain is one layer -- the `COPY /opt/m68k-amigaos` step, 93 MB
#   compressed, 265 MB extracted.  That layer is fetched straight from the
#   registry over HTTPS and pinned by its CONTENT DIGEST, so:
#
#     * no Docker daemon is needed, on any host;
#     * the pin cannot drift.  A registry blob digest is the sha256 of the
#       bytes, so the integrity check and the version pin are the same string.
#       If AmigaPorts re-pushes the tag, this keeps fetching what it always
#       fetched.
#
#   Alternatives considered and rejected: there is no GitHub Release, PPA,
#   Homebrew formula, nixpkgs derivation or plain tarball of a modern
#   m68k-amigaos GCC anywhere (bebbo's GitHub repo is gone; only the Codeberg
#   and franke.ms git remotes survive).  Building from source works but takes
#   the better part of an hour and pulls NDK 3.9 from a single unmirrored
#   third-party host, which is a worse dependency than this one.
#
# WHERE IT LANDS
#
#   $AMINETXDUO_TOOLCHAIN_CACHE, or ~/.cache/aminetxduo/toolchain by default:
#
#       <cache>/<digest12>/        the extracted tree
#       <cache>/current            symlink to it
#
#   cmake/toolchain-m68k-amigaos.cmake and tools/amiga-toolchain.sh both look
#   at <cache>/current, so nothing else has to be told about it.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------- pin ----

TC_REGISTRY="registry-1.docker.io"
TC_REPO="amigadev/crosstools"
# amigadev/crosstools:m68k-amigaos-gcc10, linux/amd64, the /opt/m68k-amigaos
# layer.  GCC 15.2.0, binutils 2.46, NDK 3.9 + Roadshow bsdsocket/SANA-II
# headers.  Recorded here as the single source of truth for the version.
TC_BLOB_AMD64="sha256:c63033fd447383b09ab739299075f11d482c79182ff99b55959dd7b970f7b12d"
TC_PREFIX_IN_TAR="opt/m68k-amigaos"
TC_GCC_VERSION="15.2.0"

# ---------------------------------------------------------------- options ----

MODE="fetch"
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --export)     MODE="export" ;;
        --print-root) MODE="print" ;;
        --force)      FORCE=1 ;;
        -h|--help)    sed -n '2,58p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--export|--print-root] [--force]" >&2; exit 2 ;;
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

# The blob is linux/amd64 ELF.  Fetching it on anything else still works and is
# occasionally useful (inspecting the headers, checking the pin), but it will
# not run, so say so rather than let CMake fail later with something cryptic.
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

# ---------------------------------------------------------------- fetch ------

say "==> fetching m68k-amigaos-gcc $TC_GCC_VERSION"
say "    $TC_REPO blob $TC_BLOB_AMD64"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/aminetxduo-tc.XXXXXX")
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# Anonymous pull token.  No credentials, no rate-limit surprises for a single
# blob, and nothing here needs an account.
TOKEN=$(curl -fsSL \
    "https://auth.docker.io/token?service=registry.docker.io&scope=repository:${TC_REPO}:pull" \
    | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[ -n "$TOKEN" ] || { echo "could not get a registry token" >&2; exit 1; }

# -L matters: the registry answers blob requests with a 307 to object storage.
curl -fsSL -H "Authorization: Bearer $TOKEN" \
    "https://${TC_REGISTRY}/v2/${TC_REPO}/blobs/${TC_BLOB_AMD64}" \
    -o "$TMP/toolchain.tgz"

GOT=$(sha256_of "$TMP/toolchain.tgz")
WANT="${TC_BLOB_AMD64#sha256:}"
if [ "$GOT" != "$WANT" ]; then
    echo "!! digest mismatch" >&2
    echo "   want $WANT" >&2
    echo "   got  $GOT" >&2
    exit 1
fi
say "    digest verified"

# --------------------------------------------------------------- extract -----

rm -rf "$TMP/x"
mkdir -p "$TMP/x"
# Only the toolchain prefix; the layer also carries a stray etc/ from the
# image build that has nothing to do with us.
tar xzf "$TMP/toolchain.tgz" -C "$TMP/x" "$TC_PREFIX_IN_TAR"

[ -x "$TMP/x/$TC_PREFIX_IN_TAR/bin/m68k-amigaos-gcc" ] || {
    echo "!! extracted tree has no bin/m68k-amigaos-gcc" >&2
    exit 1
}
[ -f "$TMP/x/$TC_PREFIX_IN_TAR/m68k-amigaos/ndk-include/exec/types.h" ] || {
    echo "!! extracted tree has no NDK headers" >&2
    exit 1
}

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
