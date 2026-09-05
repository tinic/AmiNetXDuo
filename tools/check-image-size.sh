#!/usr/bin/env bash
#
# The resident images may not grow without somebody typing the new number.
#
#   tools/check-image-size.sh <build-dir>
#   AMINETXDUO_IMAGE_ARM=default tools/check-image-size.sh <build-dir>
#
# bsdsocket.library and anxnet.device are open for the life of the machine, so
# their size is RAM and not disk.  0.26.0 shipped 27,948 bytes of serial-log
# sentences inside bsdsocket.library -- 7.2 per cent of it -- and every gate in
# this directory passed, because none of them asked how big anything got.  The
# argument for those bytes had been made and measured; what nobody did was
# decide to spend them.
#
# ONLY THE TWO CONFIGURATIONS THAT SHIP ARE BUDGETED, and they are the two
# tools/check-shipping-config.sh maps a drawer onto: `default` is the full
# drawer, `minimal` is the minimal one.  Every other arm is coverage -- nolto
# is 44 KB larger because that is what LTO is worth, census carries its side
# table, log carries the sentences -- and holding coverage to a shipping budget
# would only teach whoever hits it to raise the number.  The arm is the build
# directory's name, or AMINETXDUO_IMAGE_ARM.
#
# A budget is not a limit somebody guessed at.  It is the size the image was
# when it was last looked at, plus room to move, so growth arrives as a diff to
# this file with a reason beside it rather than as a fact found after a
# release.  Raising one is fine.  Raising one without saying why in the commit
# message is the thing this exists to stop.
#
# BSS is measured but not budgeted: an AmigaOS hunk records it as a length and
# no bytes, so it costs the image nothing and the machine the whole thing.  It
# is reported so a jump in it is visible at all.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build/ci/default}"
ARM="${AMINETXDUO_IMAGE_ARM:-$(basename "$BUILD")}"

# arm : image path under the build dir : budget in bytes
#
# Set 2026-08-28 at 0.26.1, headroom to the next round 2 KB.  bsdsocket.library
# is 361,428 in the full drawer and 233,928 in the minimal one, both with
# AMINETXDUO_LOG off, which is where they belong.
BUDGETS=(
    "default:src/bsdsocket/bsdsocket.library:357000"
    "default:src/netdev/anxnet.device:40000"
    "default:src/usergroup/usergroup.library:9000"
    "default:src/tlslib/tls.library:198000"
    "minimal:src/bsdsocket/bsdsocket.library:226000"
    "minimal:src/netdev/anxnet.device:40000"
    "minimal:src/usergroup/usergroup.library:9000"
)

budgeted=0
for row in "${BUDGETS[@]}"; do
    [ "${row%%:*}" = "$ARM" ] && budgeted=1
done

if [ "$budgeted" = 0 ]; then
    echo "image_size=skipped arm=$ARM reason=not_a_shipping_configuration"
    exit 0
fi

if [ ! -d "$BUILD" ]; then
    echo "image_size=skipped reason=no_build dir=$BUILD"
    exit 0
fi

SIZE="${AMIGA_TOOLCHAIN_ROOT:-}/bin/m68k-amigaos-size"
rc=0
seen=0

for row in "${BUDGETS[@]}"; do
    arm="${row%%:*}"
    [ "$arm" = "$ARM" ] || continue
    rest="${row#*:}"
    rel="${rest%:*}"
    budget="${rest##*:}"
    img="$BUILD/$rel"
    name="$(basename "$rel")"

    [ -f "$img" ] || continue
    seen=$((seen + 1))

    bytes=$(stat -c %s "$img" 2>/dev/null || stat -f %z "$img")
    bss=""
    [ -x "$SIZE" ] && bss=$("$SIZE" "$img" 2>/dev/null | tail -1 | awk '{print $3}')

    if [ "$bytes" -gt "$budget" ]; then
        echo "image_size=OVER arm=$arm image=$name bytes=$bytes budget=$budget over=$((bytes - budget))"
        echo "  $name is resident for the life of the machine.  Either take the"
        echo "  bytes back out, or raise its budget in tools/check-image-size.sh"
        echo "  in the same commit and say in the message what bought them."
        rc=1
    else
        echo "image_size=ok arm=$arm image=$name bytes=$bytes budget=$budget spare=$((budget - bytes))${bss:+ bss=$bss}"
    fi
done

if [ "$seen" = 0 ]; then
    echo "image_size=skipped arm=$ARM reason=no_images dir=$BUILD"
    exit 0
fi

[ "$rc" = 0 ] && echo "image_size=PASS arm=$ARM images=$seen"
exit "$rc"
