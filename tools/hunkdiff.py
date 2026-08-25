#!/usr/bin/env python3
"""Walk an AmigaOS hunk load file, and diff two of them structurally.

    tools/hunkdiff.py <file>                  # one image, hunk by hunk
    tools/hunkdiff.py <a> <b>                 # two images, side by side
    tools/hunkdiff.py --key <a> [b]           # key=value, one line per hunk
    tools/hunkdiff.py --check <file>...       # gate: any image that LoadSeg
                                              # would not relocate fails

WHY THIS EXISTS

Two images that both link, both pass every host test and both run under an
emulator can still differ in the one place LoadSeg cares about: the
relocation tables.  A hunk file is not a flat blob -- it is a stream of typed
blocks, and dos.library's LoadSeg walks it, allocates one memory block per
loadable hunk, and then APPLIES the relocations by hand.  Anything the
linker did not write into a HUNK_RELOC32 (or its short form) is a pointer
that stays pointing at the link-time address, which on a real machine is
somebody else's memory.

The tree has been bitten by exactly this once already.  9fb69360: the
profiler rebooted the guest twelve seconds into every run, Exception 3 with
PC $ffffffff on the A1200 and a silent warm reset on the A3000, before its
own first line of output.  ld's amiga backend gives each `.debug_*` section
a LOADABLE data hunk, and an executable that has one comes out with no
HUNK_RELOC32 at all -- 12 hunks and 0 relocations against 3 hunks and 1152.
LoadSeg then relocates nothing, every absolute address keeps its link-time
value, and the program jumps into low memory.  --gc-sections removed the
sections and the failure with them.

So when the question is "is this build shape safe on hardware", `ls -l` and
a green test suite do not answer it.  This does: it counts the relocation
entries per hunk, names the hunk types present, and says which hunks carry
pointers and which do not.  A loadable hunk that has relocations in one arm
and NONE in the other is the signature to look for; --check is the same
question asked of one image at a time, for a build gate.

WHAT IT UNDERSTANDS

HUNK_CODE/DATA/BSS, the three relocation forms a loadfile may legally carry
(RELOC32, the OS 2.0 short form under both of the type codes linkers emit
for it, and DREL32), HUNK_SYMBOL, HUNK_DEBUG and HUNK_END.  Anything else it
reports by number rather than guessing, because a block it cannot size is a
parse that is silently one field out from there on.

SPDX-License-Identifier: MIT
"""

import struct
import sys

HUNK_UNIT = 0x3E7
HUNK_NAME = 0x3E8
HUNK_CODE = 0x3E9
HUNK_DATA = 0x3EA
HUNK_BSS = 0x3EB
HUNK_RELOC32 = 0x3EC
HUNK_RELOC16 = 0x3ED
HUNK_RELOC8 = 0x3EE
HUNK_EXT = 0x3EF
HUNK_SYMBOL = 0x3F0
HUNK_DEBUG = 0x3F1
HUNK_END = 0x3F2
HUNK_HEADER = 0x3F3
HUNK_OVERLAY = 0x3F5
HUNK_BREAK = 0x3F6
HUNK_DREL32 = 0x3F7
HUNK_DREL16 = 0x3F8
HUNK_DREL8 = 0x3F9
HUNK_RELOC32SHORT = 0x3FC
HUNK_RELRELOC32 = 0x3FD
HUNK_ABSRELOC16 = 0x3FE

NAMES = {
    HUNK_UNIT: "HUNK_UNIT", HUNK_NAME: "HUNK_NAME", HUNK_CODE: "HUNK_CODE",
    HUNK_DATA: "HUNK_DATA", HUNK_BSS: "HUNK_BSS",
    HUNK_RELOC32: "HUNK_RELOC32", HUNK_RELOC16: "HUNK_RELOC16",
    HUNK_RELOC8: "HUNK_RELOC8", HUNK_EXT: "HUNK_EXT",
    HUNK_SYMBOL: "HUNK_SYMBOL", HUNK_DEBUG: "HUNK_DEBUG",
    HUNK_END: "HUNK_END", HUNK_HEADER: "HUNK_HEADER",
    HUNK_OVERLAY: "HUNK_OVERLAY", HUNK_BREAK: "HUNK_BREAK",
    HUNK_DREL32: "HUNK_DREL32", HUNK_DREL16: "HUNK_DREL16",
    HUNK_DREL8: "HUNK_DREL8", HUNK_RELOC32SHORT: "HUNK_RELOC32SHORT",
    HUNK_RELRELOC32: "HUNK_RELRELOC32", HUNK_ABSRELOC16: "HUNK_ABSRELOC16",
}

LOADABLE = (HUNK_CODE, HUNK_DATA, HUNK_BSS)

