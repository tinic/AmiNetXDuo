#!/usr/bin/env bash
#
# Build curl for m68k AmigaOS 3.x against bsdsocket.library.
#
#   clients/curl/build.sh [-u] [-b BUILDDIR] [-t] [-R]
#
#   -u  fetch/update third_party/curl to the pinned tag first
#   -b  build directory (default build/curl)
#   -t  build with TLS over tls.library (see WITH TLS below)
#   -R  revert third_party/curl to pristine and stop
#
# WHAT IS BEING BUILT
#
#   Upstream curl from third_party/curl (a submodule pinned to the tag in
#   CURL_TAG below).  Without -t nothing in this tree patches curl's sources
#   or its build files; everything specific to this toolchain is a flag, and
#   every flag is explained in clients/amiga-client.sh or below.
#
# WITH TLS
#
#   -t adds a vtls backend over tls.library and gives you https://.  Two
#   things go into the submodule's working tree and neither is committed:
#
#     clients/curl/amitls.c, amitls.h  -> lib/vtls/, copied
#     clients/curl/curl-amitls.patch   -> applied, 27 lines over six files
#
#   The submodule PIN never moves and no curl source is committed modified.
#   Both steps are undone before either is redone, so this is idempotent and
#   `-R` puts the checkout back the way it was.  A build without -t after a
#   build with it is therefore genuinely the unpatched curl again -- which is
#   worth having, because "does the no-TLS build still work" is a question
#   somebody has to be able to answer.
#
#   The output is build/<dir>/src/curl -- an AmigaOS hunk executable that
#   opens bsdsocket.library by name, exactly as `fetch`, `ping` and `host`
#   do.  It is not linked against anything of ours except the libc shim.
#
# WHY curl STILL BUILDS FOR A 1992 COMPUTER
#
#   Because it is tested on every push.  curl's .github/workflows/
#   non-native.yml has an `amiga` job that cross-builds m68k with both
#   autotools and CMake.  Classic AmigaOS 3.x is a live target, not a
#   historical one, and lib/amigaos.c carries a distinct OS3 branch (the
#   `#elif !defined(USE_AMISSL)` half) from the AmigaOS 4 / PPC one.  Those
#   are two different ports and support for the second implies nothing about
#   the first; this build uses the first.
#
#   That CI job uses bebbo's amiga-gcc with clib2 and -lnet.  This toolchain
#   is newlib and has neither, so the differences are all in
#   clients/amiga-client.sh and clients/compat/.
#
# THE FOUR SETTINGS THAT ARE NOT OBVIOUS
#
#   -DHAVE_SELECT=1
#       lib/select.c is `#if !defined(HAVE_SELECT) && !defined(HAVE_POLL)
#       #error`, so one of them has to be true.  curl detects select() with
#       check_symbol_exists(), which fails here because Roadshow's
#       bsdsocket.library has WaitSelect() and no select() -- and
#       lib/curl_setup.h then #defines select(a,b,c,d,e) to
#       WaitSelect(a,b,c,d,e,0) itself.  So the function IS there; the probe
#       cannot see it because the probe does not include curl_setup.h.  On
#       the clib2 toolchain upstream tests with, libc has a real select() and
#       the probe passes for the wrong reason.
#
#   -DHAVE_GETTIMEOFDAY=1
#       check_function_exists() cannot see it, because it is in
#       libamigaclient.a and CMake puts CMAKE_C_STANDARD_LIBRARIES after the
#       probe's own object -- but the probe has no object, only a link of a
#       generated main().  Without this curl falls back to time(), whose
#       resolution is ONE SECOND, and its whole timing and timeout machinery
#       runs on integers of seconds.  Measured: with time() a 559-byte GET of
#       example.com reported 31 s and a 404 reported 122 s; with
#       gettimeofday() the same fetches are seconds.  clients/compat/
#       amiga_posix.c implements it over DateStamp(), so the resolution is
#       one tick, 20 ms.
#
#   -DENABLE_IPV6=OFF
#       The Roadshow NDK has struct sockaddr_in6, so curl's probe turns IPv6
#       on -- and AMINETXDUO_IPV6 is OFF in the shipping stack, so socket()
#       would answer EAFNOSUPPORT for every AF_INET6 attempt.  Turn it on
#       here when the stack under it was built with it.
#
#   -DHAVE_GETADDRINFO=0 (curl's CMakeLists sets this for AMIGA itself)
#       Name resolution therefore goes through gethostbyname(), which is
#       IPv4-only and is one of the 121 published vectors.
#
#   -DCURL_DISABLE_ALTSVC / etc are NOT set.  Size is not a constraint for a
#       client binary in this project -- a multi-megabyte curl is fine -- so
#       nothing is disabled to save space.  What IS off is off because the
#       dependency does not exist for m68k (zlib, brotli, zstd, nghttp2,
#       libssh2, libidn2, libpsl) or because it is the next milestone (TLS).
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

