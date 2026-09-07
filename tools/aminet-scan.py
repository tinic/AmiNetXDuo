#!/usr/bin/env python3
"""
Which bsdsocket.library vectors an Amiga binary really calls.

    tools/aminet-scan.py <file|dir> [...]

WHY THIS IS IN THE REPO.  Five separate passes over Aminet each rebuilt a HUNK
parser and a SocketBase resolver from scratch in /tmp, and every one of those
is now gone.  The method is settled; only the implementation kept evaporating.

WHY NOT JUST SCAN FOR 4E AE.  `jsr d16(a6)` is how EVERY AmigaOS library call
compiles, so dos, exec, intuition, gadtools and muimaster all land in the same
displacement space.  Measured against hand-verified truth, the naive scan
over-counts distinct vectors by 13x to 95x: AmiFTP 1.843 calls 18 and the naive
scan claims 89; AWeb 3.4 calls 12 and it claims 125.  Concretely, exec
PutMsg/GetMsg/ReplyMsg/WaitPort at -366..-384 ARE the phantom bpf_* calls that
made three surveys report "minimal BREAKS", and gadtools SetGadgetAttrsA(-42)
aliases listen 28 times in one binary.

So a call counts only when a6 was loaded from the address OpenLibrary() stored
the bsdsocket base into.  That address is found, not assumed.

WHAT IT CANNOT SEE, stated because a silent zero reads like a clean result:
  packed      a cruncher hides the code; reported, never counted as clean
  ixemul      gcc/ixemul ports make no bsdsocket call of their own, the whole
              contract is inside ixemul.library
  vtable      a program dispatching through its own JMP table (AWeb builds an
              18-slot one in DATA) has displacements that are not LVOs
  AS225       socket.library has a DIFFERENT LVO table, and AveHTTPD keeps
              both bases in ONE variable behind a mode flag

SOCK_RAW is not a vector at all -- it is an argument to socket() -- so the type
argument is decoded separately at each call site.

WHERE IT STANDS, against hand-verified truth:

  AmiFTP 1.843   18 vectors real, this reports 16, ZERO false positives.
                 connect and WaitSelect are missed: their sites hold a6 from
                 an earlier load rather than reloading it, which needs a
                 register-liveness pass this does not have.
  AWeb 3.4       reports `unresolved`, which is right rather than unlucky --
                 AWeb reaches TCP through an 18-slot JMP table it builds in
                 DATA, so its d16(a6) displacements are indices into its own
                 table and are not LVOs at all.

Under-reporting is the deliberate bias.  A missed call costs a survey some
coverage; an invented one cost three surveys their conclusion, and every
`minimal BREAKS` verdict produced before attribution was a phantom.

SPDX-License-Identifier: MIT
"""

import os
import struct
import sys

HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_RELOC32SHORT = 0x3EC, 0x3F7
HUNK_SYMBOL, HUNK_DEBUG, HUNK_END = 0x3F0, 0x3F1, 0x3F2
HUNK_HEADER = 0x3F3

LVO_OPENLIBRARY = -552


def _u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def _s16(b, o):
    return struct.unpack_from(">h", b, o)[0]