# HUNK_DREL32 and HUNK_RELOC32SHORT share a body, and which type code a
# linker writes for it is history rather than meaning: Commodore's own linker
# used 1015 (DREL32) in loadfiles, the official code is 1020, and OS 2.0's
# LoadSeg accepts both.  KICKSTART 1.3 ACCEPTS NEITHER -- an image carrying
# the short form does not load on a 1.3 machine at all.  We report the code
# actually written so that fact stays visible.
SHORT_RELOC = (HUNK_DREL32, HUNK_RELOC32SHORT)

MEMF_MASK = 0xC0000000
MEMF_CHIP = 0x40000000
MEMF_FAST = 0x80000000
MEMF_EXTRA = 0xC0000000


class Bad(Exception):
    pass


class Hunk(object):
    def __init__(self, index):
        self.index = index
        self.kind = None            # HUNK_CODE / HUNK_DATA / HUNK_BSS
        self.size = 0               # bytes, as the header declared them
        self.body = 0               # bytes actually in the file
        self.memflag = ""
        self.relocs = {}            # {form: {target_hunk: count}}
        self.symbols = 0
        self.debug = 0
        self.blocks = []            # every block type seen, in order

    def reloc_total(self):
        return sum(sum(t.values()) for t in self.relocs.values())

    def kindname(self):
        return NAMES.get(self.kind, "0x%X" % (self.kind or 0))

    def forms(self):
        return "+".join(NAMES[f].replace("HUNK_", "")
                        for f in sorted(self.relocs))


class Image(object):
    def __init__(self, path):
        self.path = path
        self.hunks = []
        self.header_sizes = []
        self.resident = []
        self.bytes = 0
        self.tail = 0               # bytes after the last HUNK_END


def _u32(blob, off):
    if off + 4 > len(blob):
        raise Bad("ran off the end at %d" % off)
    return struct.unpack_from(">L", blob, off)[0]


def _reloc32(blob, off):
    """Classic long-form table: {target: count}, and the offset after it."""
    out = {}
    while True:
        n = _u32(blob, off)
        off += 4
        if n == 0:
            return out, off
        target = _u32(blob, off)
        off += 4
        out[target] = out.get(target, 0) + n
        off += n * 4


def _reloc_short(blob, off):
    """OS 2.0 short form: 16-bit counts, targets and offsets, padded even."""
    start = off
    out = {}
    while True:
        if off + 2 > len(blob):
            raise Bad("short reloc ran off the end")
        n = struct.unpack_from(">H", blob, off)[0]
        off += 2
        if n == 0:
            break
        target = struct.unpack_from(">H", blob, off)[0]
        off += 2
        out[target] = out.get(target, 0) + n
        off += n * 2
    if (off - start) % 4:
        off += 2
    return out, off


def _symbols(blob, off):
    n = 0
    while True:
        length = _u32(blob, off)
        off += 4
        if length == 0:
            return n, off
        off += length * 4 + 4
        n += 1


