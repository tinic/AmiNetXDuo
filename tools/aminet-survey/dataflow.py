"""Resolve SocketBase by abstract interpretation, not by peephole table.

WHY THIS EXISTS.  scan.py v13 attributes 731 binaries and leaves 460 with a
bsdsocket binary it cannot account for -- 43% of the corpus that holds one.
Measured over those 460 (probe, 2026-09-09), the misses are not exotic:

  ~86 sites  the base leaves d0 into an ADDRESS REGISTER  (movea.l d0,a1/a3/a5)
  ~57 sites  the name is PUSHED to a shared open-wrapper  (pea name / bsr open)
   52 sites  a direct OpenLibrary whose store is a form the table lacks
  141 bins   no reference to the name anywhere in the code hunks at all

scan.py's own comment refuses the register forms, and it is RIGHT to refuse
them as table rows: `movea.l d0,a3` says nothing about what a3 holds forty
instructions later.  The answer is not a wider table, it is to actually follow
the value.  This module tracks a tiny lattice -- NAME (the address of the
"bsdsocket.library" string), BASE (the value OpenLibrary returned), ADDR(key)
(a known memory address) -- across a linear sweep, and drops it at exactly the
points the ABI says it dies:

  * d0/d1/a0/a1 are SCRATCH across any call (AmigaOS register convention), so
    a call clears them; a2-a6 are callee-saved and survive.
  * rts/rte/rtr and `movem.l (sp)+,regs` end the frame: every register dies.
  * any write to a register through an unmodelled instruction clears it.

So a BASE in a3 is only ever used while it demonstrably still is the base.
That is the difference between following a value and guessing one, and the
difference shows up as ~-870/-906/-996 offsets past the end of our table when
you guess (measured: samba's testprns, 53 vectors against 30).

SPDX-License-Identifier: MIT
"""
import struct

OPENLIB = -552
# exec's V1.2-compatible entry, and it is NOT a curiosity: MetalWEB, AmFTP's
# and Voyager's registration tools, and a long tail besides, open every library
# with `jsr -408(a6)`.  Modelling only -552 files all of them as "opens
# libraries, never names ours" -- a verdict about the scanner.  Same contract:
# name in a1, base out in d0.
OLDOPENLIB = -408
OPENS = (OPENLIB, OLDOPENLIB)

def u16(b, i): return struct.unpack_from('>H', b, i)[0]
def u32(b, i): return struct.unpack_from('>I', b, i)[0]
def s16(b, i): return struct.unpack_from('>h', b, i)[0]
def s8(v): return v - 256 if v > 127 else v

# ---------------------------------------------------------------- EA decoding
# mode/reg as they appear in the low six bits of most instructions.  Only the
# modes that can carry a library base or a string address are decoded; anything
# else returns kind None and the caller treats the operand as unknown, which is
# the safe direction.
def ea(code, i, mode, reg):
    """(kind, value, extension_bytes) for the EA whose extension starts at i.

    kind is 'd' (data reg), 'a' (addr reg), 'ind' ((An)), 'disp' ((d16,An)),
    'abs' ((xxx).W/.L), 'pc' ((d16,PC)), 'imm' (#imm.l), or None.
    """
    if mode == 0: return ('d', reg, 0)
    if mode == 1: return ('a', reg, 0)
    if mode == 2: return ('ind', reg, 0)
    if mode == 3: return ('inc', reg, 0)
    if mode == 4: return ('dec', reg, 0)
    if mode == 5:
        if i + 2 > len(code): return (None, None, 0)
        return ('disp', (reg, s16(code, i)), 2)
    if mode == 7:
        if reg == 0:
            # ABSOLUTE SHORT IS ITS OWN NAMESPACE, and this is not pedantry.
            # `movea.l (4).W,a6` is how every Amiga program loads ExecBase, and
            # keyed as ('abs', 4) it is indistinguishable from a base stored at
            # offset 4 of a hunk.  AmiHomeassistCLI stores SocketBase exactly
            # there, so all 8 of its exec CloseLibrary calls, plus PutMsg and
            # the mbuf block, were attributed to us: 25 vectors where the
            # program uses 5.  Same failure as -954, one addressing mode over.
            if i + 2 > len(code): return (None, None, 0)
            return ('absw', s16(code, i) & 0xFFFFFFFF, 2)
        if reg == 1:
            if i + 4 > len(code): return (None, None, 0)
            return ('abs', u32(code, i), 4)
        if reg == 2:
            if i + 2 > len(code): return (None, None, 0)
            return ('pc', i + s16(code, i), 2)
        if reg == 4:
            if i + 4 > len(code): return (None, None, 0)
            return ('imm', u32(code, i), 4)
    return (None, None, 0)

