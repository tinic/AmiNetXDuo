| AmiNetXDuo, crypto68k: P-256 carry chains, 68020 assembly.
|
|   The sibling RSA work established the rule that matters on this machine:
|   GCC 15.2 emits MULU.L, so hand-written multiply loops buy 1.4x and not 4x.
|   That still holds, c68k_p256.c's field multiply calls c68k_addmul_1 and is
|   within a whisker of the MULU.L floor, and nothing here touches it.
|
|   Carry propagation is a different case, per the disassembly.  GCC cannot
|   express "add, then add the carry into the next word" as ADD.L followed by
|   ADDX.L, because C has no carry flag: the source has to say
|   `lo += v; if (lo < v) hi++;`, and -O2 -m68020 turns each such term into
|
|       movel   a2@(36),d2
|       addal   d2,a1
|       cmpal   d2,a1
|       bccs    .+4
|       addql   #1,d0
|
|, five instructions and a branch where the machine has two.  Rewriting the
|   C as a 64-bit accumulator does not fix it: GCC then emits a CLR.L for a
|   fresh zero register per term instead of hoisting one, and spills to the
|   stack besides.  Both forms were compiled and disassembled before this file
|   was written; neither reaches the ADD.L/ADDX.L pair.
|
|   The Solinas reduction for P-256 is sixty-three such terms.  Measured on the
|   emulated 68020: the C version runs it in 127 us, against 469 us for the
|   64 MULU.L of a field multiply, 26% of a field multiply.  This file takes
|   that 127 us to 84 us, which is 1.5x on the reduction, 449 us against 489 us
|   on a field multiply, and 1.13x on every elliptic curve operation above it
|   (an ECDSA verify went from 2.209 s to 1.961 s).
|
|   1.13x is the whole claim; the 3.8x that c68k_p256.c gets comes from
|   dropping NX_CRYPTO_HUGE_NUMBER, not from this file.  Before hand-writing
|   more of the module, note that the field multiply is now 74% MULU.L, so the
|   remaining ceiling from assembly is about 1.2x, most of it the per-call
|   overhead of the eight c68k_addmul_1 invocations rather than the instruction
|   selection inside them.
|
|   The MOVE instructions inside the ADDX chain are safe: on m68k, MOVE and
|   MOVEQ set N and Z but leave X alone, and only arithmetic instructions write
|   X.  So a limb's result can be stored, and the next limb's operand loaded,
|   mid-carry-chain without a save-and-restore.  Scc likewise does not touch
|   the CCR.
|
| Register rules (m68k-amigaos, stack calling convention)
|
|   Arguments on the stack from 4(sp) upward; return value in d0; d0/d1/a0/a1
|   are scratch and d2-d7/a2-a6 must be preserved.
|
| The 68060 rule applies to the module, not to this file
|
|   Nothing here uses MULU.L, so this file alone would be safe on a 68060.  It
|   is still under the same AMINETXDUO_CRYPTO68K_ASM guard as c68k_prim.S,
|   because the module is built as one unit and c68k_prim.S must never be
|   enabled for an 060: MULU.L's 32x32->64 form is unimplemented there, traps
|   to vector 61 and is emulated.  One guard, one rule.
|
| SPDX-License-Identifier: MIT

#ifdef C68K_ASM

        .text

| -----------------------------------------------------------------------------
| c68k_limb c68k_p256_add_raw(c68k_limb *r, const c68k_limb *a,
|                             const c68k_limb *b)
|
| r[0..7] = a[0..7] + b[0..7], returns the carry out (0 or 1).
| r may alias a or b.
| -----------------------------------------------------------------------------

        .globl  _c68k_p256_add_raw
_c68k_p256_add_raw:
        move.l  a2,-(sp)
        move.l  8(sp),a0                | r
        move.l  12(sp),a1               | a
        move.l  16(sp),a2               | b

        move.l  (a1)+,d0
        move.l  (a2)+,d1
        add.l   d1,d0                   | first limb: plain ADD, seeds X
        move.l  d0,(a0)+

        .rept   7
        move.l  (a1)+,d0                | MOVE does not disturb X
        move.l  (a2)+,d1
        addx.l  d1,d0
        move.l  d0,(a0)+
        .endr

        moveq   #0,d0
        addx.l  d0,d0                   | d0 = 0 + 0 + X = the carry out
        move.l  (sp)+,a2
        rts


| -----------------------------------------------------------------------------
| c68k_limb c68k_p256_sub_raw(c68k_limb *r, const c68k_limb *a,
|                             const c68k_limb *b)
|
| r[0..7] = a[0..7] - b[0..7], returns the borrow out (0 or 1).
| r may alias a or b.
| -----------------------------------------------------------------------------

        .globl  _c68k_p256_sub_raw
