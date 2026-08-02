#!/bin/sh
# AmiNetXDuo -- build the Moira evaluation harness.
#
# Deliberately a shell script and not part of the CMake tree: this is an
# evaluation, and nothing in src/ or the normal build knows it exists.
#
#   tests/moira/build.sh [<moira checkout>]
#
# Produces, in build/moira/:
#   m68000.bin/.sym, m68020.bin/.sym   the code under test, from the real
#                                      cross toolchain
#   moiracal                           the host harness
#
# SPDX-License-Identifier: MIT

set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$root/build/moira"

moira=${1:-$root/../anxd-moira-lib}
[ -d "$moira/Moira" ] || {
    echo "no Moira checkout at $moira" >&2
    echo "  git clone https://github.com/dirkwhoffmann/Moira $moira" >&2
    exit 1
}

# The cross toolchain, same search order as cmake/toolchain-m68k-amigaos.cmake
# cares about: whatever is on PATH first, then the local install.
if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    PATH="$HOME/amigaos/tools/m68k-amigaos-gcc/bin:$PATH"
    export PATH
fi
command -v m68k-amigaos-gcc >/dev/null 2>&1 || {
    echo "m68k-amigaos-gcc not found" >&2
    exit 1
}

mkdir -p "$out"

# ---------------------------------------------------------- the code under test
#
# The point of the exercise: these are the same two .S files the Amiga build
# assembles, with the same flags, and no separate copy of anything.
for cpu in 68000 68020; do

    for f in "$root/src/net68k/n68k_copy.S" \
             "$root/src/net68k/n68k_checksum.S" \
             "$here/kernels.S"; do

        m68k-amigaos-gcc -c "-m$cpu" -DAMINETXDUO_NET68K_ASM \
            "$f" -o "$out/$(basename "$f" .S).$cpu.o"
    done

    m68k-amigaos-ld "$out/kernels.$cpu.o" \
                    "$out/n68k_copy.$cpu.o" \
                    "$out/n68k_checksum.$cpu.o" \
                    -o "$out/m$cpu.elf"

    m68k-amigaos-objcopy -O binary "$out/m$cpu.elf" "$out/m$cpu.bin"
    m68k-amigaos-nm "$out/m$cpu.elf" > "$out/m$cpu.sym"
done

# ------------------------------------------------------------------ the harness

: "${CXX:=c++}"

"$CXX" -std=c++20 -O2 -g -o "$out/moiracal" \
    "$here/moiracal.cpp" \
    "$moira/Moira/Moira.cpp" \
    "$moira/Moira/MoiraDebugger.cpp" \
    -I"$moira/Moira" -I"$moira" -I"$here" \
    -Wno-unused-parameter -Wno-unused-but-set-variable

echo "built $out/moiracal"