# EXTENSION LENGTH FOR EVERY MODE, so the sweep can step by instructions where
# it knows them.  It still steps 2 bytes when it does not, exactly as v13 does;
# this is used only to skip an operand that would otherwise be read as an
# opcode -- an immediate longword containing 0x4EAE is the classic way a linear
# sweep invents a library call.
def ea_len(code, i, mode, reg):
    if mode == 5: return 2
    if mode == 6: return 2
    if mode == 7:
        return {0: 2, 1: 4, 2: 2, 3: 2, 4: 4}.get(reg, 0)
    return 0


class State:
    """Registers d0-d7 (0-7) and a0-a7 (8-15), plus the memory we care about."""
    __slots__ = ('r', 'base_keys', 'name_pushed_at')

    def __init__(self):
        self.r = [None] * 16
        self.base_keys = set()
        self.name_pushed_at = None

    def clear_scratch(self):
        # AmigaOS: d0/d1/a0/a1 are scratch across a call.  a2-a6 survive, which
        # is precisely why a base parked in a3 is still the base afterwards.
        for x in (0, 1, 8, 9):
            self.r[x] = None

    def clear_all(self):
        self.r = [None] * 16


def frame_pointers(code_hunks):
    """Address registers used as FRAME POINTERS, which cannot key globals.

    `d16(a5)` is a global in the small-data model and a local variable inside a
    function that did `link a5,#-n` -- the same key, two different meanings,
    and nothing in the encoding tells them apart.  Measured: AmiHomeassistCLI's
    real base is `('abs', 4)` with five vectors, and admitting `('a5', -28)`
    and `('a2', 4)` took it to 25, adding AddRouteTagList, bpf_read and the
    mbuf block -- which are exec's CloseLibrary, PutMsg and friends reached
    through a base that was never ours.  That is the -954 mistake with a
    different offset, so the rule is measured, not assumed:

    a register that any `link` instruction targets is a frame pointer here.
    """
    # EVIDENCE, NOT A SINGLE MATCH.  This walk steps two bytes at a time over
    # everything, so an immediate operand that happens to read as 0x4E5x is
    # indistinguishable from a `link`.  A single stray word excluded a4 -- the
    # small-data base -- and took the corpus from 817 attributed binaries to
    # 665, below where v13 started.  A real frame pointer is LINKED and
    # UNLINKED, repeatedly, so both must appear at least twice.
    link, unlk = {}, {}
    for code in code_hunks:
        for i in range(0, len(code) - 1, 2):
            w = u16(code, i)
            if (w & 0xFFF8) == 0x4E50:            # link aN,#d16
                link[w & 7] = link.get(w & 7, 0) + 1
            elif (w & 0xFFF8) == 0x4E58:          # unlk aN
                unlk[w & 7] = unlk.get(w & 7, 0) + 1
    return {n for n in link if link[n] >= 2 and unlk.get(n, 0) >= 2}


def _memkey(kind, val):
    """A stable name for a memory location, matching scan.py's key shapes."""
    if kind == 'abs':  return ('abs', val)
    if kind == 'absw': return ('absw', val)
    if kind == 'disp': return ('a%d' % val[0], val[1])
    # `(An)` IS NOT A KEY, RE-MEASURED 2026-09-09 WITH DATAFLOW IN PLACE.
    # The hope was that requiring the stored value to come from a NAMED open
    # would make displacement-0 safe where v13's "any store near an open" was
    # not.  It does not: samba's `testprns` went to 63 vectors including
    # ?-870, ?-906 and ?-996, which are past the end of the table -- the same
    # signature v13 recorded at 53.  `(a5)` names whatever a5 holds in each
    # function, and two functions' structs key the same.  The change bought 29
    # binaries and was reverted; the count went up and the claims got worse.
    return None


