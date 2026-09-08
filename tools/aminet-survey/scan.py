"""Attribute bsdsocket.library calls by resolving SocketBase, not by offset.

WHY NOT BY OFFSET.  lvo-collisions.tsv: 143 of 143 of our vectors share their
displacement with at least one other library (-366 is bpf_open AND exec PutMsg;
-144 is ObtainSocket AND dos Exit AND AS225 gethostbyaddr).  There is no
unambiguous offset, so `jsr d16(a6)` alone cannot attribute anything.  The
first harness did exactly that and ran ~80% false positives -- AmiFTP 18 real
LVOs reported as 89.

WHAT THIS DOES INSTEAD
  1. find the "bsdsocket.library" string and the lea that takes its address
  2. find the OpenLibrary() call after it            (jsr -552(a6), 4EAE FDD8)
  3. find where the result is stored                 (move.l d0,abs.l, 23C0)
     -- that operand IS SocketBase
  4. count only `movea.l SocketBase,a6` (2079) followed by jsr/jmp d16(a6)

Operands are hunk-relative before relocation, which is fine: the store and the
loads carry the SAME raw value, so they compare without resolving anything.

jmp d16(a6) (4EEE) counts too -- tail calls.  The old harness was blind to
them and missed 6 in AmiFTP alone.
"""
import os
import struct, sys, re
import hunk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_io

# The vector map: the environment, then a copy sitting beside this script,
# then the repo's.  Not one developer's home directory, which is what it was.
# The middle case is the working directory the ticks run out of, where the
# tools and the data live side by side; the repo-relative path would resolve
# outside the checkout from there and silently find nothing.
_HERE = os.path.dirname(os.path.abspath(__file__))
LVOMAP = os.environ.get('ANXD_SURVEY_LVOMAP') or next(
    (p for p in (os.path.join(_HERE, 'lvomap.tsv'),
                 os.path.join(_HERE, '..', '..', 'docs', 'aminet-survey', 'lvomap.tsv'))
     if os.path.exists(p)), '')
if not LVOMAP:
    sys.exit('scan.py: no lvomap.tsv found; set ANXD_SURVEY_LVOMAP')

# THE LEDGER MIXES SCANNER REVISIONS AND COULD NOT SAY WHICH.  Five changes
# altered what a scan returns -- 0x2079 corrected to 0x2C4x, gating the store
# on the OpenLibrary that NAMES bsdsocket.library, the three lea forms, AS225
# detection, SOCK_RAW -- and rows written before each of them are wrong in a
# way no column recorded.  codex raised it; this is the column.
#
# BUMP THIS whenever a change alters what scan() returns for the same input.
# rescan.sh re-runs every archive whose row carries an older version, off the
# local unpack tree, so no re-fetch is needed.  Rows with no scanner= field at
# all predate this and are the ones to redo first.
SCANNER_VERSION = 6

OPENLIB = 0xFDD8            # -552 as a 16-bit displacement
LVO = {}
for line in survey_io.lines(LVOMAP):
    p = line.split('\t')
    if p[0] != 'offset':
        LVO[int(p[1])] = p[3]

def u16(b, i): return struct.unpack_from('>H', b, i)[0]
def u32(b, i): return struct.unpack_from('>I', b, i)[0]
def s16(b, i): return struct.unpack_from('>h', b, i)[0]

# ENCODINGS, AND THE ONE I GOT WRONG FIRST.  0x2079 is `movea.l abs.l,A0`, not
# a6; the a6 forms are 0x2C4x.  Matching 2079 found nothing and reported
# NO_SOCKETBASE_STORE on AmiFTP, which certainly does call the library.
#
# More importantly the ABSOLUTE form is the rare one.  AmiFTP is built in the
# a4-relative small-data model: `movea.l d16(a4),a6` appears 719 times against
# 5 absolute loads, and the OpenLibrary result is stored with `move.l
# d0,d16(a4)`.  A scanner that only knows absolute addressing is blind to the
# usual case.
A6_LOADS = {
    0x2C79: 'abs.l',      # movea.l (xxx).L,a6
    0x2C78: 'abs.w',      # movea.l (xxx).W,a6
    0x2C6C: 'a4',         # movea.l d16(a4),a6   -- small data
    0x2C6D: 'a5',         # movea.l d16(a5),a6
}
D0_STORES = {
    0x23C0: 'abs.l',      # move.l d0,(xxx).L
    0x21C0: 'abs.w',      # move.l d0,(xxx).W
    0x2940: 'a4',         # move.l d0,d16(a4)
    0x2B40: 'a5',         # move.l d0,d16(a5)
}

