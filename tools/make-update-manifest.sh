#!/usr/bin/env bash
#
# The update containers and the unsigned update manifest of a release.
#
#   tools/make-update-manifest.sh -p <prev> [-v <version>] [-c <channel>]
#                                 [-o <outdir>] [<distdir>]
#
# <distdir>  what dist/make-dist.sh writes, the directory holding AmiNetXDuo/;
#            default build/dist.
# -p <prev>  DEVS:Internet/certificates as the previous release shipped it,
#            for certificates.prev: 64 hex digits, or a file whose first word
#            is those (third_party/cacert/certificates.sha256 at the previous
#            tag), or the previous certificates file itself, which is hashed.
#            Required when the tree carries a trust store.
# -v         the version; default tools/version.sh --product.
# -c         release | prerelease; default prerelease when the version has a
#            pre-release part, release otherwise.
# -o         default <distdir>/update.  Emptied first.
#
# Writes, per flavour with a Flavours/<name> in the tree:
#
#   AmiNetXDuo-<version>-<flavour>.update.lha   stored (-lh0-, dist/lhapack.py)
#   update.manifest                             src/tools/updmanifest.h
#
# THE CONTAINER holds the flavour's installed files of the classes the
# manifest knows, and nothing else.  A member is named by its class and its
# manifest path, in the manifest's order:
#
#   LIBS           Libs/<path>
#   DEVS/Networks  Devs/Networks/<path>
#   C              C/<path>
#   certificates   Devs/Internet/<path>
#
# Flavours/<name> is the file list: libs=, devs= and commands= name the
# drawers, file= the files.  The fat flavour's C: also gets every C/ command
# that C/fat does not replace, as the Installer copies C/ before C/fat.  The
# trust store goes in every flavour, as the Installer copies it for all.
#
# REPRODUCIBLE: the members are in a fixed order and dated SOURCE_DATE_EPOCH
# (default 315532800, 1980-01-01 UTC), so the same tree gives the same bytes.
#
# Output is key=value plus an exit code: 0 written, 1 a defect in the tree,
# 2 cannot run.
#
# SPDX-License-Identifier: MIT

set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# fd 3 is stdout, so a failure inside a redirected function still reaches it.
exec 3>&1
die()  { printf 'update_manifest=FAIL %s\n' "$*" >&3; exit 2; }
fail() { printf 'update_manifest=FAIL %s\n' "$*" >&3; exit 1; }

prev="" version="" channel="" outdir=""
while getopts 'p:v:c:o:' opt; do
    case "$opt" in
        p) prev="$OPTARG" ;;
        v) version="$OPTARG" ;;
        c) channel="$OPTARG" ;;
        o) outdir="$OPTARG" ;;
        *) die "usage=bad_option" ;;
    esac
done
shift $((OPTIND - 1))
distdir="${1:-$ROOT/build/dist}"
[ -d "$distdir/AmiNetXDuo/Flavours" ] || die "missing=$distdir/AmiNetXDuo/Flavours"
TREE=$(cd "$distdir/AmiNetXDuo" && pwd)
outdir="${outdir:-$distdir/update}"
command -v python3 >/dev/null 2>&1 || die "missing=python3"

[ -n "$version" ] || version=$("$ROOT/tools/version.sh" --product)
if [ -z "$channel" ]; then
    case "$version" in *-*) channel=prerelease ;; *) channel=release ;; esac
fi
case "$channel" in release|prerelease) ;; *) die "channel=$channel" ;; esac

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-315532800}"