class Ctx:
    """Everything hunk-global the walk needs: where the name is, which
    functions are open-wrappers, and how a relocated operand finds its hunk."""
    __slots__ = ('name_sites', 'name_offsets', 'target_of', 'wrappers', 'hunk',
                 'name_disps', 'openers', 'global_regs')

    def __init__(self, name_sites, name_offsets, target_of, wrappers, hunk,
                 name_disps=(), openers=(), global_regs=(4,)):
        self.name_sites = name_sites          # {(hunk_idx, offset)}
        self.name_offsets = name_offsets      # {offset} -- small-data model
        self.target_of = target_of            # operand offset -> target hunk
        self.wrappers = wrappers              # {(hunk_idx, offset)} open-wrappers
        self.hunk = hunk                      # index of the hunk being walked
        self.name_disps = set(name_disps)     # a4-relative displacements of the name
        self.openers = set(openers)           # functions that open bsdsocket themselves
        self.global_regs = set(global_regs)   # registers that key GLOBAL memory


def is_name(code, ctx, kind, val, ext_at):
    """True when this EA addresses the "bsdsocket.library" string.

    Three ways, and a binary can use all three: PC-relative (no relocation at
    all), absolute/immediate (resolved through the relocation table), and a
    displacement in the small-data model (matched against where the string sits
    in any hunk, which settles it without working out which register a4 is).
    """
    if kind == 'pc':
        return 0 <= val < len(code) - 17 and code[val:val + 17] == b'bsdsocket.library'
    if kind in ('abs', 'imm'):
        tgt = ctx.target_of.get(ext_at)
        return tgt is not None and (tgt, val) in ctx.name_sites
    if kind == 'disp' and val[0] in (4, 5):
        # Two ways, and the second is why 198 binaries reported "opens but the
        # name never reaches a1".  a4 does NOT point at the start of a hunk --
        # it points into the middle of the merged data segment -- so the
        # displacement is the string's offset MINUS a constant bias, and
        # comparing it to a hunk offset can only work when the bias is zero.
        # AEMail's string sits at an ODD offset with no relocated pointer
        # anywhere: it is inside a string pool, reachable only this way.
        # The bias is derived from the binary (see derive_bias), never assumed.
        return val[1] in ctx.name_offsets or val[1] in ctx.name_disps
    return False


def derive_bias(hunks, code_hunks, our_offsets):
    """The small-data bias, voted for by every library this binary opens.

    Take each a4/a5-relative name load before an OpenLibrary, subtract its
    displacement from every ".library" string offset in the file, and the bias
    is the difference that RECURS.  The winner has to explain most of the
    naming sites, not just ours -- AEMail's 32768 resolves 12 of 12 sites to
    nine real libraries (bsdsocket, dos, iffparse, intuition, graphics,
    layers, gadtools, icon, asl).  A bias that explains one site explains
    nothing, so a lone vote is refused: that is the difference between deriving
    the bias and choosing the one that would have been convenient.
    """
    import collections
    libs = []
    for _idx, _t, _o, pay, _r in hunks:
        m = pay.find(b'.library')
        while m != -1:
            st = m
            while st > 0 and 32 <= pay[st - 1] < 127:
                st -= 1
            libs.append((st, bytes(pay[st:m + 8])))
            m = pay.find(b'.library', m + 1)
    if not libs:
        return None, set()
    sites = []
    for code in code_hunks:
        for i in range(0, len(code) - 3, 2):
            if u16(code, i) != 0x4EAE or s16(code, i + 2) not in OPENS:
                continue
            for j in range(max(0, i - 40), i, 2):
                if u16(code, j) in (0x43EC, 0x43ED, 0x226C, 0x226D):
                    sites.append(s16(code, j + 2))
                    break
    if not sites:
        return None, set()
    votes = collections.Counter()
    for d in sites:
        for off, _nm in libs:
            votes[off - d] += 1
    bias, n = votes.most_common(1)[0]
    resolved = sum(1 for d in sites if any(off - d == bias for off, _ in libs))
    if n < 2 or resolved * 2 < len(sites):
        return None, set()
    return bias, {off - bias for off in our_offsets}


