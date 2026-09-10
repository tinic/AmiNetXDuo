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
import dataflow

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
SCANNER_VERSION = 14

OPENLIB = 0xFDD8            # -552 as a 16-bit displacement
# A NAME THAT IS NOT UNIQUE CANNOT BE A KEY.  18 offsets in lvomap.tsv are all
# called `reserved`, and the ledger records NAMES -- so one binary calling one
# reserved offset was counted, downstream, as a caller of all eighteen, and
# lvo-usage.tsv reported 76 vectors with a caller where 59 have one.  Real
# binaries do call them: AmiFTP, AveHOST, AveHTTPD, AveNTP.
#
# Ambiguous names carry their offset: `reserved@-306`.  Every other name in the
# table is unique and is emitted unchanged, so this is the only shape that
# changes and the rest of the ledger reads exactly as before.
LVO = {}
_names = {}
for line in survey_io.lines(LVOMAP):
    p = line.split('\t')
    if p[0] != 'offset':
        _names[p[3]] = _names.get(p[3], 0) + 1
for line in survey_io.lines(LVOMAP):
    p = line.split('\t')
    if p[0] != 'offset':
        LVO[int(p[1])] = p[3] if _names[p[3]] == 1 else f"{p[3]}@{p[1]}"

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
# TWO ROWS OF A FAMILY OF EIGHT, WHICH IS WHY 387 BINARIES WENT UNRESOLVED.
# `move.l d0,(d16,An)` is 0x2140 | (n << 9) and `movea.l (d16,An),a6` is
# 0x2C68 | n, for EVERY address register.  The table hardcoded a4 (0x2940 /
# 0x2C6C) and a5 (0x2B40 / 0x2C6D) -- which the arithmetic reproduces exactly,
# so generating the family is a strict superset of what was here.
#
# Sampling the OpenLibrary sites that DID name bsdsocket.library in binaries
# verdicted NO_SOCKETBASE_STORE, the instruction right after the call was
# `2540` (d16(a2)) six times and `2f40` (d16(a7), a stack local) five times.
# Neither was in the table, so the base was found, named, stored -- and thrown
# away.
#
# a7 IS THE STACK and carries the only real false-positive risk here: a
# displacement off sp names a frame slot, and a different function's frame
# reuses the same displacement for something else.  The pairing is what holds
# it: a hit needs a store AND a load at the SAME key, so an unrelated frame
# slot would have to both receive a library base and be loaded into a6 to
# collide -- at which point it is a library base.  Kept, and measured.
A6_LOADS = {0x2C79: 'abs.l', 0x2C78: 'abs.w'}
D0_STORES = {0x23C0: 'abs.l', 0x21C0: 'abs.w'}
for _n in range(8):
    A6_LOADS[0x2C68 | _n] = 'a%d' % _n           # movea.l d16(An),a6
    D0_STORES[0x2140 | (_n << 9)] = 'a%d' % _n   # move.l d0,d16(An)
    # REGISTER INDIRECT `(An)` IS NOT SAFE AND IS DELIBERATELY ABSENT.
    #
    # It looks like the same family one mode down -- `move.l d0,(An)` is
    # 0x2080|(n<<9) against 0x2140|(n<<9) -- and 24 of the unresolved binaries
    # do store the base that way, so it was added and then REMOVED.  A
    # displacement is what makes `d16(a2)` specific: it names a field at a
    # fixed offset from a base pointer.  `(a2)` names whatever a2 happens to
    # hold, and a2 is a scratch register that changes constantly, so a store
    # and a load written as `(a2)` in two unrelated places key the same and
    # pair up.
    #
    # MEASURED, which is the only reason this is known: with `(An)` in the
    # table, samba's `testprns` reported 53 distinct vectors against 30 before
    # -- including ?-870, ?-906 and ?-996, which are PAST THE END of the
    # 143-vector table and therefore cannot be ours.  That is the same
    # signature the first harness produced (AmiFTP scoring 89 against a ground
    # truth of 18).  An inflated count is worse than a missing one.

