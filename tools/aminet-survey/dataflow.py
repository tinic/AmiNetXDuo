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
(a known memory address) -- plus function-local stack and indirect-object
facts across a linear sweep, and drops each fact where its lifetime ends:

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

    kind is 'd' (data reg), 'a' (addr reg), 'ind' ((An)), 'disp'
    ((d16,An), or a stable 68020 full-extension displacement), 'abs'
    ((xxx).W/.L), 'pc' ((d16,PC)), 'imm' (#imm.l), or None.
    """
    if mode == 0: return ('d', reg, 0)
    if mode == 1: return ('a', reg, 0)
    if mode == 2: return ('ind', reg, 0)
    if mode == 3: return ('inc', reg, 0)
    if mode == 4: return ('dec', reg, 0)
    if mode == 5:
        if i + 2 > len(code): return (None, None, 0)
        return ('disp', (reg, s16(code, i)), 2)
    if mode == 6:
        if i + 2 > len(code): return (None, None, 0)
        ext = u16(code, i)
        if not (ext & 0x0100):                    # brief: live index register
            return (None, None, 2)
        # 68020 full extension.  Only the non-indirect form with the index
        # SUPPRESSED denotes one stable cell relative to An.  Samba uses
        # exactly 0x0170 + bd.l: a4 is present, Dn is suppressed, the base
        # displacement is long, and there is no memory indirection.
        base_suppressed = bool(ext & 0x0080)
        index_suppressed = bool(ext & 0x0040)
        bd_size = (ext >> 4) & 3
        indirect = ext & 7
        ln = {1: 2, 2: 4, 3: 6}.get(bd_size, 2)
        if i + ln > len(code): return (None, None, 0)
        if base_suppressed or not index_suppressed or indirect != 0:
            return (None, None, ln)
        if bd_size == 1:                         # null base displacement
            disp = 0
        elif bd_size == 2:
            disp = s16(code, i + 2)
        elif bd_size == 3:
            disp = struct.unpack_from('>i', code, i + 2)[0]
        else:                                    # reserved encoding
            return (None, None, ln)
        return ('disp', (reg, disp), ln)
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
def ea_len(code, i, mode, reg, size=4):
    if mode == 5: return 2
    if mode == 6:
        if i + 2 > len(code): return 0
        ext = u16(code, i)
        if not (ext & 0x0100): return 2
        return {1: 2, 2: 4, 3: 6}.get((ext >> 4) & 3, 2)
    if mode == 7:
        return {0: 2, 1: 4, 2: 2, 3: 2,
                4: 4 if size == 4 else 2}.get(reg, 0)
    return 0


class State:
    """Registers d0-d7 (0-7) and a0-a7 (8-15), plus the memory we care about."""
    __slots__ = ('r', 'base_keys', 'local_base', 'indirect_base',
                 'name_pushed_at')

    def __init__(self):
        self.r = [None] * 16
        self.base_keys = set()
        # Stack displacements are meaningful only inside one function.  They
        # must never enter base_keys: 4(sp) in two functions is two unrelated
        # locations, which was the source of the old cross-function matches.
        self.local_base = set()
        # A bare (An) has no program-global identity, but it is safe while the
        # same, unmodified register remains live in one function.  This covers
        # heap/object fields at offset zero without ever equating (a5) in two
        # functions (the false-positive shape that the old table admitted).
        self.indirect_base = set()
        self.name_pushed_at = None

    def clear_scratch(self):
        # AmigaOS: d0/d1/a0/a1 are scratch across a call.  a2-a6 survive, which
        # is precisely why a base parked in a3 is still the base afterwards.
        for x in (0, 1, 8, 9):
            self.r[x] = None
        self.indirect_base.discard(0)
        self.indirect_base.discard(1)

    def clear_all(self):
        self.r = [None] * 16
        self.local_base.clear()
        self.indirect_base.clear()
        self.name_pushed_at = None


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
                 'name_disps', 'name_pointer_keys', 'openers',
                 'object_openers', 'global_regs')

    def __init__(self, name_sites, name_offsets, target_of, wrappers, hunk,
                 name_disps=(), name_pointer_keys=(), openers=(),
                 object_openers=(), global_regs=(4,)):
        self.name_sites = name_sites          # {(hunk_idx, offset)}
        self.name_offsets = name_offsets      # {offset} -- small-data model
        self.target_of = target_of            # operand offset -> target hunk
        self.wrappers = wrappers              # {(hunk_idx, offset)} open-wrappers
        self.hunk = hunk                      # index of the hunk being walked
        self.name_disps = set(name_disps)     # a4-relative displacements of the name
        # Stable memory cells whose relocated initial value is the address of
        # the name.  This is the common library-table form: MOVEA.L cell,A1,
        # not LEA name,A1.  Relocations prove the pointee; adjacency does not.
        self.name_pointer_keys = set(name_pointer_keys)
        self.openers = set(openers)           # functions that open bsdsocket themselves
        # Functions proven to return a pointer whose field zero is SocketBase.
        # Kept separate from plain openers: their d0 is not itself a base.
        self.object_openers = set(object_openers)
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


NAME, BASE, BASEPTR = 'NAME', 'BASE', 'BASEPTR'

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


def _in_printable_run(code, i, width=2, minimum=6):
    """Whether the bytes at i are embedded in an inline printable string.

    This is used only to disambiguate bsr.b, whose opcode is literally any
    ASCII `a` followed by another byte.  Six contiguous printable bytes around
    the pair is positive data evidence; ordinary instructions before and after
    a genuine two-byte call do not form such a run.
    """
    lo, hi = i, i + width
    while lo > 0 and 32 <= code[lo - 1] < 127:
        lo -= 1
    while hi < len(code) and 32 <= code[hi] < 127:
        hi += 1
    return lo <= i and i + width <= hi and hi - lo >= minimum


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
    """Three sets: generic OpenLibrary wrappers, plain bsdsocket openers, and
    bsdsocket openers that return a pointer to a base-containing object.

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
            w = u16(code, i)
            # A linear sweep cannot distinguish bsr.b from inline ASCII: any
            # `a?` pair is 0x61xx.  Samba's error strings produced targets in
            # the middle of its real bsdsocket opener; those false callers
            # then made unrelated return values (including ExecBase) look
            # like SocketBase.  Preserve genuine short calls, but reject a
            # pair embedded in a printable run -- direct evidence it is data.
            short_bsr = (w & 0xFF00) == 0x6100 and (w & 0xFF) not in (0, 0xFF)
            t = None if short_bsr and _in_printable_run(code, i) \
                else _call_target(code, i, w, ctx)
            if t is not None:
                targets.add(t)
            i += 2

    regions = {idx: named_open_regions(code, ctxs[idx], limit)
               for idx, code in hunks_code.items()}

    wrappers, openers, object_openers = set(), set(), set()
    for t in targets:
        chain = follow_thunks(hunks_code, ctxs, t[0], t[1])
        h, off = chain[-1]
        code = hunks_code.get(h)
        if code is None or not (0 <= off < len(code) - 3):
            continue
        if any(lo <= off < hi for lo, hi in regions.get(h, ())):
            returned = []
            walk(code, ctxs[h], set(), collect_calls=False, start=off,
                 stop_at_return=True, return_values=returned)
            if returned and returned[-1] is BASEPTR:
                object_openers.update(chain)
            else:
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

    return wrappers, openers, object_openers


def walk(code, ctx, base_keys, collect_calls=True, start=0,
         initial_base_regs=(), stop_at_return=False, transfers=None,
         return_values=None):
    """One linear pass.  Returns (base_keys, [(site, lvo)], opens, named_opens).

    Hits carry their call SITE, not just the displacement: v13's peephole runs
    alongside this walk and the two find the same call from opposite ends, so a
    union keyed on the offset alone counts every shared call twice.  A fixture
    caught exactly that -- 6 calls where the program makes 3.

    Run it twice: the first pass discovers where the base is stored, the second
    uses those keys, because a store can sit textually after the calls it
    enables (net.lib opens the library in its own object and the program calls
    it from another -- AmFinger has three CODE hunks and that is why).

    A bounded function pass may start at a proven direct-call target, seed the
    address registers that held BASE at that exact call, and stop at its first
    return.  `transfers`, when supplied, receives the corresponding direct
    targets and live BASE registers for a conservative interprocedural queue.
    """
    st = State()
    st.base_keys = set(base_keys)
    for token in initial_base_regs:
        # Direct helper transfers encode BASE argument registers as 0/1 and
        # BASEPTR argument registers as 8/9.  Public fixture callers continue
        # to seed ordinary base registers with the original 0..6 spelling.
        if 0 <= token < 7:
            st.r[8 + token] = BASE
        elif 8 <= token < 15:
            st.r[token] = BASEPTR
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
        if kind == 'a':
            # The register itself is a pointer to an object known to contain
            # SocketBase at field zero.  This lets `move.l a5,d0` return the
            # object without confusing the pointer with the base it contains.
            return BASEPTR if v in st.indirect_base else st.r[8 + v]
        if kind == 'ind':
            held = st.r[8 + v]
            if held is BASEPTR:
                return BASE
            if isinstance(held, tuple) and held[0] == 'ADDR' \
                    and held[1] in st.base_keys:
                return BASE
            return BASE if v in st.indirect_base else None
        if kind in ('abs', 'absw', 'disp'):
            if allow_name and is_name(code, ctx, kind, v, ext_at):
                return NAME
            if kind == 'disp' and v[0] == 7:
                return BASE if v[1] in st.local_base else None
            if not usable(kind, v):
                return None
            k = key_at(kind, v, ext_at)
            if allow_name and k in ctx.name_pointer_keys:
                return NAME
            if ('baseptr', k) in st.base_keys:
                return BASEPTR
            return BASE if k in st.base_keys else None
        if kind == 'imm':
            return NAME if is_name(code, ctx, kind, v, ext_at) else None
        return None

    def store(kind, v, value, ext_at=None):
        if kind == 'disp' and v[0] == 7:
            # Function-scoped stack provenance.  A modelled overwrite kills
            # the fact just as a register overwrite does.
            if value is BASE:
                st.local_base.add(v[1])
            else:
                st.local_base.discard(v[1])
            return
        if value not in (BASE, BASEPTR):
            if kind == 'ind':
                st.indirect_base.discard(v)
            return
        if value is BASEPTR:
            if kind in ('abs', 'absw', 'disp') and usable(kind, v):
                k = key_at(kind, v, ext_at)
                if k is not None:
                    tagged = ('baseptr', k)
                    st.base_keys.add(tagged); found.add(tagged)
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
            else:
                st.indirect_base.add(v)

    i = start
    n = len(code)
    while i < n - 1:
        w = u16(code, i)
        step = 2

        if w in (0x4E75, 0x4E73, 0x4E77):            # rts / rte / rtr
            if stop_at_return:
                if return_values is not None:
                    return_values.append(st.r[0])
                break
            st.clear_all(); st.name_pushed_at = None
            i += 2; continue
        if (w & 0xFB80) == 0x4880 and (w & 0x38) >= 0x10 \
                and not ((w & 0x38) == 0x38 and (w & 7) >= 4):
                                                        # movem.w/l regs,<ea> or <ea>,regs
            # The word after the opcode is a REGISTER MASK, not another
            # instruction.  The old two-byte skip decoded that mask as code
            # and, for a load, discarded every live value.  ctelnet restores
            # a few saved registers immediately after an opener-return helper;
            # d0 is deliberately absent from the mask and remains SocketBase.
            if i + 4 > n:
                break
            mask = u16(code, i + 2)
            mode, reg = (w >> 3) & 7, w & 7
            direction_load = bool(w & 0x0400)
            if direction_load:
                for r in range(16):
                    if mask & (1 << r):
                        st.r[r] = None
                        if 8 <= r < 15:
                            st.indirect_base.discard(r - 8)
            # Addressing modes with update also change their EA register,
            # independently of the transfer mask.
            if mode in (3, 4):
                st.r[8 + reg] = None
                if reg < 7:
                    st.indirect_base.discard(reg)
            size = 4 if w & 0x0040 else 2
            i += 4 + ea_len(code, i + 4, mode, reg, size)
            continue
        if (w & 0xFFF8) in (0x4E50, 0x4E58):         # link / unlk aN
            # LINK replaces An with the old stack pointer; UNLK restores a
            # frame pointer saved before this function.  Neither value is the
            # caller's live SocketBase merely because the register was.
            reg = w & 7
            st.r[8 + reg] = None
            st.indirect_base.discard(reg)
            i += 4 if (w & 0xFFF8) == 0x4E50 else 2
            continue

        # ---- BRA / Bcc (BSR is handled as a direct call below) -------------
        # A word/long branch displacement is an operand, not code.  In gng's
        # HTTP helper a BEQ.W displacement of 0x258c looked like MOVE.L and
        # skipped the following load of another library base; the old a6 value
        # then falsely attributed that library's -378 vector as bpf_read.
        if (w & 0xF000) == 0x6000 and (w & 0x0F00) != 0x0100:
            d8 = w & 0xFF
            ln = 4 if d8 == 0 else (6 if d8 == 0xFF else 2)
            if (w & 0x0F00) == 0:                    # unconditional BRA
                if d8 == 0 and i + 4 <= n:
                    target = i + 2 + s16(code, i + 2)
                elif d8 == 0xFF and i + 6 <= n:
                    target = i + 2 + struct.unpack_from('>i', code, i + 2)[0]
                else:
                    target = i + 2 + s8(d8)
                # SAS/C argument setup emits `lea literal(pc),aN; move.l
                # aN,d0; bra after_literal`.  Here the LEA independently
                # proves that the skipped bytes are addressed data, so they
                # must not be decoded as instructions.  Do NOT follow every
                # forward BRA: UMS uses one to skip a genuine alternative
                # that opens bsdsocket, and a linear survey must inspect both
                # arms unless it has this positive inline-data evidence.
                inline = False
                if i >= 6:
                    move_an_d0 = u16(code, i - 2)
                    src = move_an_d0 & 7
                    if move_an_d0 == 0x2008 + src \
                            and u16(code, i - 6) == 0x41FA + (src << 9):
                        literal = i - 4 + s16(code, i - 4)
                        inline = i + ln <= literal < target
                if inline and target <= n:
                    i = target
                    continue
            i += ln
            continue

        # ---- CLR.B / CLR.W / CLR.L -----------------------------------------
        # CLR has a full effective-address operand.  Merely stepping over its
        # opcode lets a displacement such as ctelnet's 0x3144 masquerade as a
        # MOVE.W and skip the following, real `lea bsdsocket.library,a1`.
        if (w & 0xFF00) == 0x4200:
            sz = (w >> 6) & 3
            size = (1, 2, 4)[sz] if sz < 3 else 2
            mode, reg = (w >> 3) & 7, w & 7
            if mode in (0, 1):
                dst = (8 if mode == 1 else 0) + reg
                st.r[dst] = None
                if mode == 1:
                    st.indirect_base.discard(reg)
            i += 2 + ea_len(code, i + 2, mode, reg, size)
            continue

        # ---- TST.B / TST.W / TST.L -----------------------------------------
        # Same operand-boundary requirement as CLR.  AmiGG's `tst.w
        # $31b4.l` otherwise decodes 0x31b4 as MOVE.W, skips an ExecBase reload
        # and calls Exec CloseLibrary through a stale SocketBase proof.
        if (w & 0xFF00) == 0x4A00:
            sz = (w >> 6) & 3
            size = (1, 2, 4)[sz] if sz < 3 else 2
            mode, reg = (w >> 3) & 7, w & 7
            i += 2 + ea_len(code, i + 2, mode, reg, size)
            continue

        # ---- immediate arithmetic/logical operations -----------------------
        # ORI/ANDI/SUBI/ADDI/EORI/CMPI carry an immediate word or longword
        # before their destination EA.  MiamiHost's `andi.l #$ffff,d0` left
        # those bytes to be decoded as code and hid a subsequent library-base
        # reload, producing a false ReleaseInterfaceList call.
        if (w & 0xFF00) in (0x0000, 0x0200, 0x0400,
                            0x0600, 0x0A00, 0x0C00):
            sz = (w >> 6) & 3
            if sz < 3:
                size = (1, 2, 4)[sz]
                imm_len = 4 if size == 4 else 2
                mode, reg = (w >> 3) & 7, w & 7
                if mode == 0:
                    st.r[reg] = None
                # ORI/ANDI/EORI to CCR/SR encode 0x3c as part of the opcode;
                # there is no separate effective-address extension.
                dest_len = 0 if (w & 0x3F) == 0x3C else ea_len(
                    code, i + 2 + imm_len, mode, reg, size)
                i += 2 + imm_len + dest_len
                continue

        # ---- ADDA / SUBA / CMPA --------------------------------------------
        # These consume a source EA; arithmetic replaces its destination
        # address register, while CMPA only reads it.  MiamiHost's ADDA.L
        # displacement (0x30e8) otherwise looked like MOVE.W and hid the
        # following non-socket library-base load.  Conversely, tcp_AmiTCP's
        # `cmpa.w #0,a2` exposed its zero immediate as an ORI instruction,
        # which swallowed the following bsdsocket name load and lost every
        # real call in the program.
        adda_suba_cmpa = (w & 0xF0C0)
        if adda_suba_cmpa in (0x90C0, 0xB0C0, 0xD0C0):
            dst = (w >> 9) & 7
            mode, reg = (w >> 3) & 7, w & 7
            size = 4 if w & 0x0100 else 2
            if adda_suba_cmpa != 0xB0C0:
                st.r[8 + dst] = None
                st.indirect_base.discard(dst)
            i += 2 + ea_len(code, i + 2, mode, reg, size)
            continue

        # ---- register/memory arithmetic and comparisons -------------------
        # OR/SUB/CMP/EOR/AND/ADD all carry a full source or destination EA.
        # UMS has `cmp.l 16(a4),d7` before the Scc case below; decoding its
        # displacement as ORI consumed the Scc opcode and still hid the
        # following ExecBase reload.  The 8/C opmode 3/7 forms are DIV/MUL
        # with the same source-EA boundary.  Register-only special encodings
        # (ABCD/SBCD/ADDX/SUBX/EXG) have mode 0/1 and therefore no extension,
        # so the shared length rule remains correct for them.
        alu = w & 0xF000
        opmode = (w >> 6) & 7
        if alu in (0x8000, 0x9000, 0xB000, 0xC000, 0xD000) \
                and (opmode in (0, 1, 2, 4, 5, 6)
                     or (alu in (0x8000, 0xC000) and opmode in (3, 7))):
            dreg = (w >> 9) & 7
            mode, reg = (w >> 3) & 7, w & 7
            size = {0: 1, 1: 2, 2: 4, 3: 2,
                    4: 1, 5: 2, 6: 4, 7: 2}[opmode]
            if opmode in (0, 1, 2, 3, 7):
                st.r[dreg] = None
            elif mode in (0, 1):
                dst = (8 if mode == 1 else 0) + reg
                st.r[dst] = None
                if mode == 1:
                    st.indirect_base.discard(reg)
            i += 2 + ea_len(code, i + 2, mode, reg, size)
            continue

        # ---- Scc / DBcc ----------------------------------------------------
        # Size code 3 in the 0x5xxx group is not quick arithmetic.  Scc has a
        # byte-sized destination EA, while DBcc has a displacement word.
        # UMS uses `seq 580(a5)` immediately before loading ExecBase.  Reading
        # 580 as an ANDI operand consumed that load and left SocketBase live,
        # falsely turning Exec calls into SetSocketSignals/getdtablesize/BPF.
        if (w & 0xF0C0) == 0x50C0:
            mode, reg = (w >> 3) & 7, w & 7
            if mode == 1:                              # DBcc Dn,d16
                i += 4
            else:                                      # Scc <ea>
                if mode == 0:
                    st.r[reg] = None
                i += 2 + ea_len(code, i + 2, mode, reg, 1)
            continue

        # ---- ADDQ / SUBQ ---------------------------------------------------
        # Quick arithmetic has a destination EA.  FTPMount's `subq.l
        # #1,46(a5)` ends in 0x002e; reading that displacement as ORI.B made
        # the sweep consume the following CLR and ExecBase reload, leaving a
        # stale SocketBase in a6 and calling Exec ReplyMsg (-378) `bpf_read`.
        # Size 3 belongs to Scc/DBcc rather than ADDQ/SUBQ.
        if (w & 0xF000) == 0x5000:
            sz = (w >> 6) & 3
            if sz < 3:
                size = (1, 2, 4)[sz]
                mode, reg = (w >> 3) & 7, w & 7
                if mode in (0, 1):
                    dst = (8 if mode == 1 else 0) + reg
                    st.r[dst] = None
                    if mode == 1:
                        st.indirect_base.discard(reg)
                i += 2 + ea_len(code, i + 2, mode, reg, size)
                continue

        # ---- MOVE.B / MOVE.W -----------------------------------------------
        # These values cannot carry a pointer or library base, but their full
        # instruction LENGTH still matters.  Samba's `move.w #1,bd.l(a4)`
        # ends in the displacement word 0x268e; stepping through its operands
        # interpreted that word as `move.l a6,(a3)` and invented a SocketBase
        # store into ExecBase.  Decode both EAs and skip the whole instruction.
        if (w & 0xF000) in (0x1000, 0x3000):
            size = 1 if (w & 0xF000) == 0x1000 else 2
            dreg, dmode = (w >> 9) & 7, (w >> 6) & 7
            smode, sreg = (w >> 3) & 7, w & 7
            slen = ea_len(code, i + 2, smode, sreg, size)
            d_at = i + 2 + slen
            dlen = ea_len(code, d_at, dmode, dreg, size)
            if dmode in (0, 1):
                dst = (8 if dmode == 1 else 0) + dreg
                st.r[dst] = None
                if dmode == 1:
                    st.indirect_base.discard(dreg)
            i += 2 + slen + dlen
            continue

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
                    if dmode == 1:
                        # Preserve the identity only for an explicit address-
                        # register copy.  Every other assignment rebinds An.
                        if sk == 'a' and sv in st.indirect_base:
                            st.indirect_base.add(dreg)
                        else:
                            st.indirect_base.discard(dreg)
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
            st.indirect_base.discard(dreg)
            k, v, ln = ea(code, i + 2, (w >> 3) & 7, w & 7)
            if is_name(code, ctx, k, v, i + 2):
                st.r[8 + dreg] = NAME
            elif k in ('abs', 'absw', 'disp'):
                mk = key_at(k, v, i + 2)
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

        # ---- library call through a proven base register --------------------
        # GCC commonly keeps a library base in a4/a5 and emits jsr d16(a4)
        # directly.  The ABI does not require a6; provenance does.
        if (w & 0xFFF8) in (0x4EA8, 0x4EE8) and i + 4 <= n:
            call_reg = w & 7
            d = s16(code, i + 2)
            if call_reg == 6 and d in OPENS:
                opens += 1
                is_ours = st.r[9] is NAME or (
                    st.name_pushed_at is not None and i - st.name_pushed_at <= 40)
                st.clear_scratch()
                st.name_pushed_at = None
                if is_ours:
                    named += 1
                    st.r[0] = BASE                   # d0 = SocketBase
            else:
                if collect_calls and st.r[8 + call_reg] is BASE:
                    hits.append((i, d))          # site, so a union dedupes
                st.clear_scratch()
            i += 4; continue

        # ---- direct call: a wrapper counts as an OpenLibrary ------------------
        tgt = _call_target(code, i, w, ctx)
        if tgt is not None:
            # A direct call preserves a proof across the function boundary
            # when the caller demonstrably puts BASE in an address ARGUMENT
            # register.  Limit this to scratch argument registers a0/a1: a
            # callee-saved a2-a6 value merely remains live across every call,
            # and recursively treating it as an argument fans one fact across
            # an entire call graph.  Also refuse bsr.b here.  Inline ASCII in
            # real CODE hunks frequently spells 0x61xx (`a?`) and therefore
            # looks like a short bsr to a linear sweep; compiler-generated
            # cross-function calls in the measured cases use bsr.w/jsr.
            short_bsr = (w & 0xFF00) == 0x6100 and (w & 0xFF) not in (0, 0xFF)
            if transfers is not None and not short_bsr:
                passed = tuple(reg if st.r[8 + reg] is BASE else reg + 8
                               for reg in range(2)
                               if st.r[8 + reg] in (BASE, BASEPTR))
                if passed:
                    transfers.append((tgt, passed))
            named_here = (st.name_pushed_at is not None and i - st.name_pushed_at <= 40) \
                or any(st.r[8 + x] is NAME for x in range(8))
            wrapper = tgt in ctx.wrappers
            opener = tgt in ctx.openers
            object_opener = tgt in ctx.object_openers
            st.clear_scratch()
            st.name_pushed_at = None
            if object_opener:
                opens += 1; named += 1
                st.r[0] = BASEPTR
            elif opener:
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