NAME, BASE = 'NAME', 'BASE'

def _call_target(code, i, w, ctx):
    """(hunk, offset) a jsr/bsr reaches, or None when it is not a direct call."""
    if (w & 0xFF00) == 0x6100:                       # bsr
        d = s8(w & 0xFF)
        if d == 0:
            return (ctx.hunk, i + 2 + s16(code, i + 2)) if i + 4 <= len(code) else None
        if d == -1:
            return (ctx.hunk, i + 2 + struct.unpack_from('>i', code, i + 2)[0]) if i + 6 <= len(code) else None
        return (ctx.hunk, i + 2 + d)
    if (w & 0xFFC0) == 0x4E80:                       # jsr <ea>
        mode, reg = (w >> 3) & 7, w & 7
        if mode == 7 and reg == 2 and i + 4 <= len(code):        # d16(PC)
            return (ctx.hunk, i + 2 + s16(code, i + 2))
        if mode == 7 and reg == 1 and i + 6 <= len(code):        # abs.l
            tgt = ctx.target_of.get(i + 2)
            if tgt is not None:
                return (tgt, u32(code, i + 2))
    return None


def follow_thunks(hunks_code, ctxs, h, off, hops=4):
    """Step through linker far-jump stubs to the real entry point.

    SAS/C and blink emit tables of `jmp (xxx).L` islands so a 16-bit bsr can
    reach anything, and EVERY call in such a binary lands in the table first.
    Testing the island for an OpenLibrary finds a jump and nothing else, which
    is why AMarqueed's `pea name / bsr open_helper` read as an unnamed open:
    the helper was two instructions further on.  Returns the chain, so the
    caller can accept the island as well as the destination.
    """
    seen = [(h, off)]
    for _ in range(hops):
        code = hunks_code.get(h)
        if code is None or not (0 <= off < len(code) - 3):
            break
        w = u16(code, off)
        if w == 0x4EF9 and off + 6 <= len(code):        # jmp (xxx).L
            tgt = ctxs[h].target_of.get(off + 2)
            if tgt is None:
                break
            h, off = tgt, u32(code, off + 2)
        elif w == 0x4EFA and off + 4 <= len(code):      # jmp d16(PC)
            off = off + 2 + s16(code, off + 2)
        else:
            break
        if (h, off) in seen:
            break
        seen.append((h, off))
    return seen


def named_open_regions(code, ctx, limit=4096):
    """(start, end) of every function that opens bsdsocket by name.

    Bounded by the rts before and the rts after, NOT by a scan forward from a
    presumed entry: SAS/C parks inline string constants inside the function
    (AWeb has "bsdsocket.library" sitting between the entry and the prologue),
    and a function with an early return ends its first scan long before the
    open.  Both read as "this function does not open the library".
    """
    regions = []
    for i in range(0, len(code) - 3, 2):
        if u16(code, i) != 0x4EAE or s16(code, i + 2) not in OPENS:
            continue
        named = False
        for k in range(max(0, i - 40), i, 2):
            w2 = u16(code, k)
            if (w2 & 0xF1C0) == 0x41C0 or (w2 & 0xF1C0) == 0x2040:
                kind, val, _ln = ea(code, k + 2, (w2 >> 3) & 7, w2 & 7)
                if is_name(code, ctx, kind, val, k + 2):
                    named = True
                    break
        if not named:
            continue
        lo = 0
        for k in range(i - 2, max(0, i - limit), -2):
            if u16(code, k) in (0x4E75, 0x4E73, 0x4E77):
                lo = k + 2
                break
        hi = len(code)
        for k in range(i + 4, min(len(code) - 1, i + limit), 2):
            if u16(code, k) in (0x4E75, 0x4E73, 0x4E77):
                hi = k
                break
        regions.append((lo, hi))
    return regions


