#!/usr/bin/env bash
#
# The two releases and the mirror trees the updater scenarios run against.
#
#   tests/updater/make-releases.sh [-O <old dist>] [-o <outdir>]
#
# OLD is the tree under test as it is: tools/build-release.sh, or -O, a
# directory it already wrote (build/dist).  It is what the guest installs.
# NEW is the same tree with the version bumped (a pre-release gets ".1", so
# 1.0.0-rc4 becomes 1.0.0-rc4.1; a release gets its patch raised), built the
# same way in a copy of the tree that is deleted afterwards.  Its bytes differ
# from OLD's in every $VER, which is what an update has to replace.
#
# <outdir> (default build/updscn/releases) then holds
#
#   old/  new/              each the archive, AmiNetXDuo/ and update/
#   keys/unknown.pem        a key no build knows
#   mirror/<tree>/<channel>/   update.manifest, .sig and the containers
#   releases.env            OLD_VERSION= NEW_VERSION= OLD_ARCHIVE= ...
#
# Mirror trees, all signed with tests/updater/test-only-update-key-a.pem as
# key test-a unless named otherwise:
#
#   good          NEW in its channel's folder; release/ holds an older
#                 release (0.9.0), so a release-channel machine has nothing
#   current       OLD's own manifest: nothing newer
#   badsig        good's manifest, one digit of the signature changed
#   unknownkey    good's manifest signed by keys/unknown.pem as test-x
#   truncated     good's manifest without end=, signed as it is
#   badcontainer  good's manifest, a byte of every container inverted
#   badmember     a member of every container altered, its CRC-16 and the
#                 container= line right, the file= line not
#   drop          good, and the mirror cuts every container at half
#   broken        NEW whose full bsdsocket.library is not a library, every
#                 line right: it installs, and AddNetInterface fails
#
# Output is key=value plus an exit code: 0 written, 1 a step failed, 2 cannot
# run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OLD_DIST=""
OUT="$ROOT/build/updscn/releases"
while getopts 'O:o:' opt; do
    case "$opt" in
        O) OLD_DIST="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        *) echo "releases=FAIL usage=bad_option"; exit 2 ;;
    esac
done

kv() { printf '%s=%s\n' "$1" "$2"; }
die() { echo "releases=FAIL $*"; exit 2; }
fail() { echo "releases=FAIL $*"; exit 1; }
sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

KEY_A="$ROOT/tests/updater/test-only-update-key-a.pem"
[ -f "$KEY_A" ] || die "missing=$KEY_A"
command -v openssl >/dev/null 2>&1 || die "missing=openssl"
command -v rsync >/dev/null 2>&1 || die "missing=rsync"
MKV="$ROOT/tests/updater/mkvariant.py"
MUM="$ROOT/tools/make-update-manifest.sh"
[ -x "$MUM" ] || die "missing=$MUM"

# Both builds' UpdateAmiNetXDuo trust the TEST key test-a and nothing else
# (src/tools/updkeys.c), which is what the mirror trees are signed with.
# tools/build-release.sh records it; check-release-candidate.sh refuses it.
export AMINETXDUO_UPDATE_TEST_KEY=ON

rm -rf "$OUT"
mkdir -p "$OUT/keys" "$OUT/mirror"

# ---------------------------------------------------------------- OLD --
if [ -z "$OLD_DIST" ]; then
    "$ROOT/tools/build-release.sh" > "$OUT/build-old.log" 2>&1 ||
        fail "step=build_old log=$OUT/build-old.log"
    OLD_DIST="$ROOT/build/dist"
fi
[ -d "$OLD_DIST/AmiNetXDuo" ] && [ -f "$OLD_DIST/update/update.manifest" ] ||
    die "old_dist=$OLD_DIST missing=AmiNetXDuo_or_update"
mkdir -p "$OUT/old"
cp -a "$OLD_DIST/AmiNetXDuo" "$OLD_DIST/update" "$OUT/old/"
cp -a "$OLD_DIST"/AmiNetXDuo-*.lha "$OUT/old/"
OLD_VERSION=$(sed -n 's/^version=//p' "$OUT/old/update/update.manifest")
OLD_ARCHIVE=$(ls "$OUT/old"/AmiNetXDuo-*.lha)
kv old_version "$OLD_VERSION"