def _operand(code, i, kind):
    """(key, bytes_consumed) for the operand after the opcode word."""
    if kind == 'abs.l':
        return (('abs', u32(code, i)), 4)
    if kind == 'abs.w':
        return (('abs', s16(code, i)), 2)
    return ((kind, s16(code, i)), 2)

# HOW MANY BYTES AN OPERAND NEEDS after its opcode word.  The scan bounds used
# to be hand-written slack -- `len(code) - 10`, `len(code) - 6` -- and slack is
# not a bound.  A synthetic fixture whose last instruction is `jmp -258(a6)`
# proved it: the tail call sat 6 bytes from the end of the hunk, the loop
# stopped 6 bytes early, and the call was never seen.  A hunk's LAST
# instruction is exactly where a tail call lives.
_NEED = {'abs.l': 4, 'abs.w': 2, 'a4': 2, 'a5': 2}


def _fits(code, i, kind):
    """The opcode word at i plus its operand are inside the hunk."""
    return i + 2 + _NEED[kind] <= len(code)


def find_socketbase(code):
    """Raw keys stored from d0 right after an OpenLibrary() call."""
    bases = set()
    for i in range(0, len(code) - 3, 2):
        if u16(code, i) != 0x4EAE or u16(code, i + 2) != OPENLIB:
            continue
        for j in range(i + 4, min(i + 60, len(code) - 1), 2):
            w = u16(code, j)
            if w in D0_STORES:
                if not _fits(code, j, D0_STORES[w]):
                    break
                key, _ = _operand(code, j + 2, D0_STORES[w])
                bases.add(key)
                break
            if w == 0x4EAE:          # a different library call: give up
                break
    return bases

def calls_for(code, bases):
    """LVO displacements reached through an a6 loaded from one of `bases`."""
    hits = []
    cur = None
    # i + 4 <= len(code): opcode word plus a 16-bit displacement.  Anything
    # tighter drops the last instruction of the hunk, which is where tail calls
    # are.
    for i in range(0, len(code) - 3, 2):
        w = u16(code, i)
        if w in A6_LOADS:
            if not _fits(code, i, A6_LOADS[w]):
                cur = None
                continue
            cur, _ = _operand(code, i + 2, A6_LOADS[w])
            continue
        if w in (0x4EAE, 0x4EEE):    # jsr/jmp d16(a6) -- jmp is a tail call
            if cur is not None and cur in bases:
                hits.append(s16(code, i + 2))
            continue
        if (w & 0xFFC0) == 0x2C40:   # any other movea.l <ea>,a6 rebinds it
            cur = None
    return hits

# HOW THE NAME REACHES a1, and all three forms occur in one binary.
#
#   43FA  lea d16(PC),a1        -- no relocation, target is inside this hunk
#   43F9  lea (xxx).L,a1        -- relocated, target hunk from the reloc table
#   43EC  lea d16(A4),a1        -- small data; the displacement IS the offset
#         (43ED is the a5 form)
#
# AmiFTP names 18 of its 25 OpenLibrary calls PC-relative and the bsdsocket one
# a4-relative at +11134, which is exactly where the string sits in hunk 2.  A
# scanner that knows only one form finds the wrong library or none at all.
NAME_PCREL = 0x43FA
NAME_ABS   = 0x43F9
NAME_SMALL = (0x43EC, 0x43ED)

