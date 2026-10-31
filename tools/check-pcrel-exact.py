#!/usr/bin/env python3
"""Exact check of every 32-bit PC-relative relocation in a linked hunk image.

    tools/check-pcrel-exact.py BINARY MAPFILE [DIR...]

For every HUNK_RELRELOC32 (hunk-relative) and EXT_RELREF32 (symbol-relative)
site in every input object of the link, archive members included, the value
the linker wrote must equal S + A - P:
  S  linked base of the target input section (map) or linked symbol address
     (HUNK_SYMBOL of the image's code hunk),
  A  addend stored in the object's field,
  P  linked address of the field.
Input objects come from the linker map; DIRs are where its relative paths
resolve (the link's working directory first).  An LTO link needs
-save-temps=obj -Wl,-plugin-opt=-save-temps so the ltrans objects survive.

Output is key=value.  Exit 0: every site matches.  1: a mismatch.
2: something could not be checked (a refusal, not a pass).

SPDX-License-Identifier: MIT
"""
import os
import re
import struct
import sys

HUNK_UNIT, HUNK_NAME, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3E7, 0x3E8, 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_RELOC16, HUNK_RELOC8, HUNK_EXT, HUNK_SYMBOL = 0x3EC, 0x3ED, 0x3EE, 0x3EF, 0x3F0
HUNK_DEBUG, HUNK_END, HUNK_HEADER = 0x3F1, 0x3F2, 0x3F3
HUNK_DREL32, HUNK_DREL16, HUNK_DREL8 = 0x3F7, 0x3F8, 0x3F9
HUNK_RELOC32SHORT, HUNK_RELRELOC32, HUNK_ABSRELOC16 = 0x3FC, 0x3FD, 0x3FE
EXT_RELREF32 = 136
EXT_COMMONS = (130, 137)
# Field width of every relocation the object can carry: those bytes are the
# linker's to write, every other byte of an input code hunk must reach the
# image unchanged.
RELOC_WIDTH = {HUNK_RELOC32: 4, HUNK_RELOC16: 2, HUNK_RELOC8: 1,
               HUNK_DREL32: 4, HUNK_DREL16: 2, HUNK_DREL8: 1,
               HUNK_RELOC32SHORT: 4, HUNK_RELRELOC32: 4, HUNK_ABSRELOC16: 2}
EXT_WIDTH = {129: 4, 130: 4, 131: 2, 132: 1, 133: 4, 134: 2, 135: 1,
             136: 4, 137: 4, 138: 2, 139: 1}


def die(msg):
    print("pcrel_exact=ERROR reason=%s" % msg.replace(" ", "_"))
    sys.exit(2)


class Reader:
    def __init__(self, d):
        self.d, self.p = d, 0

    def l(self):
        if self.p + 4 > len(self.d):
            die("truncated_hunk_file")
        v = struct.unpack_from(">I", self.d, self.p)[0]
        self.p += 4
        return v

    def w(self):
        v = struct.unpack_from(">H", self.d, self.p)[0]
        self.p += 2
        return v

    def name(self, n):
        b = self.d[self.p:self.p + 4 * n]
        self.p += 4 * n
        return b.rstrip(b"\0").decode("latin-1")

    def eof(self):
        return self.p + 4 > len(self.d)


