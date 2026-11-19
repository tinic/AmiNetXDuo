#!/usr/bin/env bash
# Verify that a downloaded CI artifact belongs to the requested source SHA.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dir="${1:-build/candidate}"
want_sha="${2:-}"
manifest="$dir/release-candidate.txt"

[ -r "$manifest" ] || {
    echo "release_candidate=FAIL missing_manifest=$manifest" >&2
    exit 1
}

for key in source_sha version compound_version toolchain_sha archive archive_sha256 \
           update_manifest_sha256 update_full update_full_sha256 \
           update_fat update_fat_sha256 update_minimal update_minimal_sha256 \
           update_micro update_micro_sha256 update_test_key; do
    [ "$(grep -c "^$key=" "$manifest" || true)" = 1 ] || {
        echo "release_candidate=FAIL manifest_key=$key expected=exactly_once" >&2
        exit 1
    }
done

value() { sed -n "s/^$1=//p" "$manifest"; }
got_sha=$(value source_sha)

# A test-key build trusts a key whose private half is in the tree.
[ "$(value update_test_key)" = OFF ] || {
    echo "release_candidate=FAIL update_test_key=$(value update_test_key) wanted=OFF" >&2
    exit 1
}
archive_name=$(value archive)
want_hash=$(value archive_sha256)
archive="$dir/$archive_name"

[ -n "$want_sha" ] && [ "$got_sha" = "$want_sha" ] || {
    echo "release_candidate=FAIL source_sha=$got_sha wanted=$want_sha" >&2
    exit 1
}
case "$archive_name" in
    ''|.|..|*/*) echo "release_candidate=FAIL invalid_archive=$archive_name" >&2; exit 1 ;;
esac
case "$want_hash" in
    *[!0-9a-fA-F]*|'') echo "release_candidate=FAIL invalid_sha256=$want_hash" >&2; exit 1 ;;
esac
[ "${#want_hash}" = 64 ] || {
    echo "release_candidate=FAIL invalid_sha256_length=${#want_hash}" >&2
    exit 1
}
[ -f "$archive" ] || {
    echo "release_candidate=FAIL missing_archive=$archive" >&2
    exit 1
}

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}
got_hash=$(hash_file "$archive")
[ "$got_hash" = "$want_hash" ] || {
    echo "release_candidate=FAIL archive_sha256=$got_hash wanted=$want_hash" >&2
    exit 1
}

# The update manifest and each container are bound by digest, and the
# manifest's container= line names the same file, size and digest.
update="$dir/update"
umanifest="$update/update.manifest"
[ -f "$umanifest" ] || {
    echo "release_candidate=FAIL missing_update_manifest=$umanifest" >&2
    exit 1
}
umanifest_hash=$(hash_file "$umanifest")
[ "$umanifest_hash" = "$(value update_manifest_sha256)" ] || {
    echo "release_candidate=FAIL update_manifest_sha256=$umanifest_hash" >&2
    exit 1
}
for f in full fat minimal micro; do
    c=$(value "update_$f")
    case "$c" in
        ''|.|..|*/*) echo "release_candidate=FAIL invalid_update_$f=$c" >&2; exit 1 ;;
    esac
    [ -f "$update/$c" ] || {
        echo "release_candidate=FAIL missing_update_$f=$update/$c" >&2
        exit 1
    }
    sum=$(hash_file "$update/$c")
    [ "$sum" = "$(value "update_${f}_sha256")" ] || {
        echo "release_candidate=FAIL update_${f}_sha256=$sum" >&2
        exit 1
    }
    line="container=$f $(wc -c < "$update/$c" | tr -d ' ') $sum $c"
    grep -qxF -- "$line" "$umanifest" || {
        echo "release_candidate=FAIL update_manifest_container=$f" >&2
        exit 1
    }
done

# The UpdateAmiNetXDuo in every container holds the release keys of
# src/tools/updkeys.c and not the test key; a tree with no release key is
# refused, as its tool would accept nothing.
keys=$(python3 -I "$ROOT/tools/updkeys.py" check "$ROOT/src/tools/updkeys.c" \
           "$update"/*.update.lha) || {
    echo "release_candidate=FAIL $keys" >&2
    exit 1
}

echo "release_candidate=PASS source_sha=$got_sha archive=$archive_name sha256=$got_hash update_manifest_sha256=$umanifest_hash ${keys#update_keys=PASS }"
