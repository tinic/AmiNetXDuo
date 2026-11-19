#!/usr/bin/env bash
#
# Check a signed update release with the code the Amiga runs.
#
#   tools/verify-update-manifest.sh [-b <builddir>] [-c <channel>] [-n] <dir>
#
# <dir>  update.manifest, update.manifest.sig and, unless -n, the containers
#        (tools/sign-update-manifest.sh -o).
# -b     where updverify is built (host CMake); default build/updverify.
# -c     release | prerelease: the manifest's channel must be this.
# -n     the manifest and signature only (a published manifest fetched
#        without its containers).
#
# updverify (src/tools/updverify.c) links the updkeys.c and updsig.c of
# UpdateAmiNetXDuo, built without UPD_TEST_KEY: it accepts a signature by a
# release key only, and parses the manifest for every flavour.  Then every
# container= line must name a file here of that size and SHA-256.
#
# Output is key=value plus an exit code: 0 accepted, 1 refused, 2 cannot run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

die()  { printf 'update_verify=FAIL %s\n' "$*"; exit 2; }
fail() { printf 'update_verify=FAIL %s\n' "$*"; exit 1; }

build="$ROOT/build/updverify" channel="" nocontainers=0
while getopts 'b:c:n' opt; do
    case "$opt" in
        b) build="$OPTARG" ;;
        c) channel="$OPTARG" ;;
        n) nocontainers=1 ;;
        *) die "usage=bad_option" ;;
    esac
done
shift $((OPTIND - 1))
[ $# = 1 ] || die "usage=dir"
dir="$1"
[ -f "$dir/update.manifest" ] || die "missing=$dir/update.manifest"
[ -f "$dir/update.manifest.sig" ] || die "missing=$dir/update.manifest.sig"

# Always built from this tree, so the keys are this tree's.
if ! { cmake -S "$ROOT" -B "$build" -DCMAKE_BUILD_TYPE=Release &&
       cmake --build "$build" --target updverify; } > "$build.log" 2>&1; then
    die "step=build log=$build.log"
fi
uv=$(find "$build" -type f -name updverify -perm -u+x -print -quit)
[ -n "$uv" ] || die "step=build missing=updverify"

out=$("$uv" "$dir/update.manifest" "$dir/update.manifest.sig" ${channel:+"$channel"}) || {
    printf '%s\n' "$out"
    exit 1
}

if [ "$nocontainers" = 0 ]; then
    sha() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1; }
    n=0
    while read -r flavour size sum name; do
        case "$name" in ''|.|..|*/*) fail "container=$flavour name=$name" ;; esac
        [ -f "$dir/$name" ] || fail "container=$flavour missing=$name"
        [ "$(wc -c < "$dir/$name" | tr -d ' ')" = "$size" ] ||
            fail "container=$flavour size=$(wc -c < "$dir/$name" | tr -d ' ') wanted=$size"
        [ "$(sha "$dir/$name")" = "$sum" ] || fail "container=$flavour sha256"
        n=$((n + 1))
    done < <(sed -n 's/^container=//p' "$dir/update.manifest")
    [ "$n" -gt 0 ] || fail "containers=0"
    extra=$(find "$dir" -maxdepth 1 -type f ! -name update.manifest \
                 ! -name update.manifest.sig -print | while read -r f; do
                grep -q " ${f##*/}\$" <(sed -n 's/^container=//p' "$dir/update.manifest") ||
                    echo "${f##*/}"
            done | head -n 1)
    [ -z "$extra" ] || fail "unlisted_file=$extra"
    out="$out containers=$n"
fi
printf '%s\n' "$out"