def parse_units(data):
    """Hunk object or hunk library -> [(unit name, [hunk dict])]."""
    r = Reader(data)
    units = []
    hunks = None
    cur = None
    name = None

    def need_cur():
        if cur is None:
            die("reloc_outside_hunk")
        return cur

    def reloc_long(dst, width):
        h = need_cur()
        while True:
            n = r.l()
            if n == 0:
                return
            t = r.l()
            for _ in range(n):
                o = r.l()
                dst.append((o, t))
                h["fields"].append((o, width))

    def reloc_short(width):
        h = need_cur()
        while True:
            n = r.w()
            if n == 0:
                break
            r.w()
            for _ in range(n):
                h["fields"].append((r.w(), width))
        if r.p % 4:
            r.p += 2

    def new_hunk(t, default, has_data):
        n = r.l()
        if n & 0xC0000000 == 0xC0000000:
            r.l()
        n &= 0x3FFFFFFF
        h = dict(name=name or default, type=t, size=4 * n,
                 data=r.d[r.p:r.p + 4 * n] if has_data else b"",
                 relrel=[], extrel=[], fields=[], idx=len(hunks))
        if has_data:
            r.p += 4 * n
        hunks.append(h)
        return h

    while not r.eof():
        t = r.l() & 0x3FFFFFFF
        if t == HUNK_UNIT:
            hunks = []
            units.append((r.name(r.l()), hunks))
            continue
        if hunks is None:
            die("hunk_before_unit")
        if t == HUNK_NAME:
            name = r.name(r.l())
        elif t == HUNK_CODE:
            cur = new_hunk(t, ".text", True)
        elif t == HUNK_DATA:
            cur = new_hunk(t, ".data", True)
        elif t == HUNK_BSS:
            cur = new_hunk(t, ".bss", False)
        elif t in (HUNK_RELOC32, HUNK_RELOC16, HUNK_RELOC8,
                   HUNK_DREL32, HUNK_DREL16, HUNK_DREL8):
            reloc_long([], RELOC_WIDTH[t])
        elif t == HUNK_RELRELOC32:
            reloc_long(need_cur()["relrel"], 4)
        elif t in (HUNK_RELOC32SHORT, HUNK_ABSRELOC16):
            reloc_short(RELOC_WIDTH[t])
        elif t == HUNK_EXT:
            while True:
                h = r.l()
                if h == 0:
                    break
                ty, nl = h >> 24, h & 0xFFFFFF
                nm = r.name(nl)
                if ty < 128:
                    r.l()
                    continue
                if ty in EXT_COMMONS:
                    r.l()
                if ty not in EXT_WIDTH:
                    die("unknown_ext_type_%d" % ty)
                for _ in range(r.l()):
                    o = r.l()
                    need_cur()["fields"].append((o, EXT_WIDTH[ty]))
                    if ty == EXT_RELREF32:
                        cur["extrel"].append((o, nm))
        elif t == HUNK_SYMBOL:
            while True:
                nl = r.l()
                if nl == 0:
                    break
                r.name(nl)
                r.l()
        elif t == HUNK_DEBUG and cur is None:
            # A debug section of its own (.debug_frame in libgcc), with its own
            # relocations up to HUNK_END.  It takes a hunk number like any
            # other: libgcc's unwind-dw2.o relocates against hunk 3, the data
            # hunk after its .debug_frame at 2, exactly as objdump -h numbers.
            n = r.l()
            cur = dict(name=name or ".debug", type=t, size=4 * n,
                       data=r.d[r.p:r.p + 4 * n], relrel=[], extrel=[], fields=[],
                       idx=len(hunks))
            r.p += 4 * n
            hunks.append(cur)
        elif t == HUNK_DEBUG:
            n = r.l()
            r.p += 4 * n
        elif t == HUNK_END:
            cur = None
            name = None
        else:
            die("unknown_object_hunk_0x%x" % t)
    return units