# NOT ADDED, AND ON PURPOSE: the base kept in an ADDRESS REGISTER.
#
#   movea.l d0,a3        0x2040 | (n << 9)   -- 2640 in the sample, x2
#   movea.l a3,a6        0x2C48 | n          -- the matching load
#
# It appeared 3 times in 14 named opens, so it is real and it is the next
# thing worth having.  It is left out because a register is not a location: a3
# holding SocketBase at one instruction says nothing about a3 forty
# instructions later, and every intervening call clobbers it.  The memory
# forms above are safe precisely because a store and a load naming the same
# address are talking about the same variable.  Adding the register forms
# without dataflow would attribute whatever a3 happens to hold at each call
# site, which manufactures callers rather than finding them -- and an inflated
# count is worse here than a missing one.

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
_NEED = {'abs.l': 4, 'abs.w': 2}
_NEED.update({'a%d' % _n: 2 for _n in range(8)})


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

def calls_for(code, bases, target_of=None):
    """LVO displacements reached through an a6 loaded from one of `bases`."""
    return [d for _site, d in calls_for_sites(code, bases, target_of)]


def calls_for_sites(code, bases, target_of=None):
    """(site, displacement) for each such call.  The site is what lets v13's
    hits and the dataflow walk's hits be unioned without counting a call the
    two of them both found twice."""
    # a6 IS ALSO LOADED VIA d0, AND THAT WAS THE WHOLE OF BASE_BUT_NO_CALLS.
    # 83 binaries opened bsdsocket, named it, stored the base -- and then made
    # no call this scanner could see.  The instruction feeding a6 at those call
    # sites was `movea.l d0,a6` (0x2C40) 2,190 times, and 2,078 of those were
    # immediately preceded by `move.l (base).L,d0` (0x2039):
    #
    #     move.l  (SocketBase).L,d0     2039 xxxxxxxx
    #     movea.l d0,a6                 2C40
    #     jsr     -xxx(a6)
    #
    # d0 is tracked only across that one idiom -- the load and the transfer
    # adjacent -- so no dataflow is being guessed at; any other write to d0
    # clears it.
    hits = []
    cur = None
    d0 = None
    areg = [None] * 8
    just_opened = False
    # i + 4 <= len(code): opcode word plus a 16-bit displacement.  Anything
    # tighter drops the last instruction of the hunk, which is where tail calls
    # are.
    for i in range(0, len(code) - 3, 2):
        w = u16(code, i)
        opened_prev, just_opened = just_opened, False
        if w == 0x2039 and _fits(code, i, 'abs.l'):      # move.l abs.l,d0
            d0 = ('abs', u32(code, i + 2))
            continue
        if w == 0x2C40:                                  # movea.l d0,a6
            cur = d0
            continue

        # ONE HOP THROUGH AN ADDRESS REGISTER, which is where the rest of
        # BASE_BUT_NO_CALLS went.  The base is loaded into a2/a3/a5 and only
        # then into a6:
        #
        #     movea.l 42(a7),a3     266F 002A     -- 146 sites
        #     movea.l a3,a6         2C4B
        #     jsr     -30(a6)
        #
        # 352 of the ~460 register-to-a6 transfers in those binaries are fed
        # from the stack this way, which v8 already accepts when a6 is loaded
        # from d16(a7) DIRECTLY -- this is the same value taking one more step.
        # Tracked per register and cleared by any other write to it, so the
        # window is the same bounded idiom the d0 case uses and no dataflow is
        # being inferred across a call.
        # A FUNCTION BOUNDARY ENDS EVERY REGISTER'S LIFETIME.  `movem.l (sp)+,regs`
        # (0x4CDF/0x4CD8) restores the caller's values and `rts` leaves, so a
        # register tracked across either is a register holding something else.
        # Without this the base found in one function would be credited with
        # whatever the NEXT function calls through that register -- which is
        # how a count gets inflated rather than corrected.
        if w == 0x4E75 or (w & 0xFFF8) == 0x4CD8 or (w & 0xFFF8) == 0x48E0:
            # ONLY the register file.  Clearing `cur` here as well cost real
            # vectors -- perch lost connect and send, rcp and rshd lost accept,
            # talkd lost recvfrom -- because this walk steps two bytes without
            # decoding, so an operand word that happens to read as 0x4E75 or a
            # movem resets state in the middle of a live sequence.  `cur` is
            # keyed by a memory location and survives that; `areg` is keyed by
            # a register, which genuinely does not outlive the frame.
            areg = [None] * 8
            continue

        reg = (w >> 9) & 7
        if (w & 0xF1C0) == 0x2040 and reg not in (6, 7):   # movea.l <ea>,aN
            mode = w & 0x3F
            kind = {0x39: 'abs.l', 0x38: 'abs.w', 0x2C: 'a4',
                    0x2D: 'a5', 0x2F: 'a7'}.get(mode)
            if kind and _fits(code, i, kind):
                areg[reg], _ = _operand(code, i + 2, kind)
            else:
                areg[reg] = None
            continue
        if (w & 0xFFF8) == 0x2C48:                       # movea.l aN,a6
            cur = areg[w & 7]
            continue
        if w in A6_LOADS:
            if not _fits(code, i, A6_LOADS[w]):
                cur = None
                continue
            cur, _ = _operand(code, i + 2, A6_LOADS[w])
            # Qualify an absolute operand by its relocation, so ExecBase read
            # from address 4 cannot share a key with a base stored at hunk
            # offset 4.  See dataflow.key_at.
            if target_of is not None and A6_LOADS[w] == 'abs.l':
                tgt = target_of.get(i + 2)
                cur = ('h%d' % tgt, cur[1]) if tgt is not None else cur
            d0 = None
            continue
        if (w & 0xFF00) == 0x2000 or (w & 0xF000) == 0x7000:
            d0 = None          # any other write to d0 ends the idiom
        if w in (0x4EAE, 0x4EEE):    # jsr/jmp d16(a6) -- jmp is a tail call
            if cur is not None and cur in bases:
                hits.append((i, s16(code, i + 2)))
            just_opened = (s16(code, i + 2) == -552)
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

