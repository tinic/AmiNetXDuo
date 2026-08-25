#!/usr/bin/env bash
# Fetch Fitz and unpack it into build/fitz.
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
    echo "lha not found, brew install lhasa (macOS) or apt install lhasa" >&2
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