# ---------------------------------------------------------------- NEW --
case "$OLD_VERSION" in
    *-*) NEW_VERSION="$OLD_VERSION.1" ;;
    *)   NEW_VERSION="${OLD_VERSION%.*}.$(( ${OLD_VERSION##*.} + 1 ))" ;;
esac
kv new_version "$NEW_VERSION"
SRC="$OUT/src-new"
rsync -a --exclude=/build "$ROOT/" "$SRC/"
case "$NEW_VERSION" in
    *-*) sed -i "s/^set(AMINETXDUO_VERSION_PRERELEASE \"[^\"]*\")/set(AMINETXDUO_VERSION_PRERELEASE \"${NEW_VERSION#*-}\")/" \
             "$SRC/CMakeLists.txt" ;;
    *)   sed -i "s/^\([[:space:]]*project([[:space:]]*AmiNetXDuo[[:space:]]\{1,\}VERSION[[:space:]]\{1,\}\)[0-9.]*/\1$NEW_VERSION/" \
             "$SRC/CMakeLists.txt" ;;
esac
[ "$("$SRC/tools/version.sh" --product)" = "$NEW_VERSION" ] ||
    fail "step=bump got=$("$SRC/tools/version.sh" --product)"
"$SRC/tools/build-release.sh" > "$OUT/build-new.log" 2>&1 ||
    fail "step=build_new log=$OUT/build-new.log"
mkdir -p "$OUT/new"
cp -a "$SRC/build/dist/AmiNetXDuo" "$SRC/build/dist/update" "$OUT/new/"
cp -a "$SRC/build/dist"/AmiNetXDuo-*.lha "$OUT/new/"
rm -rf "$SRC"
NEW_ARCHIVE=$(ls "$OUT/new"/AmiNetXDuo-*.lha)
[ "$(sed -n 's/^version=//p' "$OUT/new/update/update.manifest")" = "$NEW_VERSION" ] ||
    fail "step=new_manifest_version"

# ---------------------------------------------------------------- keys --
openssl genpkey -algorithm ed25519 -out "$OUT/keys/unknown.pem" 2>/dev/null ||
    fail "step=genkey"

# $1 manifest, $2 key, $3 key id -> $1.sig
sign() {
    local m="$1" key="$2" id="$3" raw="$1.raw"
    openssl pkeyutl -sign -rawin -inkey "$key" -in "$m" -out "$raw" ||
        fail "step=sign manifest=$m"
    openssl pkeyutl -verify -rawin -inkey "$key" -in "$m" -sigfile "$raw" \
        >/dev/null || fail "step=verify manifest=$m"
    printf 'key=%s sig=%s\n' "$id" "$(od -An -tx1 -v "$raw" | tr -d ' \n')" \
        > "$m.sig"
    rm -f "$raw"
}

NEW_CHANNEL=$(sed -n 's/^channel=//p' "$OUT/new/update/update.manifest")
OLD_CHANNEL=$(sed -n 's/^channel=//p' "$OUT/old/update/update.manifest")

