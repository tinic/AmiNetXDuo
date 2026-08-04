#!/usr/bin/env python3
"""AmiNetXDuo, turn sampled PCs back into a ranked list of functions.

Three things stand between a sampled address and a name, and getting any of
them wrong produces a ranking that looks entirely reasonable and is not.

1. RELOCATION.  LoadSeg() puts each hunk wherever it likes.  tests/perf/prof
   records the base and length of every hunk of the running executable, in
   load order, and the hunk table in the file gives the same hunks' link-time
   sizes.  The two are cross-checked here before any address is resolved: if
   they disagree the segment table does not describe this binary and nothing
   downstream is worth printing.

2. STATICS.  The linker does not put file-static symbols in HUNK_SYMBOL, so
   `nm` on the executable shows 409 globals for 79 KB of code and every sample
   in a static lands silently on the preceding global.  They ARE in the .obj
   files, so this walks the link map for the address each object's .text was
   placed at and runs `nm` per object to recover the rest.  Found the hard way:
   a first pass with executable symbols only ranked half a dozen functions
   that were not running.

3. KICKSTART.  A sample can land in our code, in Exec, or anywhere else in
   ROM, and Forbid()/Permit()/Signal()/Wait() time is part of what is being
   hunted, a separate probe put the ThreadX bracket at 214 us per socket
   call and all of it is in Exec.  Every AmigaOS library is a jump table, so
   the Amiga side resolved every LVO of every library, device and resource to
   its ROM address; a ROM sample is named by the nearest preceding one.  The
   NDK's lvo/*.i files turn the offset back into a function name.

Usage:
    tools/prof-report.py profile-loop.bin \\
        --exe build/p20/tests/perf/prof/tcpprof \\
        --map build/p20/tests/perf/prof/tcpprof.map \\
        --objdir build/p20/tests/perf/prof \\
        --phase transfer

SPDX-License-Identifier: MIT
"""

import argparse
import bisect
import os
import re
import struct
import subprocess
import sys
from collections import defaultdict

HDR = struct.Struct(">16L")
SEG = struct.Struct(">2L")
LIB = struct.Struct(">L2H32s")
LVO = struct.Struct(">L2H")
MARK = struct.Struct(">L28s")
SAMPLE = struct.Struct(">L2HL")

PROF_MAGIC = 0x41505246
FMTVALID = 1
OVERFLOW = 2
ODDFORMAT = 4

# A ROM sample is named by the nearest preceding jump-table target, but only
# if it is near one.  Kickstart entry points are dense enough that 12 KB is
# generous; past that the name would be a guess, and an honest "unattributed"
# is worth more than a plausible wrong one.
LVO_WINDOW = 12 * 1024


def die(msg):
    sys.stderr.write("prof-report: %s\n" % msg)
    sys.exit(1)


# ------------------------------------------------------------ the profile --

class Profile:
    def __init__(self, path):
        with open(path, "rb") as fh:
            blob = fh.read()

        if len(blob) < HDR.size:
            die("%s: too short to be a profile" % path)

        f = HDR.unpack_from(blob, 0)
        (self.magic, self.version, self.flags, self.rate, self.hits,
         self.stored, self.dropped, self.vbr, self.attnflags, self.level,
         self.nsegs, self.nlibs, self.nlvos, self.nmarks, self.execver,
         _) = f

        if self.magic != PROF_MAGIC:
            die("%s: bad magic $%08x" % (path, self.magic))

        off = HDR.size
        self.segs = []
        for _ in range(self.nsegs):
            self.segs.append(SEG.unpack_from(blob, off))
            off += SEG.size

        self.libs = []
        for _ in range(self.nlibs):
            base, negsize, ntype, name = LIB.unpack_from(blob, off)
            off += LIB.size
            self.libs.append((base, negsize, ntype,
                              name.split(b"\0")[0].decode("latin-1")))

        self.lvos = []
        for _ in range(self.nlvos):
            self.lvos.append(LVO.unpack_from(blob, off))
            off += LVO.size

        self.marks = []
        for _ in range(self.nmarks):
            idx, label = MARK.unpack_from(blob, off)
            off += MARK.size
            self.marks.append((idx, label.split(b"\0")[0].decode("latin-1")))

        self.samples = []
        for _ in range(self.stored):
            self.samples.append(SAMPLE.unpack_from(blob, off))
            off += SAMPLE.size

    def phase(self, name):
        """Samples between mark `name` and the next mark."""
        if name is None:
            return self.samples
        for i, (idx, label) in enumerate(self.marks):
            if label == name:
                end = (self.marks[i + 1][0] if i + 1 < len(self.marks)
                       else len(self.samples))
                return self.samples[idx:end]
        die("no phase %r; have %s" %
            (name, ", ".join(m[1] for m in self.marks)))


