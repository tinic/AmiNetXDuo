#!/usr/bin/env bash
# Verify that a downloaded CI artifact belongs to the requested source SHA.
# SPDX-License-Identifier: MIT

set -euo pipefail

dir="${1:-build/candidate}"
want_sha="${2:-}"
manifest="$dir/release-candidate.txt"

[ -r "$manifest" ] || {
    echo "release_candidate=FAIL missing_manifest=$manifest" >&2
    exit 1
}

for key in source_sha version compound_version toolchain_sha archive archive_sha256; do
    [ "$(grep -c "^$key=" "$manifest" || true)" = 1 ] || {
        echo "release_candidate=FAIL manifest_key=$key expected=exactly_once" >&2
        exit 1
    }
done

value() { sed -n "s/^$1=//p" "$manifest"; }
got_sha=$(value source_sha)
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

if command -v sha256sum >/dev/null 2>&1; then
    got_hash=$(sha256sum "$archive" | cut -d' ' -f1)
else
    got_hash=$(shasum -a 256 "$archive" | cut -d' ' -f1)
fi
[ "$got_hash" = "$want_hash" ] || {
    echo "release_candidate=FAIL archive_sha256=$got_hash wanted=$want_hash" >&2
    exit 1
}

echo "release_candidate=PASS source_sha=$got_sha archive=$archive_name sha256=$got_hash"