_c68k_p256_sub_raw:
        move.l  a2,-(sp)
        move.l  8(sp),a0                | r
        move.l  12(sp),a1               | a
        move.l  16(sp),a2               | b

        move.l  (a1)+,d0
        move.l  (a2)+,d1
        sub.l   d1,d0                   | d0 = a - b
        move.l  d0,(a0)+

        .rept   7
        move.l  (a1)+,d0
        move.l  (a2)+,d1
        subx.l  d1,d0
        move.l  d0,(a0)+
        .endr

        moveq   #0,d0
        addx.l  d0,d0                   | X holds the borrow; ADDX extracts it
        move.l  (sp)+,a2
        rts


| -----------------------------------------------------------------------------
| INT c68k_p256_reduce_core(c68k_limb *r, const c68k_limb *t)
|
| The Solinas reduction of FIPS 186-4 D.2.3, collapsed to one pass over the
| eight output limbs.  Writes r[0..7] and returns the leftover signed
| coefficient of 2^256; the caller folds it in with conditional additions and
| subtractions of p.  Measured to stay within [-4, 4].
|
| The term lists are the same ones c68k_p256.c documents and the C fallback
| implements, and were verified against Python's arbitrary precision
| arithmetic over 20000 random 512-bit inputs before either was written.
|
|   d0  lo, the limb being accumulated
|   d1  hi, the signed carry into the next limb
|   d2  a permanent zero, so ADDX/SUBX can add or subtract X alone
|   a0  r, post-incremented
|   a1  t
| -----------------------------------------------------------------------------

| One term.  o is a byte offset into t: 4*i for limb i.
.macro  PADD    o
        add.l   \o(a1),d0
        addx.l  d2,d1
.endm

.macro  PSUB    o
        sub.l   \o(a1),d0
        subx.l  d2,d1
.endm

| Store the finished limb and shift the signed carry down into lo.
| MOVE.L sets N from hi, Scc reads it, and neither writes X, immaterial here
| because every limb below starts with a plain ADD.L.
.macro  PNEXT
        move.l  d0,(a0)+
        move.l  d1,d0
        smi     d1
        extb.l  d1
.endm

        .globl  _c68k_p256_reduce_core
_c68k_p256_reduce_core:
        movem.l d2-d3,-(sp)
        move.l  12(sp),a0               | r
        move.l  16(sp),a1               | t
        moveq   #0,d2
        moveq   #0,d0
        moveq   #0,d1

        | limb 0:  c0 + c8 + c9 - c11 - c12 - c13 - c14
        PADD    0
        PADD    32
        PADD    36
        PSUB    44
        PSUB    48
        PSUB    52
        PSUB    56
        PNEXT

        | limb 1:  c1 + c9 + c10 - c12 - c13 - c14 - c15
        PADD    4
        PADD    36
        PADD    40
        PSUB    48
        PSUB    52
        PSUB    56
        PSUB    60
        PNEXT

        | limb 2:  c2 + c10 + c11 - c13 - c14 - c15
        PADD    8
        PADD    40
        PADD    44
        PSUB    52
        PSUB    56
        PSUB    60
        PNEXT

        | limb 3:  c3 + 2*c11 + 2*c12 + c13 - c15 - c8 - c9
        PADD    12
        PADD    44
        PADD    44
        PADD    48
        PADD    48
        PADD    52
        PSUB    60
        PSUB    32
        PSUB    36
        PNEXT

        | limb 4:  c4 + 2*c12 + 2*c13 + c14 - c9 - c10
        PADD    16
        PADD    48
        PADD    48
        PADD    52
        PADD    52
        PADD    56
        PSUB    36
        PSUB    40
        PNEXT

        | limb 5:  c5 + 2*c13 + 2*c14 + c15 - c10 - c11
        PADD    20
        PADD    52
        PADD    52
        PADD    56
        PADD    56
        PADD    60
        PSUB    40
        PSUB    44
        PNEXT

        | limb 6:  c6 + 3*c14 + 2*c15 + c13 - c8 - c9
        PADD    24
        PADD    56
        PADD    56
        PADD    56
        PADD    60
        PADD    60
        PADD    52
        PSUB    32
        PSUB    36
        PNEXT

        | limb 7:  c7 + 3*c15 + c8 - c10 - c11 - c12 - c13
        PADD    28
        PADD    60
        PADD    60
        PADD    60
        PADD    32
        PSUB    40
        PSUB    44
        PSUB    48
        PSUB    52
        move.l  d0,(a0)

        move.l  d1,d0                   | the leftover 2^256 coefficient
        movem.l (sp)+,d2-d3
        rts

#endif /* C68K_ASM */
