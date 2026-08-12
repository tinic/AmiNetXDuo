#!/usr/bin/env bash
#
# Build the tbdye/bsdsocktest conformance suite with our toolchain.
#
#   tests/conformance/build.sh [-u] [-b BUILDDIR]
#
# -u  fetch/update the checkout before building
#
# The suite lives in third_party/bsdsocktest (upstream GPL-3, unmodified --
# nothing in this directory patches its sources).  Its own Makefile assumes
# /opt/amiga and -noixemul, neither of which works here, so we drive the
# compiler ourselves rather than fighting the Makefile's link rule:
#
#   * -noixemul is dropped, it is a libnix switch and it breaks
#     sys/reent.h on this newlib toolchain (docs/RESEARCH.md 5.4).
#   * -include sys/types.h, the Roadshow NDK sys/socket.h uses ssize_t
#     without pulling in the newlib header that declares it.
#   * -include stdio.h, -Dstricmp=strcasecmp, helper_proto.c calls sprintf()
#     without including stdio.h, and stricmp is an SAS/libnix name that newlib
#     spells strcasecmp.  Both are -noixemul-isms, not suite bugs.
#   * compat/ goes first on the include path so our regenerated
#     inline/bsdsocket.h wins over the NDK one, see that file for why.
#   * src/common/ami_udivdi3.c supplies __udivdi3 and friends, which this
#     toolchain's empty libgcc.a does not, newlib's printf references them.
#     Compiled straight out of src/common rather than copied here: it is the
#     one copy in the tree (see the header comment in that file).
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
# Where conf_launcher finds the crash guard.  build/cm is what dist/make-dist.sh
# and a developer's own build produce; CI builds build/ci/default long before
# it builds build/cm, so -b lets the caller name the tree it actually has.
BUILDDIR="$ROOT/build/cm"

while getopts "ub:" opt; do
    case "$opt" in
        u) UPDATE=1 ;;
        b) case "$OPTARG" in /*) BUILDDIR="$OPTARG" ;;
                             *)  BUILDDIR="$ROOT/$OPTARG" ;; esac ;;
        *) echo "usage: $0 [-u] [-b BUILDDIR]" >&2; exit 2 ;;
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

. "$ROOT/tools/amiga-toolchain.sh"
TOOLCHAIN="$AMIGA_TOOLCHAIN_ROOT"
NDK="$AMIGA_NDK"
CC="$AMIGA_GCC"

# -Wno-error=incompatible-pointer-types, because none of this is our code and
# the mismatch is between two NDKs, not a bug.  bsdsocktest's main.c declares
# its ToolTypes array CONST_STRPTR*, which is exactly what NDK 3.2's
# FindToolType() takes; NDK 3.9's <inline/icon.h> writes the parameter `const
# STRPTR *` (a const array of non-const strings) instead, so the same call is a
# pointer mismatch there, and GCC 14 promoted that from a warning to an error
# by default.  We cannot fix a submodule we do not own, and we want the suite
# to build against either NDK, so the diagnostic goes back to being a warning.
CFLAGS=(-O2 -Wall -Wno-error=incompatible-pointer-types
        -m68020 -fomit-frame-pointer -fno-strict-aliasing
        -I"$HERE/compat" -I"$NDK"
        -include sys/types.h -include strings.h -include stdio.h
        -Dstricmp=strcasecmp
        -Dprintf=iprintf -Dsprintf=siprintf
        -Dvfprintf=vfiprintf -Dvsnprintf=vsniprintf)

OBJ="$ROOT/build/bsdsocktest/obj"
OUT="$ROOT/build/bsdsocktest/bsdsocktest"
mkdir -p "$OBJ"

# nullglob: compat/ currently holds headers only, so compat/*.c may match
# nothing.  Without it the unmatched pattern would be passed to the compiler
# verbatim.
shopt -s nullglob
sources=("$SRC"/src/*.c "$HERE"/compat/*.c "$ROOT/src/common/ami_udivdi3.c")
shopt -u nullglob

objs=()
for c in "${sources[@]}"; do
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
COMMON="$BUILDDIR/src/common/libaminetxduo_common.a"
if [ -f "$COMMON" ]; then
    echo "  CC conf_launcher.c"
    "$CC" -O2 -m68020 -fomit-frame-pointer -I"$ROOT/include" -I"$NDK" \
          -o "$ROOT/build/bsdsocktest/conf_launcher" \
          "$HERE/conf_launcher.c" "$COMMON"
else
    echo " , skipping conf_launcher: no $COMMON, build it or pass -b" >&2
fi

echo "  CC conf_probe.c"
"$CC" -O2 -m68020 -fomit-frame-pointer -fno-strict-aliasing \
      -I"$HERE/compat" -I"$NDK" -include sys/types.h \
      -o "$ROOT/build/bsdsocktest/conf_probe" "$HERE/conf_probe.c"

echo "==> $OUT"
"$TOOLCHAIN/bin/m68k-amigaos-size" "$OUT" || true