class Hunks:
    """CODE hunks only, with RELOC32 applied so absolute operands are real."""

    HUNK_SPAN = 0x100000    # synthetic address space, one megabyte per hunk

    def __init__(self, blob):
        self.hunks = []         # [index, kind, bytearray]
        self.relocs = []        # (hunk, offset, target_hunk)
        self.ok = False
        self.packed = False
        self._parse(blob)
        self._relocate()
        self.code = [(i, bytes(d)) for i, k, d in self.hunks if k == HUNK_CODE]

    def base(self, idx):
        return idx * self.HUNK_SPAN

    def _relocate(self):
        """Apply RELOC32 so an absolute operand is an address we can compare.

        Without this there is no way to tell which OpenLibrary() call names
        bsdsocket.library, and picking the base by "most used" instead picks
        intuition in any program with a window."""
        by_idx = {i: d for i, _k, d in self.hunks}
        for hidx, off, target in self.relocs:
            d = by_idx.get(hidx)
            if d is None or off + 4 > len(d):
                continue
            v = struct.unpack_from(">I", d, off)[0]
            struct.pack_into(">I", d, off, (v + self.base(target)) & 0xFFFFFFFF)

    def find_bytes(self, needle):
        """[(hunk, offset, address)] for every occurrence."""
        out = []
        for i, _k, d in self.hunks:
            start = 0
            while True:
                j = bytes(d).find(needle, start)
                if j < 0:
                    break
                out.append((i, j, self.base(i) + j))
                start = j + 1
        return out

    def _parse(self, b):
        if len(b) < 8 or _u32(b, 0) != HUNK_HEADER:
            return
        o = 4
        while o + 4 <= len(b) and _u32(b, o):     # library names, normally none
            o += 4 + _u32(b, o) * 4
        o += 4
        if o + 12 > len(b):
            return
        n = _u32(b, o)
        o += 12 + n * 4                            # count, first, last, sizes
        idx = 0
        total_code = 0
        while o + 4 <= len(b):
            t = _u32(b, o) & 0x3FFFFFFF
            o += 4
            if t in (HUNK_CODE, HUNK_DATA):
                if o + 4 > len(b):
                    break
                ln = _u32(b, o) * 4
                o += 4
                self.hunks.append([idx, t, bytearray(b[o:o + ln])])
                if t == HUNK_CODE:
                    total_code += ln
                o += ln
            elif t == HUNK_BSS:
                self.hunks.append([idx, HUNK_BSS, bytearray()])
                o += 4
            elif t == HUNK_RELOC32:
                while o + 4 <= len(b):
                    cnt = _u32(b, o)
                    o += 4
                    if cnt == 0:
                        break
                    tgt = _u32(b, o)
                    o += 4
                    for k in range(cnt):
                        if o + 4 > len(b):
                            break
                        self.relocs.append((idx, _u32(b, o), tgt))
                        o += 4
            elif t == HUNK_RELOC32SHORT:
                if o + 4 > len(b):
                    break
                while o + 4 <= len(b):
                    cnt = struct.unpack_from(">H", b, o)[0]
                    o += 2
                    if cnt == 0:
                        break
                    nxt = struct.unpack_from(">H", b, o)[0]
                    o += 2
                    o += cnt * 2
                    if o % 4:
                        o += 2
                    if nxt == 0:
                        break
                o = (o + 3) & ~3
            elif t in (HUNK_SYMBOL, HUNK_DEBUG):
                if t == HUNK_SYMBOL:
                    while o + 4 <= len(b):
                        ln = _u32(b, o)
                        o += 4
                        if ln == 0:
                            break
                        o += ln * 4 + 4
                else:
                    if o + 4 > len(b):
                        break
                    o += 4 + _u32(b, o) * 4
            elif t == HUNK_END:
                idx += 1
            else:
                break
        self.ok = any(k == HUNK_CODE for _i, k, _d in self.hunks)
        # A cruncher leaves a tiny stub and hides the payload; total code far
        # below the file size is the signature.  AmIRC 3.5 is 232 KB of file
        # and 80 bytes of CODE.
        if self.ok and total_code * 8 < len(b) and total_code < 4096:
            self.packed = True


def _branch_targets(code):
    """
    Every offset something can branch to.

    a6 is tracked forward through straight-line code, so tracking must STOP at
    any label: a branch arriving there may carry a different a6, and assuming
    otherwise is how a scan invents calls.  Bcc/BRA/BSR with 8- and 16-bit
    displacements plus DBcc cover what these compilers emit.
    """
    t = set()
    i = 0
    n = len(code)
    while i + 2 <= n:
        op = struct.unpack_from(">H", code, i)[0]
        hi = op >> 12
        if hi == 0x6:                              # Bcc / BRA / BSR
            d8 = op & 0xFF
            if d8 == 0 and i + 4 <= n:
                t.add(i + 2 + _s16(code, i + 2))
                i += 4
                continue
            if d8 == 0xFF and i + 6 <= n:          # 32-bit displacement
                t.add(i + 2 + struct.unpack_from(">i", code, i + 2)[0])
                i += 6
                continue
            t.add(i + 2 + (d8 - 256 if d8 > 127 else d8))
            i += 2
            continue
        if (op & 0xF0F8) == 0x50C8 and i + 4 <= n:  # DBcc
            t.add(i + 2 + _s16(code, i + 2))
            i += 4
            continue
        i += 2
    return t