# ---------------------------------------------------------- the hunk file --

def hunk_sizes(path):
    """Link-time byte size of each loadable hunk, in file order."""
    with open(path, "rb") as fh:
        blob = fh.read()

    if struct.unpack_from(">L", blob, 0)[0] != 0x3F3:
        die("%s: not a HUNK executable" % path)

    off = 4
    while True:                                 # resident library name list
        n = struct.unpack_from(">L", blob, off)[0]
        off += 4
        if n == 0:
            break
        off += n * 4

    _table, first, last = struct.unpack_from(">3L", blob, off)
    off += 12

    sizes = []
    for _ in range(last - first + 1):
        raw = struct.unpack_from(">L", blob, off)[0]
        off += 4
        if (raw & 0xC0000000) == 0xC0000000:    # HUNKF_MEMFLAGS: extra long
            off += 4
        sizes.append((raw & 0x3FFFFFFF) * 4)
    return sizes


# -------------------------------------------------------------- symbols ----

SEC_OF_TYPE = {"t": ".text", "T": ".text", "w": ".text", "W": ".text",
               "d": ".data", "D": ".data", "g": ".data", "G": ".data",
               "b": ".bss", "B": ".bss"}

MAP_SEC = re.compile(r"^\s(\.(?:text|data|bss))\s+"
                     r"0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")
MAP_SEC_SPLIT = re.compile(r"^\s(\.(?:text|data|bss))\s*$")
MAP_SEC_TAIL = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")


def parse_map(path):
    """[(section, out_addr, size, object)] for every input contribution."""
    out = []
    pending = None
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            if pending is not None:
                m = MAP_SEC_TAIL.match(line)
                if m:
                    out.append((pending, int(m.group(1), 16),
                                int(m.group(2), 16), m.group(3)))
                pending = None
                continue
            m = MAP_SEC.match(line)
            if m:
                out.append((m.group(1), int(m.group(2), 16),
                            int(m.group(3), 16), m.group(4)))
                continue
            m = MAP_SEC_SPLIT.match(line)
            if m:
                pending = m.group(1)
    return out


def nm_symbols(nm, path, member=None):
    """[(value, type, name)] for one object, or one archive member."""
    try:
        raw = subprocess.run([nm, "--defined-only", path],
                             capture_output=True, text=True, check=False)
    except FileNotFoundError:
        die("cannot run %s" % nm)

    syms = []
    current = None
    for line in raw.stdout.splitlines():
        line = line.rstrip()
        if not line:
            continue
        if line.endswith(":") and " " not in line:
            current = os.path.basename(line[:-1])
            continue
        parts = line.split(None, 2)
        if len(parts) != 3 or not re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            continue
        if member is not None and current != member:
            continue
        syms.append((int(parts[0], 16), parts[1], parts[2]))
    return syms


def build_symbol_table(nm, mapfile, objdir):
    """Link-time address -> name, per section, statics included.

    The map says where each object's .text landed in the output; nm says where
    each symbol sits inside that object.  Adding them is the whole trick, and
    it is the only route to the statics, 79 KB of code carries 409 global
    symbols and several hundred file-static ones that HUNK_SYMBOL never saw.
    """
    contributions = parse_map(mapfile)
    if not contributions:
        die("%s: no section placements found -- is this a linker map?" % mapfile)

    cache = {}
    table = defaultdict(list)          # section -> [(addr, name, module)]

    for section, addr, size, obj in contributions:
        if size == 0:
            continue

        member = None
        spec = obj
        m = re.match(r"^(.*\.a)\((.*)\)$", obj)
        if m:
            spec, member = m.group(1), m.group(2)

        path = spec if os.path.isabs(spec) else os.path.join(objdir, spec)
        if not os.path.exists(path):
            continue

        key = (path, member)
        if key not in cache:
            cache[key] = nm_symbols(nm, path, member)

        module = member if member else os.path.basename(spec)
        if m:
            module = "%s(%s)" % (os.path.basename(spec), member)

        for value, stype, name in cache[key]:
            if SEC_OF_TYPE.get(stype) != section:
                continue
            if value > size:
                continue
            table[section].append((addr + value, name, module))

        # A contribution with no symbols still needs to exist in the table so
        # its samples are attributed to the object rather than to whatever
        # global happens to precede it.
        table[section].append((addr, "[%s]" % module, module))

    for section in table:
        table[section] = sorted(set(table[section]))
    return table


# ----------------------------------------------------------- attribution --

