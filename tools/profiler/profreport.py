#!/usr/bin/env python3
"""Profile, turn sampled PCs back into a ranked list of functions.

Four things stand between a sampled address and a name, and getting any of
them wrong produces a ranking that looks entirely reasonable and is not.

1. RELOCATION.  LoadSeg() puts each hunk wherever it likes.  The Amiga side
   records the base and length of every hunk of the profiled program, in load
   order, and the hunk table in the file gives the same hunks' link-time sizes.
   The two are cross-checked here before any address is resolved: if they
   disagree the segment table does not describe this binary and nothing
   downstream is worth printing.

2. STATICS.  The linker does not put file-static symbols in HUNK_SYMBOL, so
   `nm` on the executable shows a few hundred globals and every sample in a
   static lands silently on the preceding global.  They ARE in the .obj files,
   so this walks the link map for the address each object's .text was placed at
   and runs `nm` per object to recover the rest.

3. SHARED LIBRARIES.  A library's code is hunks LoadSeg() put wherever it
   liked, exactly like the program's, so it needs exactly the same treatment --
   and it is where a profile of a real application spends its time.  A library
   that tells the profiler where its seglist is (prof.h) gets its hunk bases
   recorded, and --lib then does 1 and 2 over again against the library's own
   file.  Without that a sample in a library can only be named by module.

4. EVERYTHING ELSE.  A sample can land in Kickstart, in a device driver, in a
   library that does not cooperate, most of them, since 3 is this tool's
   convention and not Exec's.  Every AmigaOS library is a jump table, so the
   Amiga side resolved every LVO of every library, device and resource to its
   address; a sample there is named by the nearest preceding one, and one that
   is in a module's code but near no entry point is named by the MODULE, which
   is the honest answer.  The NDK's lvo/*.i files turn the offset into a name.

Usage:
    tools/profiler/profreport.py spin.prof \\
        --exe build/p20/tools/profiler/profspin \\
        --map build/p20/tools/profiler/profspin.map \\
        --objdir build/p20/tools/profiler \\
        --folded spin.folded --trace spin.json

    tools/profiler/profreport.py fitz.prof --ndk "$AMIGA_NDK" \\
        --lib exe=build/cm/src/bsdsocket/bsdsocket.library,\\
map=build/cm/src/bsdsocket/bsdsocket.library.map,\\
objdir=build/cm/src/bsdsocket

SPDX-License-Identifier: MIT
"""

import argparse
import bisect
import json
import os
import re
import struct
import subprocess
import sys
from collections import defaultdict

HDR = struct.Struct(">28L")
SEG = struct.Struct(">2L")
LIB = struct.Struct(">L2H32s")
LVO = struct.Struct(">L2H")
MARK = struct.Struct(">L28s")
RANGE = struct.Struct(">2L2H32s")
TASK = struct.Struct(">L28s")
WINDOW = struct.Struct(">3L")
SAMPLE = struct.Struct(">L2H2L")
LIBSEG = struct.Struct(">2L2H")

PROF_MAGIC = 0x41505232          # 'APR2'
FMTVALID, OVERFLOW, ODDFORMAT, NTSC, LOSTAUDIO, RATEDIP = 1, 2, 4, 8, 16, 32

PRK_TARGET, PRK_PROFILER, PRK_LIB, PRK_MEMORY, PRK_LIBSEG = 0, 1, 2, 3, 4

CCK_LINE = 227

# A sample near no jump-table target is named by the nearest preceding one only
# if it is NEAR one.  Kickstart entry points are dense enough that 12 KB is
# generous; past that the name would be a guess, and the module range takes
# over, an honest "a2065.device" is worth more than a plausible wrong
# "a2065.device/CMD_WRITE".
LVO_WINDOW = 12 * 1024


