#!/usr/bin/env bash
#
# Print the AmiNetXDuo version, without configuring cmake and without running
# an Amiga binary.
#
#   tools/version.sh              0.1.0+nx6.5.1     the compound version
#   tools/version.sh --product    0.1.0             ours alone
#   tools/version.sh --netxduo    6.5.1             what we are built on
#   tools/version.sh --threadx    6.5.1
#   tools/version.sh --build      1234              commit count, or empty
#   tools/version.sh --long       AmiNetXDuo 0.1.0 (NetX Duo 6.5.1, ...)
#   tools/version.sh --tag        v0.1.0            the release tag for it
#   tools/version.sh --env        KEY=VALUE lines, for $GITHUB_OUTPUT
#   tools/version.sh --check H    H is a generated version.h; do we agree?
#
# WHY THIS EXISTS SEPARATELY FROM CMAKE
#
# A workflow has to label its own artefacts before -- or without -- a build:
# naming the .lha, naming an upload, deciding whether a tag matches the tree.
# Making that require a configured build directory would mean a job that only
# uploads has to compile first, and making it require running a built m68k
# binary would mean it cannot be done at all on a Linux runner.
#
# So this reads the same three files cmake/AmiNetXDuoVersion.cmake reads --
# the project() line, nx_api.h, tx_api.h -- and applies the same pin check.
# The two implementations agreeing is enforced by the `version_scheme` host
# test, which diffs this script's answers against the generated header.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

CMAKELISTS="$ROOT/CMakeLists.txt"
VERSION_CMAKE="$ROOT/cmake/AmiNetXDuoVersion.cmake"
NX_API="$ROOT/third_party/netxduo/common/inc/nx_api.h"
TX_API="$ROOT/third_party/threadx/common/inc/tx_api.h"

die() { printf '%s: %s\n' "${0##*/}" "$*" >&2; exit 2; }

# ----------------------------------------------------------------- ours -----

# project(AmiNetXDuo VERSION 0.1.0 LANGUAGES C ASM)
product=$(sed -n \
    's/^[[:space:]]*project([[:space:]]*AmiNetXDuo[[:space:]]\{1,\}VERSION[[:space:]]\{1,\}\([0-9]\{1,\}\.[0-9]\{1,\}\.[0-9]\{1,\}\).*/\1/p' \
    "$CMAKELISTS" | head -1)
[ -n "$product" ] || die "no project(AmiNetXDuo VERSION x.y.z ...) in $CMAKELISTS"

# ------------------------------------------------------------- upstream -----

# MAJOR.MINOR.PATCH out of a vendored api header, by macro stem.
read_upstream() {
    local header="$1" prefix="$2" field value out=""
    [ -f "$header" ] || die "$header is missing -- git submodule update --init --recursive"
    for field in MAJOR MINOR PATCH; do
        value=$(sed -n \
            "s/^[[:space:]]*#define[[:space:]]\{1,\}${prefix}_${field}_VERSION[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p" \
            "$header" | head -1)
        [ -n "$value" ] || die "${prefix}_${field}_VERSION not found in $header"
        out="${out:+$out.}$value"
    done
    printf '%s' "$out"
}

# The pin, as recorded for cmake.  Read rather than repeated: two files
# claiming to know the NetX Duo version is exactly the failure this whole
# exercise is about.
read_pin() {
    sed -n "s/^set(AMINETXDUO_$1_VERSION_PIN[[:space:]]\{1,\}\"\([^\"]*\)\").*/\1/p" \
        "$VERSION_CMAKE" | head -1
}

check_pin() {
    local what="$1" found="$2" pinned="$3" header="$4"
    [ "$found" = "$pinned" ] && return 0
    printf '%s: %s has moved.\n' "${0##*/}" "$what" >&2
    printf '  recorded : %s   (AMINETXDUO_%s_VERSION_PIN)\n' "$pinned" "$what" >&2
    printf '  submodule: %s   (%s)\n' "$found" "$header" >&2
    printf '  Set the pin in cmake/AmiNetXDuoVersion.cmake in the same\n' >&2
    printf '  commit as the submodule bump.\n' >&2
    exit 3
}

