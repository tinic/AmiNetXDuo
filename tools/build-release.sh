#!/usr/bin/env bash
# Build and verify the canonical release candidate for the current commit.
# CI runs this once; release.yml promotes the resulting immutable artifact.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

cmake --preset default -B build/release
cmake --build build/release --parallel --target release_payload

cmake --preset minimal -B build/release-minimal
cmake --build build/release-minimal --parallel --target release_libraries

cmake --preset micro -B build/release-micro
cmake --build build/release-micro --parallel --target release_libraries

AMINETXDUO_CLIENT_ANY=1 AMIGA_CLIENT_ARCH=-m68000 \
    clients/dropbear/build.sh -b build/ssh

AMINETXDUO_SSH=build/ssh/dbclient \
AMINETXDUO_SCP=build/ssh/scp \
AMINETXDUO_SCP_RUNNER=build/ssh/scp-runner \
AMINETXDUO_DIST_NO_BUILD=1 \
    dist/make-dist.sh -b build/release

for path in \
    C/ssh C/scp C/scp-runner Installer \
    Docs/AmiNetXDuo.guide Docs/AmiNetXDuo.guide.info \
    Libs/bsdsocket.library Libs/tls.library \
    Libs/minimal/bsdsocket.library; do
    [ -f "build/dist/AmiNetXDuo/$path" ] || {
        echo "release_candidate=FAIL missing=$path" >&2
        exit 1
    }
done

[ ! -e build/dist/AmiNetXDuo/Libs/minimal/tls.library ] || {
    echo "release_candidate=FAIL unexpected=Libs/minimal/tls.library" >&2
    exit 1
}

hidden=$(find build/dist/AmiNetXDuo -type f -name '.*' -print -quit)
[ -z "$hidden" ] || {
    echo "release_candidate=FAIL hidden_file=$hidden" >&2
    exit 1
}

tools/check-changelog-prose.sh

archive=$(find build/dist -maxdepth 1 -type f -name 'AmiNetXDuo-*.lha' -print)
[ "$(printf '%s\n' "$archive" | sed '/^$/d' | wc -l | tr -d ' ')" = 1 ] || {
    echo "release_candidate=FAIL expected_one_archive" >&2
    printf '%s\n' "$archive" >&2
    exit 1
}

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

manifest=build/dist/release-candidate.txt
{
    echo "source_sha=$(git rev-parse HEAD)"
    echo "version=$(tools/version.sh --product)"
    echo "compound_version=$(tools/version.sh --compound)"
    echo "toolchain_sha=$(tools/fetch-toolchain.sh --print-sha)"
    echo "archive=$(basename "$archive")"
    echo "archive_sha256=$(hash_file "$archive")"
} > "$manifest"

echo "release_candidate=PASS archive=$(basename "$archive") sha256=$(hash_file "$archive")"