sha256_of() {
    (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d' ' -f1
}
is_hex64() { [[ "$1" =~ ^[0-9a-f]{64}$ ]]; }

certs="$TREE/Devs/Internet/certificates"
[ -f "$certs" ] || certs=""
if [ -n "$certs" ]; then
    [ -n "$prev" ] || die "missing=-p certificates_prev"
    if [ -f "$prev" ]; then
        word=$(head -c 64 "$prev" | tr -d '\0')
        if is_hex64 "$word" && [ "$(wc -c < "$prev")" -le 256 ]; then
            prev="$word"
        else
            prev=$(sha256_of "$prev")
        fi
    fi
    is_hex64 "$prev" || die "certificates_prev=$prev"
fi

rm -rf "$outdir"
mkdir -p "$outdir"
manifest="$outdir/update.manifest"
stage="$outdir/.stage"
# A failed run leaves no output, so nothing half-written is mistaken for one.
cleanup() {
    local st=$?
    rm -rf "$stage" "$outdir/.rows" "$outdir/.rows.raw"
    [ "$st" = 0 ] || rm -rf "$outdir"
}
trap cleanup EXIT

{
    echo "format=1"
    echo "product=AmiNetXDuo"
    echo "version=$version"
    echo "channel=$channel"
    [ -z "$certs" ] || echo "certificates.prev=$prev"
} > "$manifest"

# One row per member: <class> <member> <source> <size> <sha256> <path>.
rows() {  # $1 flavour list
    local fl="$1" libs devs cdir line rel bytes sum f name
    libs=$(sed -n 's/^libs=//p' "$fl")
    devs=$(sed -n 's/^devs=//p' "$fl")
    cdir=$(sed -n 's/^commands=//p' "$fl")
    [ -n "$libs" ] && [ -n "$devs" ] && [ -n "$cdir" ] ||
        fail "flavour_list=$fl missing=libs_devs_commands"
    while IFS= read -r line; do
        rel=$(sed -n 's/^file=\([^ ]*\) .*/\1/p' <<< "$line")
        bytes=$(sed -n 's/.* bytes=\([0-9]*\).*/\1/p' <<< "$line")
        sum=$(sed -n 's/.* sha256=\([0-9a-f]*\).*/\1/p' <<< "$line")
        f="$TREE/$rel"
        [ -f "$f" ] || fail "missing=$rel"
        [ "$(wc -c < "$f" | tr -d ' ')" = "$bytes" ] && [ "$(sha256_of "$f")" = "$sum" ] ||
            fail "stale_flavour_list=$fl file=$rel"
        case "$rel" in
            "$libs"/*) echo "1 LIBS Libs/${rel#"$libs"/} $rel $bytes $sum ${rel#"$libs"/}" ;;
            "$devs"/*) echo "2 DEVS/Networks Devs/Networks/${rel#"$devs"/} $rel $bytes $sum ${rel#"$devs"/}" ;;
            "$cdir"/*) echo "3 C C/${rel#"$cdir"/} $rel $bytes $sum ${rel#"$cdir"/}" ;;
            *) fail "flavour_list=$fl unclassified=$rel" ;;
        esac
    done < <(grep '^file=' "$fl")
    if [ "$cdir" != C ]; then
        for f in "$TREE"/C/*; do
            [ -f "$f" ] || continue
            name=${f##*/}
            case "$name" in *.info) continue ;; esac
            [ -e "$TREE/$cdir/$name" ] && continue
            echo "3 C C/$name C/$name $(wc -c < "$f" | tr -d ' ') $(sha256_of "$f") $name"
        done
    fi
    [ -z "$certs" ] ||
        echo "4 certificates Devs/Internet/certificates Devs/Internet/certificates $(wc -c < "$certs" | tr -d ' ') $(sha256_of "$certs") certificates"
}

flavours=""
for flavour in full minimal micro fat; do
    fl="$TREE/Flavours/$flavour"
    [ -f "$fl" ] || continue
    name="AmiNetXDuo-$version-$flavour.update.lha"
    rows "$fl" > "$outdir/.rows.raw"
    sort -k1,1n -k7,7 "$outdir/.rows.raw" > "$outdir/.rows"
    [ -s "$outdir/.rows" ] || fail "flavour=$flavour files=0"
    dup=$(cut -d' ' -f2,7 "$outdir/.rows" | tr '[:upper:]' '[:lower:]' | sort | uniq -d | head -n 1)
    [ -z "$dup" ] || fail "flavour=$flavour duplicate=$dup"

    # The members under their container names, as links to the tree.
    rm -rf "$stage"
    members=()
    while read -r _ cls member src bytes sum path; do
        mkdir -p "$stage/$(dirname "$member")"
        ln -s "$TREE/$src" "$stage/$member"
        members+=("$member")
    done < "$outdir/.rows"
    python3 "$ROOT/dist/lhapack.py" "$outdir/$name" "$stage" "${members[@]}" >/dev/null ||
        fail "lhapack=$name"

    echo "container=$flavour $(wc -c < "$outdir/$name" | tr -d ' ') $(sha256_of "$outdir/$name") $name" >> "$manifest"
    while read -r _ cls member src bytes sum path; do
        echo "file=$flavour $cls $bytes $sum $path"
    done < "$outdir/.rows" >> "$manifest"

    printf 'update_container=%s name=%s bytes=%s sha256=%s files=%s\n' \
        "$flavour" "$name" "$(wc -c < "$outdir/$name" | tr -d ' ')" \
        "$(sha256_of "$outdir/$name")" "$(wc -l < "$outdir/.rows" | tr -d ' ')"
    flavours="$flavours${flavours:+,}$flavour"
done
[ -n "$flavours" ] || fail "flavours=none"

printf 'end=%s\n' "$(wc -l < "$manifest" | tr -d ' ')" >> "$manifest"
long=$(awk 'length($0) > 255 { print NR; exit }' "$manifest")
[ -z "$long" ] || fail "line_too_long=$long"

printf 'update_manifest=PASS path=%s version=%s channel=%s flavours=%s lines=%s sha256=%s\n' \
    "$manifest" "$version" "$channel" "$flavours" \
    "$(wc -l < "$manifest" | tr -d ' ')" "$(sha256_of "$manifest")"