def parse(path):
    """Read one hunk loadfile into an Image, or raise Bad."""
    with open(path, "rb") as fh:
        blob = fh.read()

    img = Image(path)
    img.bytes = len(blob)

    if _u32(blob, 0) != HUNK_HEADER:
        raise Bad("%s: first longword is 0x%X, not HUNK_HEADER (0x3F3)"
                  % (path, _u32(blob, 0)))
    off = 4

    while True:                                 # resident library name list
        n = _u32(blob, off)
        off += 4
        if n == 0:
            break
        img.resident.append(
            blob[off:off + n * 4].split(b"\0")[0].decode("latin-1"))
        off += n * 4

    _table = _u32(blob, off)
    first = _u32(blob, off + 4)
    last = _u32(blob, off + 8)
    off += 12

    for _ in range(last - first + 1):
        raw = _u32(blob, off)
        off += 4
        flag = ""
        if (raw & MEMF_MASK) == MEMF_EXTRA:
            off += 4                            # explicit AllocMem flags
            flag = "extra"
        elif raw & MEMF_CHIP:
            flag = "chip"
        elif raw & MEMF_FAST:
            flag = "fast"
        img.header_sizes.append(((raw & 0x3FFFFFFF) * 4, flag))

    # The hunk blocks themselves.  A new loadable block starts a hunk; every
    # other block belongs to the hunk it follows.
    cur = None
    while off < len(blob):
        kind = _u32(blob, off) & 0x3FFFFFFF
        off += 4

        if kind in LOADABLE:
            cur = Hunk(len(img.hunks))
            img.hunks.append(cur)
            cur.kind = kind
            longs = _u32(blob, off)
            off += 4
            cur.size = longs * 4
            if kind != HUNK_BSS:
                cur.body = longs * 4
                off += longs * 4
            cur.blocks.append(NAMES[kind])
            if cur.index < len(img.header_sizes):
                cur.memflag = img.header_sizes[cur.index][1]
            continue

        if cur is None:
            raise Bad("%s: block 0x%X before any loadable hunk" % (path, kind))

        if kind == HUNK_RELOC32:
            table, off = _reloc32(blob, off)
            cur.relocs.setdefault(kind, {})
            for t, n in table.items():
                cur.relocs[kind][t] = cur.relocs[kind].get(t, 0) + n
            cur.blocks.append("RELOC32")
        elif kind in SHORT_RELOC:
            table, off = _reloc_short(blob, off)
            cur.relocs.setdefault(kind, {})
            for t, n in table.items():
                cur.relocs[kind][t] = cur.relocs[kind].get(t, 0) + n
            cur.blocks.append(NAMES[kind].replace("HUNK_", ""))
        elif kind in (HUNK_RELOC16, HUNK_RELOC8, HUNK_DREL16, HUNK_DREL8,
                      HUNK_RELRELOC32, HUNK_ABSRELOC16):
            table, off = _reloc32(blob, off)
            cur.relocs.setdefault(kind, {})
            for t, n in table.items():
                cur.relocs[kind][t] = cur.relocs[kind].get(t, 0) + n
            cur.blocks.append(NAMES[kind].replace("HUNK_", ""))
        elif kind == HUNK_SYMBOL:
            n, off = _symbols(blob, off)
            cur.symbols += n
            cur.blocks.append("SYMBOL")
        elif kind == HUNK_DEBUG:
            longs = _u32(blob, off)
            off += 4 + longs * 4
            cur.debug += longs * 4
            cur.blocks.append("DEBUG")
        elif kind == HUNK_END:
            cur.blocks.append("END")
            cur = None
            # Trailing padding after the last END is not a parse error.
            if off < len(blob) and all(b == 0 for b in blob[off:]):
                img.tail = len(blob) - off
                break
        elif kind == HUNK_NAME:
            longs = _u32(blob, off)
            off += 4 + longs * 4
            cur.blocks.append("NAME")
        elif kind == HUNK_OVERLAY or kind == HUNK_BREAK:
            raise Bad("%s: %s at %d -- an overlaid loadfile, not handled"
                      % (path, NAMES[kind], off - 4))
        else:
            raise Bad("%s: unknown block 0x%X at offset %d; the parse would "
                      "be one field out from here on" % (path, kind, off - 4))

    return img


# ------------------------------------------------------------- reporting ---

def describe(img):
    rows = []
    for h in img.hunks:
        rows.append(dict(
            index=h.index, kind=h.kindname().replace("HUNK_", ""),
            size=h.size, relocs=h.reloc_total(), forms=h.forms() or "-",
            targets=",".join("%d:%d" % (t, n)
                             for f in sorted(h.relocs)
                             for t, n in sorted(h.relocs[f].items())) or "-",
            symbols=h.symbols, debug=h.debug, memflag=h.memflag or "-",
            blocks="/".join(h.blocks)))
    return rows


def print_one(img):
    print("file      %s" % img.path)
    print("bytes     %d" % img.bytes)
    print("hunks     %d" % len(img.hunks))
    if img.resident:
        print("resident  %s" % ", ".join(img.resident))
    if img.tail:
        print("tail      %d zero bytes after the last HUNK_END" % img.tail)
    print()
    print("  %-3s %-6s %10s %8s %-18s %-24s %7s %8s"
          % ("#", "kind", "size", "relocs", "form", "targets", "symbols",
             "debug"))
    for r in describe(img):
        print("  %-3d %-6s %10d %8d %-18s %-24s %7d %8d"
              % (r["index"], r["kind"], r["size"], r["relocs"], r["forms"],
                 r["targets"], r["symbols"], r["debug"]))
    tot = sum(h.reloc_total() for h in img.hunks)
    print()
    print("  total size %d bytes, %d relocation entries"
          % (sum(h.size for h in img.hunks), tot))


def print_key(img, tag=""):
    p = ("img=%s " % tag) if tag else ""
    print("%sfile=%s bytes=%d hunks=%d relocs=%d"
          % (p, img.path, img.bytes, len(img.hunks),
             sum(h.reloc_total() for h in img.hunks)))
    for r in describe(img):
        print("%shunk=%d kind=%s size=%d relocs=%d form=%s targets=%s "
              "symbols=%d debug=%d blocks=%s"
              % (p, r["index"], r["kind"], r["size"], r["relocs"], r["forms"],
                 r["targets"], r["symbols"], r["debug"], r["blocks"]))