def find_wrappers(hunks_code, ctxs, limit=4096):
    """Two sets: functions that call OpenLibrary at all (`wrappers`), and
    functions that open OUR library by name (`openers`).

    A caller that pushes "bsdsocket.library" into a wrapper has opened our
    library; so has a caller that simply calls an opener and stores d0, which
    is the shape AWeb and 100-odd others use -- the name is loaded inside the
    callee and the caller never mentions it.
    """
    targets = set()
    for idx, code in hunks_code.items():
        ctx = ctxs[idx]
        i = 0
        while i < len(code) - 3:
            t = _call_target(code, i, u16(code, i), ctx)
            if t is not None:
                targets.add(t)
            i += 2

    regions = {idx: named_open_regions(code, ctxs[idx], limit)
               for idx, code in hunks_code.items()}

    wrappers, openers = set(), set()
    for t in targets:
        chain = follow_thunks(hunks_code, ctxs, t[0], t[1])
        h, off = chain[-1]
        code = hunks_code.get(h)
        if code is None or not (0 <= off < len(code) - 3):
            continue
        if any(lo <= off < hi for lo, hi in regions.get(h, ())):
            openers.update(chain)
            wrappers.update(chain)
            continue
        j, end = off, min(len(code) - 3, off + limit)
        while j < end:
            w = u16(code, j)
            if w in (0x4E75, 0x4E73, 0x4E77):
                break
            if w == 0x4EAE and s16(code, j + 2) == OPENLIB:
                wrappers.update(chain)          # the island counts too
                break
            j += 2
    return wrappers, openers