# `lea` IS NOT THE ONLY WAY TO PUT AN ADDRESS IN a1, and only modelling it left
# 433 binaries at NO_SOCKETBASE_STORE.  Over 100 sampled, the loads before an
# OpenLibrary this scanner could not name were `movea.l #imm.l,a1` 118 times,
# `movea.l d16(a4/a5),a1` 34 and `movea.l abs.l,a1` 16.  Each is the same
# question as a lea form already handled -- an immediate or absolute operand
# resolved through the relocation table, or a small-data displacement matched
# against where the string sits -- so they resolve the same way.
#
# `movea.l d16(a7),a1` (30 sites) is NOT here: the address was pushed on the
# stack by code this scanner does not follow, and guessing which value a frame
# slot holds is how a false attribution gets made.
MOVEA_ABS   = (0x2279, 0x227C)      # movea.l abs.l,a1 / movea.l #imm.l,a1
MOVEA_SMALL = (0x226C, 0x226D)      # movea.l d16(a4),a1 / d16(a5),a1

# WHAT IS LEFT IS NOT AN ADDRESSING MODE, AND ADDING MORE WILL NOT REACH IT.
# Measured over 149 still-unresolved binaries, the instruction putting a name
# in a1 at an OpenLibrary call was already-modelled `lea` 1,027 times -- those
# are OTHER libraries, which is why they do not match -- against 126
# `movea.l d16(a7),a1`.  That last one is a SHARED OPEN WRAPPER:
#
#     open_helper:  movea.l 4(a7),a1     ; the name, pushed by the caller
#                   jsr     -552(a6)
#                   rts
#     ...           pea     name(pc)
#                   jsr     open_helper
#
# and 195 of 235 name-leas sit more than 200 bytes from the nearest
# OpenLibrary, which is what a different function looks like.  Resolving these
# means finding the wrapper, finding its callers, and reading which one pushes
# `bsdsocket.library` -- call-graph work, not a table entry.  Guessing that a
# frame slot holds our name because some caller somewhere pushed it is how a
# false attribution gets made, and an inflated count is worse than a missing
# one.