def die(msg):
    sys.stderr.write("profreport: %s\n" % msg)
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
         self.nranges, self.ntasks, self.nwindows, self.framecck,
         self.colorclock, self.frames, self.channel,
         self.winframes) = f[:23]

        # Version 2 files have no library segment table and a zero here, that
        # longword having been reserved and written as zero.  So this needs no
        # version test: the count is the count.
        self.nlibsegs = f[23]

        if self.magic != PROF_MAGIC:
            die("%s: bad magic $%08x, tests/perf/prof writes a different, "
                "older format ('APRF'); use tools/prof-report.py for those"
                % (path, self.magic))

        off = HDR.size

        def take(sfmt, n):
            nonlocal off
            out = []
            for _ in range(n):
                out.append(sfmt.unpack_from(blob, off))
                off += sfmt.size
            return out

        self.segs = take(SEG, self.nsegs)
        self.libs = [(b, neg, t, nm.split(b"\0")[0].decode("latin-1"))
                     for b, neg, t, nm in take(LIB, self.nlibs)]
        self.lvos = take(LVO, self.nlvos)
        self.marks = [(i, nm.split(b"\0")[0].decode("latin-1"))
                      for i, nm in take(MARK, self.nmarks)]
        self.ranges = [(lo, hi, k, i, nm.split(b"\0")[0].decode("latin-1"))
                       for lo, hi, k, i, nm in take(RANGE, self.nranges)]
        self.tasks = {t: nm.split(b"\0")[0].decode("latin-1") or "$%08x" % t
                      for t, nm in take(TASK, self.ntasks)}
        self.windows = take(WINDOW, self.nwindows)
        self.samples = take(SAMPLE, self.stored)
        # After the samples, so a version-3 file read by a version-2 reader is
        # a version-2 file with bytes after the end rather than a misparse.
        self.libsegs = take(LIBSEG, self.nlibsegs)

        # The one integrity check that costs nothing: every section is a fixed
        # record size times a count in the header, so the file has exactly one
        # correct length.  A short or long one means the writer and this reader
        # disagree about a struct, and every field after the disagreement is
        # garbage that would still resolve to plausible addresses.
        if off != len(blob):
            die("%s: %d bytes of sections but the file is %d, the writer and "
                "this reader disagree about a record size" % (path, off, len(blob)))

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

    # -- time ---------------------------------------------------------------

    def unwrap(self):
        """[(colour clock since the first sample)] for every sample.

        ps_Time is the raw beam position, which wraps once a video frame.
        Consecutive samples at any rate this tool runs at are far closer
        together than a frame, so a position that went backwards means one
        frame passed.  A gap that swallowed a WHOLE frame aliases to a short
        one, that is what the window table is for, and gap_report() says so
        when the two disagree.
        """
        out, base, prev = [], 0, None
        for _pc, _sr, _fmt, _task, raw in self.samples:
            vpos = ((raw >> 16) & 1) << 8 | ((raw >> 8) & 0xFF)
            hpos = raw & 0xFF
            pos = vpos * CCK_LINE + hpos        # hpos is already colour clocks
            if prev is not None and pos < prev:
                base += self.framecck
            prev = pos
            out.append(base + pos)
        if out:
            first = out[0]
            out = [t - first for t in out]
        return out

    def us(self, cck):
        return cck * 1000000.0 / self.colorclock


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
    it is the only route to the statics that HUNK_SYMBOL never saw.
    """
    contributions = parse_map(mapfile)
    if not contributions:
        die("%s: no section placements found, is this a linker map?" % mapfile)

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


# ------------------------------------------------------ a library's file --

def parse_lib_spec(spec):
    """--lib exe=...,map=...,objdir=...[,name=...] -> dict.

    `name` is what the library calls itself on Exec's list, which is what the
    profile records.  It defaults to the basename of the file, because a
    library is normally called what it is stored as.
    """
    out = {}
    for field in spec.split(","):
        if "=" not in field:
            die("--lib %r: every field is key=value; got %r" % (spec, field))
        k, v = field.split("=", 1)
        k = k.strip()
        if k not in ("name", "exe", "map", "objdir"):
            die("--lib %r: no such field %r (name, exe, map, objdir)" % (spec, k))
        out[k] = v.strip()
    if "exe" not in out:
        die("--lib %r: needs exe=<the library file>" % spec)
    out.setdefault("name", os.path.basename(out["exe"]))
    out.setdefault("objdir", os.path.dirname(out["map"]) if out.get("map") else ".")
    return out


class LibSymbols:
    """One shared library's hunks and symbols, cross-checked against the run.

    The same two things the target executable gets, for the same reasons: the
    hunk sizes in the file must match the ones the run recorded or the symbols
    belong to a different build, and the statics are only reachable through
    the map plus nm per object.
    """

    def __init__(self, name, spec, run_sizes, nm):
        self.name = name
        self.ok = False
        self.nsyms = 0

        sizes = hunk_sizes(spec["exe"])
        if len(sizes) != len(run_sizes):
            self.note = ("%s: the file has %d hunks, the run had %d"
                         % (spec["exe"], len(sizes), len(run_sizes)))
            return
        if any(a != b for a, b in zip(sizes, run_sizes)):
            self.note = ("%s: hunk sizes differ, file %s, run %s"
                         % (spec["exe"], sizes, list(run_sizes)))
            return

        self.symtab = (build_symbol_table(nm, spec["map"], spec["objdir"])
                       if spec.get("map") else {})
        self.nsyms = sum(len(v) for v in self.symtab.values())
        self.sections = [".text", ".data", ".bss"][:len(sizes)]
        self.sorted_syms = {}
        for sec, rows in self.symtab.items():
            self.sorted_syms[sec] = ([r[0] for r in rows], rows)

        self.ok = True
        self.note = ("%d hunks, sizes %s, %d symbols from %s"
                     % (len(sizes), sizes, self.nsyms,
                        spec.get("map") or "(no map, globals only)"))

    def lookup(self, hunk, off):
        """(function, object) for an offset into one hunk, or None."""
        if hunk >= len(self.sections):
            return None
        sec = self.sections[hunk]
        addrs, rows = self.sorted_syms.get(sec, ([], []))
        if not addrs:
            return None
        i = bisect.bisect_right(addrs, off) - 1
        if i < 0:
            return None
        return rows[i][1], rows[i][2]


# ----------------------------------------------------------- attribution --

class Resolver:
    def __init__(self, prof, exe, symtab, libspecs=None, nm=None):
        self.prof = prof
        self.symtab = symtab

        sizes = hunk_sizes(exe) if exe else []
        segs = prof.segs

        # The cross-check.  Every hunk's runtime allocation must match its
        # link-time size or the segment table belongs to a different build, and
        # every address resolved from it would be quietly wrong.
        self.reloc_ok = True
        if not segs:
            self.reloc_ok = False
            self.reloc_note = "the profile carries no segment table"
        elif not sizes:
            self.reloc_note = "%d hunks, sizes %s (no executable to check against)" \
                              % (len(segs), [s[1] for s in segs])
        elif len(sizes) != len(segs):
            self.reloc_ok = False
            self.reloc_note = ("executable has %d hunks, the run had %d"
                               % (len(sizes), len(segs)))
        elif any(a != b[1] for a, b in zip(sizes, segs)):
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
            self.sorted_syms[sec] = ([r[0] for r in rows], rows)

        lvos = sorted(prof.lvos)
        self.lvo_addrs = [t for t, _, _ in lvos]
        self.lvo_rows = lvos

        # Named ranges, narrowest wins.  They overlap on purpose: a library's
        # jump table and the hull of its entry points are two ranges over the
        # same module, and a target hunk sits inside neither.
        self.ranges = sorted(prof.ranges)
        self.range_lo = [r[0] for r in self.ranges]

        self._init_libs(libspecs or [], nm)

    # -- shared libraries ---------------------------------------------------

    def _init_libs(self, libspecs, nm):
        """Where each cooperating library's hunks landed, and its symbols.

        A library that told the Amiga side where its seglist is has an entry
        here and its samples get an offset into a named hunk.  One that did
        not is absent, and falls through to the hull-and-module path below --
        which is what every third-party library gets, and is the reason that
        path is still here.
        """
        self.libsegs = defaultdict(list)          # libidx -> [(base, size)]
        for base, size, libidx, hunk in self.prof.libsegs:
            self.libsegs[libidx].append((hunk, base, size))
        for libidx in self.libsegs:
            self.libsegs[libidx].sort()

        # Flat and sorted, so a PC costs one bisect rather than a scan over
        # every hunk of every library.
        self._lib_lo, self._lib_rows = [], []
        for libidx, hunks in self.libsegs.items():
            for hunk, base, size in hunks:
                self._lib_rows.append((base, size, libidx, hunk))
        self._lib_rows.sort()
        self._lib_lo = [r[0] for r in self._lib_rows]

        byname = {}
        for spec in libspecs:
            byname[spec["name"]] = spec

        self.libsyms = {}                          # libidx -> LibSymbols
        self.libnotes = []                         # what to print about them
        for libidx, hunks in sorted(self.libsegs.items()):
            name = (self.prof.libs[libidx][3]
                    if libidx < len(self.prof.libs) else "?")
            sizes = [size for _h, _b, size in hunks]
            spec = byname.get(name)
            if spec is None:
                self.libnotes.append(
                    "%s: %d hunks from its seglist, no --lib for it -- named "
                    "by hunk and offset" % (name, len(hunks)))
                continue
            syms = LibSymbols(name, spec, sizes, nm)
            self.libnotes.append("%s: %s" % (name, syms.note))
            if syms.ok:
                self.libsyms[libidx] = syms

    def lib_link_time(self, pc):
        """(library index, hunk, offset) for a PC inside a library's hunks."""
        i = bisect.bisect_right(self._lib_lo, pc) - 1
        if i < 0:
            return None
        base, size, libidx, hunk = self._lib_rows[i]
        if pc >= base + size:
            return None
        return libidx, hunk, pc - base

    def module_of(self, pc):
        i = bisect.bisect_right(self.range_lo, pc)
        best = None
        for k in range(i - 1, max(-1, i - 64), -1):
            lo, hi, kind, _idx, name = self.ranges[k]
            if pc < hi and (best is None or (hi - lo) < (best[1] - best[0])):
                best = (lo, hi, kind, name)
        return best

    def link_time(self, pc):
        for i, (base, size) in enumerate(self.prof.segs):
            if base <= pc < base + size:
                return i, pc - base
        return None, None

    def resolve(self, pc):
        """(function, module) for one PC."""
        hunk, off = self.link_time(pc)
        if hunk is not None:
            if hunk >= len(self.sections):
                return ("hunk%d+$%x" % (hunk, off), "the program")
            sec = self.sections[hunk]
            addrs, rows = self.sorted_syms.get(sec, ([], []))
            if addrs:
                i = bisect.bisect_right(addrs, off) - 1
                if i >= 0:
                    return (rows[i][1], rows[i][2])
            return ("%s+$%x" % (sec, off), "the program")

        # Inside a shared library that said where its seglist is.  This is the
        # same mapping the target gets and is checked the same way, so it
        # comes before the hull: a hull is a bracket, this is an extent.
        hit = self.lib_link_time(pc)
        if hit is not None:
            libidx, hunk, off = hit
            name = (self.prof.libs[libidx][3]
                    if libidx < len(self.prof.libs) else "?")
            syms = self.libsyms.get(libidx)
            if syms is not None:
                found = syms.lookup(hunk, off)
                if found is not None:
                    # The library's name in front of the object's, because the
                    # target's objects are in this same column and two builds'
                    # worth of file names would otherwise be indistinguishable.
                    return (found[0], "%s/%s" % (name.split(".")[0], found[1]))
            return ("%s:%d+$%x" % (name, hunk, off), name)

        rng = self.module_of(pc)

        # Standing in a jump table: name the slot.  Exec keeps inline code in
        # some of them, Forbid() and Permit() among them, so a PC there
        # resolves to no target at all and would otherwise go missing.
        for base, neg, _t, name in self.prof.libs:
            if neg and base - neg <= pc < base:
                lvo = 6 * ((base - pc + 5) // 6)
                return (lvo_name(name, lvo), name)

        if self.lvo_addrs:
            i = bisect.bisect_right(self.lvo_addrs, pc) - 1
            if i >= 0 and pc - self.lvo_addrs[i] <= LVO_WINDOW:
                _target, libidx, lvo = self.lvo_rows[i]
                libname = (self.prof.libs[libidx][3]
                           if libidx < len(self.prof.libs) else "?")
                # Only if it is in that module's range, or in no range at all.
                if rng is None or rng[3] == libname:
                    return (lvo_name(libname, lvo), libname)

        if rng is not None:
            lo, _hi, kind, name = rng
            if kind == PRK_PROFILER:
                return ("Profile+$%x" % (pc - lo), "Profile (the profiler)")
            return ("%s+$%x" % (name, pc - lo), name)

        return ("$%08x" % pc, "unattributed")


# --------------------------------------------------------- LVO -> name -----

_LVO_CACHE = {}

# Exec's scheduler entry points are private, so the NDK's exec_lib.i does not
# list them, and they are among the most interesting things in any profile of
# this system.  From the V33+ exec.library LVO table.
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


# ------------------------------------------------------------- the gaps ----

def gap_report(prof, times, res):
    """What the profiler never saw, and where.

    Exec's Disable() clears INTENA's master enable, so no sample is taken
    inside a Disable()/Enable() pair and that time is charged to whatever runs
    next.  With the sample ordinal as the clock this is invisible: a dropped
    sample simply shifts every sample after it.  With a real clock it is a
    measured quantity, and if it is large then every share in the ranking above
    is a share of the part of the run that happened to be visible.
    """
    if len(times) < 2:
        return None

    nominal = prof.colorclock / prof.rate if prof.rate else 0
    if not nominal:
        return None
    threshold = 2 * nominal

    total = times[-1] - times[0]
    missing = 0
    gaps = []
    for i in range(1, len(times)):
        step = times[i] - times[i - 1]
        if step > threshold:
            missing += step - nominal
            gaps.append((step, i))
    gaps.sort(reverse=True)

    # The frame cross-check.  The vertical-blank server counted real video
    # frames; unwrapping the beam position counted the ones the samples went
    # through.  A gap longer than a whole frame aliases to a short one and is
    # the only way these two can disagree, so when they do, say so rather than
    # quietly under-reporting.
    seen_frames = (times[-1] - times[0]) // prof.framecck if prof.framecck else 0
    lost_frames = max(0, prof.frames - seen_frames - 2)
    extra_frames = max(0, seen_frames - prof.frames - 2)

    return {
        "total": total, "missing": missing, "gaps": gaps[:5],
        "lost_frames": lost_frames, "extra_frames": extra_frames,
        "seen_frames": seen_frames, "nominal": nominal,
    }


# --------------------------------------------------------- folded stacks ---

def folded_stacks(prof, res, samples):
    """{"task;context;module;function": count}

    THERE ARE NO CALL STACKS IN THIS.  A PC sampler records one address, so a
    literal folded stack is one frame deep and an icicle graph of it is a bar
    chart with extra steps.  What is emitted instead is a synthetic hierarchy
    the sample already carries, task, task-versus-interrupt context, module,
    function, and every level of it is measured rather than inferred.  For
    "how much of this is Exec, and under which task" it is more use than a real
    call graph would be, and it renders in speedscope, flamegraph.pl and
    inferno without any of them being told it is synthetic.
    """
    out = defaultdict(int)
    for pc, sr, _fmt, task, _t in samples:
        name, module = res.resolve(pc)
        stack = ";".join((
            prof.tasks.get(task, "$%08x" % task),
            "interrupt" if sr & 0x2000 else "task",
            module,
            name,
        ))
        out[stack] += 1
    return out


def write_folded(path, folded):
    with open(path, "w") as fh:
        for stack, n in sorted(folded.items(), key=lambda kv: -kv[1]):
            fh.write("%s %d\n" % (stack, n))


# ------------------------------------------------------- the chrome trace --

def write_trace(path, prof, res, samples, times):
    """Chrome Trace Event JSON, which Perfetto and speedscope both read.

    An aggregate ranking cannot show alternation.  If two threads trade the CPU
    once per packet, a timeline shows the switch rate on its face; a table of
    shares shows two numbers that add to 100 and say nothing about the order.
    That is only possible because the samples carry a real clock, with
    ordinals it would be a picture of the sample index.

    Consecutive samples with the same task, context and function become one
    duration event, so the file is a few thousand events rather than one per
    sample.  A run of samples ends where the next one differs OR where the
    clock says more than two intervals passed, which is the same gap the report
    calls out: an event is never drawn across time nothing was sampled in.
    """
    events = []
    tids = {}
    for t, name in prof.tasks.items():
        tids[t] = len(tids) + 1
        events.append({"name": "thread_name", "ph": "M", "pid": 1,
                       "tid": tids[t], "args": {"name": name}})

    gap = 2.0 * prof.colorclock / prof.rate if prof.rate else 0

    cur = None
    for i, (pc, sr, _fmt, task, _raw) in enumerate(samples):
        name, module = res.resolve(pc)
        tid = tids.setdefault(task, len(tids) + 1)
        key = (tid, bool(sr & 0x2000), name)
        broke = cur is not None and (i > 0 and times[i] - times[i - 1] > gap)

        if cur is not None and (cur["key"] != key or broke):
            events.append({
                "name": cur["name"], "cat": cur["module"], "ph": "X", "pid": 1,
                "tid": cur["key"][0],
                "ts": round(prof.us(cur["start"]), 2),
                "dur": round(max(prof.us(times[i - 1] - cur["start"]),
                                 prof.us(gap / 2)), 2),
                "args": {"context": "interrupt" if cur["key"][1] else "task",
                         "samples": cur["n"]},
            })
            cur = None

        if cur is None:
            cur = {"key": key, "name": name, "module": module,
                   "start": times[i], "n": 0}
        cur["n"] += 1

    if cur is not None:
        events.append({
            "name": cur["name"], "cat": cur["module"], "ph": "X", "pid": 1,
            "tid": cur["key"][0], "ts": round(prof.us(cur["start"]), 2),
            "dur": round(max(prof.us(times[-1] - cur["start"]),
                             prof.us(gap / 2)), 2),
            "args": {"context": "interrupt" if cur["key"][1] else "task",
                     "samples": cur["n"]},
        })

    with open(path, "w") as fh:
        json.dump({"traceEvents": events, "displayTimeUnit": "ms"}, fh)
    return len(events)


# ---------------------------------------------------------- containment ----

def check_contain(prof, res, samples, path):
    """Verify the sampler against a program whose byte ranges are known.

    tools/profiler/profspin writes this file: the exact [start, end) of each of
    its assembly kernels, from the linker's own labels, and the wall clock it
    measured in each phase.  Two questions, and both have to hold.

      CONTAINMENT is the frame-offset test.  The kernels contain no calls, so
      a sample taken while one was running must land inside it.  A vector that
      read the exception frame two bytes off would record half an SR
      concatenated with half a PC, an address nowhere near any of these
      ranges, so this scores approximately zero rather than a bit less.

      PROPORTIONALITY is the bias test.  A sampler can be correctly aimed and
      still fire only when something else periodic is also running.  Each
      kernel's share of the samples has to match its share of the wall clock
      the program measured for itself.
    """
    ranges, ms = {}, {}
    with open(path) as fh:
        for line in fh:
            w = line.split()
            if len(w) == 4 and w[0] == "range":
                ranges[w[1]] = (int(w[2], 16), int(w[3], 16))
            elif len(w) == 3 and w[0] == "ms":
                ms[w[1]] = int(w[2])

    if not ranges:
        die("%s: no ranges in it" % path)

    counts = defaultdict(int)
    other = 0
    for pc, _sr, _fmt, _task, _t in samples:
        for name, (lo, hi) in ranges.items():
            if lo <= pc < hi:
                counts[name] += 1
                break
        else:
            other += 1

    total = len(samples)
    total_ms = sum(ms.values()) or 1
    failures = 0

    print()
    print("containment and proportionality against %s" % path)
    print("%-10s %10s %8s %10s %8s %8s"
          % ("kernel", "bytes", "ms", "samples", "time%", "sample%"))
    print("-" * 60)
    for name in sorted(ranges):
        lo, hi = ranges[name]
        ts = 100.0 * ms.get(name, 0) / total_ms
        ss = 100.0 * counts[name] / total if total else 0.0
        print("%-10s %10d %8d %10d %7.1f%% %7.1f%%"
              % (name, hi - lo, ms.get(name, 0), counts[name], ts, ss))
        if counts[name] < 100:
            print("   FAIL  only %d samples landed in %s, expected the bulk "
                  "of its phase" % (counts[name], name))
            failures += 1
        if abs(ts - ss) > 4.0:
            print("   FAIL  %s has %.1f%% of the wall clock and %.1f%% of the "
                  "samples" % (name, ts, ss))
            failures += 1

    inside = sum(counts.values())
    kernel_ms = sum(v for k, v in ms.items() if k in ranges)
    expect = 100.0 * kernel_ms / total_ms
    got = 100.0 * inside / total if total else 0.0
    print("-" * 60)
    print("%-10s %10s %8d %10d %7.1f%% %7.1f%%"
          % ("all three", "", kernel_ms, inside, expect, got))
    print("%d samples landed outside every kernel (the exec phase, "
          "the C runtime and the profiler)" % other)

    # A wrong frame offset scores ~0 here.  Ten percent is far below anything a
    # correct sampler produces and far above anything a broken one can reach.
    if got < 10.0:
        print("   FAIL  almost nothing landed in the kernels at all, this is "
              "what a wrong exception-frame offset looks like")
        failures += 1

    print()
    print("%d failures, %s" % (failures, "PASS" if failures == 0 else "FAIL"))
    return failures


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
    ap.add_argument("--folded", help="write folded stacks here (speedscope, "
                                     "flamegraph.pl, inferno)")
    ap.add_argument("--trace", help="write Chrome Trace Event JSON here "
                                    "(Perfetto, speedscope)")
    ap.add_argument("--contain", help="verify against a profspin ranges file")
    ap.add_argument("--lib", action="append", default=[], metavar="SPEC",
                    help="a shared library to name functions in, as "
                         "exe=<file>,map=<file>,objdir=<dir>[,name=<what it "
                         "calls itself>].  Repeatable.  Only works for a "
                         "library that told the profiler where its seglist "
                         "is; any other is still named by module.")
    args = ap.parse_args()

    _NDK[0] = args.ndk

    prof = Profile(args.profile)
    symtab = (build_symbol_table(args.nm, args.mapfile, args.objdir)
              if args.mapfile else {})
    libspecs = [parse_lib_spec(spec) for spec in args.lib]
    res = Resolver(prof, args.exe, symtab, libspecs, args.nm)

    # A --lib for a library that never appeared, or appeared without a
    # seglist, silently does nothing, so say so rather than printing a
    # report that is missing exactly what was asked for.
    named = {res.prof.libs[i][3] for i in res.libsegs
             if i < len(res.prof.libs)}
    unmatched = [spec["name"] for spec in libspecs if spec["name"] not in named]

    samples = prof.phase(args.phase)
    all_times = prof.unwrap()
    if args.phase:
        i = prof.samples.index(samples[0]) if samples else 0
        times = all_times[i:i + len(samples)]
    else:
        times = all_times

    print("profile      %s" % args.profile)
    print("rate         %d Hz on interrupt level %d, %s"
          % (prof.rate, prof.level,
             "audio channel %d" % prof.channel if prof.channel < 4
             else "a CIA timer"))
    print("samples      %d of %d (%s)"
          % (len(samples), prof.stored,
             args.phase if args.phase else "whole run"))
    print("interrupts   %d taken, %d dropped" % (prof.hits, prof.dropped))
    print("CPU          AttnFlags $%08x, VBR $%08x, Exec %d, %s"
          % (prof.attnflags, prof.vbr, prof.execver,
             "NTSC" if prof.flags & NTSC else "PAL"))

    if prof.flags & ODDFORMAT:
        print("!! a frame was not format $0, these PCs are NOT trustworthy")
    elif prof.flags & FMTVALID:
        print("frames       all format $0 (checked on the Amiga)")
    else:
        print("frames       68000, six-byte frame, no format word to check")
    if prof.flags & OVERFLOW:
        print("!! the sample buffer filled; the tail of the run is missing")
    if prof.flags & LOSTAUDIO:
        print("!! the sampling source was interfered with, see the Amiga's "
              "own output for which channel")
    if prof.flags & RATEDIP:
        print("!! at least one half-second window ran well under the "
              "programmed rate")

    print("relocation   %s, %s"
          % ("ok" if res.reloc_ok else "MISMATCH", res.reloc_note))
    if not res.reloc_ok:
        print("!! refusing to rank: the segment table does not describe this "
              "executable")
        return 2

    nsym = sum(len(v) for v in symtab.values())
    print("symbols      %d from %s" % (nsym, args.mapfile or "(none)"))
    print("modules      %d libraries, %d resolved jump-table entries, "
          "%d named ranges" % (prof.nlibs, prof.nlvos, prof.nranges))
    for note in res.libnotes:
        print("library      %s" % note)
    for name in unmatched:
        print("!! --lib %s: no library of that name gave the run a seglist; "
              "its samples stay named by module" % name)
    print()

    if not samples:
        print("no samples in this phase")
        return 0

    by_fn = defaultdict(int)
    by_mod = defaultdict(int)
    by_task = defaultdict(int)
    supervisor = 0

    for pc, sr, _fmt, task, _t in samples:
        name, module = res.resolve(pc)
        by_fn[(name, module)] += 1
        by_mod[module] += 1
        by_task[task] += 1
        if sr & 0x2000:
            supervisor += 1

    total = len(samples)

    print("context      %d%% task, %d%% supervisor/interrupt"
          % (100 * (total - supervisor) // total, 100 * supervisor // total))
    if len(by_task) > 1:
        print("tasks        " + ", ".join(
            "%s %d%%" % (prof.tasks.get(t, "$%08x" % t), 100 * n // total)
            for t, n in sorted(by_task.items(), key=lambda kv: -kv[1])[:6]))

    gaps = gap_report(prof, times, res)
    if gaps and gaps["total"]:
        pct = 100.0 * gaps["missing"] / gaps["total"]
        print("unsampled    %.1f%% of %.2f s, time with interrupts masked "
              "(Disable(), or a level 5/6 handler)"
              % (pct, prof.us(gaps["total"]) / 1e6))
        for step, i in gaps["gaps"][:3]:
            a, _ = res.resolve(samples[i - 1][0])
            b, _ = res.resolve(samples[i][0])
            print("             %8.2f ms between %s and %s"
                  % (prof.us(step) / 1000.0, a, b))
        print("             %d video frames by the beam, %d by the vertical "
              "blank" % (gaps["seen_frames"], prof.frames))
        if gaps["lost_frames"] > 0:
            print("!! the vertical blank counted %d frames the samples did "
                  "not go through, at least one gap was longer than a whole "
                  "frame and the figure above is a floor, not a total"
                  % gaps["lost_frames"])
        if gaps["extra_frames"] > 0:
            # The two clocks are independent, so this cannot be a property of
            # the run, it means the beam position is being decoded wrongly
            # and every duration here is inflated.  It is how the hpos scaling
            # bug was found; leaving the check in is what stops it coming back.
            print("!! the beam says %d frames and the vertical blank says %d "
                  "-- the timestamps are being decoded wrongly and every "
                  "duration above is inflated"
                  % (gaps["seen_frames"], prof.frames))
        if pct > 25.0:
            print("!! more than a quarter of the run was never sampled, "
                  "every share below is a share of the part that was visible")
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
        print("unattributed %d samples (%.1f%%), in no hunk, no library range "
              "and no jump-table entry within %d KB"
              % (unattr, 100.0 * unattr / total, LVO_WINDOW // 1024))

    if args.folded:
        folded = folded_stacks(prof, res, samples)
        write_folded(args.folded, folded)
        print()
        print("wrote %s, %d unique stacks.  Drop it on speedscope.app, or"
              % (args.folded, len(folded)))
        print("  flamegraph.pl %s > %s.svg" % (args.folded, args.folded))

    if args.trace:
        n = write_trace(args.trace, prof, res, samples, times)
        print()
        print("wrote %s, %d events.  Drop it on ui.perfetto.dev or "
              "speedscope.app." % (args.trace, n))

    if args.contain:
        return 1 if check_contain(prof, res, samples, args.contain) else 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
