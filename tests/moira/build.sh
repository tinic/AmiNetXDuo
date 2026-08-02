#!/bin/sh
# AmiNetXDuo -- build the Moira evaluation harness.
#
# Deliberately a shell script and not part of the CMake tree: this is an
# evaluation, and nothing in src/ or the normal build knows it exists.
#
#   tests/moira/build.sh [<moira checkout>]
#
# Produces, in build/moira/:
#   m68000.exe, m68020.exe             the code under test: src/net68k/'s two
#   m68000.locals, m68020.locals       primitives plus workload.c, from the real
#                                      cross toolchain, and the statics the
#                                      load file's symbol table leaves out
#   moiracal-musashi                   audit against upstream Moira defaults
#   moiracal-accurate                  audit with MOIRA_MIMIC_MUSASHI off
#   moiracal-precise                   audit with precise timing as well
#   moiraprof                          the cycle-attribution prototype
#
# Four Moira variants because the accuracy-relevant settings in MoiraConfig.h
# are unconditional #defines, so the only way to change one is to patch a copy.
# What each is for is at the stage() calls below.
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

    m68k-amigaos-gcc -c "-m$cpu" -O2 -g -fomit-frame-pointer \
        "$here/workload.c" -o "$out/workload.$cpu.o"

    # An AmigaOS load file, which is what the toolchain can actually emit --
    # there is no ELF target in this binutils (`objdump -i` lists amiga,
    # a.out-amiga, srec, binary and nothing else).  The harness reads the hunk
    # stream directly; see hunkload.h.
    m68k-amigaos-ld -M "$out/kernels.$cpu.o" \
                    "$out/workload.$cpu.o" \
                    "$out/n68k_copy.$cpu.o" \
                    "$out/n68k_checksum.$cpu.o" \
                    -o "$out/m$cpu.exe" > "$out/m$cpu.map"

    # HUNK_SYMBOL carries globals only, and carries no sizes, so a static
    # function is invisible and its cycles land silently on whichever global
    # precedes it -- which for a profiler is worse than a gap.  The link map
    # places each object and nm lists that object's locals; together they
    # recover the rest.
    python3 - "$out/m$cpu.map" "$out/m$cpu.locals" <<'PY'
import re, subprocess, sys

mapfile, outfile = sys.argv[1], sys.argv[2]
base, rows = {}, []

for line in open(mapfile):
    m = re.match(r'\s+\.text\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+(\S+\.o)\s*$', line)
    if m:
        base[m.group(2)] = int(m.group(1), 16)

for obj, off in base.items():
    nm = subprocess.run(['m68k-amigaos-nm', obj], capture_output=True, text=True).stdout
    for line in nm.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in 'tdbr':          # lowercase: file-local
            rows.append((int(f[0], 16) + off, f[2]))

with open(outfile, 'w') as f:
    for a, n in sorted(rows):
        f.write('%08x %s\n' % (a, n))
PY
done

# --------------------------------------------------------- the Moira variants

stage()                                 # stage <variant> [<sed script>...]
{
    d="$out/moira-$1"; shift
    rm -rf "$d"; mkdir -p "$d"
    cp "$moira"/Moira/* "$d/"

    for s in "$@"; do
        sed -i.bak "$s" "$d/MoiraConfig.h"
    done
    rm -f "$d/MoiraConfig.h.bak"
}

# Upstream, untouched.
stage musashi

# What MoiraConfig.h's own comment recommends: "Set to false for improved
# accuracy".  On its own this is not enough -- see below.
stage accurate 's/^#define MOIRA_MIMIC_MUSASHI.*/#define MOIRA_MIMIC_MUSASHI false/'

# The configuration that is actually cycle exact.  With MOIRA_PRECISE_TIMING
# false, MoiraMacros.h defines SYNC(x) to nothing and every instruction is
# charged the flat number in its CYCLES_ table -- so the data-dependent cost of
# MULU, MULS, DIVU and DIVS is computed and then discarded, and a 68000 MULS.W
# is charged its worst case whatever the operand.  Turning it on restores that
# and costs nothing here.  It has no effect on a 68020: MoiraConfig.h says so,
# and the CYCLES_68020 macro is the same in both branches.
stage precise 's/^#define MOIRA_MIMIC_MUSASHI.*/#define MOIRA_MIMIC_MUSASHI false/' \
              's/^#define MOIRA_PRECISE_TIMING.*/#define MOIRA_PRECISE_TIMING true/'

# The same, plus the instruction hook.  Upstream restricts willExecute() to
# STOP, TAS and BKPT; a profiler needs it on every instruction, and that is a
# recompile of the core, not a runtime switch.
stage profile 's/^#define MOIRA_MIMIC_MUSASHI.*/#define MOIRA_MIMIC_MUSASHI false/' \
              's/^#define MOIRA_PRECISE_TIMING.*/#define MOIRA_PRECISE_TIMING true/' \
              's/^#define MOIRA_WILL_EXECUTE.*/#define MOIRA_WILL_EXECUTE    true/'

# ------------------------------------------------------------------ the harness

: "${CXX:=c++}"

build()                                 # build <exe> <source> <variant>
{
    "$CXX" -std=c++20 -O2 -g -o "$out/$1" "$here/$2" \
        "$out/moira-$3/Moira.cpp" "$out/moira-$3/MoiraDebugger.cpp" \
        -I"$out/moira-$3" -I"$here" \
        -Wno-unused-parameter -Wno-unused-but-set-variable
}

build moiracal-musashi  moiracal.cpp  musashi
build moiracal-accurate moiracal.cpp  accurate
build moiracal-precise  moiracal.cpp  precise
build moiraprof         moiraprof.cpp profile

echo "built $out/moiracal-{musashi,accurate,precise} $out/moiraprof"