# movea.l <ea>,a6 is 0x2C40..0x2C7F over the whole addressing-mode field.  Only
# the four forms _a6_loads() decodes are followed; EVERY other one has to clear
# the tracked base, and `movea.l 4.w,a6` (0x2C78) is the one that matters:
# fetching SysBase from absolute 4 is how every Amiga program reaches exec, and
# leaving it out let exec calls inherit the socket base.  That put
# AddRouteTagList -- displacement -414, which is exec CloseLibrary -- into an
# email client's vector list, four times.
A6_LOAD_FOLLOWED = (0x2C79, 0x2C6C, 0x2C6D, 0x2C7A)


def _clobbers_a6(op):
    """True when this word writes a6 with something this scanner cannot follow."""
    if 0x2C40 <= op <= 0x2C7F:
        return op not in A6_LOAD_FOLLOWED
    if op in (0x4CDF, 0x4CD7, 0x4CEE, 0x4CE7):   # movem.l restore forms
        return True
    return False


def _a6_loads(code):
    """offset -> a6 source key, for the forms these binaries actually use."""
    out = {}
    i = 0
    n = len(code)
    while i + 2 <= n:
        op = struct.unpack_from(">H", code, i)[0]
        if op == 0x2C79 and i + 6 <= n:           # movea.l (abs).L,a6
            out[i + 6] = ("abs", _u32(code, i + 2))
            i += 6
            continue
        if op in (0x2C6C, 0x2C6D) and i + 4 <= n:  # movea.l d16(a4/a5),a6
            reg = "a4" if op == 0x2C6C else "a5"
            out[i + 4] = (reg, _s16(code, i + 2))
            i += 4
            continue
        if op == 0x2C7A and i + 4 <= n:            # movea.l d16(pc),a6
            out[i + 4] = ("pc", i + 2 + _s16(code, i + 2))
            i += 4
            continue
        i += 2
    return out


def _d0_stores(code):
    """offset of the instruction after `move.l d0,<x>` -> the same key shape."""
    out = {}
    i = 0
    n = len(code)
    while i + 2 <= n:
        op = struct.unpack_from(">H", code, i)[0]
        if op == 0x23C0 and i + 6 <= n:            # move.l d0,(abs).L
            out[i] = ("abs", _u32(code, i + 2))
            i += 6
            continue
        if op in (0x2940, 0x2B40) and i + 4 <= n:  # move.l d0,d16(a4/a5)
            reg = "a4" if op == 0x2940 else "a5"
            out[i] = (reg, _s16(code, i + 2))
            i += 4
            continue
        i += 2
    return out


def _live_a6(code, loads):
    """
    Carry a6 forward from where it was loaded to where it is used.

    A compiler loads the base once and makes several calls on it; requiring the
    load to sit immediately before each call found 16 of AmiFTP's 18 vectors
    and missed connect and WaitSelect for that reason alone.  Propagation stops
    at a branch target or an instruction that writes a6 some other way, so a
    stale value cannot follow control flow into another context.
    """
    labels = _branch_targets(code)
    live = dict(loads)
    cur = None
    i = 0
    n = len(code)
    while i + 2 <= n:
        if i in labels:
            cur = None
        if i in loads:
            cur = loads[i]
        elif i + 2 <= n and _clobbers_a6(struct.unpack_from(">H", code, i)[0]):
            cur = None
        if cur is not None and i not in live:
            live[i] = cur
        i += 2
    return live


def _calls(code):
    """offset -> displacement, for jsr/jmp d16(a6)."""
    out = {}
    i = 0
    n = len(code)
    while i + 4 <= n:
        op = struct.unpack_from(">H", code, i)[0]
        if op in (0x4EAE, 0x4EEE):
            out[i] = _s16(code, i + 2)
            i += 4
            continue
        i += 2
    return out