class Resolver:
    def __init__(self, prof, exe, symtab):
        self.prof = prof
        self.symtab = symtab

        sizes = hunk_sizes(exe) if exe else []
        segs = prof.segs

        # The cross-check.  Every hunk's runtime allocation must match its
        # link-time size or the segment table belongs to a different build,
        # and every address resolved from it would be quietly wrong.
        self.reloc_ok = True
        if not segs:
            self.reloc_ok = False
            self.reloc_note = "the profile carries no segment table"
        elif sizes and len(sizes) != len(segs):
            self.reloc_ok = False
            self.reloc_note = ("executable has %d hunks, the run had %d"
                               % (len(sizes), len(segs)))
        elif sizes and any(a != b[1] for a, b in zip(sizes, segs)):
            self.reloc_ok = False
            self.reloc_note = ("hunk sizes differ: file %s, run %s"
                               % (sizes, [s[1] for s in segs]))
        else:
            self.reloc_note = ("%d hunks, sizes %s"
                               % (len(segs), [s[1] for s in segs]))

        # Hunk index -> section name.  ld emits .text, .data, .bss in that
        # order and the loader keeps it, which the size check above confirms.
        self.sections = [".text", ".data", ".bss"][:len(segs)]

        self.sorted_syms = {}
        for sec, rows in symtab.items():
            addrs = [r[0] for r in rows]
            self.sorted_syms[sec] = (addrs, rows)

        # ROM: every resolved jump-table target, sorted.
        lvos = sorted(prof.lvos)
        self.lvo_addrs = [t for t, _, _ in lvos]
        self.lvo_rows = lvos

        # The jump tables themselves.  Not every entry is a JMP: Exec keeps
        # inline code in some slots, Forbid() and Permit() among them, so
        # the Amiga side could not resolve a target for them and a PC landing
        # there matches no target at all.  In the self-test that was 7.2% of
        # the profile sitting in "unattributed", and it was Forbid/Permit
        # time, which is precisely what must not go missing.  A PC inside
        # [base - negsize, base) is named from the slot it is standing in.
        self.neg = sorted((b - neg, b, i)
                          for i, (b, neg, _t, _n) in enumerate(prof.libs)
                          if neg)
        self.neg_lo = [r[0] for r in self.neg]

    def link_time(self, pc):
        for i, (base, size) in enumerate(self.prof.segs):
            if base <= pc < base + size:
                return i, pc - base
        return None, None

    def resolve(self, pc):
        """(bucket, module) for one PC."""
        hunk, off = self.link_time(pc)
        if hunk is not None:
            if hunk >= len(self.sections):
                return ("hunk%d+$%x" % (hunk, off), "our code")
            sec = self.sections[hunk]
            addrs, rows = self.sorted_syms.get(sec, ([], []))
            if addrs:
                i = bisect.bisect_right(addrs, off) - 1
                if i >= 0:
                    return (rows[i][1], rows[i][2])
            return ("%s+$%x" % (sec, off), "our code")

        # Standing in a jump table: name the slot.
        j = bisect.bisect_right(self.neg_lo, pc) - 1
        if j >= 0:
            lo, base, libidx = self.neg[j]
            if lo <= pc < base:
                delta = base - pc
                lvo = 6 * ((delta + 5) // 6)
                libname = self.prof.libs[libidx][3]
                return (lvo_name(libname, lvo), libname)

        if self.lvo_addrs:
            i = bisect.bisect_right(self.lvo_addrs, pc) - 1
            if i >= 0 and pc - self.lvo_addrs[i] <= LVO_WINDOW:
                target, libidx, lvo = self.lvo_rows[i]
                libname = self.prof.libs[libidx][3] if libidx < len(self.prof.libs) else "?"
                return (lvo_name(libname, lvo), libname)

        return ("$%08x" % (pc & 0xFFFF0000), "unattributed")


# --------------------------------------------------------- LVO -> name -----

_LVO_CACHE = {}

# Exec's scheduler entry points are private, so the NDK's exec_lib.i does not
# list them, and they are among the most interesting things in a profile of
# this stack, because a ThreadX thread switch on this port is an Exec task
# switch.  From the V33+ exec.library LVO table.
EXEC_PRIVATE = {
    36: "ExitIntr", 42: "Schedule", 48: "Reschedule", 54: "Switch",
    60: "Dispatch", 66: "Exception",
}


def load_lvo_names(ndk, libname):
    """{offset: name} for one library, from the NDK's lvo/*.i."""
    if libname in _LVO_CACHE:
        return _LVO_CACHE[libname]

    names = {}
    if ndk:
        stem = libname.split(".")[0]
        for cand in ("%s_lib.i" % stem, "%s.i" % stem):
            path = os.path.join(ndk, "lvo", cand)
            if os.path.exists(path):
                with open(path, "r", errors="replace") as fh:
                    for line in fh:
                        m = re.match(r"\s*_LVO(\w+)\s+EQU\s+(-\d+)", line)
                        if m:
                            names[-int(m.group(2))] = m.group(1)
                break
    _LVO_CACHE[libname] = names
    return names


_NDK = [None]


def lvo_name(libname, lvo):
    names = load_lvo_names(_NDK[0], libname)
    if lvo in names:
        return "%s/%s" % (libname, names[lvo])
    if libname == "exec.library" and lvo in EXEC_PRIVATE:
        return "exec.library/%s" % EXEC_PRIVATE[lvo]
    return "%s/LVO-%d" % (libname, lvo)


# ------------------------------------------------------------------ main --

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--exe", help="the HUNK executable that was profiled")
    ap.add_argument("--map", dest="mapfile", help="its linker map")
    ap.add_argument("--objdir", default=".",
                    help="what the map's relative object paths are relative to")
    ap.add_argument("--nm", default="m68k-amigaos-nm")
    ap.add_argument("--ndk", help="NDK include dir, for lvo/*.i")
    ap.add_argument("--phase", help="only samples between this mark and the next")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--by-module", action="store_true")
    args = ap.parse_args()

    _NDK[0] = args.ndk

    prof = Profile(args.profile)
    symtab = (build_symbol_table(args.nm, args.mapfile, args.objdir)
              if args.mapfile else {})
    res = Resolver(prof, args.exe, symtab)

    samples = prof.phase(args.phase)

    print("profile      %s" % args.profile)
    print("rate         %d Hz on interrupt level %d" % (prof.rate, prof.level))
    print("samples      %d of %d (%s)"
          % (len(samples), prof.stored,
             args.phase if args.phase else "whole run"))
    print("interrupts   %d taken, %d dropped" % (prof.hits, prof.dropped))
    print("CPU          AttnFlags $%08x, VBR $%08x, Exec %d"
          % (prof.attnflags, prof.vbr, prof.execver))

    if prof.flags & ODDFORMAT:
        print("!! a frame was not format $0 -- these PCs are NOT trustworthy")
    elif prof.flags & FMTVALID:
        print("frames       all format $0 (checked on the Amiga)")
    else:
        print("frames       68000, six-byte frame, no format word to check")
    if prof.flags & OVERFLOW:
        print("!! the sample buffer filled; the tail of the run is missing")

    print("relocation   %s -- %s"
          % ("ok" if res.reloc_ok else "MISMATCH", res.reloc_note))
    if not res.reloc_ok:
        print("!! refusing to rank: the segment table does not describe this "
              "executable")
        return 2

    nsym = sum(len(v) for v in symtab.values())
    print("symbols      %d from %s" % (nsym, args.mapfile or "(none)"))
    print("libraries    %d, %d resolved jump-table entries"
          % (prof.nlibs, prof.nlvos))
    print()

    if not samples:
        print("no samples in this phase")
        return 0

    by_fn = defaultdict(int)
    by_mod = defaultdict(int)
    by_task = defaultdict(int)
    supervisor = 0

    for pc, sr, _fmt, task in samples:
        name, module = res.resolve(pc)
        by_fn[(name, module)] += 1
        by_mod[module] += 1
        by_task[task] += 1
        if sr & 0x2000:
            supervisor += 1

    total = len(samples)

    print("context      %d%% task, %d%% supervisor/interrupt"
          % (100 * (total - supervisor) // total, 100 * supervisor // total))
    print()

    if args.by_module:
        print("%-46s %8s %7s" % ("module", "samples", "share"))
        print("-" * 64)
        for module, n in sorted(by_mod.items(), key=lambda kv: -kv[1]):
            print("%-46s %8d %6.1f%%" % (module[:46], n, 100.0 * n / total))
        print()

    print("%-38s %-24s %7s %7s" % ("function", "module", "samples", "share"))
    print("-" * 80)
    cum = 0
    for i, ((name, module), n) in enumerate(
            sorted(by_fn.items(), key=lambda kv: -kv[1])):
        if i >= args.top:
            break
        cum += n
        print("%-38s %-24s %7d %6.1f%%"
              % (name[:38], module[:24], n, 100.0 * n / total))
    print("-" * 80)
    print("%-63s %7d %6.1f%%" % ("top %d" % min(args.top, len(by_fn)),
                                 cum, 100.0 * cum / total))

    unattr = by_mod.get("unattributed", 0)
    if unattr:
        print()
        print("unattributed %d samples (%.1f%%) -- no hunk and no jump-table "
              "entry within %d KB" % (unattr, 100.0 * unattr / total,
                                      LVO_WINDOW // 1024))

    return 0


if __name__ == "__main__":
    sys.exit(main())
