| AmiNetXDuo, crypto68k limb primitives, 68020 assembly.
|
|   GCC 15.2 does emit MULU.L for the portable C.  Checked, not assumed:
|   nx_crypto_huge_number.c built -O2 -m68020 disassembles to
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
|   (To repeat this: the default architecture of objdump is plain 68000, which
|   cannot decode MULU.L and prints it as `.short 0x4c06`.  Pass
|   `-m m68k:68020`, or the disassembly shows no multiply.)
|
|   So there is no __muldi3 helper call to eliminate.  That matters for
|   calibration against the prior art: when Howard Chu measured his 68k bignum
|   assembly at "over 4x faster than gcc -O3" in 2002 (openssl-dev, "M68020
|   bn_asm", https://marc.info/?l=openssl-dev&m=101407286200398&w=2, the code
|   that lives on in AmiSSL as crypto/bn/asm/bn_m68k.s), he was measuring
|   against GCC 2.95, which called a helper.  Against a compiler that emits the
|   instruction, the headroom is far smaller.  Nineteen instructions per limb
|   above (seventeen when the same C is compiled standalone, where `a` survives
|   in a register instead of being spilled) against five and a half below, on a
|   CPU where MULU.L alone is 44 of the ~101 cycles.  Measured on the emulated
|   68020, this file is 1.4x the portable C for a 64-limb multiply-accumulate,
|   and no instruction selection makes it 4x: at 44 cycles of a ~67 cycle
|   loop, the multiply is the floor and everything else is rounding.
|
|   The two-limb unroll with the carry alternated between d0 and d1 is the GMP
|   design, from mpn/m68k/mc68020/aorsmul_1.asm (25 cycles/limb on a 68040).
|   The mpi/m68k/mc68020/mpih-mul1.S of libgcrypt is the same code.  It beats
|   the obvious formulation for two reasons:
|
|     - an alternated carry register removes a MOVE.L per limb.
|     - ADDX.L folds the memory carry (X) of the previous limb into the carry
|       add of this limb, so one instruction does two jobs.
|
|   Nothing between the ADDX chain is allowed to disturb the condition codes,
|   which is why the loop counter is DBRA: it is the only
|   decrement-and-branch on this machine that leaves CCR alone.  MULU.L and
|   MOVE do not affect X, which is what makes the fold legal across the
|   multiply.
|
| Cycle counts (MC68020UM section 8.2.8 / MC68030UM section 11.6.8)
|
|   MULU.L <ea>,Dh:Dl is 41 best / 43 cached / 44 worst on 68020, and 44 max on
|   68030.  It is 20 on a 68040.  On a 68060 the 64-bit form is unimplemented.
|   It traps to vector 61 and is emulated in software, so this file is a
|   pessimisation there and must not be enabled for a 68060 build.  Confirmed
|   elsewhere: AmiSSL shipped the same finding in 4.4 ("disabled m68k asm
|   replacement as it's slower"), and Roberto Ragusa's 1998 PGP 68060 patch got
|   2x by removing MULU.L.  This project's floor is 68020 (docs/RESEARCH.md 9).
|
| Register rules (m68k-amigaos, stack calling convention)
|
|   Arguments on the stack from 4(sp) upward; return value in d0; d0/d1/a0/a1
|   are scratch and d2-d7/a2-a6 must be preserved.
|
| SPDX-License-Identifier: MIT

#ifdef C68K_ASM

#include "c68k_variant.h"

        .text
        .align  2

| ---------------------------------------------------------------------------
| c68k_limb c68k_addmul_1(c68k_limb *r, const c68k_limb *b, UINT n,
|                         c68k_limb a)
|
|   r[0..n-1] += a * b[0..n-1].  Returns the carry out.
|
| The running carry is split across a register (d0 or d1) and the X flag.  The
| pair is always worth at most 2^32-1 together, so
|
|     a*b[j] + (carry_reg + X) + r[j]  <=  (2^32-1)^2 + 2*(2^32-1)  =  2^64-1
|
| and neither ADDX can carry out of the high half.
| ---------------------------------------------------------------------------

        .globl  C68K_MV_SYM(_c68k_addmul_1)
C68K_MV_SYM(_c68k_addmul_1):
        movem.l d2-d5,-(sp)             | 4 longs
        move.l  20(sp),a0               | r
        move.l  24(sp),a1               | b
        move.l  28(sp),d4               | n
        move.l  32(sp),d2               | a
        moveq   #0,d0                   | carry, phase A
        moveq   #0,d1                   | carry, phase B
        moveq   #0,d5                   | a hard zero for the ADDX sinks
        tst.l   d4
        beq.s   Lam_zero

        move.l  d4,d3
        lsr.l   #1,d4                   | d4 = n/2   (LSR sets X, cleared below)
        andi.l  #1,d3                   | d3 = n odd?
        bne.s   Lam_odd

        subq.l  #1,d4                   | even: n/2 - 1 full passes for DBRA
        move.l  d5,d3
        add.l   d5,d3                   | X = 0, and nothing else touches it
        bra.s   Lam_loop

Lam_odd:
        move.l  d5,d3
        add.l   d5,d3                   | X = 0
        bra.s   Lam_half                | one limb, then n/2 full passes

Lam_loop:
        move.l  (a1)+,d3                | b[j], MOVE leaves X alone
        mulu.l  d2,d1:d3                | d1:d3 = a * b[j], as does MULU
        addx.l  d0,d3                   | + carry + X
        addx.l  d5,d1
        add.l   d3,(a0)+                | r[j] += ; X = carry out

Lam_half:
        move.l  (a1)+,d3
        mulu.l  d2,d0:d3                | carry now lands in d0
        addx.l  d1,d3
        addx.l  d5,d0
        add.l   d3,(a0)+
        dbra    d4,Lam_loop             | DBRA leaves CCR alone

        moveq   #0,d3                   | MOVEQ leaves X alone
        addx.l  d3,d0                   | fold the last memory carry in
        movem.l (sp)+,d2-d5
        rts

Lam_zero:
        moveq   #0,d0
        movem.l (sp)+,d2-d5
        rts


| ---------------------------------------------------------------------------
| c68k_limb c68k_add_carry(c68k_limb *dst, const c68k_limb *src, UINT n,
|                          c68k_limb carry)
|
| X is consumed by the ADDX immediately after the ADD that sets it, so the
| SUBQ/BNE loop control here is free to clobber it.
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
| c68k_limb c68k_div_2by1(c68k_limb hi, c68k_limb lo, c68k_limb d,
|                         c68k_limb *rem)
|
|   (hi:lo) / d.  Quotient returned, remainder stored through rem.
|
| One instruction, and the reason this file grew a division at all: the
| portable form is a 64-bit divide, which on this toolchain reaches __udivdi3
| in src/common/ami_udivdi3.c because libgcc.a is empty here.
|
| The caller must check that hi < d.  DIVU.L traps on quotient overflow rather
| than saturating, and Knuth's algorithm D produces exactly one case that
| overflows, the top limb of the partial remainder equal to the divisor, whose
| quotient digit is B-1.  c68k_mod() tests for it and never calls here with it.
|
| DIVU.L is 78 cycles on a 68020 and, like the 32x32->64 form of MULU.L, is
| unimplemented on a 68060.  It traps to vector 61 and is emulated.  Same
| guard, same rule: AMINETXDUO_CRYPTO68K_ASM must never be enabled for a 68060
| build.
|
| d0, d1 and a0 are scratch in this ABI, so nothing needs saving.
| ---------------------------------------------------------------------------

        .globl  C68K_MV_SYM(_c68k_div_2by1)
C68K_MV_SYM(_c68k_div_2by1):
        move.l  4(sp),d1                | hi, the remainder half
        move.l  8(sp),d0                | lo, the quotient half
        divu.l  12(sp),d1:d0            | d0 = quotient, d1 = remainder
        move.l  16(sp),a0
        move.l  d1,(a0)
        rts


| ---------------------------------------------------------------------------
| c68k_limb c68k_add(c68k_limb *r, const c68k_limb *b, UINT n)
|
|   r += b, returns the carry out.  The mirror of c68k_sub below, and DBRA for
|   the same reason: it is the only decrement-and-branch that leaves the CCR
|   alone, so the X flag survives from the ADDX of one limb to the next.
|
|   Added for Karatsuba.  The recombination is four passes of add and subtract
|   over half-width operands and nothing else, so on the 64-limb squaring it
|   runs about 320 limbs per call, small against the 1,108 limb products the
|   split removes, but not small enough to leave to the compiler, which spends
|   five instructions and a branch where this needs two.
| ---------------------------------------------------------------------------

        .globl  _c68k_add
_c68k_add:
        movem.l d2-d3,-(sp)
        move.l  12(sp),a0               | r
        move.l  16(sp),a1               | b
        move.l  20(sp),d0               | n
        tst.l   d0
        beq.s   Ladd_zero
        subq.l  #1,d0                   | DBRA counts down to -1
        moveq   #0,d2
        add.l   d2,d2                   | X = 0

Ladd_loop:
        move.l  (a1)+,d2                | MOVE does not disturb X
        move.l  (a0),d3
        addx.l  d2,d3
        move.l  d3,(a0)+
        dbra    d0,Ladd_loop

        moveq   #0,d0
        addx.l  d0,d0                   | d0 = 0 + 0 + X = the carry out
        movem.l (sp)+,d2-d3
        rts

Ladd_zero:
        moveq   #0,d0
        movem.l (sp)+,d2-d3
        rts


| ---------------------------------------------------------------------------
| c68k_limb c68k_sub(c68k_limb *r, const c68k_limb *b, UINT n)
|
|   r -= b, returns the borrow out.  DBRA again, for the same CCR reason.
| ---------------------------------------------------------------------------

        .globl  _c68k_sub
_c68k_sub:
        movem.l d2-d3,-(sp)
        move.l  12(sp),a0               | r
        move.l  16(sp),a1               | b
        move.l  20(sp),d0               | n
        tst.l   d0
        beq.s   Lsub_zero
        subq.l  #1,d0                   | DBRA counts down to -1
        moveq   #0,d2
        sub.l   d2,d2                   | X = 0

Lsub_loop:
        move.l  (a1)+,d2
        move.l  (a0),d3
        subx.l  d2,d3
        move.l  d3,(a0)+
        dbra    d0,Lsub_loop

        moveq   #0,d0
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
        move.l  -(a1),d0                | d0 = b[j]
        cmp.l   -(a0),d0                | d0 - a[j]
        bne.s   Lcmp_diff
        subq.l  #1,d1
        bne.s   Lcmp_loop
        moveq   #0,d0
        rts

Lcmp_diff:
        bcs.s   Lcmp_greater            | borrow means b[j] < a[j]
        moveq   #-1,d0
        rts

Lcmp_greater:
        moveq   #1,d0

Lcmp_done:
        rts

#endif /* C68K_ASM */