def _a1_name_loads(code, names):
    """offset after the load -> True when a1 was pointed at one of `names`.

    lea d16(pc),a1 / lea (abs).L,a1 / movea.l #abs,a1 are the three forms
    these binaries use to hand OpenLibrary() its argument.
    """
    out = {}
    i = 0
    n = len(code)
    while i + 4 <= n:
        op = struct.unpack_from(">H", code, i)[0]
        if op == 0x43FA:                            # lea d16(pc),a1
            tgt = i + 2 + _s16(code, i + 2)
            out[i + 4] = ("pcrel", tgt)
            i += 4
            continue
        if op in (0x43F9, 0x227C) and i + 6 <= n:   # lea (abs).L,a1 / movea.l #
            out[i + 6] = ("abs", _u32(code, i + 2))
            i += 6
            continue
        if op in (0x43EC, 0x43ED) and i + 4 <= n:   # lea d16(a4/a5),a1
            reg = "a4" if op == 0x43EC else "a5"
            out[i + 4] = (reg, _s16(code, i + 2))
            i += 4
            continue
        i += 2
    return out


def calibrate_small_data(hunks, code, name_addrs):
    """
    What a4 (or a5) holds, for compilers that address data through it.

    SAS/C and DICE put the data hunk in a4 -- DICE at DataHunk+0x7FFE, not at
    DataHunk -- and a wrong guess shifts every name reference, which is how a
    binary with a perfectly findable OpenLibrary() call reads as unresolved.
    Rather than assume a convention, each candidate is tried and the one that
    lands a `lea d16(a4),a1` on a real library name wins.
    """
    cands = []
    for i, k, d in hunks.hunks:
        if k in (HUNK_DATA, HUNK_BSS):
            for adj in (0, 0x7FFE, 0x8000):
                cands.append(hunks.base(i) + adj)
    a1 = _a1_name_loads(code, name_addrs)
    best, best_hits = {}, {}
    for reg in ("a4", "a5"):
        for c in cands:
            hits = 0
            for _off, (kind, val) in a1.items():
                if kind == reg and (c + val) in name_addrs:
                    hits += 1
            if hits > best_hits.get(reg, 0):
                best_hits[reg] = hits
                best[reg] = c
    return best


def socket_base_of(hunks, hidx, code, name_addrs, code_base, small=None):
    """
    The variable OpenLibrary("bsdsocket.library") stored its result into.

    PROVEN, NOT GUESSED.  Each OpenLibrary() call is matched to the string a1
    was pointed at; only a call naming bsdsocket.library counts.  An earlier
    version picked "the base used by the most a6 loads" instead and chose
    intuition.library in anything with a window -- AmiFTP came out at 21
    vectors against a hand-verified 18, with bpf_close and NetStackQuery among
    them, which are the phantoms this whole approach exists to remove.

    Returns a key, or None when it cannot be proven.  None is a refusal, not
    an empty result: the caller reports `unresolved` rather than a guess.
    """
    a1 = _a1_name_loads(code, name_addrs)
    calls = _calls(code)
    stores = _d0_stores(code)
    for off, disp in sorted(calls.items()):
        if disp != LVO_OPENLIBRARY:
            continue
        src = a1.get(off)
        if src is None:
            # a1 may have been set further back; walk a short way
            for probe in range(off - 2, max(0, off - 24), -2):
                if probe + 4 in a1 or probe + 6 in a1:
                    src = a1.get(probe + 4) or a1.get(probe + 6)
                    break
        if src is None:
            continue
        kind, val = src
        if kind == "pcrel":
            addr = code_base + val
        elif kind in ("a4", "a5"):
            if not small or kind not in small:
                continue
            addr = small[kind] + val
        else:
            addr = val
        if addr not in name_addrs:
            continue
        for probe in range(off + 4, min(off + 40, len(code)), 2):
            if probe in stores:
                return stores[probe]
    return None


