#!/usr/bin/env bash
#
# No image in a build tree may link with no relocation table.
#
#   tools/check-hunk-relocs.sh <build-dir>
#
# LoadSeg does not map a hunk file, it walks it and APPLIES the relocations by
# hand.  An executable that carries none keeps every absolute address at its
# link-time value, which on a real machine belongs to somebody else -- while
# still linking and still carrying the right symbols.
#
# It runs over the WHOLE build tree, tests included: it is the test tier that
# produced dead binaries, both from a LOADABLE data hunk for .debug_* sections
# and under LTO, where libgcc.a's 64-bit helpers are pulled in and carry DWARF.
# tools/hunkdiff.py --check is the judgement; this finds it the files.
#
# Output is key=value; exit 0 clean, 1 an image would load unrelocated,
# 2 nothing to check.
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