upstream() {
    netxduo=$(read_upstream "$NX_API" NETXDUO)
    threadx=$(read_upstream "$TX_API" THREADX)
    check_pin NETXDUO "$netxduo" "$(read_pin NETXDUO)" "$NX_API"
    check_pin THREADX "$threadx" "$(read_pin THREADX)" "$TX_API"
}

# Commit count, empty outside a checkout -- an unpacked source tarball has no
# .git and must still be able to say what version it is.
build_count() {
    if command -v git >/dev/null 2>&1 && [ -e "$ROOT/.git" ]; then
        git -C "$ROOT" rev-list --count HEAD 2>/dev/null || true
    fi
}

# ----------------------------------------------------------------- main -----

case "${1:---compound}" in
    --product)  printf '%s\n' "$product" ;;
    --tag)      printf 'v%s\n' "$product" ;;
    --netxduo)  upstream; printf '%s\n' "$netxduo" ;;
    --threadx)  upstream; printf '%s\n' "$threadx" ;;
    --build)    printf '%s\n' "$(build_count)" ;;
    --compound) upstream; printf '%s+nx%s\n' "$product" "$netxduo" ;;
    --long)     upstream
                printf 'AmiNetXDuo %s (NetX Duo %s, ThreadX %s)\n' \
                       "$product" "$netxduo" "$threadx" ;;
    --env)      upstream
                printf 'AMINETXDUO_VERSION=%s\n' "$product"
                printf 'AMINETXDUO_NETXDUO_VERSION=%s\n' "$netxduo"
                printf 'AMINETXDUO_THREADX_VERSION=%s\n' "$threadx"
                printf 'AMINETXDUO_VERSION_COMPOUND=%s+nx%s\n' "$product" "$netxduo"
                printf 'AMINETXDUO_VERSION_LONG=AmiNetXDuo %s (NetX Duo %s, ThreadX %s)\n' \
                       "$product" "$netxduo" "$threadx"
                printf 'AMINETXDUO_VERSION_TAG=v%s\n' "$product"
                printf 'AMINETXDUO_VERSION_BUILD=%s\n' "$(build_count)" ;;
    --check)
                # The generated header and this script read the same three
                # files by two different implementations; this is what keeps
                # them from drifting apart.  Registered as the `version_scheme`
                # ctest.
                header="${2:-}"
                [ -n "$header" ] || die "--check needs a path to version.h"
                [ -f "$header" ] || die "$header does not exist -- configure first"
                upstream
                bad=0
                expect() {
                    local macro="$1" want="$2" got
                    got=$(sed -n "s/^#define[[:space:]]\{1,\}${macro}[[:space:]]\{1,\}\"\(.*\)\"[[:space:]]*$/\1/p" \
                          "$header" | head -1)
                    if [ "$got" != "$want" ]; then
                        printf '  %-28s header %-40s script %s\n' \
                               "$macro" "\"$got\"" "\"$want\"" >&2
                        bad=1
                    fi
                }
                expect AMINETXDUO_VERSION          "$product"
                expect AMINETXDUO_NETXDUO_VERSION  "$netxduo"
                expect AMINETXDUO_THREADX_VERSION  "$threadx"
                expect AMINETXDUO_VERSION_COMPOUND "$product+nx$netxduo"
                expect AMINETXDUO_VERSION_LONG \
                       "AmiNetXDuo $product (NetX Duo $netxduo, ThreadX $threadx)"
                if [ "$bad" != 0 ]; then
                    printf '%s: %s disagrees with tools/version.sh\n' \
                           "${0##*/}" "$header" >&2
                    exit 1
                fi
                printf 'version_scheme: %s+nx%s, header and script agree\n' \
                       "$product" "$netxduo" ;;
    -h|--help)  sed -n '3,27p' "$0" | sed 's/^#\{0,1\} \{0,1\}//' ;;
    *)          die "unknown option: $1  (try --help)" ;;
esac