def walk(code, ctx, base_keys, collect_calls=True):
    """One linear pass.  Returns (base_keys, [(site, lvo)], opens, named_opens).

    Hits carry their call SITE, not just the displacement: v13's peephole runs
    alongside this walk and the two find the same call from opposite ends, so a
    union keyed on the offset alone counts every shared call twice.  A fixture
    caught exactly that -- 6 calls where the program makes 3.

    Run it twice: the first pass discovers where the base is stored, the second
    uses those keys, because a store can sit textually after the calls it
    enables (net.lib opens the library in its own object and the program calls
    it from another -- AmFinger has three CODE hunks and that is why).
    """
    st = State()
    st.base_keys = set(base_keys)
    hits, opens, named = [], 0, 0
    found = set()

    def key_at(kind, v, ext_at):
        """Memory key, with an absolute operand qualified by its RELOCATION.

        A relocated longword is an offset into some hunk; an unrelocated one is
        a real address.  Both look like `('abs', 4)`, and that is how ExecBase
        loaded as `(4).L` came to share a key with a SocketBase stored at hunk
        offset 4 -- AmiHomeassistCLI's exec calls read as bpf_close/bpf_write.
        The relocation table is the only thing that tells them apart, and it is
        already parsed.
        """
        if kind == 'abs':
            tgt = ctx.target_of.get(ext_at)
            return ('h%d' % tgt, v) if tgt is not None else ('abs', v)
        return _memkey(kind, v)

    def usable(kind, v):
        # An absolute address is the same location everywhere.  A displacement
        # off a register is only a global if that register holds one value for
        # the whole program -- the small-data base does, a frame or struct
        # pointer does not.
        return kind != 'disp' or v[0] in ctx.global_regs

    def val_of(kind, v, ext_at, allow_name):
        if kind == 'd':  return st.r[v]
        if kind == 'a':  return st.r[8 + v]
        if kind in ('abs', 'absw', 'disp'):
            if allow_name and is_name(code, ctx, kind, v, ext_at):
                return NAME
            if not usable(kind, v):
                return None
            k = key_at(kind, v, ext_at)
            return BASE if k in st.base_keys else None
        if kind == 'imm':
            return NAME if is_name(code, ctx, kind, v, ext_at) else None
        return None

    def store(kind, v, value, ext_at=None):
        if value is not BASE:
            return
        if kind in ('abs', 'absw', 'disp'):
            if not usable(kind, v):
                return
            k = key_at(kind, v, ext_at)
            if k is not None:
                st.base_keys.add(k); found.add(k)
        elif kind == 'ind':
            # move.l d0,(a3) with a3 from `lea SocketBase,a3`.  A bare (An) is
            # NOT a usable key -- scan.py measured that and reverted it -- but
            # an An whose value is KNOWN names a real address, and then it is
            # the same fact as an absolute store.
            held = st.r[8 + v]
            if isinstance(held, tuple) and held[0] == 'ADDR':
                st.base_keys.add(held[1]); found.add(held[1])

    i = 0
    n = len(code)
    while i < n - 1:
        w = u16(code, i)
        step = 2

        if w in (0x4E75, 0x4E73, 0x4E77):            # rts / rte / rtr
            st.clear_all(); st.name_pushed_at = None
            i += 2; continue
        if (w & 0xFFC0) == 0x4CC0:                   # movem.l <ea>,regs
            st.clear_all()
            i += 2; continue

        # ---- MOVE.L / MOVEA.L ------------------------------------------------
        if (w & 0xF000) == 0x2000:
            dreg, dmode = (w >> 9) & 7, (w >> 6) & 7
            smode, sreg = (w >> 3) & 7, w & 7
            sk, sv, slen = ea(code, i + 2, smode, sreg)
            if i + 2 + slen <= n:
                value = val_of(sk, sv, i + 2, allow_name=(dmode == 1 or smode == 7))
                dk, dv, dlen = ea(code, i + 2 + slen, dmode, dreg)
                if dmode in (0, 1):
                    st.r[(8 if dmode == 1 else 0) + dreg] = value
                elif dmode == 4 and dreg == 7:       # move.l x,-(sp) : a push
                    if value is NAME:
                        st.name_pushed_at = i
                elif dk in ('abs', 'absw', 'disp', 'ind'):
                    store(dk, dv, value, i + 2 + slen)
                step = 2 + slen + dlen
            i += step; continue

        # ---- LEA -------------------------------------------------------------
        if (w & 0xF1C0) == 0x41C0:
            dreg = (w >> 9) & 7
            k, v, ln = ea(code, i + 2, (w >> 3) & 7, w & 7)
            if is_name(code, ctx, k, v, i + 2):
                st.r[8 + dreg] = NAME
            elif k in ('abs', 'absw', 'disp'):
                mk = _memkey(k, v)
                st.r[8 + dreg] = ('ADDR', mk) if mk else None
            else:
                st.r[8 + dreg] = None
            i += 2 + ln; continue

        # ---- PEA -------------------------------------------------------------
        if (w & 0xFFC0) == 0x4840:
            k, v, ln = ea(code, i + 2, (w >> 3) & 7, w & 7)
            if is_name(code, ctx, k, v, i + 2):
                st.name_pushed_at = i
            i += 2 + ln; continue

        # ---- library call through a6 ----------------------------------------
        if w in (0x4EAE, 0x4EEE) and i + 4 <= n:
            d = s16(code, i + 2)
            if d in OPENS:
                opens += 1
                is_ours = st.r[9] is NAME or (
                    st.name_pushed_at is not None and i - st.name_pushed_at <= 40)
                st.clear_scratch()
                st.name_pushed_at = None
                if is_ours:
                    named += 1
                    st.r[0] = BASE                   # d0 = SocketBase
            else:
                if collect_calls and st.r[14] is BASE:
                    hits.append((i, d))          # site, so a union dedupes
                st.clear_scratch()
            i += 4; continue

        # ---- direct call: a wrapper counts as an OpenLibrary ------------------
        tgt = _call_target(code, i, w, ctx)
        if tgt is not None:
            named_here = (st.name_pushed_at is not None and i - st.name_pushed_at <= 40) \
                or any(st.r[8 + x] is NAME for x in range(8))
            wrapper = tgt in ctx.wrappers
            opener = tgt in ctx.openers
            st.clear_scratch()
            st.name_pushed_at = None
            if opener:
                opens += 1; named += 1
                st.r[0] = BASE
            elif wrapper and named_here:
                opens += 1; named += 1
                st.r[0] = BASE
            if (w & 0xFF00) == 0x6100:
                d8 = w & 0xFF
                i += 4 if d8 == 0 else (6 if d8 == 0xFF else 2)
            else:
                _k, _v, ln = ea(code, i + 2, (w >> 3) & 7, w & 7)
                i += 2 + ln
            continue

        i += 2

    return found, hits, opens, named