CURL_URL="https://github.com/curl/curl.git"
CURL_TAG="curl-8_21_0"

BUILD="build/curl"
UPDATE=0
WITH_TLS=0
REVERT=0

while getopts "ub:tR" opt; do
    case "$opt" in
        u) UPDATE=1 ;;
        b) BUILD="$OPTARG" ;;
        t) WITH_TLS=1 ;;
        R) REVERT=1 ;;
        *) echo "usage: $0 [-u] [-b builddir] [-t] [-R]" >&2; exit 2 ;;
    esac
done

CURL_DIR="$ROOT/third_party/curl"
PATCH="$ROOT/clients/curl/curl-amitls.patch"

# Put the submodule back the way git has it recorded: drop our two copied
# files and undo the patch.  Deliberately not `git checkout -- .`, which would
# also throw away somebody else's local experiment without saying so.
curl_unpatch()
{
    rm -f "$CURL_DIR/lib/vtls/amitls.c" "$CURL_DIR/lib/vtls/amitls.h"
    if ! git -C "$CURL_DIR" diff --quiet -- \
            CMakeLists.txt include/curl/curl.h lib/Makefile.inc \
            lib/curl_config-cmake.h.in lib/curl_setup.h lib/vtls/vtls.c
    then
        git -C "$CURL_DIR" apply -R "$PATCH" 2>/dev/null || {
            echo "!! third_party/curl is modified and it is not our patch." >&2
            echo "   Sort that out first; this script will not guess." >&2
            git -C "$CURL_DIR" status --short >&2
            return 1
        }
    fi
    return 0
}

if [ "$REVERT" = "1" ]; then
    curl_unpatch || exit 1
    echo "third_party/curl is pristine"
    git -C "$CURL_DIR" status --short
    exit 0
fi

. "$ROOT/clients/amiga-client.sh"

amiga_client_checkout "$CURL_DIR" "$CURL_URL" "$CURL_TAG" "$UPDATE"

curl_unpatch || exit 1

if [ "$WITH_TLS" = "1" ]; then
    echo "==> TLS backend: clients/curl/amitls.c + curl-amitls.patch"
    cp "$ROOT/clients/curl/amitls.c" "$CURL_DIR/lib/vtls/amitls.c"
    cp "$ROOT/clients/curl/amitls.h" "$CURL_DIR/lib/vtls/amitls.h"
    git -C "$CURL_DIR" apply "$PATCH" || {
        echo "!! curl-amitls.patch does not apply to $CURL_TAG." >&2
        echo "   Six hunks, six files; rebase it by hand." >&2
        exit 1
    }
fi

echo "==> support archives"
amiga_client_prepare "$ROOT/build/clients"

OUT="$ROOT/$BUILD"
LOG="$OUT-configure.log"
mkdir -p "$OUT"

# CURL_ENABLE_SSL is a cached option and lib/Makefile.inc is read at configure
# time, so a build directory that was configured the other way round would
# quietly keep its old answer.  Throw the cache away when the setting changes
# rather than debug that later.
if [ -f "$OUT/CMakeCache.txt" ]; then
    if grep -q '^CURL_USE_AMITLS:BOOL=ON' "$OUT/CMakeCache.txt"; then
        WAS_TLS=1
    else
        WAS_TLS=0
    fi
    if [ "$WAS_TLS" != "$WITH_TLS" ]; then
        echo "==> TLS setting changed; reconfiguring ${BUILD} from scratch"
        rm -rf "$OUT"
        mkdir -p "$OUT"
    fi
