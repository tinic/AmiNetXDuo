| AmiNetXDuo -- crypto68k limb primitives, 68020 assembly.
|
| WHAT THE COMPILER WAS ALREADY DOING
|
|   GCC 15.2 does emit MULU.L for the portable C.  This was checked, not
|   assumed: nx_crypto_huge_number.c built -O2 -m68020 disassembles to
|
|       1aea:  2c19       movel a1@+,d6         | y[j]
|       1aec:  2a2f 007c  movel sp@(124),d5     | xi, reloaded every iteration
|       1af0:  4c06 5404  mulul d6,d4,d5        | <-- 32x32 -> 64, correct
|       1af4:  2650       moveal a0@,a3         | r[j]
|       1af6:  95ca       subal a2,a2           | a2 = 0
|       1af8:  2c0a       movel a2,d6
|       1afa:  2e0b       movel a3,d7
|       1afc:  de85       addl d5,d7
|       1afe:  dd84       addxl d4,d6
|       1b00:  2406       movel d6,d2
|       1b02:  2607       movel d7,d3
|       1b04:  d680       addl d0,d3
|       1b06:  4082       negxl d2              | carry-into-high, the long way
|       1b08:  4482       negl d2
|       1b0a:  2002       movel d2,d0
|       1b0c:  2203       movel d3,d1
|       1b0e:  20c1       movel d1,a0@+
|       1b10:  b1cc       cmpal a4,a0
|       1b12:  66d6       bnes 1aea
|
|   (Note for anyone repeating this: the default objdump architecture is plain
|   68000, which cannot decode MULU.L and prints it as `.short 0x4c06`.  Pass
|   `-m m68k:68020` or you will conclude there is no multiply at all.)
|
|   So there is no __muldi3 helper call to eliminate -- the ISA-level question
|   has a good answer.  What is left is nineteen instructions where ten will
|   do: xi is reloaded from the stack every iteration, a zero register is
|   rematerialised inside the loop, and the carry is folded into the high half
|   with a NEGX/NEG pair instead of by choosing the right register in the first
|   place.  Roughly 101 cycles per limb against 75 for the loop below, on a
|   68020 where MULU.L itself is 44 of them.
|
| WHY THE GAIN IS ONLY ~1.3x
|
|   Because MULU.L dominates.  On a 68020/68030 the 32x32 -> 64 multiply is
|   ~44 clocks; everything else in the loop together is ~31.  No amount of
|   instruction selection moves the 44.  That is also why the real wins in this
|   module are algorithmic (see crypto68k.h) and why Karatsuba is not worth it
|   here: it trades multiplies for additions at a ratio that only pays off far
|   above 64 limbs.
|
| REGISTER RULES (m68k-amigaos, stack calling convention)
|
|   Arguments on the stack, 4(sp) upward.  Return value in d0.  d0/d1/a0/a1
|   are scratch; d2-d7 and a2-a6 must be preserved.
|
| SPDX-License-Identifier: MIT

#ifdef C68K_ASM

        .text
        .align  2

| ---------------------------------------------------------------------------
| c68k_limb c68k_mul_acc(c68k_limb *dst, const c68k_limb *src,
|                        const c68k_limb *b, UINT n,
|                        c68k_limb a, c68k_limb carry)
|
|   for j in 0..n-1:  t = src[j] + a*b[j] + carry
|                     dst[j] = low(t); carry = high(t)
|   returns carry
|
| The 64-bit intermediate cannot overflow -- (2^32-1)^2 + 2*(2^32-1) is exactly
| 2^64-1 -- so the two ADDX.L instructions below can never carry out of d3.
| ---------------------------------------------------------------------------

        .globl  _c68k_mul_acc
_c68k_mul_acc:
        movem.l d2-d5/a2,-(sp)          | 5 longs
        move.l  24(sp),a0               | dst
        move.l  28(sp),a1               | src
        move.l  32(sp),a2               | b
        move.l  36(sp),d4               | n
        move.l  40(sp),d0               | a
        move.l  44(sp),d1               | carry
        moveq   #0,d5                   | the zero ADDX needs
        tst.l   d4
        beq.s   Lmac_done

