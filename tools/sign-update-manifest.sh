#!/usr/bin/env bash
#
# Sign a release's update manifest.
#
#   tools/sign-update-manifest.sh -k <key id> [-p <certificates.prev>]
#                                 -o <outdir> <update dir>
#
# <update dir>  what tools/make-update-manifest.sh wrote: update.manifest and
#               the *.update.lha containers (a CI candidate's update/).
# -k            the key id, as src/tools/updkeys.c lists it in the release
#               table, e.g. rel-2026.
# -p            64 hex digits: replaces the manifest's certificates.prev line
#               (release.yml takes it from the channel's published manifest;
#               tools/update-certificates-prev.sh).  Nothing else changes.
# -o            emptied first; gets update.manifest, update.manifest.sig and
#               the containers.
#
# THE KEY is the Ed25519 PKCS#8 PEM in the environment variable
# AMINETXDUO_UPDATE_SIGNING_KEY, never an argument.  It is written to a
# mktemp file under umask 077 for openssl and removed on exit.  The script
# refuses a key whose public half is not the release key <key id> in
# updkeys.c, so a wrong secret cannot sign a manifest the tool refuses.
#
# update.manifest.sig is one line, "key=<id> sig=<128 hex digits>", the
# format src/tools/updmanifest.h describes.  openssl verifies it here;
# tools/verify-update-manifest.sh then checks it with the code the Amiga
# runs, before anything is published.
#
# Output is key=value plus an exit code: 0 signed, 1 refused, 2 cannot run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

die()  { printf 'update_sign=FAIL %s\n' "$*"; exit 2; }
fail() { printf 'update_sign=FAIL %s\n' "$*"; exit 1; }

keyid="" prev="" outdir=""
while getopts 'k:p:o:' opt; do
    case "$opt" in
        k) keyid="$OPTARG" ;;
        p) prev="$OPTARG" ;;
        o) outdir="$OPTARG" ;;
        *) die "usage=bad_option" ;;
    esac
done
shift $((OPTIND - 1))
[ $# = 1 ] || die "usage=update_dir"
src="$1"
[ -n "$keyid" ] || die "missing=-k"
[ -n "$outdir" ] || die "missing=-o"
[ -f "$src/update.manifest" ] || die "missing=$src/update.manifest"
[ -n "${AMINETXDUO_UPDATE_SIGNING_KEY:-}" ] || die "missing=AMINETXDUO_UPDATE_SIGNING_KEY"
command -v openssl >/dev/null 2>&1 || die "missing=openssl"
command -v python3 >/dev/null 2>&1 || die "missing=python3"
if [ -n "$prev" ]; then
    [[ "$prev" =~ ^[0-9a-f]{64}$ ]] || die "certificates_prev=$prev"
fi

want_pk=$(python3 -I "$ROOT/tools/updkeys.py" list "$ROOT/src/tools/updkeys.c" |
          sed -n "s/^key=$keyid pk=\([0-9a-f]*\) build=release\$/\1/p")
[ -n "$want_pk" ] || fail "key=$keyid not_in=src/tools/updkeys.c_release_table"

umask 077
work=$(mktemp -d)
cleanup() {
    local st=$?
    rm -rf "$work"
    [ "$st" = 0 ] || rm -rf "$outdir"
}
trap cleanup EXIT
key="$work/key.pem"
printf '%s\n' "$AMINETXDUO_UPDATE_SIGNING_KEY" > "$key"

got_pk=$(openssl pkey -in "$key" -pubout -outform DER 2>/dev/null |
         tail -c 32 | od -An -tx1 -v | tr -d ' \n') ||
    fail "step=read_key"
[ "$got_pk" = "$want_pk" ] || fail "step=key_match key=$keyid got_pk=$got_pk"
openssl pkey -in "$key" -pubout -out "$work/pub.pem" 2>/dev/null ||
    fail "step=public_key"

rm -rf "$outdir"
mkdir -p "$outdir"
umask 022
manifest="$outdir/update.manifest"
if [ -n "$prev" ]; then
    [ "$(grep -c '^certificates\.prev=' "$src/update.manifest" || true)" = 1 ] ||
        fail "step=certificates_prev lines_not_one"
    sed "s/^certificates\.prev=.*/certificates.prev=$prev/" \
        "$src/update.manifest" > "$manifest"
    # The one line, and only it, changed (or nothing, when it was the same).
    changed=$(diff "$src/update.manifest" "$manifest" | grep -c '^[<>]' || true)
    case "$changed" in 0|2) ;; *) fail "step=certificates_prev changed=$changed" ;; esac
else
    cp "$src/update.manifest" "$manifest"
fi
cp "$src"/*.update.lha "$outdir/" 2>/dev/null || fail "step=containers none_in=$src"

openssl pkeyutl -sign -rawin -inkey "$key" -in "$manifest" -out "$work/sig" ||
    fail "step=sign"
rm -f "$key"
openssl pkeyutl -verify -rawin -pubin -inkey "$work/pub.pem" -in "$manifest" \
    -sigfile "$work/sig" >/dev/null || fail "step=openssl_verify"
[ "$(wc -c < "$work/sig" | tr -d ' ')" = 64 ] || fail "step=sig_length"
printf 'key=%s sig=%s\n' "$keyid" "$(od -An -tx1 -v "$work/sig" | tr -d ' \n')" \
    > "$manifest.sig"

sha() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1; }
printf 'update_sign=PASS key=%s version=%s channel=%s certificates_prev=%s manifest_sha256=%s\n' \
    "$keyid" "$(sed -n 's/^version=//p' "$manifest")" \
    "$(sed -n 's/^channel=//p' "$manifest")" \
    "$(sed -n 's/^certificates\.prev=//p' "$manifest")" "$(sha "$manifest")"