def _name_target(code, j, target_of, name_sites, name_offsets):
    """True when the lea at `j` addresses the bsdsocket.library string."""
    w = u16(code, j)
    if w == NAME_PCREL:
        t = (j + 2) + s16(code, j + 2)
        return 0 <= t < len(code) - 17 and code[t:t + 17] == b'bsdsocket.library'
    if w == NAME_ABS:
        tgt = target_of.get(j + 2)
        return tgt is not None and (tgt, u32(code, j + 2)) in name_sites
    if w in NAME_SMALL:
        # The base register is whatever hunk the small-data model points at;
        # matching the displacement against the string's offset in ANY hunk
        # settles it without having to work out which.
        return s16(code, j + 2) in name_offsets
    return False

SOCKET_LVO = -30

def raw_socket_sites(code, bases):
    """socket(AF_INET, SOCK_RAW, ...) calls, which no LVO scan can see.

    SOCK_RAW is an ARGUMENT to socket(), not a vector, so a displacement scan
    is blind to it by construction -- and it is a REAL micro breakage: with
    AMINETXDUO_RAWSOCKET=OFF, ping and traceroute fail with no missing LVO
    anywhere.  AvePING pushes it at 0x1772 as pea 1.w (IPPROTO_ICMP), pea 3.w
    (SOCK_RAW), pea 2.w (AF_INET).

    TWO CALLING CONVENTIONS, and the register one is the common case.  Stack:
    `pea 3.w` = 4878 0003.  Registers: socket() takes d0=domain, d1=type, so
    SOCK_RAW is `moveq #3,d1` = 7203 -- MiamiPing has `7002 7203` (AF_INET,
    SOCK_RAW) right before the call, and MiamiTraceRoute the same, while a
    stream client shows `7002 7201`.  Looking only for the stack form found
    NEITHER ping.

    Anchored to the argument setup immediately before a socket() call through a
    resolved SocketBase, so it cannot match the constant 3 elsewhere.
    """
    n = 0
    cur = None
    for i in range(0, len(code) - 3, 2):
        w = u16(code, i)
        if w in A6_LOADS:
            if not _fits(code, i, A6_LOADS[w]):
                cur = None
                continue
            cur, _ = _operand(code, i + 2, A6_LOADS[w])
            continue
        if w in (0x4EAE, 0x4EEE) and s16(code, i + 2) == SOCKET_LVO:
            if cur is not None and cur in bases:
                lo = max(0, i - 24)
                for j in range(lo, i, 2):
                    w2 = u16(code, j)
                    if w2 == 0x7203:                    # moveq #3,d1
                        n += 1
                        break
                    if w2 == 0x4878 and u16(code, j + 2) == 3:
                        n += 1
                        break
            continue
        if (w & 0xFFC0) == 0x2C40:
            cur = None
    return n