def _name_target(code, j, target_of, name_sites, name_offsets):
    """True when the lea at `j` addresses the bsdsocket.library string."""
    w = u16(code, j)
    if w == NAME_PCREL:
        t = (j + 2) + s16(code, j + 2)
        return 0 <= t < len(code) - 17 and code[t:t + 17] == b'bsdsocket.library'
    if w == NAME_ABS:
        tgt = target_of.get(j + 2)
        return tgt is not None and (tgt, u32(code, j + 2)) in name_sites
    if w in NAME_SMALL or w in MOVEA_SMALL:
        # The base register is whatever hunk the small-data model points at;
        # matching the displacement against the string's offset in ANY hunk
        # settles it without having to work out which.
        return s16(code, j + 2) in name_offsets
    if w in MOVEA_ABS:
        # Same resolution as NAME_ABS: the operand is an offset within some
        # hunk until the relocation table says which one.
        tgt = target_of.get(j + 2)
        return tgt is not None and (tgt, u32(code, j + 2)) in name_sites
    return False

SOCKET_LVO = -30

def raw_socket_sites(code, bases, target_of=None):
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
    just_opened = False
    # SAME a6 TRACKING AS calls_for, so the same names must exist here.
    # The idiom handling below is shared text between the two walks, and
    # having it only initialised in one of them raised NameError: areg on
    # the first binary scanned -- past the fixtures, because they exercise
    # calls_for directly and never reach raw_socket_sites.
    d0 = None
    areg = [None] * 8
    for i in range(0, len(code) - 3, 2):
        w = u16(code, i)
        opened_prev, just_opened = just_opened, False
        if w == 0x2039 and _fits(code, i, 'abs.l'):      # move.l abs.l,d0
            d0 = ('abs', u32(code, i + 2))
            continue
        if w == 0x2C40:                                  # movea.l d0,a6
            cur = d0
            continue

        # ONE HOP THROUGH AN ADDRESS REGISTER, which is where the rest of
        # BASE_BUT_NO_CALLS went.  The base is loaded into a2/a3/a5 and only
        # then into a6:
        #
        #     movea.l 42(a7),a3     266F 002A     -- 146 sites
        #     movea.l a3,a6         2C4B
        #     jsr     -30(a6)
        #
        # 352 of the ~460 register-to-a6 transfers in those binaries are fed
        # from the stack this way, which v8 already accepts when a6 is loaded
        # from d16(a7) DIRECTLY -- this is the same value taking one more step.
        # Tracked per register and cleared by any other write to it, so the
        # window is the same bounded idiom the d0 case uses and no dataflow is
        # being inferred across a call.
        # A FUNCTION BOUNDARY ENDS EVERY REGISTER'S LIFETIME.  `movem.l (sp)+,regs`
        # (0x4CDF/0x4CD8) restores the caller's values and `rts` leaves, so a
        # register tracked across either is a register holding something else.
        # Without this the base found in one function would be credited with
        # whatever the NEXT function calls through that register -- which is
        # how a count gets inflated rather than corrected.
        if w == 0x4E75 or (w & 0xFFF8) == 0x4CD8 or (w & 0xFFF8) == 0x48E0:
            # ONLY the register file.  Clearing `cur` here as well cost real
            # vectors -- perch lost connect and send, rcp and rshd lost accept,
            # talkd lost recvfrom -- because this walk steps two bytes without
            # decoding, so an operand word that happens to read as 0x4E75 or a
            # movem resets state in the middle of a live sequence.  `cur` is
            # keyed by a memory location and survives that; `areg` is keyed by
            # a register, which genuinely does not outlive the frame.
            areg = [None] * 8
            continue

        reg = (w >> 9) & 7
        if (w & 0xF1C0) == 0x2040 and reg not in (6, 7):   # movea.l <ea>,aN
            mode = w & 0x3F
            kind = {0x39: 'abs.l', 0x38: 'abs.w', 0x2C: 'a4',
                    0x2D: 'a5', 0x2F: 'a7'}.get(mode)
            if kind and _fits(code, i, kind):
                areg[reg], _ = _operand(code, i + 2, kind)
            else:
                areg[reg] = None
            continue
        if (w & 0xFFF8) == 0x2C48:                       # movea.l aN,a6
            cur = areg[w & 7]
            continue
        if w in A6_LOADS:
            if not _fits(code, i, A6_LOADS[w]):
                cur = None
                continue
            cur, _ = _operand(code, i + 2, A6_LOADS[w])
            # Qualify an absolute operand by its relocation, so ExecBase read
            # from address 4 cannot share a key with a base stored at hunk
            # offset 4.  See dataflow.key_at.
            if target_of is not None and A6_LOADS[w] == 'abs.l':
                tgt = target_of.get(i + 2)
                cur = ('h%d' % tgt, cur[1]) if tgt is not None else cur
            d0 = None
            continue
        if (w & 0xFF00) == 0x2000 or (w & 0xF000) == 0x7000:
            d0 = None          # any other write to d0 ends the idiom
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

