#!/usr/bin/env bash
#
# Publish a signed update release to the mirror, atomically.
#
#   tools/publish-update.sh -c <channel> [-n | -r] <signed dir>
#
# <signed dir>  update.manifest, update.manifest.sig and the containers,
#               already accepted by tools/verify-update-manifest.sh.
# -c            release | prerelease
# -n            print what would run; upload nothing.
# -r            the read-back below only: upload nothing, check what the
#               mirror serves now against <signed dir>.
#
# The mirror (CT 175, http://aminetxduo.tinic.info/) takes uploads from the
# self-hosted runner only, as user mirror-upload with the key in
# ~/ai/.secrets/aminetxduo-mirror-upload.  Its forced command allows two
# things: rsync into the never-served staging folder of a channel, and
# "publish <channel>", which copies staging into a new snapshot and swaps
# the channel's symlink with one rename.  A client sees the old tree or the
# new one, never a new manifest beside half a container.
#
# After the swap the read-back fetches the manifest, its signature and
# every container the manifest lists over HTTP, as a machine would, compares
# each byte for byte with <signed dir>, and checks the fetched set with the
# updater's code (tools/verify-update-manifest.sh, full SHA-256).  Publish
# passes only when everything is reachable and correct.
#
# Environment (release.yml sets the first two from the variables of the
# environment `release`, so a move of the mirror needs no code change):
#   AMINETXDUO_MIRROR_UPLOAD_HOST  default 192.168.243.28
#   AMINETXDUO_MIRROR_UPLOAD_USER  default mirror-upload
#   AMINETXDUO_MIRROR_UPLOAD_KEY   default ~/ai/.secrets/aminetxduo-mirror-upload
#   AMINETXDUO_UPDATE_MIRROR       default http://aminetxduo.tinic.info/
#
# Output is key=value plus an exit code: 0 published, 1 failed, 2 cannot run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C

die()  { printf 'update_publish=FAIL %s\n' "$*"; exit 2; }
fail() { printf 'update_publish=FAIL %s\n' "$*"; exit 1; }

channel="" dry=0 readback_only=0
while getopts 'c:nr' opt; do
    case "$opt" in
        c) channel="$OPTARG" ;;
        n) dry=1 ;;
        r) readback_only=1 ;;
        *) die "usage=bad_option" ;;
    esac
done
shift $((OPTIND - 1))
[ $# = 1 ] || die "usage=signed_dir"
dir="${1%/}"
case "$channel" in release|prerelease) ;; *) die "channel=$channel" ;; esac
[ -f "$dir/update.manifest" ] && [ -f "$dir/update.manifest.sig" ] ||
    die "missing=$dir/update.manifest_or_sig"
[ "$(sed -n 's/^channel=//p' "$dir/update.manifest")" = "$channel" ] ||
    fail "manifest_channel_not=$channel"

key="${AMINETXDUO_MIRROR_UPLOAD_KEY:-$HOME/ai/.secrets/aminetxduo-mirror-upload}"
host="${AMINETXDUO_MIRROR_UPLOAD_USER:-mirror-upload}@${AMINETXDUO_MIRROR_UPLOAD_HOST:-192.168.243.28}"
mirror="${AMINETXDUO_UPDATE_MIRROR:-http://aminetxduo.tinic.info/}"
case "$mirror" in */) ;; *) mirror="$mirror/" ;; esac
[ "$readback_only" = 1 ] || [ -r "$key" ] || die "missing=upload_key"
for c in rsync ssh curl cmp; do
    command -v "$c" >/dev/null 2>&1 || die "missing=$c"
done
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ssh_cmd="ssh -i $key -o IdentitiesOnly=yes -o BatchMode=yes"

if [ "$dry" = 1 ]; then
    printf 'would=rsync -rt --delete %s/ %s:%s/\n' "$dir" "$host" "$channel"
    printf 'would=ssh %s publish %s\n' "$host" "$channel"
    exit 0
fi

out="result=ok readback_only=1"
if [ "$readback_only" = 0 ]; then
    # No -a: the staging side is rrsync -wo and ownership is not ours to set.
    rsync -rt --delete -e "$ssh_cmd" "$dir/" "$host:$channel/" ||
        fail "step=upload"
    # shellcheck disable=SC2086
    out=$($ssh_cmd "$host" publish "$channel") || fail "step=publish $out"
    case "$out" in *result=ok*) ;; *) fail "step=publish $out" ;; esac
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
files=(update.manifest update.manifest.sig)
while read -r _ _ _ name; do
    case "$name" in ''|.|..|*/*) fail "container_name=$name" ;; esac
    files+=("$name")
done < <(sed -n 's/^container=//p' "$dir/update.manifest")
[ "${#files[@]}" -gt 2 ] || fail "containers=0"
for f in "${files[@]}"; do
    curl -fsS --max-time 300 -H 'Cache-Control: no-cache' -o "$work/$f" \
        "$mirror$channel/$f" || fail "step=readback url=$mirror$channel/$f"
    cmp -s "$dir/$f" "$work/$f" || fail "step=readback differs=$f"
done
v=$("$ROOT/tools/verify-update-manifest.sh" -c "$channel" \
        ${AMINETXDUO_UPDVERIFY_BUILD:+-b "$AMINETXDUO_UPDVERIFY_BUILD"} "$work") ||
    fail "step=readback_verify $v"

printf 'update_publish=PASS channel=%s version=%s files=%s %s\n' "$channel" \
    "$(sed -n 's/^version=//p' "$dir/update.manifest")" "${#files[@]}" "${out#result=ok }"
