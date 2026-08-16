| AmiNetXDuo, crypto68k limb multiply-accumulate, 68060 assembly.
|
|   c68k_prim.S is 68020 code and stays that way.  Two of the instructions it
|   is built on, MULU.L's 32x32 -> 64 form and DIVU.L, were dropped from the
|   68060: they trap to vector 61 and 68060.library emulates them, so that file
|   is a pessimisation there and AMINETXDUO_CRYPTO68K_ASM is off for a 68060
|   build.  The whole module then falls back to the portable C, and the C
|   spends two thirds of a TLS 1.3 transfer in one function.
|
|   This file is that one function and nothing else.  The 68060 does implement
|   MULU.W (16x16 -> 32) and MULU.L's 32x32 -> 32 form, so the product is built
|   from four MULU.W the way c68k_wide_mul() in c68k_prim.c builds it.  The
|   arithmetic is therefore the same as the arithmetic of the C.  What the C
|   cannot do is keep it in registers.  From objdump of the 68060 object:
|
|       move.l  d1,40(sp)       | spill, in the loop
|       add.l   40(sp),d1       | reload
|       move.w  62(sp),d7       | `a`, loop-invariant, reloaded every limb
|       moveq   #16,d4          | the shift constant, rematerialised every limb
|
|   with movem.l d2-d7/a2-a5 at the top: ten registers saved and the loop still
|   goes to the stack.  Below, the two halves of `a`, the carry, the zero the
|   ADDX sinks need and both pointers are live in registers for the whole loop,
|   and nothing touches the stack between entry and exit.
|
| Per limb
|
|   With a = ah:al and b = bh:bl as 16-bit halves,
|
|       a*b = (ah*bh << 32) + ((ah*bl + al*bh) << 16) + (al*bl)
|
|   T = ah*bl + al*bh is 33 bits, so its carry out belongs at bit 48, which is
|   bit 16 of the high limb.  SUBX Dn,Dn turns the X flag into 0 or -1 without
|   a second register, AND leaves it as 0 or 0x10000, and OR.W drops the high
|   half of T into the low word beside it, so the whole contribution of T to
|   the high limb is one ADD.
|
|   Twenty instructions per limb, no memory traffic but b[j] in and r[j]
|   read-modify-written.
|
| The carry
|
|   Split across a register and the X flag, as in c68k_prim.S.  X is the carry
|   out of the `add.l d3,(a0)+` of the previous limb, which has the weight of
|   bit 0 of this limb, so ADDX folds it into the first add for free.  The
|   register
|   and X are worth at most 2^32-1 together, by induction on
|
|       a*b[j] + (carry + X) + r[j]  <=  (2^32-1)^2 + 2*(2^32-1)  =  2^64-1
|
|   so nothing below can carry out of the high limb.
|
|   Nothing between the deferred X and the ADDX that consumes it is allowed to
|   disturb it.  MOVE, MOVEM, MOVEQ, SWAP, CLR, AND, OR, MULU, BTST, CMP and
|   CMPA all leave X alone.  Only the add/subtract/shift group touches it,
|   which is why the loop is closed with CMPA/BNE against a precomputed end
|   pointer rather than with SUB/BNE.
|
| Two phases
|
|   The loop is unrolled by two with the carry alternated between d0 and d7,
|   which is the structure of c68k_prim.S and of GMP before it: phase A reads
|   its carry from d0 and leaves the next one in d7, phase B the other way
|   round, so no limb spends an instruction on a move of the carry into place.
|   Each phase reuses its own spent carry register as the scratch the T split
|   needs, which is what makes eight data registers enough.  An odd n enters at
|   phase B.
|
| Register rules (m68k-amigaos, stack calling convention)
|
|   Arguments on the stack from 4(sp) upward; return value in d0; d0/d1/a0/a1
|   are scratch and d2-d7/a2-a6 must be preserved.
|
| SPDX-License-Identifier: MIT

#ifdef C68K_ASM_MULW