def ar_members(path):
    """-> [(member name, unit hunks)] for a GNU ar or a hunk library."""
    d = open(path, "rb").read()
    if d[:4] == struct.pack(">I", HUNK_UNIT):
        return [(os.path.basename(n), hs) for n, hs in parse_units(d)]
    if d[:8] != b"!<arch>\n":
        die("unknown_archive_format_%s" % path)
    p, longnames, out = 8, b"", []
    while p + 60 <= len(d):
        hdr = d[p:p + 60]
        nm = hdr[:16].decode("latin-1").rstrip()
        sz = int(hdr[48:58].decode().strip())
        body = d[p + 60:p + 60 + sz]
        p += 60 + sz + (sz & 1)
        if nm == "//":
            longnames = body
            continue
        if nm in ("/", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED"):
            continue
        if nm.startswith("/") and nm[1:].isdigit():
            o = int(nm[1:])
            nm = longnames[o:longnames.index(b"/\n", o)].decode("latin-1")
        elif nm.startswith("#1/"):
            n = int(nm[3:])
            nm = body[:n].rstrip(b"\0").decode("latin-1")
            body = body[n:]
        else:
            nm = nm.rstrip("/")
        for _, hs in parse_units(body):
            out.append((nm, hs))
    return out


def parse_exe(path):
    """-> (first code hunk bytes, {symbol: offset in it})."""
    r = Reader(open(path, "rb").read())
    if r.l() != HUNK_HEADER:
        die("not_a_hunk_executable")
    while True:
        n = r.l()
        if n == 0:
            break
        r.p += 4 * n
    r.l()
    first, last = r.l(), r.l()
    r.p += 4 * (last - first + 1)
    code = None
    syms = {}
    while not r.eof():
        t = r.l() & 0x3FFFFFFF
        if t == HUNK_NAME:
            r.name(r.l())
        elif t == HUNK_CODE and code is None:
            n = r.l() & 0x3FFFFFFF
            code = r.d[r.p:r.p + 4 * n]
            r.p += 4 * n
        elif code is None:
            die("first_hunk_not_code_0x%x" % t)
        elif t == HUNK_RELOC32:
            while True:
                n = r.l()
                if n == 0:
                    break
                r.p += 4 * (n + 1)
        elif t in (HUNK_RELOC32SHORT, HUNK_DREL32):
            while True:
                n = r.w()
                if n == 0:
                    break
                r.p += 2 * (n + 1)
            if r.p % 4:
                r.p += 2
        elif t == HUNK_SYMBOL:
            while True:
                nl = r.l()
                if nl == 0:
                    break
                s = r.name(nl)
                syms.setdefault(s, r.l())
        elif t == HUNK_DEBUG:
            n = r.l()
            r.p += 4 * n
        elif t == HUNK_END:
            break
        else:
            die("unknown_image_hunk_0x%x" % t)
    if code is None:
        die("no_code_hunk")
    if not syms:
        die("image_has_no_symbols")
    return code, syms


def parse_map(mapfile):
    """-> ((.text start, size), [(input section, addr, size, file)])."""
    lines = open(mapfile, encoding="latin-1").read().split("\n")
    try:
        start = next(i for i, l in enumerate(lines)
                     if l.startswith("Linker script and memory map"))
    except StopIteration:
        die("map_has_no_memory_map")
    text_out = None
    entries = []
    pending = None
    for l in lines[start:]:
        m = re.match(r"^\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", l)
        if m:
            text_out = (int(m.group(1), 16), int(m.group(2), 16))
        if pending:
            m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", l)
            pending, name = None, pending
            if m:
                entries.append((name, int(m.group(1), 16),
                                int(m.group(2), 16), m.group(3).strip()))
                continue
        m = re.match(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", l)
        if m:
            entries.append((m.group(1), int(m.group(2), 16),
                            int(m.group(3), 16), m.group(4).strip()))
            continue
        m = re.match(r"^ (\.\S+)\s*$", l)
        if m:
            pending = m.group(1)
    if not text_out or text_out[0] != 0:
        die("map_text_not_at_0")
    return text_out, entries


def parse_discarded(mapfile):
    """-> {(input section, file): size} that --gc-sections dropped."""
    lines = open(mapfile, encoding="latin-1").read().split("\n")
    out, on, pending = {}, False, None
    for l in lines:
        if l.startswith("Discarded input sections"):
            on = True
            continue
        if on and (l.startswith("Memory Configuration") or
                   l.startswith("Linker script and memory map")):
            break
        if not on:
            continue
        m = re.match(r"^ (\.\S+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S.*)$", l)
        if m:
            out[(m.group(1), m.group(3).strip())] = int(m.group(2), 16)
            pending = None
            continue
        m = re.match(r"^\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S.*)$", l)
        if m and pending:
            out[(pending, m.group(2).strip())] = int(m.group(1), 16)
            pending = None
            continue
        m = re.match(r"^ (\.\S+)\s*$", l)
        pending = m.group(1) if m else None
    return out


def main():
    if len(sys.argv) < 3:
        die("usage")
    binary, mapfile = sys.argv[1:3]
    dirs = sys.argv[3:] or [os.path.dirname(os.path.abspath(mapfile))]
    bname = os.path.basename(binary)

    def resolve(p):
        if os.path.isabs(p):
            return p
        for d in dirs:
            if os.path.exists(os.path.join(d, p)):
                return os.path.join(d, p)
        return p

    code, syms = parse_exe(binary)
    text_out, entries = parse_map(mapfile)
    discarded = parse_discarded(mapfile)
    if text_out[1] > len(code):
        die("map_text_larger_than_code_hunk")

    byfile = {}
    for e in entries:
        byfile.setdefault(e[3], []).append(e)

    cache = {}

    def load(path):
        m = re.match(r"^(.*\.a)\((.*)\)$", path)
        if m:
            arch = resolve(m.group(1))
            if not os.path.exists(arch):
                return []
            if arch not in cache:
                cache[arch] = ar_members(arch)
            return [hs for n, hs in cache[arch] if n == m.group(2)]
        path = resolve(path)
        if not os.path.exists(path):
            return []
        return [hs for _, hs in parse_units(open(path, "rb").read())]

    def word(a):
        return struct.unpack_from(">H", code, a)[0] if 0 <= a < len(code) - 1 else None

    def kind(f):
        if word(f - 2) == 0x60FF:
            return "bra.l"
        if word(f - 2) == 0x61FF:
            return "bsr.l"
        if word(f - 4) in (0x4EBB, 0x4EFB) and word(f - 2) == 0x0170:
            return "jsr" if word(f - 4) == 0x4EBB else "jmp"
        return "other"

    def text_entry(e):
        return e[0] == ".text" or e[0].startswith(".text.")

    checked = bad = unscanned = branches = objects = verbatim = mismatches = covered = 0
    dropped = 0
    for path, ents in byfile.items():
        if not any(text_entry(e) and e[2] for e in ents):
            continue
        cands = load(path)
        if not cands:
            print("pcrel_unscanned=%s reason=no_object" % path)
            unscanned += 1
            continue
        placed = None
        for hs in cands:
            used = set()
            base = {}
            ok = True
            for (sec, addr, size, _) in ents:
                if not text_entry((sec,)):
                    continue
                c = next((h for h in hs if h["idx"] not in used and h["type"] == HUNK_CODE and
                          h["name"] == sec and h["size"] in (size, (size + 3) & ~3)), None)
                if c is None:
                    if size == 0:
                        continue
                    ok = False
                    break
                used.add(c["idx"])
                base[c["idx"]] = (addr, True, size)
            if ok:
                placed = (hs, base)
                break
        if placed is None:
            print("pcrel_unscanned=%s reason=map_hunk_mismatch" % path)
            unscanned += 1
            continue
        objects += 1
        hs, base = placed
        for h in hs:
            if h["type"] != HUNK_CODE:
                continue
            if h["idx"] not in base:
                # A section --gc-sections dropped is not in the image, so
                # its relocations were never applied.  One section per
                # function makes these common.
                dsize = discarded.get((h["name"], path))
                if dsize is not None and h["size"] in (dsize, (dsize + 3) & ~3):
                    dropped += 1
                    continue
                if h["relrel"] or h["extrel"]:
                    print("pcrel_unscanned=%s hunk=%d reason=not_in_map" % (path, h["idx"]))
                    unscanned += 1
                continue
            hb, in_text, msize = base[h["idx"]]
            if not in_text or hb + msize > text_out[1]:
                print("pcrel_unscanned=%s hunk=%d reason=not_in_text" % (path, h["idx"]))
                unscanned += 1
                continue
            # Every byte that is not a relocation field is the assembler's, so it
            # must reach the image unchanged: this covers the branches as(1)
            # resolved inside one section, which carry no relocation.
            mask = bytearray(msize)
            for o, wd in h["fields"]:
                mask[o:o + wd] = b"\1" * wd
            for i in range(msize):
                if not mask[i] and h["data"][i] != code[hb + i]:
                    bad += 1
                    print("pcrel_bytes_differ=%s site=0x%x object=%s hunk=%d offset=0x%x "
                          "want=0x%02x got=0x%02x"
                          % (bname, hb + i, path, h["idx"], i, h["data"][i], code[hb + i]))
                    break
            verbatim += msize - sum(mask)
            covered += msize
            sites = [(o, "hunk", t) for o, t in h["relrel"]] + \
                    [(o, "sym", s) for o, s in h["extrel"]]
            for o, how, tgt in sites:
                A = struct.unpack_from(">i", h["data"], o)[0]
                P = hb + o
                got = P + struct.unpack_from(">i", code, P)[0]
                k = kind(P)
                if k != "other":
                    branches += 1
                if how == "hunk":
                    if tgt not in base:
                        print("pcrel_unscanned=%s site=0x%x reason=target_hunk_%d_unplaced"
                              % (path, P, tgt))
                        unscanned += 1
                        continue
                    wants = [base[tgt][0] + A]
                    desc = "%s:hunk%d+0x%x" % (os.path.basename(path), tgt, A)
                else:
                    names = [tgt, "___wrap_" + tgt[1:]]
                    if tgt.startswith("___real_"):
                        names.append("_" + tgt[8:])
                    wants = [syms[n] + A for n in names if n in syms]
                    desc = "%s+0x%x" % (tgt, A)
                    if not wants:
                        print("pcrel_unresolved=%s site=0x%x sym=%s" % (path, P, tgt))
                        unscanned += 1
                        continue
                checked += 1
                if os.environ.get("PCREL_LIST"):
                    print("pcrel_site=%s site=0x%x kind=%s from=%s want=0x%x got=0x%x"
                          % (bname, P, k, desc, wants[0], got))
                if got not in wants:
                    bad += 1
                    mismatches += 1
                    insn = P - (2 if k in ("bra.l", "bsr.l") else 4 if k != "other" else 0)
                    print("pcrel_mismatch=%s site=0x%x kind=%s object=%s from=%s "
                          "want=0x%x got=0x%x delta=%d"
                          % (bname, insn, k, path, desc, wants[0], got, got - wants[0]))
    verdict = "FAIL" if bad else ("INCOMPLETE" if unscanned else "PASS")
    print("pcrel_exact=%s binary=%s objects=%d checked=%d branches=%d ok=%d "
          "mismatches=%d unscanned=%d text_bytes=%d covered_bytes=%d verbatim_bytes=%d"
          " discarded=%d"
          % (verdict, bname, objects, checked, branches, checked - mismatches, bad,
             unscanned, text_out[1], covered, verbatim, dropped))
    sys.exit(1 if bad else (2 if unscanned else 0))


main()
