| AmiNetXDuo -- one's-complement longword sum, 68020.
|
| ULONG n68k_sum_longwords(const ULONG *p, ULONG count);
|
| Two instructions per longword:
|
|       add.l   (a0)+,d0        | 32-bit add, sets X on carry out
|       addx.l  d2,d0           | d2 is zero, so this adds the carry back in
|
| add.l reloads X every time, so the chain cannot leak a carry from the addx
| into the following add.  The result is the one's-complement 32-bit sum, which
| is congruent to the plain 16-bit sum modulo 0xFFFF and is zero only when
| every longword was zero -- exactly the interchangeability the C caller relies
| on (see n68k_checksum.c).
|
| WHY THIS IS HAND WRITTEN, measured against what the compiler emits.  Three
| spellings of the same loop were compiled with GCC 15.2 -O2 -m68020 and
| disassembled before any of this was written:
|
|   64-bit accumulator   movea.l (a2)+,a1 / suba.l a0,a0 / move.l a0,d1 /
|                        add.l a1,d5 / addx.l d1,d4 / dbf   -- 5 + dbf, and it
|                        rebuilds the zero register on every iteration
|   explicit carry test  movea.l (a1)+,a0 / add.l a0,d0 / cmp.l a0,d0 /
|                        bcc.s / addq.l #1,d0 / dbf         -- 4-5 + dbf
|   the vendored form    move.l (a1)+,d1 / move.l d1,d0 / andi.l #65535,d0 /
|                        clr.w d1 / swap d1 / add.l d0,d1 /
|                        adda.l d1,a0 / dbf                 -- 7 + dbf
|
| None of them reaches two, because C cannot name the carry flag and cannot
| fold the load into the add's addressing mode once the value has to become a
| variable.  That is the whole justification; the fallback in n68k_checksum.c
| is the middle spelling and is within ~2x, so this file is worth about that
| much and no more.
|
| 68060 NOTE: nothing here is 68060-hostile -- add.l/addx.l are fully
| implemented on every 68k -- but the file is built under the same
| AMINETXDUO_NET68K_ASM guard src/crypto68k/ uses for its own assembly, and
| that guard must never be enabled for a 68060 target, because crypto68k's
| MULU.L 32x32 traps to vector 61 there.  Keeping one switch for "the hand
| written 68020 code" is deliberate.
|
| SPDX-License-Identifier: MIT

#ifdef AMINETXDUO_NET68K_ASM

        .text
        .even
        .globl  _n68k_sum_longwords

| a0 = p, d1 = count, d0 = accumulator, d2 = zero, d3 = scratch
_n68k_sum_longwords:
        movem.l d2-d3,sp@-
        movea.l sp@(12),a0
        move.l  sp@(16),d1

        moveq   #0,d0
        moveq   #0,d2

        move.l  d1,d3
        andi.l  #7,d3                   | the 0..7 longword tail
        lsr.l   #3,d1                   | groups of eight
        beq     .Ltail

.Loop8:
        add.l   a0@+,d0
        addx.l  d2,d0
        add.l   a0@+,d0
        addx.l  d2,d0
        add.l   a0@+,d0
        addx.l  d2,d0
        add.l   a0@+,d0
        addx.l  d2,d0
        add.l   a0@+,d0
        addx.l  d2,d0
        add.l   a0@+,d0
        addx.l  d2,d0
        add.l   a0@+,d0
        addx.l  d2,d0
        add.l   a0@+,d0
        addx.l  d2,d0
        subq.l  #1,d1
        bne     .Loop8

.Ltail:
        tst.l   d3
        beq     .Ldone

.Loop1:
        add.l   a0@+,d0
        addx.l  d2,d0
        subq.l  #1,d3
        bne     .Loop1

.Ldone:
        movem.l sp@+,d2-d3
        rts

#endif /* AMINETXDUO_NET68K_ASM */