# THE ixemul CLASS, WHICH THIS SCANNER STRUCTURALLY CANNOT SEE.
#
# GeekGadgets programs -- wget, ircd, pop3d, wserv, AmPOP3D and friends -- call
# ixemul's C socket()/connect(), and `ixnet.library` opens bsdsocket.library on
# their behalf.  The application binary never names bsdsocket and never touches
# an LVO, so it is verdicted NO_BSDSOCKET_BINARY and is invisible here.  They
# are recognisable by naming `ixemul.library` and carrying the BSD errno table
# ("Can't send after socket shutdown"); 73 such executables in 16 of 600
# sampled archives, so roughly 142 archives corpus-wide.
#
# IT IS NOT WORTH CHASING, for a reason that matters more than the count: the
# vectors those programs need are whatever ixnet.library calls, not whatever
# each program calls -- one binary, not 142.  And ixnet.library scans
# DUAL_STACK_AS225, so it runs on AS225 socket.library or on ours
# interchangeably and its LVO set mixes two tables.
#
# What this DOES mean is that "N binaries call vector X" is a floor, not a
# census, for anything the GeekGadgets toolchain built.

def scan_v13(path):
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

    # A BASE THAT REACHES OFFSETS WE DO NOT HAVE IS NOT OUR BASE.
    #
    # Charon_AmiSSL.library resolved a base and then "called" -2310, -4596,
    # -5004 ... -7014.  Our table runs from -30 to -900, and no bsdsocket
    # implementation has a vector at -7014: the scanner had locked onto
    # AmiSSL's own library base.  It sat in the ledger as an ordinary OK row
    # from v8 onward, and its `reserved@-846` was quoted as evidence that a
    # real program calls a reserved vector.  It was not.
    #
    # The cut was -1200 here, not -900, to leave offset -954 visible on the
    # theory that it was a Roadshow extension this table did not name.  IT WAS
    # NOT: -954 is dos.library VPrintf reached through DOSBase, which sits four
    # bytes from SocketBase in Atalkd's globals block and which this scanner
    # admitted as a base.  v14 cuts at -900, the table's real end.
    if any(o < -1200 for o in offs):
        return ('BASE_NOT_OURS', [], 0)

    v = 'DUAL_STACK_AS225' if dual else 'OK'
    if raw:
        v += '+SOCK_RAW'
    return (v, offs, len(allhits))