fi

SSL_ARGS=(-DCURL_ENABLE_SSL=OFF)
if [ "$WITH_TLS" = "1" ]; then
    # CURL_CA_BUNDLE is an AMIGA path on purpose.  curl's CA auto-detection
    # searches the BUILD host's filesystem, and cross-compiling turns it off
    # rather than making it right -- so without this `curl -V` reports no CA
    # bundle at all and CURLINFO_CAINFO is empty, while tls.library quietly
    # uses its own default.  Naming the same path in both places is what makes
    # `--cacert` an override of something visible rather than of a secret.
    SSL_ARGS=(-DCURL_ENABLE_SSL=ON
              -DCURL_USE_AMITLS=ON
              -DCURL_USE_OPENSSL=OFF
              -DCURL_CA_BUNDLE=DEVS:Internet/certificates
              -DCURL_CA_PATH=none)
fi

echo "==> configuring curl in ${BUILD}"
cmake -S "$ROOT/third_party/curl" -B "$OUT" -G Ninja \
    "${SSL_ARGS[@]}" \
    -DAMIGA=1 \
    -DCMAKE_SYSTEM_NAME=Generic \
    -DCMAKE_SYSTEM_PROCESSOR=m68k \
    -DCMAKE_C_COMPILER_TARGET=m68k-unknown-amigaos \
    -DCMAKE_C_COMPILER="$AMIGA_GCC" \
    -DCMAKE_AR="$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ar" \
    -DCMAKE_RANLIB="$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-ranlib" \
    -DCMAKE_C_FLAGS="$AMIGA_CLIENT_CFLAGS -I$ROOT/include" \
    -DCMAKE_EXE_LINKER_FLAGS="$AMIGA_CLIENT_LDFLAGS" \
    -DCMAKE_C_STANDARD_LIBRARIES="-Wl,--start-group -lamigaclient -lc -Wl,--end-group" \
    -DHAVE_SELECT=1 \
    -DHAVE_GETTIMEOFDAY=1 \
    -DENABLE_IPV6=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_LIBCURL_DOCS=OFF \
    -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_LIBPSL=OFF \
    -DCURL_USE_LIBSSH2=OFF \
    -DUSE_NGHTTP2=OFF \
    -DUSE_LIBIDN2=OFF \
    -DCURL_BROTLI=OFF \
    -DCURL_ZSTD=OFF \
    -DCURL_ZLIB=OFF \
    >"$LOG" 2>&1 || {
        echo "!! configure failed; see $LOG" >&2
        tail -30 "$LOG" >&2
        exit 1
    }

# The one thing worth failing loudly on: without this curl builds against a
# libc networking API that this toolchain does not have, and the link error is
# 200 lines of undefined socket functions rather than one legible line.
if ! grep -q '^#define HAVE_PROTO_BSDSOCKET_H 1' "$OUT/lib/curl_config.h"; then
    echo "!! curl did not detect bsdsocket.library -- see $OUT/lib/curl_config.h" >&2
    echo "   (proto/bsdsocket.h failed to compile; check the NDK flags in" >&2
    echo "    clients/amiga-client.sh)" >&2
    exit 1
fi

echo "==> building"
cmake --build "$OUT" --parallel

echo
"$AMIGA_SIZE" "$OUT/src/curl" "$OUT/lib/libcurl.a" 2>/dev/null | head -3 || true
echo
ls -l "$OUT/src/curl"
echo
echo "curl for AmigaOS: $BUILD/src/curl"
grep -E '^(#define )?(HAVE_PROTO_BSDSOCKET_H|HAVE_CLOSESOCKET_CAMEL|HAVE_IOCTLSOCKET_CAMEL_FIONBIO|USE_AMITLS|USE_SSL)' \
     "$OUT/lib/curl_config.h" | sed 's/^/  /'

if [ "$WITH_TLS" = "1" ]; then
    echo
    echo "  https: needs LIBS:tls.library and DEVS:Internet/certificates on"
    echo "  the target -- clients/curl/run-fsuae.sh stages both."
fi
