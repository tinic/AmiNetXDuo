#!/usr/bin/env bash
#
# Fetch Fitz and unpack it into build/fitz.
#
#   tests/endurance/fetch-fitz.sh [-u URL]
#
# Fitz is Timm S. Müller's cross-platform network file server and mounter,
# MIT licensed with full source.  It is not vendored into this tree: it is a
# third-party program, and the value of testing against it is precisely that
# it is somebody else's -- a copy in third_party/ would be a copy we could
# drift from, and a stack that only passes against a pinned copy of a client
# has not been tested against that client at all.
#
# What lands in build/fitz:
#
#   Fitz/fitz          the RELEASED m68k binary, as a user would install it
#   Fitz/src/          the sources, from which tests/endurance/build.sh makes
#                      a second, debug-enabled binary
#
# Needs `lha` (brew install lhasa, or apt install lhasa).
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
URL="${FITZ_URL:-https://home.sm41.de/projects/fitz/Fitz.lha}"

while getopts "u:" opt; do
    case "$opt" in
        u) URL="$OPTARG" ;;
        *) echo "usage: $0 [-u URL]" >&2; exit 2 ;;
    esac
done

DEST="$ROOT/build/fitz"

command -v lha >/dev/null 2>&1 || {
    echo "lha not found -- brew install lhasa (macOS) or apt install lhasa" >&2
    exit 2
}

if [ -f "$DEST/Fitz/fitz" ] && [ -f "$DEST/Fitz/src/amiga-client.c" ]; then
    echo "==> build/fitz already unpacked"
    exit 0
fi

mkdir -p "$DEST"
cd "$DEST"

echo "==> fetching $URL"
curl -fsSL -o Fitz.lha "$URL"
curl -fsSL -o Fitz.readme "${URL%/*}/Fitz.readme" || true

rm -rf Fitz
lha xq Fitz.lha >/dev/null

[ -f Fitz/fitz ] || { echo "no Fitz/fitz in the archive" >&2; exit 1; }
[ -f Fitz/src/amiga-client.c ] || { echo "no sources in the archive" >&2; exit 1; }

grep -m1 'FITZ_VERSION_STR' Fitz/src/fitz-version.h || true

echo "==> $DEST/Fitz"