Lmac_loop:
        move.l  (a2)+,d2                | b[j]
        mulu.l  d0,d3:d2                | d3:d2 = a * b[j]
        add.l   d1,d2                   | + carry in
        addx.l  d5,d3
        add.l   (a1)+,d2                | + src[j]
        addx.l  d5,d3
        move.l  d2,(a0)+                | dst[j] = low
        move.l  d3,d1                   | carry = high
        subq.l  #1,d4
        bne.s   Lmac_loop

Lmac_done:
        move.l  d1,d0
        movem.l (sp)+,d2-d5/a2
        rts


| ---------------------------------------------------------------------------
| c68k_limb c68k_add_carry(c68k_limb *dst, const c68k_limb *src, UINT n,
|                          c68k_limb carry)
|
| X is consumed by the ADDX immediately after the ADD that sets it, so the
| SUBQ/BNE loop control below is free to clobber it.
| ---------------------------------------------------------------------------

        .globl  _c68k_add_carry
_c68k_add_carry:
        move.l  d2,-(sp)
        move.l  8(sp),a0                | dst
        move.l  12(sp),a1               | src
        move.l  16(sp),d1               | n
        move.l  20(sp),d0               | carry
        tst.l   d1
        beq.s   Lac_done

Lac_loop:
        move.l  (a1)+,d2
        add.l   d0,d2
        moveq   #0,d0
        addx.l  d0,d0                   | d0 = 0 + 0 + X
        move.l  d2,(a0)+
        subq.l  #1,d1
        bne.s   Lac_loop

Lac_done:
        move.l  (sp)+,d2
        rts


| ---------------------------------------------------------------------------
| c68k_limb c68k_sub(c68k_limb *r, const c68k_limb *b, UINT n)
|
|   r -= b, returns the borrow out.
|
| SUBX chains through X, so nothing between the SUBX instructions may touch
| the condition codes.  DBRA is the only decrement-and-branch on this machine
| that leaves them alone, which is why the counter is 16-bit here; n is a limb
| count of at most a few hundred, so that is not a limitation in practice.
| ---------------------------------------------------------------------------

        .globl  _c68k_sub
_c68k_sub:
        movem.l d2-d3,-(sp)
        move.l  12(sp),a0               | r
        move.l  16(sp),a1               | b
        move.l  20(sp),d0               | n
        tst.l   d0
        beq.s   Lsub_zero
        subq.l  #1,d0                   | DBRA counts to -1
        moveq   #0,d2
        sub.l   d2,d2                   | X = 0

Lsub_loop:
        move.l  (a1)+,d2                | MOVE leaves X alone
        move.l  (a0),d3
        subx.l  d2,d3
        move.l  d3,(a0)+
        dbra    d0,Lsub_loop

        moveq   #0,d0                   | MOVEQ leaves X alone
        addx.l  d0,d0                   | d0 = borrow
        movem.l (sp)+,d2-d3
        rts

Lsub_zero:
        moveq   #0,d0
        movem.l (sp)+,d2-d3
        rts


| ---------------------------------------------------------------------------
| INT c68k_cmp(const c68k_limb *a, const c68k_limb *b, UINT n)
|
|   Unsigned compare, most significant limb first.  -1, 0 or 1.
| ---------------------------------------------------------------------------

        .globl  _c68k_cmp
_c68k_cmp:
        move.l  4(sp),a0                | a
        move.l  8(sp),a1                | b
        move.l  12(sp),d1               | n
        moveq   #0,d0
        tst.l   d1
        beq.s   Lcmp_done
        move.l  d1,d0
        lsl.l   #2,d0
        lea     (a0,d0.l),a0            | one past the top limb
        lea     (a1,d0.l),a1
        moveq   #0,d0

Lcmp_loop:
        move.l  -(a1),d0
        cmp.l   -(a0),d0                | sets flags for a[j] - b[j]
        bne.s   Lcmp_diff
        subq.l  #1,d1
        bne.s   Lcmp_loop
        moveq   #0,d0
        rts

Lcmp_diff:
        bcs.s   Lcmp_greater            | b < a  ->  a is greater
        moveq   #-1,d0
        rts

Lcmp_greater:
        moveq   #1,d0

Lcmp_done:
        rts

#endif /* C68K_ASM */
