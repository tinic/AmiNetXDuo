#!/usr/bin/env bash
#
# Build the tbdye/bsdsocktest conformance suite with our toolchain.
#
#   tests/conformance/build.sh [-u]
#
# -u  fetch/update the checkout before building
#
# The suite lives in third_party/bsdsocktest (upstream GPL-3, unmodified --
# nothing in this directory patches its sources).  Its own Makefile assumes
# /opt/amiga and -noixemul, neither of which works here, so we drive the
# compiler ourselves rather than fighting the Makefile's link rule:
#
#   * -noixemul is dropped -- it is a libnix switch and it breaks
#     sys/reent.h on this newlib toolchain (docs/RESEARCH.md 5.4).
#   * -include sys/types.h -- the Roadshow NDK sys/socket.h uses ssize_t
#     without pulling in the newlib header that declares it.
#   * -include stdio.h, -Dstricmp=strcasecmp -- helper_proto.c calls sprintf()
#     without including stdio.h, and stricmp is an SAS/libnix name that newlib
#     spells strcasecmp.  Both are -noixemul-isms, not suite bugs.
#   * compat/ goes first on the include path so our regenerated
#     inline/bsdsocket.h wins over the NDK one -- see that file for why.
#   * compat/libgcc64.c supplies __udivdi3 and friends, which this toolchain's
#     empty libgcc.a does not -- see that file for why.
#   * printf -> iprintf etc: newlib's float-capable printf makes the startup
#     open mathieeedoubbas.library, which is not in ROM and not on our bare
#     boot drive, so the program dies with "mathieeedoubbas.library failed to
#     load" before main().  The suite formats no floats at all, so the
#     integer-only newlib variants are exactly equivalent here.
#
# Output, all under build/bsdsocktest/:
#   bsdsocktest     the suite
#   conf_launcher   what the FS-UAE harness actually runs (see conf_launcher.c)
#   conf_probe      hand-written triage walk (see conf_probe.c)
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/conformance"
SRC="$ROOT/third_party/bsdsocktest"
UPSTREAM="https://github.com/tbdye/bsdsocktest.git"
UPDATE=0

while getopts "u" opt; do
    case "$opt" in
        u) UPDATE=1 ;;
        *) echo "usage: $0 [-u]" >&2; exit 2 ;;
    esac
done

# -e, not -d: a git submodule's .git is a file, and re-cloning over one fails.
if [ ! -e "$SRC/.git" ]; then
    echo "==> fetching $UPSTREAM into third_party/bsdsocktest"
    mkdir -p "$ROOT/third_party"
    git clone --depth 50 "$UPSTREAM" "$SRC"
elif [ "$UPDATE" = "1" ]; then
    git -C "$SRC" pull --ff-only
fi

TOOLCHAIN="${AMIGA_TOOLCHAIN_ROOT:-$HOME/amigaos/tools/m68k-amigaos-gcc}"
NDK="${AMIGA_NDK:-$TOOLCHAIN/m68k-amigaos/ndk-include}"
CC="$TOOLCHAIN/bin/m68k-amigaos-gcc"

[ -x "$CC" ] || { echo "no m68k-amigaos-gcc at $CC" >&2; exit 2; }

CFLAGS=(-O2 -Wall -m68020 -fomit-frame-pointer -fno-strict-aliasing
        -I"$HERE/compat" -I"$NDK"
        -include sys/types.h -include strings.h -include stdio.h
        -Dstricmp=strcasecmp
        -Dprintf=iprintf -Dsprintf=siprintf
        -Dvfprintf=vfiprintf -Dvsnprintf=vsniprintf)

OBJ="$ROOT/build/bsdsocktest/obj"
OUT="$ROOT/build/bsdsocktest/bsdsocktest"
mkdir -p "$OBJ"

objs=()
for c in "$SRC"/src/*.c "$HERE"/compat/*.c; do
    o="$OBJ/$(basename "${c%.c}").o"
    if [ ! -f "$o" ] || [ "$c" -nt "$o" ]; then
        echo "  CC $(basename "$c")"
        "$CC" "${CFLAGS[@]}" -c -o "$o" "$c"
    fi
    objs+=("$o")
done

echo "  LD $(basename "$OUT")"
"$CC" -m68020 -o "$OUT" "${objs[@]}"

# The launcher and the triage probe are ours, built against the same compat
# headers.  The launcher links the crash guard out of the CMake build, so it
# needs that to have run first; skip it rather than fail if it has not.
COMMON="$ROOT/build/cm/src/common/libaminetxduo_common.a"
if [ -f "$COMMON" ]; then
    echo "  CC conf_launcher.c"
    "$CC" -O2 -m68020 -fomit-frame-pointer -I"$ROOT/include" -I"$NDK" \
          -o "$ROOT/build/bsdsocktest/conf_launcher" \
          "$HERE/conf_launcher.c" "$COMMON"
else
    echo "  -- skipping conf_launcher: build build/cm first" >&2
fi

echo "  CC conf_probe.c"
"$CC" -O2 -m68020 -fomit-frame-pointer -fno-strict-aliasing \
      -I"$HERE/compat" -I"$NDK" -include sys/types.h \
      -o "$ROOT/build/bsdsocktest/conf_probe" "$HERE/conf_probe.c"

echo "==> $OUT"
"$TOOLCHAIN/bin/m68k-amigaos-size" "$OUT" || true
