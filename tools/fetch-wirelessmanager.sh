#!/usr/bin/env bash
#
# Fetch WirelessManager from Aminet and check it against the vendored copy.
#
#   tools/fetch-wirelessmanager.sh             # compare only
#   tools/fetch-wirelessmanager.sh --install   # write third_party/wirelessmanager/WirelessManager
#
# The pins are the archive's sha256, the member's size and its $VER string;
# third_party/wirelessmanager/PROVENANCE holds the member's sha256, which
# dist/make-dist.sh checks before packing.  Output is key=value plus the exit
# code: 0 match, 1 a mismatch, 2 cannot run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DIR="$ROOT/third_party/wirelessmanager"

URL=https://aminet.net/driver/net/prism2v2.lha
ARCHIVE_SHA256=25b400ef25c44af940e1887576106b8312dcd28fe6c8030fcb78804480c07bc5
MEMBER=prism2v2/C/WirelessManager
MEMBER_BYTES=196780
MEMBER_VER='$VER: WirelessManager 1.5 (24.2.2025)'

INSTALL=0
case "${1:-}" in
    "") ;;
    --install) INSTALL=1 ;;
    *) echo "usage: $0 [--install]" >&2; exit 2 ;;
esac

sha256_of() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1; }

WORK="$ROOT/build/fetch-wirelessmanager"
rm -rf "$WORK"
mkdir -p "$WORK/x"
trap 'rm -rf "$WORK"' EXIT

curl -fsSL --max-time 60 -o "$WORK/prism2v2.lha" "$URL" || {
    echo "wirelessmanager_fetch=FAIL reason=download url=$URL"; exit 2; }

got=$(sha256_of "$WORK/prism2v2.lha")
[ "$got" = "$ARCHIVE_SHA256" ] || {
    echo "wirelessmanager_fetch=FAIL archive_sha256=$got wanted=$ARCHIVE_SHA256"; exit 1; }

# lha and jlha extract quietly with xq; lhasa answers to lha too.
if command -v lha >/dev/null 2>&1; then
    (cd "$WORK/x" && lha xq ../prism2v2.lha) >/dev/null 2>&1 || true
fi
if [ ! -f "$WORK/x/$MEMBER" ] && command -v 7z >/dev/null 2>&1; then
    (cd "$WORK/x" && 7z x -y ../prism2v2.lha) >/dev/null 2>&1 || true
fi
[ -f "$WORK/x/$MEMBER" ] || {
    echo "wirelessmanager_fetch=FAIL reason=no_extractor_or_member member=$MEMBER"; exit 2; }

bytes=$(wc -c < "$WORK/x/$MEMBER" | tr -d ' ')
[ "$bytes" = "$MEMBER_BYTES" ] || {
    echo "wirelessmanager_fetch=FAIL member_bytes=$bytes wanted=$MEMBER_BYTES"; exit 1; }
LC_ALL=C grep -aqF "$MEMBER_VER" "$WORK/x/$MEMBER" || {
    echo "wirelessmanager_fetch=FAIL reason=version wanted=\"$MEMBER_VER\""; exit 1; }

member_sha=$(sha256_of "$WORK/x/$MEMBER")
pinned=$(awk '!/^#/ && $2 == "WirelessManager" { print $1 }' "$DIR/PROVENANCE")
[ "$member_sha" = "$pinned" ] || {
    echo "wirelessmanager_fetch=FAIL member_sha256=$member_sha pinned=$pinned"; exit 1; }

if [ "$INSTALL" = 1 ]; then
    cp "$WORK/x/$MEMBER" "$DIR/WirelessManager"
    chmod 644 "$DIR/WirelessManager"
fi
if ! cmp -s "$WORK/x/$MEMBER" "$DIR/WirelessManager"; then
    echo "wirelessmanager_fetch=FAIL reason=vendored_copy_differs sha256=$(sha256_of "$DIR/WirelessManager")"
    exit 1
fi
echo "wirelessmanager_fetch=PASS archive_sha256=$got member_bytes=$bytes member_sha256=$member_sha"
