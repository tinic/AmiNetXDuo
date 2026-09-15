"""Synthetic HUNK fixtures for the scanner.

    tools/aminet-survey/test-scan.py

Every number in docs/aminet-survey comes out of scan.py, and until now the only
thing it had ever been checked against was real archives -- where a miss is
invisible, because nobody knows what the right answer was.  These fixtures are
built here, so the right answer is known exactly.

They also PIN A KNOWN LIMITATION rather than paper over it.  The scanner walks
code hunks two bytes at a time and does not decode instruction boundaries, so
an immediate operand whose bytes happen to read as `jsr d16(a6)` is
indistinguishable from the real thing.  The last fixture builds exactly that
and asserts the scanner is fooled.  If someone adds a decoder, this test fails
and says the limitation is gone -- which is the point of writing it down as an
assertion instead of a comment.

SPDX-License-Identifier: MIT
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hunk
import scan

HUNK_CODE, HUNK_DATA, HUNK_RELOC32 = 0x3E9, 0x3EA, 0x3EC
HUNK_END, HUNK_HEADER = 0x3F2, 0x3F3
NAME = b'bsdsocket.library\x00\x00\x00'      # padded to a longword

fails = []


def check(what, got, want):
    if got != want:
        fails.append(f"{what}: got {got!r}, want {want!r}")


def be32(*v):
    return b''.join(struct.pack('>I', x) for x in v)


def be16(*v):
    return b''.join(struct.pack('>H', x & 0xFFFF) for x in v)


def build(code, data=NAME, relocs=None, data_relocs=None):
    """A two-hunk executable: CODE then DATA, with RELOC32 from code to data.

    relocs is a list of byte offsets within `code` holding a pointer into
    hunk 1.  That is the whole reason RELOC32 has to be parsed at all: before
    relocation the operand of `lea (xxx).L,a1` is just an offset inside some
    hunk, and which hunk is only knowable from this table.
    """
    code = code + b'\x00' * (-len(code) % 4)
    data = data + b'\x00' * (-len(data) % 4)
    out = be32(HUNK_HEADER, 0, 2, 0, 1, len(code) // 4, len(data) // 4)
    out += be32(HUNK_CODE, len(code) // 4) + code
    if relocs:
        out += be32(HUNK_RELOC32, len(relocs), 1) + be32(*relocs) + be32(0)
    out += be32(HUNK_END)
    out += be32(HUNK_DATA, len(data) // 4) + data
    if data_relocs:
        out += be32(HUNK_RELOC32, len(data_relocs), 0) + be32(*data_relocs) + be32(0)
    out += be32(HUNK_END)
    return out


# ---- the shape every attributed row comes from ---------------------------
#
#   lea (name).L,a1     43F9 00000000   <- relocated into hunk 1
#   movea.l (4).W,a6    2C78 0004
#   jsr -552(a6)        4EAE FDD8       <- OpenLibrary
#   move.l d0,(400).L   23C0 00000190   <- SocketBase
#   movea.l (400).L,a6  2C79 00000190
#   jsr -30(a6)         4EAE FFE2       <- socket
#   jsr -120(a6)        4EAE FF88       <- CloseSocket
#   jmp -258(a6)        4EEE FEFE       <- vsyslog, a TAIL CALL
#   rts                 4E75
CODE = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
        + be16(0x4EAE, 0xFDD8)
        + be16(0x23C0) + be32(400)
        + be16(0x2C79) + be32(400)
        + be16(0x4EAE, 0xFFE2)
        + be16(0x4EAE, 0xFF88)
        + be16(0x4EEE, 0xFEFE)
        + be16(0x4E75))
blob = build(CODE, relocs=[2])

codes = list(hunk.code_hunks(blob))
check("code hunks", len(codes), 1)
check("code bytes", codes[0][1], CODE + b'\x00' * (-len(CODE) % 4))

bases = scan.find_socketbase(codes[0][1])
check("SocketBase key", bases, {('abs', 400)})

lvos = scan.calls_for(codes[0][1], bases)
check("LVO displacements", sorted(lvos), [-258, -120, -30])
check("tail call found", -258 in lvos, True)

# ---- a6 rebound to another library must not attribute its calls ----------
#
# `movea.l (800).L,a6` between the calls rebinds a6, and everything after it
# belongs to a different library.  Attributing those was the defect that
# produced 68 stores where the ground truth was 18.
CODE2 = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
         + be16(0x4EAE, 0xFDD8)
         + be16(0x23C0) + be32(400)
         + be16(0x2C79) + be32(400)
         + be16(0x4EAE, 0xFFE2)          # ours
         + be16(0x2C79) + be32(800)      # a6 now points somewhere else
         + be16(0x4EAE, 0xFF88)          # NOT ours
         + be16(0x4E75))
b2 = list(hunk.code_hunks(build(CODE2, relocs=[2])))[0][1]
check("rebound a6", sorted(scan.calls_for(b2, {('abs', 400)})), [-30])

# ---- DATA hunks are not scanned -----------------------------------------
#
# The old harness stepped from byte 0 of the whole file, so a jsr-shaped pair
# of bytes inside DATA counted as a call.  Only HUNK_CODE is yielded.
payload = be16(0x2C79) + be32(400) + be16(0x4EAE, 0xFFE2) + NAME
b3 = build(be16(0x4E75), data=payload, relocs=None)
check("data not scanned", [c for _o, c in hunk.code_hunks(b3)],
      [be16(0x4E75) + b'\x00' * 2])

# ---- a6 loaded through d0, the BASE_BUT_NO_CALLS shape -------------------
#
#   move.l  (400).L,d0     2039 00000190
#   movea.l d0,a6          2C40
#   jsr     -30(a6)        4EAE FFE2
#
# 2,078 of the 2,190 `movea.l d0,a6` sites in binaries verdicted
# BASE_BUT_NO_CALLS are preceded by exactly this load.
CODE5 = (be16(0x2039) + be32(400) + be16(0x2C40)
         + be16(0x4EAE, 0xFFE2)
         + be16(0x4E75))
b5 = list(hunk.code_hunks(build(CODE5)))[0][1]
check("a6 via d0", scan.calls_for(b5, {('abs', 400)}), [-30])

# And the idiom must NOT survive an intervening write to d0: `moveq #0,d0`
# between the load and the transfer means a6 is loaded with something else.
CODE6 = (be16(0x2039) + be32(400) + be16(0x7000) + be16(0x2C40)
         + be16(0x4EAE, 0xFFE2)
         + be16(0x4E75))
b6 = list(hunk.code_hunks(build(CODE6)))[0][1]
check("d0 clobbered between load and transfer", scan.calls_for(b6, {('abs', 400)}), [])

# ---- a6 via one hop through an address register --------------------------
#
#     movea.l (400).L,a3    2679 00000190
#     movea.l a3,a6         2C4B
#     jsr     -30(a6)
#
# 352 of the ~460 register-to-a6 transfers in BASE_BUT_NO_CALLS binaries are
# fed this way, mostly from a stack slot.
CODE7 = (be16(0x2679) + be32(400) + be16(0x2C4B)
         + be16(0x4EAE, 0xFFE2)
         + be16(0x4E75))
b7 = list(hunk.code_hunks(build(CODE7)))[0][1]
check("a6 via one register hop", scan.calls_for(b7, {('abs', 400)}), [-30])

# Rewriting the register with something else must break the chain: a3 is
# reloaded from a DIFFERENT address before the transfer, so a6 is not our base.
CODE8 = (be16(0x2679) + be32(400) + be16(0x2679) + be32(800) + be16(0x2C4B)
         + be16(0x4EAE, 0xFFE2)
         + be16(0x4E75))
b8 = list(hunk.code_hunks(build(CODE8)))[0][1]
check("register rewritten before the hop", scan.calls_for(b8, {('abs', 400)}), [])

# ---- KNOWN LIMITATION: an immediate that reads as an instruction ---------
#
#   move.l #$4EAEFFE2,d0    203C 4EAE FFE2
#
# The four operand bytes are byte-identical to `jsr -30(a6)`.  Stepping two
# bytes at a time without decoding, the scanner cannot tell them apart, and
# counts a socket() call that does not exist.  Asserted as IS, not as ought:
# when this starts failing, the scanner has learned to decode.
CODE4 = (be16(0x2C79) + be32(400)
         + be16(0x203C, 0x4EAE, 0xFFE2)   # an immediate, not a call
         + be16(0x4E75))
b4 = list(hunk.code_hunks(build(CODE4)))[0][1]
check("known limitation: immediate misread as a call",
      scan.calls_for(b4, {('abs', 400)}), [-30])

# ---- a base that reaches impossible offsets is not our base ---------------
#
# Charon_AmiSSL.library resolved a base and then "called" -2310 through -7014.
# The table runs -30 to -900; those are AmiSSL's own vectors, and the row sat
# in the ledger as an ordinary OK from v8 onward.
CODED = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
         + be16(0x4EAE, 0xFDD8)
         + be16(0x23C0) + be32(400)
         + be16(0x2C79) + be32(400)
         + be16(0x4EAE, 0xFFE2)               # -30, a real vector
         + be16(0x4EAE, 0xE4AA)               # -7014, impossible
         + be16(0x4E75))
with tempfile.NamedTemporaryFile(suffix='.exe', delete=False) as fh:
    fh.write(build(CODED, relocs=[2]))
    _p = fh.name
try:
    verdict, offs, _t = scan.scan(_p)
    check("impossible offset rejects the whole row", verdict, 'BASE_NOT_OURS')
    check("and reports no vectors", offs, [])
finally:
    os.unlink(_p)

# ---- scan() END TO END, which the fixtures above never reach --------------
#
# Everything above calls calls_for() directly.  scan() also calls
# raw_socket_sites(), and a v10 edit that landed in BOTH walks initialised its
# state in only one -- so every real binary raised `NameError: areg` while this
# suite stayed green.  A fixture that exercises the entry point catches that
# class; one that exercises a helper cannot.
import tempfile
with tempfile.NamedTemporaryFile(suffix='.exe', delete=False) as fh:
    fh.write(build(CODE, relocs=[2]))
    _p = fh.name
try:
    verdict, offs, total = scan.scan(_p)
    check("scan() verdict", verdict, 'OK')
    check("scan() offsets", sorted(offs), [-258, -120, -30])
    check("scan() call count", total, 3)
finally:
    os.unlink(_p)

# And the SOCK_RAW argument pattern, which lives only in raw_socket_sites:
#     moveq #3,d1 ; jsr -30(a6)   -- socket(..., SOCK_RAW, ...)
CODE9 = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
         + be16(0x4EAE, 0xFDD8)
         + be16(0x23C0) + be32(400)
         + be16(0x2C79) + be32(400)
         + be16(0x2650)                       # movea.l (a0),a3 -- a mode the
                                              # register tracker does not map,
                                              # so it must CLEAR a3 rather than
                                              # raise; this is the line that
                                              # crashed every real binary while
                                              # the suite stayed green
         + be16(0x7203)                       # moveq #3,d1
         + be16(0x4EAE, 0xFFE2)               # jsr -30(a6) == socket
         + be16(0x4E75))
with tempfile.NamedTemporaryFile(suffix='.exe', delete=False) as fh:
    fh.write(build(CODE9, relocs=[2]))
    _p = fh.name
try:
    verdict, _o, _t = scan.scan(_p)
    check("scan() detects SOCK_RAW", verdict, 'OK+SOCK_RAW')
finally:
    os.unlink(_p)


# ==========================================================================
# v14 fixtures.  Each one is a shape measured in the corpus that v13 filed as
# "no SocketBase store" -- a verdict about the scanner where a fact about the
# program belongs.  They are here so a later simplification cannot quietly put
# any of them back.
# ==========================================================================

def scan_blob(blob):
    with tempfile.NamedTemporaryFile(suffix='.exe', delete=False) as fh:
        fh.write(blob)
        path = fh.name
    try:
        return scan.scan(path)
    finally:
        os.unlink(path)


# ---- OldOpenLibrary -------------------------------------------------------
# exec -408, the V1.2 entry.  MetalWEB and the AmFTP/Voyager registration tools
# open every library with it; modelling only -552 files them all as unnamed.
CODE_OLD = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
            + be16(0x4EAE, 0xFE68)               # jsr -408(a6) OldOpenLibrary
            + be16(0x23C0) + be32(400)
            + be16(0x2C79) + be32(400)
            + be16(0x4EAE, 0xFFE2)               # socket
            + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_OLD, relocs=[2]))
check("OldOpenLibrary named", (v, offs), ('OK', [-30]))


# ---- the base kept in an address register, never stored -------------------
# AMarqueed: `movea.l d0,a2 / movea.l a2,a6 / jsr -294(a6)`.  There is no
# memory location to key on, so a table of store forms can never see it.
CODE_REG = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
            + be16(0x4EAE, 0xFDD8)
            + be16(0x2440)                       # movea.l d0,a2
            + be16(0x2C4A)                       # movea.l a2,a6
            + be16(0x4EAE, 0xFFE2)               # socket
            + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_REG, relocs=[2]))
check("register-held base", (v, offs), ('OK', [-30]))

# The Amiga library ABI does not require calls through a6.  AmiVNC keeps the
# returned base in a4 and emits jsr d16(a4); only proven BASE provenance makes
# that safe, not the choice of register.
CODE_REG_CALL = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
                 + be16(0x4EAE, 0xFDD8)
                 + be16(0x2640)                  # movea.l d0,a3
                 + be16(0x4EAB, 0xFFE2)          # jsr -30(a3)
                 + be16(0x267C) + be32(123)      # rebind a3
                 + be16(0x4EAB, 0xFF88)          # not ours
                 + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_REG_CALL, relocs=[2]))
check("direct call through non-a6 base", (v, offs), ('OK', [-30]))


# ---- a base passed to a directly called helper ---------------------------
# FTPMount keeps SocketBase in a5, passes it to a helper in a0, and the helper
# moves it to its own callee-saved register before calling vectors.  The
# direct target and the live BASE value are both proven at the call site.
_direct_main = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
                + be16(0x4EAE, 0xFDD8)
                + be16(0x2A40)                  # movea.l d0,a5
                + be16(0x204D)                  # movea.l a5,a0
                + be16(0x6100, 0)               # bsr.w helper
                + be16(0x4E75))
_direct_helper = (be16(0x2648)                   # movea.l a0,a3
                  + be16(0x4EAB, 0xFFE2)         # jsr -30(a3)
                  + be16(0x4E75))
_bsr_at = len(_direct_main) - 6
CODE_DIRECT_ARG = (_direct_main[:_bsr_at]
                   + be16(0x6100, len(_direct_main) - (_bsr_at + 2))
                   + _direct_main[_bsr_at + 4:] + _direct_helper)
v, offs, _t = scan_blob(build(CODE_DIRECT_ARG, relocs=[2]))
check("base passed to direct helper", (v, offs), ('OK', [-30]))

# Rebinding the argument before the call removes the proof.  The same helper
# body must not be scanned as though every a0 argument were SocketBase.
_clobber = be16(0x207C) + be32(123)              # movea.l #123,a0
_bad_prefix = _direct_main[:_bsr_at] + _clobber
_bad_bsr_at = len(_bad_prefix)
_bad_main = (_bad_prefix
             + be16(0x6100, _bad_bsr_at + 6 - (_bad_bsr_at + 2))
             + be16(0x4E75))
CODE_DIRECT_ARG_BAD = _bad_main + _direct_helper
v, offs, _t = scan_blob(build(CODE_DIRECT_ARG_BAD, relocs=[2]))
check("rebound direct helper argument", offs, [])

# A short bsr is intentionally not enough evidence for interprocedural flow.
# Real CODE hunks contain inline ASCII, where any bytes 0x61xx spell bsr.b to
# a linear scanner; admitting those targets recursively spread one a6 fact
# through hundreds of invented callees and manufactured reserved-vector hits.
_short_main = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
               + be16(0x4EAE, 0xFDD8)
               + be16(0x2040)                   # movea.l d0,a0
               + be16(0x6102)                   # bsr.b helper
               + be16(0x4E75))
v, offs, _t = scan_blob(build(_short_main + _direct_helper, relocs=[2]))
check("short-bsr helper is not propagated", offs, [])


# ---- 68020 full-extension stable displacement ---------------------------
# Samba uses mode 6 with extension 0x0170 and a long base displacement.  It
# looks indexed syntactically, but 0x0170 explicitly suppresses the index and
# memory indirection, so `bd.l(a4)` is the same stable global on every use.
_FULL_A4_400 = be16(0x0170) + be32(400)
CODE_FULL_DISP = (be16(0x45F4) + _FULL_A4_400     # lea 400(a4),a2
                  + be16(0x43F9) + be32(0)
                  + be16(0x2C78, 0x0004)
                  + be16(0x4EAE, 0xFDD8)
                  + be16(0x2480)                 # move.l d0,(a2)
                  + be16(0x2C74) + _FULL_A4_400  # movea.l 400(a4),a6
                  + be16(0x4EAE, 0xFFE2)
                  + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_FULL_DISP, relocs=[10]))
check("68020 suppressed-index global", (v, offs), ('OK', [-30]))

# With the index active, the effective address varies with d0 and is not a
# stable key.  The scanner must not equate it with the fixed SocketBase cell.
_FULL_A4_D0_400 = be16(0x0130) + be32(400)
CODE_FULL_INDEXED = (be16(0x45F4) + _FULL_A4_400
                     + be16(0x43F9) + be32(0)
                     + be16(0x2C78, 0x0004)
                     + be16(0x4EAE, 0xFDD8)
                     + be16(0x2480)
                     + be16(0x2C74) + _FULL_A4_D0_400
                     + be16(0x4EAE, 0xFFE2)
                     + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_FULL_INDEXED, relocs=[10]))
check("68020 active-index global rejected", offs, [])

# The same short-bsr ambiguity must not manufacture an opener return.  The
# bytes `ar` below sit inside a printable run and decode as bsr.b +0x72 only if
# inline text is mistaken for code.  A real short opener call remains valid.
_short_opener = (be16(0x43F9) + be32(0)
                 + be16(0x2C78, 0x0004)
                 + be16(0x4EAE, 0xFDD8)
                 + be16(0x4E75))
_short_caller = (b'xxxxxxar'                    # false bsr.b at byte 6
                 + be16(0x23C0) + be32(400)
                 + be16(0x2C79) + be32(400)
                 + be16(0x4EAE, 0xFFE2)
                 + be16(0x4E75))
CODE_SHORT_OPENER = (_short_caller
                     + b'\0' * (122 - len(_short_caller)) + _short_opener)
v, offs, _t = scan_blob(build(CODE_SHORT_OPENER,
                               relocs=[122 + 2]))
check("printable short-bsr opener is not inferred", offs, [])

_real_short_caller = (be16(0x6112)              # bsr.b opener
                      + be16(0x23C0) + be32(400)
                      + be16(0x2C79) + be32(400)
                      + be16(0x4EAE, 0xFFE2)
                      + be16(0x4E75))
CODE_REAL_SHORT_OPENER = _real_short_caller + _short_opener
v, offs, _t = scan_blob(build(CODE_REAL_SHORT_OPENER,
                               relocs=[len(_real_short_caller) + 2]))
check("real short-bsr opener remains valid", offs, [-30])

# Operand words are not instructions.  Samba's full-extension displacement
# ended in 0x268e, which is also `move.l a6,(a3)` when decoded out of context;
# that used to copy BASE into the unrelated global addressed by a3.
CODE_WORD_OPERAND = (be16(0x43F9) + be32(0)
                     + be16(0x2C78, 0x0004)
                     + be16(0x4EAE, 0xFDD8)
                     + be16(0x23C0) + be32(400)
                     + be16(0x47EC, 800)         # lea 800(a4),a3
                     + be16(0x2C79) + be32(400)
                     + be16(0x39BC, 0x0001, 0x0170)
                     + be32(0x0000268E)          # not a move.l opcode
                     + be16(0x2C6C, 800)
                     + be16(0x4EAE, 0xFFE2)
                     + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_WORD_OPERAND, relocs=[2]))
check("MOVE.W full-extension operand is skipped", offs, [])

# ctelnet has `clr.l $3144(a4)` directly before loading the library name.
# 0x3144 is itself a valid MOVE.W opcode; failing to consume CLR's EA operand
# makes a linear decoder skip the following LEA and lose the obvious base.
CODE_CLR_BOUNDARY = (be16(0x42AC, 0x3144)
                     + be16(0x43F9) + be32(0)
                     + be16(0x2C78, 0x0004)
                     + be16(0x4EAE, 0xFDD8)
                     + be16(0x23C0) + be32(400)
                     + be16(0x2C79) + be32(400)
                     + be16(0x4EAE, 0xFFE2)
                     + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_CLR_BOUNDARY, relocs=[6]))
check("CLR effective-address boundary", (v, offs), ('OK', [-30]))

# A Bcc extension can itself look like a MOVE instruction.  It must not skip
# an explicit a6 rebind and leave the previous SocketBase live at a call made
# through another library (the false bpf_read in GiambyNetGrabber's http).
CODE_BRANCH_BOUNDARY = (be16(0x2C4C)              # a6 = BASE (seeded a4)
                        + be16(0x6700, 0x258C)     # beq.w; 0x258c is operand
                        + be16(0x2C79) + be32(404) # a6 = unrelated location
                        + be16(0x4EAE, 0xFE86)
                        + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_BRANCH_BOUNDARY, ctx, set(), initial_base_regs=(4,))
check("Bcc displacement cannot hide a6 rebind", hits, [])

CODE_TST_BOUNDARY = (be16(0x2C4C)                 # a6 = BASE (seeded a4)
                     + be16(0x4A79) + be32(0x31B4)
                     + be16(0x2C79) + be32(404)   # a6 = unrelated location
                     + be16(0x4EAE, 0xFE62)
                     + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_TST_BOUNDARY, ctx, set(), initial_base_regs=(4,))
check("TST operand cannot hide a6 rebind", hits, [])

CODE_IMMEDIATE_BOUNDARY = (be16(0x2C4C)             # a6 = BASE (seeded a4)
                           + be16(0x0280) + be32(0x0000FFFF)
                           + be16(0x2C79) + be32(404)
                           + be16(0x4EAE, 0xFE38)
                           + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_IMMEDIATE_BOUNDARY, ctx, set(), initial_base_regs=(4,))
check("immediate operand cannot hide a6 rebind", hits, [])

CODE_ADDA_BOUNDARY = (be16(0x2C4C)                  # a6 = BASE (seeded a4)
                      + be16(0xD3EF, 0x30E8)        # adda.l d16(a7),a1
                      + be16(0x2C6C, 404)           # a6 = unrelated location
                      + be16(0x4EAE, 0xFE38)
                      + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_ADDA_BOUNDARY, ctx, set(), initial_base_regs=(4,))
check("ADDA operand cannot hide a6 rebind", hits, [])

CODE_CMPA_BOUNDARY = (be16(0xB4FC, 0x0000)       # cmpa.w #0,a2
                      + be16(0x284C)              # a4 = BASE (seeded a4)
                      + be16(0x2C4C)
                      + be16(0x4EAE, 0xFFE2)
                      + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_CMPA_BOUNDARY, ctx, set(), initial_base_regs=(4,))
check("CMPA immediate cannot hide a base use", [d for _site, d in hits], [-30])

# A CODE hunk may put padding/data immediately before a called function.  The
# padding's last zero word decodes as ORI.B and would consume the first opcode
# when sweeping from hunk offset zero.  The direct call proves the opener's
# aligned entry, from which its real base store can be recovered.
_aligned_head = (be16(0x6100, 6) + be16(0x4E75) + be16(0x0000))
_aligned_opener = (be16(0x43F9) + be32(0)
                   + be16(0x2C78, 0x0004)
                   + be16(0x4EAE, 0xFDD8)
                   + be16(0x23C0) + be32(400)
                   + be16(0x4E75))
_aligned_user = (be16(0x2C79) + be32(400)
                 + be16(0x4EAE, 0xFFE2) + be16(0x4E75))
v, offs, _t = scan_blob(build(_aligned_head + _aligned_opener + _aligned_user,
                              relocs=[len(_aligned_head) + 2]))
check("called opener aligned after inline data", (v, offs), ('OK', [-30]))

# AmFTP's opener returns a connection object, not SocketBase itself.  The
# callee proves that field zero is the named OpenLibrary result and returns the
# same object pointer; the caller stores and passes it to a helper, which loads
# field zero into a6.  Keep this provenance distinct from BASE throughout.
_obj_main = (be16(0x6100, 20)
             + be16(0x23C0) + be32(400)
             + be16(0x2079) + be32(400)
             + be16(0x6100, 24)
             + be16(0x4E75))
_obj_opener = (be16(0x43F9) + be32(0)
               + be16(0x2C78, 0x0004)
               + be16(0x4EAE, 0xFDD8)
               + be16(0x2A80)              # move.l d0,(a5)
               + be16(0x200D)              # move.l a5,d0
               + be16(0x4E75))
_obj_helper = (be16(0x2A48)                 # movea.l a0,a5
               + be16(0x2C55)              # movea.l (a5),a6
               + be16(0x4EAE, 0xFFE2)
               + be16(0x4E75))
v, offs, _t = scan_blob(build(_obj_main + _obj_opener + _obj_helper,
                              relocs=[len(_obj_main) + 2]))
check("base in returned object field zero", (v, offs), ('OK', [-30]))

# Merely storing the base through one register is insufficient: returning a
# different pointer must not confer BASEPTR provenance on the caller.
_not_obj_opener = _obj_opener[:-4] + be16(0x200C) + be16(0x4E75)
v, offs, _t = scan_blob(build(_obj_main + _not_obj_opener + _obj_helper,
                              relocs=[len(_obj_main) + 2]))
check("unrelated returned pointer is not a base object", offs, [])

# The bytes for move.l a5,d0 inside another instruction's operand are not a
# return-value proof (a real corpus binary has CMPI.W #$200d,d0 here).
_operand_return_opener = (_obj_opener[:-4]
                          + be16(0x0C40, 0x200D) + be16(0x4E75))
v, offs, _t = scan_blob(build(_obj_main + _operand_return_opener + _obj_helper,
                              relocs=[len(_obj_main) + 2]))
check("operand bytes are not a returned base object", offs, [])

CODE_QUICK_BOUNDARY = (be16(0x2C4C)               # a6 = BASE (seeded a4)
                       + be16(0x53AD, 0x002E)       # subq.l #1,46(a5)
                       + be16(0x42AD, 0x0032)       # clr.l 50(a5)
                       + be16(0x2C78, 0x0004)       # a6 = ExecBase
                       + be16(0x4EAE, 0xFE86)       # Exec ReplyMsg
                       + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_QUICK_BOUNDARY, ctx, set(), initial_base_regs=(4,))
check("ADDQ/SUBQ operand cannot hide a6 rebind", hits, [])

# Scc has an EA even though the same size bits spell an invalid ADDQ size.
# Its displacement must not be decoded as a fresh immediate instruction and
# allowed to swallow the following ExecBase reload.
CODE_SCC_BOUNDARY = (be16(0x2C4C)               # a6 = BASE (seeded a4)
                     + be16(0xBEAC, 0x0010)      # cmp.l 16(a4),d7
                     + be16(0x57ED, 0x0244)      # seq 580(a5)
                     + be16(0x2C78, 0x0004)      # a6 = ExecBase
                     + be16(0x4EAE, 0xFE7A)      # Exec call at -390
                     + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_SCC_BOUNDARY, ctx, set(), initial_base_regs=(4,))
check("ALU and Scc operands cannot hide a6 rebind", hits, [])

# A forward unconditional branch skips inline data; execution resumes at the
# proven target.  Sweeping through the literal words as instructions can hide
# the first real instruction there and retain a stale library base.
CODE_BRA_INLINE = (be16(0x2C4C)                  # a6 = BASE (seeded a4)
                   + be16(0x41FA, 0x0008)        # lea literal(pc),a0
                   + be16(0x2008)                # move.l a0,d0
                   + be16(0x6000, 0x0008)        # bra.w target below
                   + be16(0x0001, 0x0000, 0x0000)# literal: six bytes
                   + be16(0x2C78, 0x0004)        # target: a6 = ExecBase
                   + be16(0x4EAE, 0xFE9E)
                   + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_BRA_INLINE, ctx, set(), initial_base_regs=(4,))
check("forward BRA skips inline data", hits, [])


# ---- an opener function: the callee names the library, the caller stores ---
# AWeb's helper.  The caller never mentions bsdsocket, so nothing at the store
# site says which library the base belongs to; the callee's code does.
_opener = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
           + be16(0x4EAE, 0xFDD8)
           + be16(0x4E75))
_head = (be16(0x6100, 0) + be16(0x23C0) + be32(400)
         + be16(0x2C79) + be32(400)
         + be16(0x4EAE, 0xFFE2) + be16(0x4E75))
CODE_OPENER = _head + _opener
# bsr displacement is measured from the word AFTER the opcode
CODE_OPENER = (be16(0x6100, len(_head) - 2) + CODE_OPENER[4:])
v, offs, _t = scan_blob(build(CODE_OPENER, relocs=[len(_head) + 2]))
check("opener function", (v, offs), ('OK', [-30]))

# ctelnet restores selected saved registers after the opener-return helper.
# MOVEM's register mask is an operand word, and d0 remains live when its bit is
# clear; decoding the mask as instructions both lost that fact and fabricated
# arbitrary operations from the mask bits.
CODE_MOVEM_RESULT = (be16(0x207C) + be32(400)     # a0 = known global address
                     + be16(0x200C)              # d0 = BASE (seeded a4)
                     + be16(0x4CDF, 0x6800)      # restore a3/a5/a6, not d0
                     + be16(0x2080)              # move.l d0,(a0)
                     + be16(0x2C50)              # movea.l (a0),a6
                     + be16(0x4EAE, 0xFFE2)
                     + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_MOVEM_RESULT, ctx, set(), initial_base_regs=(4,))
check("movem preserves unmasked base", [d for _site, d in hits], [-30])

# EXT.W/EXT.L share MOVEM's upper opcode bits, but register-direct mode is not
# legal for MOVEM.  Treating EXT.W d0 as a mask word consumed the following
# DOSBase reload in AmiTCP rsh and mislabelled DOS FPutC (-312) as SocketBase.
CODE_EXT_NOT_MOVEM = (be16(0x2C4C)                  # a6 = BASE (seeded a4)
                      + be16(0x4880)                # ext.w d0
                      + be16(0x2C79) + be32(404)    # a6 = unrelated location
                      + be16(0x4EAE, 0xFEC8)
                      + be16(0x4E75))
ctx = scan.dataflow.Ctx(set(), set(), {}, set(), 0)
_found, hits, _o, _n = scan.dataflow.walk(
    CODE_EXT_NOT_MOVEM, ctx, set(), initial_base_regs=(4,))
check("EXT is not MOVEM", hits, [])


# ---- a shared wrapper reached through a linker jump island ----------------
# AMarqueed again: `pea name / bsr stub`, and the stub is one entry in a table
# of `jmp` islands.  Testing the island for an OpenLibrary finds a jump.
_wrap = (be16(0x226F, 0x0004)                    # movea.l 4(a7),a1
         + be16(0x2C78, 0x0004)
         + be16(0x4EAE, 0xFDD8)
         + be16(0x4E75))
_main = (be16(0x4879) + be32(0)                  # pea (name).L
         + be16(0x6100, 0)                       # bsr.w stub
         + be16(0x23C0) + be32(400)
         + be16(0x2C79) + be32(400)
         + be16(0x4EAE, 0xFFE2) + be16(0x4E75))
_stub_at = len(_main)                            # the island sits after main
_island = be16(0x4EFA, 2)                        # jmp 2(pc) -> the wrapper
# The bsr occupies bytes 6..9; its displacement is measured from byte 8.
CODE_WRAP = (_main[:6] + be16(0x6100, _stub_at - 8) + _main[10:]
             + _island + _wrap)
v, offs, _t = scan_blob(build(CODE_WRAP, relocs=[2]))
check("wrapper behind a jump island", (v, offs), ('OK', [-30]))


# ---- the small-data bias, derived from a second library -------------------
# a4 points into the middle of the merged data segment, so a name displacement
# is the string's offset MINUS a constant.  The bias is only accepted when a
# SECOND library confirms it, which is what stops it being a fitted constant.
DATA2 = b'bsdsocket.library\x00' + b'\x00' * 2 + b'dos.library\x00'
CODE_BIAS = (be16(0x43EC, 0xFF9C)                # lea -100(a4),a1  (0 - -100)
             + be16(0x2C78, 0x0004) + be16(0x4EAE, 0xFDD8)
             + be16(0x23C0) + be32(400)
             + be16(0x43EC, 0xFFB0)              # lea -80(a4),a1   (20 - -80)
             + be16(0x2C78, 0x0004) + be16(0x4EAE, 0xFDD8)
             + be16(0x2C79) + be32(400)
             + be16(0x4EAE, 0xFFE2) + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_BIAS, data=DATA2))
check("small-data bias", (v, offs), ('OK', [-30]))


# ---- THE WRONG-BASE TEST, and it is the -954 retraction ------------------
# DOSBase sits four bytes from SocketBase in Atalkd's globals block.  v13 put
# it in the base set, so `jsr -954(a6)` -- dos.library VPrintf -- was published
# as a bsdsocket vector nine slots past the end of our table, in 14 binaries.
# Nothing may attribute a call through a base this scanner did not see stored
# from a NAMED open.
CODE_DOS = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
            + be16(0x4EAE, 0xFDD8)
            + be16(0x23C0) + be32(400)           # SocketBase
            + be16(0x2C79) + be32(400)
            + be16(0x4EAE, 0xFFE2)               # socket -- ours
            + be16(0x2C79) + be32(404)           # DOSBase, four bytes away
            + be16(0x4EAE, 0xFC46)               # jsr -954(a6) == VPrintf
            + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_DOS, relocs=[2]))
check("adjacent DOSBase is not ours", (v, offs), ('OK', [-30]))


# ---- a function-local stack slot -----------------------------------------
# AmiBabel stores the named result at 0x490(sp), crosses branches and calls,
# then reloads that slot into a6.  The raw displacement is NOT global: the
# identical 8(sp) in the following function must not inherit the first one's
# provenance.
CODE_STACK = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
              + be16(0x4EAE, 0xFDD8)
              + be16(0x2F40, 0x0008)           # move.l d0,8(sp)
              + be16(0x2C6F, 0x0008)           # movea.l 8(sp),a6
              + be16(0x4EAE, 0xFFE2)           # socket -- ours
              + be16(0x4E75)
              + be16(0x2C6F, 0x0008)           # another function's 8(sp)
              + be16(0x4EAE, 0xFF88)           # must not be attributed
              + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_STACK, relocs=[2]))
check("function-scoped stack base", (v, offs), ('OK', [-30]))


# ---- a relocated library-name table -------------------------------------
# SAS/C startup tables load a pointer from a stable a4 cell into a1.  The data
# relocation proves that cell points at our exact name; the zero beside it is
# deliberately not assumed to be the base slot.  Only the explicit d0 store
# establishes that second cell as SocketBase.
_table_head = (be16(0x49F9) + be32(0)            # lea data:0,a4
               + be16(0x226C, 0x0000)           # movea.l 0(a4),a1
               + be16(0x2C78, 0x0004)
               + be16(0x4EAE, 0xFDD8)
               + be16(0x2940, 0x0004)           # move.l d0,4(a4)
               + be16(0x2C6C, 0x0004)
               + be16(0x4EAE, 0xFFE2)
               + be16(0x4E75))
_table_name_at = len(_table_head)
CODE_TABLE = _table_head + NAME
v, offs, _t = scan_blob(build(CODE_TABLE, data=be32(_table_name_at, 0),
                              relocs=[2], data_relocs=[0]))
check("relocated name-pointer table", (v, offs), ('OK', [-30]))


# ---- an offset-zero field of a live object -------------------------------
# RegistrationUtility allocates an object into a5, stores SocketBase at
# (a5), then reloads it twice.  The next function reuses the encoding (a5),
# but that must not inherit the first function's object identity.
CODE_OBJECT = (be16(0x43F9) + be32(0) + be16(0x2C78, 0x0004)
               + be16(0x4EAE, 0xFDD8)
               + be16(0x2A80)                   # move.l d0,(a5)
               + be16(0x2C55)                   # movea.l (a5),a6
               + be16(0x4EAE, 0xFFE2)
               + be16(0x4E75)
               + be16(0x2C55)                   # another function's (a5)
               + be16(0x4EAE, 0xFF88)
               + be16(0x4E75))
v, offs, _t = scan_blob(build(CODE_OBJECT, relocs=[2]))
check("function-scoped indirect object", (v, offs), ('OK', [-30]))


# ---- a transient pointer walking a {base,name} table ---------------------
# The opener receives a2 from its caller, so the store has no stable address
# locally.  The field relation is nevertheless proven by code, the name cell
# by relocation, and the base cell by an exact a6 load elsewhere.
_walk_head = (be16(0x49F9) + be32(0)             # stable a4 origin
              + be16(0x226A, 0x0004)            # movea.l 4(a2),a1
              + be16(0x2C78, 0x0004)
              + be16(0x4EAE, 0xFDD8)
              + be16(0x2480)                    # move.l d0,(a2)
              + be16(0x4E75)
              + be16(0x45EC, 0x0000)            # lea 0(a4),a2
              + be16(0x2C52)                    # movea.l (a2),a6
              + be16(0x4EAE, 0xFFE2)
              + be16(0x4E75))
_walk_name_at = len(_walk_head)
CODE_TABLE_WALK = _walk_head + NAME
v, offs, _t = scan_blob(build(CODE_TABLE_WALK,
                              data=be32(0, _walk_name_at),
                              relocs=[2], data_relocs=[4]))
check("transient table walker", (v, offs), ('OK', [-30]))

# Relocated adjacency and a later a6 load are not enough without code proving
# the table's field relation.
_no_walk_head = (be16(0x49F9) + be32(0)
                 + be16(0x2C6C, 0x0000)
                 + be16(0x4EAE, 0xFFE2)
                 + be16(0x4E75))
_no_walk_name_at = len(_no_walk_head)
v, offs, _t = scan_blob(build(_no_walk_head + NAME,
                              data=be32(0, _no_walk_name_at),
                              relocs=[2], data_relocs=[4]))
check("table adjacency without opener proof", offs, [])


if fails:
    for f in fails:
        print(f"scan_fixture=FAIL {f}")
    sys.exit(1)
print("scan_fixture=PASS 47 fixtures: call shapes, tail call, rebound a6, "
      "data hunks, a6 via d0 and via a register hop with both clobbers, "
      "scan() end to end incl SOCK_RAW, one pinned limitation, and the six "
      "v14 shapes: OldOpenLibrary, register-held base, opener function, "
      "wrapper behind a jump island, derived small-data bias, adjacent DOSBase, "
      "function-scoped stack base, relocated name-pointer table, "
      "function-scoped indirect object, transient table walker and its "
      "adjacency-only rejection, direct non-a6 base call, and direct helper "
      "argument propagation with its rebound and short-bsr rejections, and "
      "68020 full-extension globals with active-index and short-opener "
      "rejections, full-extension MOVE.W, CLR, TST and Bcc operand-boundary "
      "guards, immediate-operation, ADDA/SUBA/CMPA, general ALU, ADDQ/SUBQ and Scc operands, forward-BRA inline data, aligned opener recovery, returned base-object provenance and rejection, selective MOVEM restoration, "
      "and EXT/MOVEM disambiguation")
