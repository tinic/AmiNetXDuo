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


def build(code, data=NAME, relocs=None):
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

if fails:
    for f in fails:
        print(f"scan_fixture=FAIL {f}")
    sys.exit(1)
print("scan_fixture=PASS 12 fixtures: call shapes, tail call, rebound a6, "
      "data hunks, a6 via d0 and via a register hop with both clobbers, "
      "scan() end to end incl SOCK_RAW, one pinned limitation")