def scan(path, lvomap):
    blob = open(path, "rb").read()
    h = Hunks(blob)
    if not h.ok:
        return None
    if h.packed:
        return {"verdict": "packed", "lvos": {}, "raw": 0, "sockraw": 0}
    if b"ixemul.library" in blob:
        return {"verdict": "ixemul", "lvos": {}, "raw": 0, "sockraw": 0}
    if b"bsdsocket.library" not in blob:
        return {"verdict": "no-bsdsocket", "lvos": {}, "raw": 0, "sockraw": 0}

    # every address the name sits at, so an OpenLibrary call can be matched
    name_addrs = {a for _i, _o, a in h.find_bytes(b"bsdsocket.library\x00")}
    if not name_addrs:
        name_addrs = {a for _i, _o, a in h.find_bytes(b"bsdsocket.library")}

    named, raw_total, sockraw, proven = {}, 0, 0, False
    for idx, code in h.code:
        calls = _calls(code)
        raw_total += len(calls)
        small = calibrate_small_data(h, code, name_addrs)
        base = socket_base_of(h, idx, code, name_addrs, h.base(idx), small)
        if base is None:
            continue
        proven = True
        loads = _live_a6(code, _a6_loads(code))
        for off, disp in calls.items():
            if loads.get(off) != base:
                continue
            api = lvomap.get(disp)
            if api:
                named[api] = named.get(api, 0) + 1
        sockraw += _sock_raw_sites(code, calls, loads, base, lvomap)
    if not proven:
        # The base could not be proven from a name.  Saying so is the point;
        # a guess here is what produced phantom bpf_* calls in three surveys.
        return {"verdict": "unresolved", "lvos": {}, "raw": raw_total,
                "sockraw": 0}
    return {
        "verdict": "attributed" if named else "no-attributed-call",
        "lvos": named,
        "raw": raw_total,
        "sockraw": sockraw,
    }


def _sock_raw_sites(code, calls, loads, base, lvomap):
    """
    socket(domain, type, protocol) with type == SOCK_RAW(3).

    Not a vector, so no displacement can carry it, and micro removes raw
    sockets: AvePING breaks and an LVO-only scan calls it clean.  The three
    arguments are constant immediates at every call site examined.
    """
    hits = 0
    for off, disp in calls.items():
        if lvomap.get(disp) != "socket" or loads.get(off) != base:
            continue
        window = code[max(0, off - 40):off]
        # pea #n  (4878 000n) and moveq #n,dN (7n00 in the low byte)
        peas = []
        i = 0
        while i + 4 <= len(window):
            if struct.unpack_from(">H", window, i)[0] == 0x4878:
                peas.append(struct.unpack_from(">H", window, i + 2)[0])
                i += 4
                continue
            i += 2
        # arguments are pushed right to left, so type is the middle of three
        if len(peas) >= 3 and peas[-2] == 3:
            hits += 1
        elif b"\x70\x03" in window and b"\x2f" in window:
            hits += 1
    return hits


def load_lvomap(root):
    """displacement -> api name, from the checked-in profile matrix."""
    p = os.path.join(root, "tests/profiles/lvo-matrix.tsv")
    out = {}
    with open(p) as fh:
        for line in fh:
            f = line.rstrip("\n").split("\t")
            if len(f) < 4 or f[0] == "offset":
                continue
            try:
                out[int(f[0], 16)] = f[2]
            except ValueError:
                pass
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    lvomap = load_lvomap(root)
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip())
        return 2
    paths = []
    for a in args:
        if os.path.isdir(a):
            for d, _, fs in os.walk(a):
                paths += [os.path.join(d, f) for f in fs]
        else:
            paths.append(a)
    for p in sorted(paths):
        try:
            r = scan(p, lvomap)
        except Exception as e:                     # a corpus file, not our code
            print(f"{os.path.basename(p)}\terror\t{e}")
            continue
        if r is None:
            continue
        apis = ",".join(sorted(r["lvos"])) or "-"
        print(f"{os.path.basename(p)}\t{r['verdict']}\tlvos={len(r['lvos'])}"
              f"\traw_jsr_a6={r['raw']}\tsock_raw={r['sockraw']}\t{apis}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