def scan(path):
    """v14: SocketBase resolved by dataflow (see dataflow.py).

    v13 attributed 731 binaries and filed 460 -- 43% of everything holding a
    bsdsocket binary -- as NO_SOCKETBASE_STORE or BASE_BUT_NO_CALLS, which are
    verdicts about the scanner printed where a fact about the program belongs.
    Measured against those 460, the misses were: the base kept in an address
    register, the name pushed into a shared open-wrapper reached through a
    linker jump island, an opener function that names the library itself and
    returns the base to a caller that never mentions it, the small-data model's
    BIASED displacement, and `jsr -408(a6)` OldOpenLibrary.  Each is handled by
    following the value, and each was measured before and after.

    v13's peephole runs too and its hits are unioned in, so nothing it found
    can go missing -- verified over all 731: zero binaries lost a vector.
    """
    blob = open(path, 'rb').read()
    if b'bsdsocket.library' not in blob:
        return ('NO_BSDSOCKET_STRING', [], 0)
    hs = list(hunk.hunks(blob))
    if not hs:
        return ('NO_HUNK', [], 0)

    name_sites, name_offsets = set(), set()
    for idx, _t, _o, pay, _r in hs:
        at = pay.find(b'bsdsocket.library')
        while at != -1:
            name_sites.add((idx, at)); name_offsets.add(at)
            at = pay.find(b'bsdsocket.library', at + 1)
    if not name_sites:
        return ('NAME_NOT_IN_A_HUNK', [], 0)

    dual = False
    for _i, _t, _o, pay, _r in hs:
        at = pay.find(b'socket.library')
        while at != -1:
            if pay[max(0, at - 3):at] != b'bsd':
                dual = True
            at = pay.find(b'socket.library', at + 1)

    bias, name_disps = dataflow.derive_bias(
        hs, [pay for _i, t, _o, pay, _r in hs if t == hunk.HUNK_CODE],
        {off for _i, off in name_sites})

    code_pay = [pay for _i, t, _o, pay, _r in hs if t == hunk.HUNK_CODE]
    # a4 is the small-data base; a5 is too in some compilers, but only when it
    # is not being used as a frame pointer.  Every other address register keys
    # locals and struct fields, which are not the same variable twice.
    # a4 is the small-data base in every m68k-amigaos compiler and is never
    # dropped; a5 keys globals in some and is a frame pointer in others, so it
    # is admitted only when the link/unlk evidence does not say otherwise.
    fp = dataflow.frame_pointers(code_pay)
    # MEASURED BOTH WAYS, 2026-09-09.  Allowing every register except proven
    # frame pointers attributes 852 binaries and 76 vectors; restricting to the
    # small-data registers attributes 833 and 56.  The 20 extra vectors are the
    # collision band -- ProcessIsServer 12 (exec FreeVec -690), ObtainServerSocket
    # 3 (exec CreatePool -696), bpf_read 6, CreateAddrAllocMessageA 2 -- reached
    # through struct fields that key the same in unrelated functions.  19 real
    # binaries is not worth 25 invented callers on the exact vectors the micro
    # build is deciding about.
    global_regs = {4} | ({5} if 5 not in fp else set())

    codes, ctxs = {}, {}
    for idx, t, _o, pay, rel in hs:
        if t != hunk.HUNK_CODE:
            continue
        target_of = {}
        for tgt, offs in rel.items():
            for o in offs:
                target_of[o] = tgt
        codes[idx] = pay
        ctxs[idx] = dataflow.Ctx(name_sites, name_offsets, target_of, set(), idx,
                                 name_disps, global_regs=global_regs)

    wrappers, openers = dataflow.find_wrappers(codes, ctxs)
    for c in ctxs.values():
        c.wrappers = wrappers
        c.openers = openers

    base_keys, opens, named = set(), 0, 0
    for idx, code in codes.items():
        found, _h, o, nm = dataflow.walk(code, ctxs[idx], base_keys,
                                         collect_calls=False)
        base_keys |= found; opens += o; named += nm

    # THE CALL PASS RUNS EVEN WITH NO STORED BASE: a base that never reaches
    # memory is still a base (AMarqueed keeps it in a2 and calls
    # `movea.l a2,a6 / jsr -294(a6)`).
    sites = {}
    for idx, code in codes.items():
        _f, h, _o, _n = dataflow.walk(code, ctxs[idx], base_keys)
        for site, d in h:
            sites[(idx, site)] = d
    if base_keys:
        for idx, code in codes.items():
            for site, d in calls_for_sites(code, base_keys, ctxs[idx].target_of):
                sites[(idx, site)] = d

    hits = list(sites.values())
    offs = sorted(set(hits), reverse=True)
    if not base_keys and not offs:
        # NO_SOCKETBASE_STORE was one bucket for four different situations, and
        # only two of them are scanner limits.  A program that prints "cannot
        # open bsdsocket.library" contains the string, references it, and has no
        # base to find -- filing that as a failed scan overstates the gap and
        # hides the cases worth working on.  These labels say what was OBSERVED.
        if named:
            # We watched OUR library get opened and lost the base afterwards:
            # it goes into a heap struct or is handed to another function.
            return ('NAMED_OPEN_BASE_UNKEYED opens=%d' % opens, [], 0)
        ptr = False
        for _i, _t, _o, pay, rel in hs:
            for tgt, offs2 in rel.items():
                for o in offs2:
                    if o + 4 <= len(pay) and (tgt, struct.unpack_from('>I', pay, o)[0]) in name_sites:
                        ptr = True
        ref = False
        for idx, code in codes.items():
            ctx = ctxs[idx]
            for i in range(0, len(code) - 3, 2):
                w = u16(code, i)
                if not ((w & 0xF1C0) == 0x41C0 or (w & 0xFFC0) == 0x4840
                        or (w & 0xF000) == 0x2000):
                    continue
                k, v2, _l = dataflow.ea(code, i + 2, (w >> 3) & 7, w & 7)
                if k and dataflow.is_name(code, ctx, k, v2, i + 2):
                    ref = True
                    break
            if ref:
                break
        if ref:
            return ('NAME_NEVER_AT_AN_OPEN opens=%d' % opens, [], 0)
        if ptr:
            return ('NAME_ONLY_VIA_DATA_POINTER opens=%d' % opens, [], 0)
        # NOT "the program does not use it".  AmiPhone's copy sits in a string
        # POOL between workbench.library and gadtools.library, reachable only
        # a4-relative through a bias this binary gave no way to derive.  The
        # label says what the scan could reach, which is all it knows.
        return ('NAME_UNREACHED opens=%d' % opens, [], 0)
    if not offs:
        return ('BASE_BUT_NO_CALLS', [], 0)
    # Past the end of the table is not our base.  The cut is -900, the table's
    # real end: v13 kept it at -1200 to leave offset -954 visible, and -954 is
    # dos.library VPrintf through DOSBase (retracted 2026-09-09).
    if any(o < -900 for o in offs):
        return ('BASE_NOT_OURS', [], 0)
    v = 'DUAL_STACK_AS225' if dual else 'OK'
    raw = 0
    for idx, code in codes.items():
        raw += raw_socket_sites(code, base_keys, ctxs[idx].target_of)
    if raw:
        v += '+SOCK_RAW'
    return (v, offs, len(hits))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        verdict, offs, total = scan(p)
        named = [LVO.get(o, '?%d' % o) for o in offs]
        print("%s\t%s\tdistinct=%d\tcalls=%d\t%s\tscanner=%d"
              % (p.split('/')[-1], verdict, len(offs), total, ",".join(named),
                 SCANNER_VERSION))