def scan(path):
    blob = open(path, 'rb').read()
    if b'bsdsocket.library' not in blob:
        return ('NO_BSDSOCKET_STRING', [], 0)

    hs = list(hunk.hunks(blob))
    if not hs:
        return ('NO_HUNK', [], 0)

    # Where the name lives: (hunk index, offset within it).
    name_sites = []
    for idx, _t, _off, pay, _rel in hs:
        at = pay.find(b'bsdsocket.library')
        while at != -1:
            name_sites.append((idx, at))
            at = pay.find(b'bsdsocket.library', at + 1)
    if not name_sites:
        return ('NAME_NOT_IN_A_HUNK', [], 0)
    name_offsets = {off for _idx, off in name_sites}

    # AS225 DUAL STACK -- REPORT IT, DO NOT AVERAGE OVER IT.  A binary that can
    # open either socket.library (AS225) or bsdsocket.library usually stores
    # both into the SAME variable and calls through it, and the two have
    # DIFFERENT LVO tables: -144 is ObtainSocket for us and gethostbyaddr for
    # AS225.  Attributing those calls to us inflates the set with names the
    # program never used -- AmiFTP scores 34 this way against a ground truth
    # of 18, and the extras are exactly the bpf_* block.  Splitting the branch
    # needs the flag test, which is per-binary work; until then this is a
    # verdict of its own rather than a number nobody can trust.
    dual = False
    for _idx, _t, _off, pay, _rel in hs:
        at = pay.find(b'socket.library')
        while at != -1:
            if pay[max(0, at - 3):at] != b'bsd':
                dual = True
            at = pay.find(b'socket.library', at + 1)

    # BASES ARE GLOBAL, CALLS ARE NOT.  Collected per hunk and used only within
    # that hunk, a base opened in one object never reaches the calls in
    # another -- and that is the NORMAL layout: net.lib's autoinit opens the
    # library from its own object, so its hunk holds the OpenLibrary and the
    # program's hunk holds every call.  AmFinger has three CODE hunks and
    # reported NO_SOCKETBASE_STORE with opens=7 because of this.  a4-relative
    # displacements and absolute addresses are both program-global, so the
    # union is the right scope.
    allhits, nbases, opens = [], 0, 0
    all_bases = set()
    code_hunks = [(i, c, r) for i, t, _o, c, r in hs if t == hunk.HUNK_CODE]

    for idx, code, rel in code_hunks:

        # offset-in-this-hunk -> target hunk, for every relocated longword
        target_of = {}
        for tgt, offs in rel.items():
            for o in offs:
                target_of[o] = tgt

        bases = set()
        # Same bound as calls_for, and for the same reason.  `len(code) - 10`
        # was slack, not a bound: an OpenLibrary within ten bytes of the end of
        # a hunk was never seen, and a binary whose only bsdsocket open sits
        # there is filed NO_SOCKETBASE_STORE -- a wrong verdict, not a missing
        # one, and there are 369 rows carrying it.
        for i in range(0, len(code) - 3, 2):
            if u16(code, i) != 0x4EAE or u16(code, i + 2) != OPENLIB:
                continue
            opens += 1

            # ONLY the OpenLibrary that names bsdsocket.library.  Without this
            # the store after EVERY OpenLibrary is taken as a base -- AmiFTP
            # opens 25 libraries, so the scan returned the union of four
            # libraries' LVOs: 68 offsets against a ground truth of 18,
            # including -870 and -972, which are past the end of our table.
            named = False
            for j in range(max(0, i - 40), i, 2):
                if _name_target(code, j, target_of, name_sites, name_offsets):
                    named = True
                    break
            if not named:
                continue

            for j in range(i + 4, min(i + 60, len(code) - 1), 2):
                w = u16(code, j)
                if w in D0_STORES:
                    if not _fits(code, j, D0_STORES[w]):
                        break
                    key, _ = _operand(code, j + 2, D0_STORES[w])
                    bases.add(key)
                    break
                if w == 0x4EAE:
                    break

        all_bases |= bases

    nbases = len(all_bases)
    raw = 0
    if all_bases:
        for _idx, code, _rel in code_hunks:
            allhits += calls_for(code, all_bases)
            raw += raw_socket_sites(code, all_bases)

    if not nbases:
        return ('NO_SOCKETBASE_STORE opens=%d' % opens, [], 0)
    offs = sorted(set(allhits), reverse=True)

    # A RESOLVED BASE WITH NO CALLS IS NOT A CLEAN ZERO.  It means the
    # OpenLibrary was found and named, the result was stored, and then nothing
    # matched the a6 pattern -- so the program calls the library some way this
    # scanner does not model (a jump table, a6 loaded through a register, a
    # different addressing mode).  Reported as OK it is indistinguishable from
    # "this program does not use bsdsocket", which is how a false negative
    # becomes a finding.  curl and fping both land here.
    if not offs:
        return ('BASE_BUT_NO_CALLS', [], 0)

    v = 'DUAL_STACK_AS225' if dual else 'OK'
    if raw:
        v += '+SOCK_RAW'
    return (v, offs, len(allhits))

if __name__ == '__main__':
    for p in sys.argv[1:]:
        verdict, offs, total = scan(p)
        named = [LVO.get(o, '?%d' % o) for o in offs]
        print("%s\t%s\tdistinct=%d\tcalls=%d\t%s\tscanner=%d"
              % (p.split('/')[-1], verdict, len(offs), total, ",".join(named),
                 SCANNER_VERSION))
