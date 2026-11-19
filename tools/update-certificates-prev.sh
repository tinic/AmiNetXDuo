#!/usr/bin/env bash
#
# certificates.prev for a release's update manifest: the trust store the
# machines on its channel have now.
#
#   tools/update-certificates-prev.sh -m <mirror> -c <channel> -v <version>
#                                     [-b <builddir>]
#
# -m  the mirror base URL, e.g. http://aminetxduo.tinic.info/
# -c  release | prerelease
# -v  the version being released (tools/version.sh --product)
# -b  passed to tools/verify-update-manifest.sh
#
# The channel's published update.manifest and .sig are fetched and checked
# with the release keys (tools/verify-update-manifest.sh -n), so a manifest
# nobody signed cannot choose the value.  Then:
#
#   published is an older version   its certificates file's SHA-256 (the
#                                   file= line of class certificates)
#   published is this version       its own certificates.prev: a re-run of
#                                   the same release publishes the same line
#   nothing published (HTTP 404)    the first release on the channel: the
#                                   pin third_party/cacert/certificates.sha256
#                                   at the previous v* tag, which is what
#                                   that tag's archive installed
#
# Any other answer from the mirror is a failure, never a fallback.
#
# Output: certificates_prev=<64 hex> source=<...>, exit 0; 1 refused; 2
# cannot run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

die()  { printf 'certificates_prev=FAIL %s\n' "$*"; exit 2; }
fail() { printf 'certificates_prev=FAIL %s\n' "$*"; exit 1; }

mirror="" channel="" version="" build=""
while getopts 'm:c:v:b:' opt; do
    case "$opt" in
        m) mirror="$OPTARG" ;;
        c) channel="$OPTARG" ;;
        v) version="$OPTARG" ;;
        b) build="$OPTARG" ;;
        *) die "usage=bad_option" ;;
    esac
done
[ -n "$mirror" ] && [ -n "$version" ] || die "usage=-m_-c_-v"
case "$channel" in release|prerelease) ;; *) die "channel=$channel" ;; esac
case "$mirror" in */) ;; *) mirror="$mirror/" ;; esac
command -v curl >/dev/null 2>&1 || die "missing=curl"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

get() {  # $1 name -> $work/$1, prints the HTTP status
    curl -sS --max-time 60 -o "$work/$1" -w '%{http_code}' \
        "$mirror$channel/$1" || true
}

code=$(get update.manifest)
case "$code" in
    200)
        [ "$(get update.manifest.sig)" = 200 ] || fail "url=$mirror$channel/update.manifest.sig missing"
        v=$("$ROOT/tools/verify-update-manifest.sh" -n -c "$channel" ${build:+-b "$build"} "$work") ||
            fail "published=$mirror$channel/ $v"
        pv=$(sed -n 's/^version=//p' "$work/update.manifest")
        if [ "$pv" = "$version" ]; then
            prev=$(sed -n 's/^certificates\.prev=//p' "$work/update.manifest")
            src="published_same:$pv"
        else
            prev=$(awk '$1 ~ /^file=/ && $2 == "certificates" { print $4; exit }' \
                       "$work/update.manifest")
            src="published:$pv"
        fi
        ;;
    404)
        tag=$(git -C "$ROOT" describe --tags --abbrev=0 --match 'v*' HEAD^ 2>/dev/null) ||
            fail "first_release no_previous_tag"
        prev=$(git -C "$ROOT" show "$tag:third_party/cacert/certificates.sha256" 2>/dev/null |
               awk 'NR == 1 { print $1 }') || true
        src="tag:$tag"
        ;;
    *)
        fail "url=$mirror${channel}/update.manifest http=$code"
        ;;
esac

[[ "${prev:-}" =~ ^[0-9a-f]{64}$ ]] || fail "source=$src value=${prev:-none}"
printf 'certificates_prev=%s source=%s\n' "$prev" "$src"