#include "c68k_variant.h"

        .text
        .align  2

| ---------------------------------------------------------------------------
| c68k_limb c68k_addmul_1(c68k_limb *r, const c68k_limb *b, UINT n,
|                         c68k_limb a)
|
|   r[0..n-1] += a * b[0..n-1].  Returns the carry out.
|
| Seven registers saved, so the arguments sit 28 bytes further up than the
| return address: r at 32(sp), b at 36(sp), n at 40(sp), a at 44(sp).
| ---------------------------------------------------------------------------

        .globl  C68K_MV_SYM(_c68k_addmul_1)
C68K_MV_SYM(_c68k_addmul_1):
        movem.l d2-d7/a2,-(sp)          | 7 longs
        move.l  40(sp),d0               | n, and MOVE sets Z for the test
        beq     Lam60_zero
        move.l  32(sp),a0               | r
        move.l  36(sp),a1               | b
        move.l  44(sp),d2               | a; its low word is a_lo
        move.l  d2,d5
        swap    d5                      | d5's low word is a_hi
        lsl.l   #2,d0                   | limbs to bytes
        lea     (a1,d0.l),a2            | one past the last b limb
        moveq   #0,d0                   | carry, phase A
        moveq   #0,d1                   | the hard zero the ADDX sinks need
        moveq   #0,d7                   | carry, phase B
        add.l   d1,d1                   | X = 0; d1 is 0 and stays 0
        btst    #0,43(sp)               | n odd?  (the low byte of n)
        bne     Lam60_half

| ------------------------------------------------------------- phase A ----
| carry in d0, carry out d7.  d0 is dead after the first ADDX and becomes the
| scratch the T split needs.

Lam60_loop:
        move.l  (a1)+,d3                | b[j]
        move.l  d3,d6
        move.l  d3,d4
        swap    d4                      | d4's low word is b_hi
        move.l  d4,d7
        mulu.w  d2,d3                   | al*bl
        mulu.w  d5,d6                   | ah*bl
        mulu.w  d2,d4                   | al*bh
        mulu.w  d5,d7                   | ah*bh
        addx.l  d0,d3                   | low  = al*bl + carry + X
        addx.l  d1,d7                   | high = ah*bh + the carry out of that
        add.l   d6,d4                   | T = ah*bl + al*bh; X = T's bit 32
        subx.l  d0,d0                   | d0 = -X
        and.l   #0x10000,d0             | d0 = X << 16
        swap    d4                      | d4 = T_lo:T_hi
        or.w    d4,d0                   | d0 = T's whole high-limb share
        clr.w   d4                      | d4 = T_lo << 16
        add.l   d4,d3                   | low += it; X = the carry
        addx.l  d0,d7                   | high += T's share + that carry
        add.l   d3,(a0)+                | r[j] += low; X carries to the next limb

| ------------------------------------------------------------- phase B ----
| The same, with d0 and d7 exchanged.  An odd n starts here.

Lam60_half:
        move.l  (a1)+,d3
        move.l  d3,d6
        move.l  d3,d4
        swap    d4
        move.l  d4,d0
        mulu.w  d2,d3
        mulu.w  d5,d6
        mulu.w  d2,d4
        mulu.w  d5,d0
        addx.l  d7,d3
        addx.l  d1,d0
        add.l   d6,d4
        subx.l  d7,d7
        and.l   #0x10000,d7
        swap    d4
        or.w    d4,d7
        clr.w   d4
        add.l   d4,d3
        addx.l  d7,d0
        add.l   d3,(a0)+
        cmpa.l  a2,a1                   | CMPA leaves X alone
        bne     Lam60_loop

        moveq   #0,d3                   | MOVEQ leaves X alone
        addx.l  d3,d0                   | fold the last memory carry in
        movem.l (sp)+,d2-d7/a2
        rts

Lam60_zero:
        moveq   #0,d0
        movem.l (sp)+,d2-d7/a2
        rts

#endif /* C68K_ASM_MULW */