def diff(a, b):
    """Side by side.  Returns the number of findings worth a human's time."""
    findings = []
    print("A  %s  (%d bytes, %d hunks)" % (a.path, a.bytes, len(a.hunks)))
    print("B  %s  (%d bytes, %d hunks)" % (b.path, b.bytes, len(b.hunks)))
    print()

    if len(a.hunks) != len(b.hunks):
        findings.append("hunk COUNT differs: A has %d, B has %d"
                        % (len(a.hunks), len(b.hunks)))

    ra = describe(a)
    rb = describe(b)
    n = max(len(ra), len(rb))
    print("  %-3s | %-6s %10s %8s %-14s | %-6s %10s %8s %-14s"
          % ("#", "A kind", "A size", "A reloc", "A form",
             "B kind", "B size", "B reloc", "B form"))
    for i in range(n):
        x = ra[i] if i < len(ra) else None
        y = rb[i] if i < len(rb) else None
        print("  %-3d | %-6s %10s %8s %-14s | %-6s %10s %8s %-14s"
              % (i,
                 x["kind"] if x else "-", x["size"] if x else "-",
                 x["relocs"] if x else "-", x["forms"] if x else "-",
                 y["kind"] if y else "-", y["size"] if y else "-",
                 y["relocs"] if y else "-", y["forms"] if y else "-"))
        if x and y:
            if x["kind"] != y["kind"]:
                findings.append("hunk %d is %s in A and %s in B"
                                % (i, x["kind"], y["kind"]))
            if x["forms"] != y["forms"]:
                findings.append("hunk %d relocation FORM differs: %s vs %s"
                                % (i, x["forms"], y["forms"]))
            # The one that locks a machine: pointers in one arm, none in the
            # other, in a hunk that has content to point with.
            if bool(x["relocs"]) != bool(y["relocs"]) and x["kind"] != "BSS":
                findings.append(
                    "hunk %d has %d relocations in A and %d in B -- a "
                    "loadable hunk that LoadSeg would relocate in one image "
                    "and not the other" % (i, x["relocs"], y["relocs"]))

    types_a = set()
    types_b = set()
    for h in a.hunks:
        types_a.update(h.blocks)
    for h in b.hunks:
        types_b.update(h.blocks)
    only_a = types_a - types_b
    only_b = types_b - types_a
    print()
    print("  block types in A only: %s"
          % (", ".join(sorted(only_a)) or "none"))
    print("  block types in B only: %s"
          % (", ".join(sorted(only_b)) or "none"))
    if only_a or only_b:
        findings.append("block types differ: A only %s, B only %s"
                        % (sorted(only_a) or "-", sorted(only_b) or "-"))

    ta = sum(h.reloc_total() for h in a.hunks)
    tb = sum(h.reloc_total() for h in b.hunks)
    print("  relocation entries: A %d, B %d" % (ta, tb))
    if (ta == 0) != (tb == 0):
        findings.append("one image has NO relocations at all")

    print()
    if findings:
        print("FINDINGS")
        for f in findings:
            print("  * %s" % f)
    else:
        print("no structural difference beyond sizes and counts")
    return len(findings)


def check(paths):
    """One line per image; nonzero if any of them would load unrelocated.

    The rule is not "must have relocations".  A two-instruction stub legally
    has none.  The rule is that an image whose code reaches an address
    absolutely must carry a table saying so, and the only shape seen to
    break it puts DWARF in loadable hunks, so that is what this names: more
    loadable hunks than the three a normal link produces, together with an
    empty relocation table.
    """
    bad = 0
    for p in paths:
        try:
            img = parse(p)
        except (Bad, IOError) as e:
            print("hunk=BAD file=%s reason=%s" % (p, e))
            bad += 1
            continue
        n = sum(h.reloc_total() for h in img.hunks)
        loadable = len(img.hunks)
        content = sum(h.body for h in img.hunks)
        verdict = "ok"
        if n == 0 and content > 512:
            verdict = "UNRELOCATED"
            bad += 1
        elif loadable > 3:
            verdict = "EXTRA_HUNKS"
            bad += 1
        print("check=%s file=%s hunks=%d relocs=%d bytes=%d"
              % (verdict, p, loadable, n, img.bytes))
    if bad:
        print("hunkdiff: %d image(s) LoadSeg would not relocate correctly"
              % bad)
    return 1 if bad else 0


def main(argv):
    key = False
    gate = False
    args = []
    for a in argv[1:]:
        if a == "--key":
            key = True
        elif a == "--check":
            gate = True
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            args.append(a)

    if not args:
        print(__doc__)
        return 2
    if gate:
        return check(args)
    try:
        if len(args) == 1:
            img = parse(args[0])
            print_key(img) if key else print_one(img)
            return 0
        a = parse(args[0])
        b = parse(args[1])
        if key:
            print_key(a, "A")
            print_key(b, "B")
            return 0
        return 1 if diff(a, b) else 0
    except Bad as e:
        print("hunkdiff: %s" % e, file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