# $1 tree, $2 source update/ dir, $3 channel: the manifest and containers.
tree() {
    local t="$OUT/mirror/$1/$3"
    mkdir -p "$t"
    cp "$2"/update.manifest "$2"/*.update.lha "$t/"
}

# good: NEW, and an older release in release/.
tree good "$OUT/new/update" "$NEW_CHANNEL"
sign "$OUT/mirror/good/$NEW_CHANNEL/update.manifest" "$KEY_A" test-a
if [ "$NEW_CHANNEL" = prerelease ]; then
    _prev=$(sha256_of "$OUT/old/AmiNetXDuo/Devs/Internet/certificates")
    "$MUM" -p "$_prev" -v 0.9.0 -c release -o "$OUT/release-0.9.0" "$OUT/old" \
        > "$OUT/release-0.9.0.log" 2>&1 || fail "step=release_0_9_0"
    tree good "$OUT/release-0.9.0" release
    rm -rf "$OUT/release-0.9.0" "$OUT/release-0.9.0.log"
    sign "$OUT/mirror/good/release/update.manifest" "$KEY_A" test-a
fi

tree current "$OUT/old/update" "$OLD_CHANNEL"
sign "$OUT/mirror/current/$OLD_CHANNEL/update.manifest" "$KEY_A" test-a

G="$OUT/mirror/good/$NEW_CHANNEL"

# badsig: the first hex digit of the signature changed.
tree badsig "$OUT/new/update" "$NEW_CHANNEL"
awk '{ s = substr($2, 5); c = substr(s, 1, 1)
       print $1 " sig=" (c == "0" ? "1" : "0") substr(s, 2) }' \
    "$G/update.manifest.sig" > "$OUT/mirror/badsig/$NEW_CHANNEL/update.manifest.sig"
! cmp -s "$G/update.manifest.sig" "$OUT/mirror/badsig/$NEW_CHANNEL/update.manifest.sig" ||
    fail "step=badsig unchanged"

tree unknownkey "$OUT/new/update" "$NEW_CHANNEL"
sign "$OUT/mirror/unknownkey/$NEW_CHANNEL/update.manifest" "$OUT/keys/unknown.pem" test-x

tree truncated "$OUT/new/update" "$NEW_CHANNEL"
python3 "$MKV" truncate "$G/update.manifest" \
    "$OUT/mirror/truncated/$NEW_CHANNEL/update.manifest" >/dev/null || fail "step=truncated"
sign "$OUT/mirror/truncated/$NEW_CHANNEL/update.manifest" "$KEY_A" test-a

tree badcontainer "$OUT/new/update" "$NEW_CHANNEL"
for c in "$OUT/mirror/badcontainer/$NEW_CHANNEL"/*.update.lha; do
    python3 "$MKV" flipbyte "$c" "$c.x" >/dev/null && mv "$c.x" "$c" || fail "step=badcontainer"
done
cp "$G/update.manifest.sig" "$OUT/mirror/badcontainer/$NEW_CHANNEL/"

# badmember: Libs/bsdsocket.library of every container.
tree badmember "$OUT/new/update" "$NEW_CHANNEL"
_m="$OUT/mirror/badmember/$NEW_CHANNEL"
_args=()
for c in "$_m"/*.update.lha; do
    fl=${c##*-}; fl=${fl%.update.lha}
    python3 "$MKV" flipmember "$c" "$c.x" Libs/bsdsocket.library >/dev/null &&
        mv "$c.x" "$c" || fail "step=badmember container=$c"
    _args+=("container.$fl=$c")
done
python3 "$MKV" manifest "$G/update.manifest" "$_m/update.manifest" "${_args[@]}" \
    >/dev/null || fail "step=badmember_manifest"
sign "$_m/update.manifest" "$KEY_A" test-a

tree drop "$OUT/new/update" "$NEW_CHANNEL"
cp "$G/update.manifest.sig" "$OUT/mirror/drop/$NEW_CHANNEL/"
echo half > "$OUT/mirror/drop/DROP"

# broken: the full flavour's bsdsocket.library is text that names us.
tree broken "$OUT/new/update" "$NEW_CHANNEL"
_m="$OUT/mirror/broken/$NEW_CHANNEL"
_c=$(ls "$_m"/*-full.update.lha)
printf '$VER: bsdsocket.library %s (1.1.2026) AmiNetXDuo -- not a library\n' \
    "$NEW_VERSION" > "$OUT/broken-bsdsocket.library"
python3 "$MKV" member "$_c" "$_c.x" Libs/bsdsocket.library "$OUT/broken-bsdsocket.library" \
    >/dev/null && mv "$_c.x" "$_c" || fail "step=broken"
python3 "$MKV" manifest "$G/update.manifest" "$_m/update.manifest" \
    "container.full=$_c" "file.full.bsdsocket.library=$OUT/broken-bsdsocket.library" \
    >/dev/null || fail "step=broken_manifest"
rm -f "$OUT/broken-bsdsocket.library"
sign "$_m/update.manifest" "$KEY_A" test-a

{
    kv OLD_VERSION "$OLD_VERSION"
    kv NEW_VERSION "$NEW_VERSION"
    kv OLD_CHANNEL "$OLD_CHANNEL"
    kv NEW_CHANNEL "$NEW_CHANNEL"
    kv OLD_ARCHIVE "$OLD_ARCHIVE"
    kv NEW_ARCHIVE "$NEW_ARCHIVE"
} > "$OUT/releases.env"

for t in "$OUT"/mirror/*/; do
    t=${t%/}
    printf 'mirror_tree=%s files=%s\n' "${t##*/}" "$(find "$t" -type f | wc -l | tr -d ' ')"
done
echo "releases=PASS out=$OUT old=$OLD_VERSION new=$NEW_VERSION"
