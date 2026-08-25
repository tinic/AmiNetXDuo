#!/usr/bin/env bash
#
# No shipped image may take libgcc's 64-bit helpers instead of the dispatched
# ones.
#
#   tools/check-rt-helpers.sh <build-dir>
#
# src/common/ami_udivdi3.c is the tree's CPU-dispatched copy of __udivdi3 and
# its siblings, and it is 32 bytes where libgcc's is 1,576.  An image gets it
# only if libaminetxduo_m68k_rt.a is on its link line AND something pulls that
# archive before LTRANS invents the libcall -- GCC does not create the call
# until RTL expansion, so an archive scanned at WPA time has already been
# passed over.  Miss either half and the link still succeeds, against libgcc.
#
# That is how `iperf`, `httpd`, `Profile` and `profspin` shipped for as long as
# they did: nothing failed, nothing warned, and the only evidence was in a
# linker map nobody read.  This reads them.
#
# EVERY SHIPPED IMAGE, FOUND BY ITS MAGIC rather than from a list: a HUNK_HEADER
# in one of the directories below is an image that ships, and it must have a
# linker map beside it.  A new target with no map is a FAILURE and not a skip,
# because an unread map is exactly the silence this exists to end.
#
# tests/ is out of scope.  Those images are not installed and several link none
# of our code at all, so there is nothing of ours for them to prefer.
#
# Output is key=value and an exit code: 0 clean, 1 an image took libgcc's,
# 2 nothing to check.
#
# SPDX-License-Identifier: MIT

set -eu

BUILD="${1:-}"

[ -n "$BUILD" ] || { echo "usage: check-rt-helpers.sh <build-dir>" >&2; exit 2; }

# Where the archive's binaries are built.  src/tools holds the commands,
# tools/profiler the two the Developer drawer carries.
DIRS="
src/tools
src/bsdsocket
src/netdev
src/usergroup
src/tlslib
tools/profiler
"

# The members libgcc supplies for the routines ami_udivdi3.c defines, plus
# _clz.o, which has no call site of its own and arrives as _udivdi3.o's table.
MEMBERS='_udivdi3|_umoddi3|_divdi3|_moddi3|_udivmoddi4|_muldi3|_mulsi3'
MEMBERS="$MEMBERS"'|_udivsi3|_umodsi3|_divsi3|_modsi3|_lshrdi3|_ashldi3'
MEMBERS="$MEMBERS"'|_ashrdi3|_clz'

rc=0
checked=0
nomap=""

for dir in $DIRS; do
    [ -d "$BUILD/$dir" ] || continue

    for img in "$BUILD/$dir"/*; do
        [ -f "$img" ] || continue
        case "$img" in *.map|*.a|*.o|*.obj|*.cmake|*.html|*.gz|Makefile) continue ;; esac

        # HUNK_HEADER, 0x000003f3.  An AmigaOS executable and nothing else.
        [ "$(od -An -N4 -tx1 "$img" 2>/dev/null | tr -d ' \n')" = "000003f3" ] || continue

        name="${img##*/}"

        if [ ! -f "$img.map" ]; then
            nomap="$nomap,$dir/$name"
            rc=1
            continue
        fi

        checked=$((checked + 1))

        bad=$(grep -oE "libgcc\.a\(($MEMBERS)\.o\)" "$img.map" | sort -u | tr '\n' ' ')
        if [ -n "$bad" ]; then
            echo "rt_helpers=FAILED image=$dir/$name members=$bad"
            rc=1
        fi
    done
done

if [ -n "$nomap" ]; then
    echo "rt_helpers=FAILED reason=no_map images=${nomap#,}"
    echo "  A shipped image with no linker map cannot be checked.  Give the"
    echo "  target -Wl,-Map=<image>.map, as every other one here has."
fi

if [ "$checked" = 0 ]; then
    echo "rt_helpers=skipped reason=no_images build=$BUILD"
    exit 2
fi

if [ "$rc" = 0 ]; then
    echo "rt_helpers=clean images=$checked"
else
    echo "  src/common/ami_udivdi3.c has these.  A command takes them with the"
    echo "  RT keyword on aminetxduo_add_tool(); anything else links"
    echo "  aminetxduo_m68k_rt and pulls it with -Wl,-u,_ami_rt_cpu_select."
fi

exit "$rc"
