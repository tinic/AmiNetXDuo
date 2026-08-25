#!/usr/bin/env bash
#
# No image in a build tree may link with no relocation table.
#
#   tools/check-hunk-relocs.sh <build-dir>
#
# dos.library's LoadSeg does not map a hunk file, it walks it: one allocation
# per loadable hunk, then it APPLIES the relocations by hand.  An executable
# that carries none keeps every absolute address at its link-time value, and
# on a real machine those belong to somebody else.  It still links, still has
# the right symbols and still passes `ls -l`.
#
# It has happened twice, by two routes.  9fb69360: `Profile` linked the
# toolchain's crt0 and libc, ld's amiga backend gave every .debug_* section a
# LOADABLE data hunk, and the guest rebooted twelve seconds into every run --
# 12 hunks and 0 relocations against 3 and 1152.  Then again under LTO, where
# the 64-bit helpers are synthesised in LTRANS after libaminetxduo_m68k_rt.a
# has been scanned, libgcc.a(_muldi3,_udivdi3,_umoddi3) are pulled instead of
# ours, and those carry DWARF: ten test images, 10 or 12 hunks and zero
# relocations, `crypto68k_25519_test` timing out with an empty stdout where
# its non-LTO twin passes 16,636 checks.
#
# Both were found by hand, months apart, by somebody looking at the right
# file for another reason.  This is what looks instead.  It runs over the
# WHOLE build tree, tests included: the libraries have linked --gc-sections
# all along, and it is the test tier -- the thing that is supposed to gate a
# release -- that produced dead binaries under the configuration the release
# is built with.
#
# tools/hunkdiff.py --check is the judgement; this finds it the files.  A
# build directory is mostly objects, maps and CMake bookkeeping, so the
# obvious non-images are pruned by name and hunkdiff skips whatever is left
# that does not begin with HUNK_HEADER.
#
# Output is key=value and an exit code: 0 clean, 1 an image would load
# unrelocated, 2 nothing to check.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-}"

[ -n "$BUILD" ] || { echo "usage: check-hunk-relocs.sh <build-dir>" >&2; exit 2; }

if [ ! -d "$BUILD" ]; then
    echo "hunk_relocs=skipped reason=no_build_dir build=$BUILD"
    exit 2
fi

OUT=$(find "$BUILD" -type f \
        -not -path '*/CMakeFiles/*' \
        -not -name '*.o' -not -name '*.obj' -not -name '*.a' \
        -not -name '*.d' -not -name '*.map' -not -name '*.log' \
        -not -name '*.txt' -not -name '*.json' -not -name '*.cmake' \
        -print0 2>/dev/null \
      | xargs -0 -r python3 "$ROOT/tools/hunkdiff.py" --check 2>&1)
rc=$?

# Non-images are the bulk of any build directory and are not news.
bad=$(printf '%s\n' "$OUT" | grep -c -E '^check=(UNRELOCATED|EXTRA_HUNKS|BAD) ')
ok=$(printf '%s\n' "$OUT" | grep -c '^check=ok ')

if [ "$ok" = 0 ] && [ "$bad" = 0 ]; then
    echo "hunk_relocs=skipped reason=no_images build=$BUILD"
    exit 2
fi

if [ "$bad" != 0 ]; then
    echo "hunk_relocs=FAILED images=$ok bad=$bad build=$BUILD"
    printf '%s\n' "$OUT" | grep -E '^check=(UNRELOCATED|EXTRA_HUNKS|BAD) '
    echo "Every one of these would be loaded by LoadSeg with its absolute"
    echo "addresses left at their link-time values.  The fix that has worked"
    echo "both times is -Wl,--gc-sections on the target; see"
    echo "tests/crypto68k/CMakeLists.txt and tools/profiler/CMakeLists.txt."
    exit 1
fi

# xargs answers 123 when any batch exited 1-125, and hunkdiff only does that
# for a bad image, which the count above has already ruled out.  Anything
# else from the pipeline is a broken run rather than a verdict.
if [ "$rc" != 0 ] && [ "$rc" != 123 ]; then
    echo "hunk_relocs=skipped reason=hunkdiff_rc_$rc build=$BUILD"
    printf '%s\n' "$OUT" | tail -5
    exit 2
fi

echo "hunk_relocs=clean images=$ok build=$BUILD"
exit 0
