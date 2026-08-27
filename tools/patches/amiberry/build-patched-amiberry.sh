#!/usr/bin/env bash
#
# Build an Amiberry carrying every patch in tools/patches/amiberry/, in a
# directory of the caller's own, and give it the capabilities a bridged run
# needs.
#
#   tools/patches/amiberry/build-patched-amiberry.sh [-s source] [-d dest]
#                                                    [-r rev] [-j jobs]
#
# IT NEEDS NO ROOT to build and NO ROOT PASSWORD to grant the capabilities:
# the lab hosts carry a passwordless setcap rule (/etc/sudoers.d on playhouse3)
# scoped to setcap alone -- `sudo -n true` fails there and proves nothing about
# it.  A build takes about twenty minutes on an eight-core host, so RUN IT
# DETACHED and poll the log:
#
#   nohup tools/patches/amiberry/build-patched-amiberry.sh > build/amiberry.log 2>&1 &
#
# EVERY RELINK CLEARS THE CAPABILITY.  A run against an emulator that has lost
# it fails with `UAENET: Failed to open device ... Operation not permitted` AND
# AN EMPTY SERIAL LOG, which reads as a staging fault rather than a permissions
# one.  This script re-applies it after every build for that reason, and says
# whether it took.
#
# NEVER SHARE A CHECKOUT.  Several agents run arms on one host at a time and a
# relink under a running arm is a swapped emulator mid-measurement, so -d
# defaults inside the calling tree's own build/.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

SOURCE="${AMINETXDUO_AMIBERRY_SOURCE:-$HOME/amiberry}"
DEST="$ROOT/build/amiberry-src"
REV=""
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

while getopts "s:d:r:j:" opt; do
    case "$opt" in
        s) SOURCE="$OPTARG" ;;
        d) DEST="$OPTARG" ;;
        r) REV="$OPTARG" ;;
        j) JOBS="$OPTARG" ;;
        *) sed -n '3,9p' "$0" >&2; exit 2 ;;
    esac
done

PATCHDIR="$ROOT/tools/patches/amiberry"
[ -d "$PATCHDIR" ] || { echo "amiberry_patches=missing:$PATCHDIR" >&2; exit 2; }

if [ ! -d "$DEST/.git" ]; then
    [ -d "$SOURCE/.git" ] || {
        echo "amiberry_source=missing:$SOURCE" >&2
        echo "-s takes an Amiberry checkout to clone from." >&2
        exit 2; }
    echo "amiberry_clone=$SOURCE -> $DEST"
    rm -rf "$DEST"
    git clone -q "$SOURCE" "$DEST"
fi

cd "$DEST"
# A clean base every time: the patches are applied to a pristine tree, so
# running this twice is the same as running it once.
git checkout -q -- .
[ -z "$REV" ] || git checkout -q "$REV"
echo "amiberry_base=$(git rev-parse --short HEAD)"

# patch(1) rather than `git apply`: a diff whose last line has no newline is
# refused by git apply with "corrupt patch" and taken by patch(1) with a note.
for d in "$PATCHDIR"/*.diff; do
    [ -f "$d" ] || continue
    echo "amiberry_patch=$(basename "$d")"
    patch -p1 --forward < "$d" || {
        echo "amiberry_patch_failed=$(basename "$d")" >&2; exit 1; }
done

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DUSE_UAENET_PCAP=ON -DUSE_UAENET_TAP=ON > /dev/null
cmake --build build --parallel "$JOBS"

BIN="$DEST/build/amiberry"
[ -x "$BIN" ] || { echo "amiberry_binary=missing:$BIN" >&2; exit 1; }

# Scoped to setcap, and it is the only thing here that goes near sudo.
if sudo -n /usr/sbin/setcap cap_net_admin,cap_net_raw=eip "$BIN" 2>/dev/null; then
    echo "amiberry_setcap=ok"
else
    echo "amiberry_setcap=refused" >&2
    echo "A bridged run needs CAP_NET_RAW on the binary.  Without it the run" >&2
    echo "fails with 'UAENET: Failed to open device' and an EMPTY serial log." >&2
fi
echo "amiberry_caps=$(/usr/sbin/getcap "$BIN" 2>/dev/null || echo none)"
echo "amiberry_binary=$BIN"
echo "amiberry=$BIN"
exit 0
