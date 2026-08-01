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
| every longword was zero -- the interchangeability the C caller relies on (see
| n68k_checksum.c).
|
| Hand written, measured against what the compiler emits.  Three spellings of
| the same loop were compiled with GCC 15.2 -O2 -m68020 and disassembled before
| this was written:
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
| variable.  The fallback in n68k_checksum.c is the middle spelling and is
| within ~2x, so this file is worth about that much.
|
| 68060: nothing here is 68060-hostile -- add.l/addx.l are implemented on every
| 68k -- but the file is built under the same AMINETXDUO_NET68K_ASM guard
| src/crypto68k/ uses for its own assembly, and that guard must never be
| enabled for a 68060 target, because crypto68k's MULU.L 32x32 traps to vector
| 61 there.  One switch covers all the hand-written 68020 code.
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


| ULONG n68k_copy_sum_longwords(ULONG *to, const ULONG *from, ULONG count);
|
| Copies count longwords and returns the same sum n68k_sum_longwords() would
| have returned over the source.  Both pointers must be longword aligned.
|
| The point is the second read.  Separately, a copy is 16 bus accesses per 32
| bytes (83.4 cycles on a 14.19 MHz 68020) and the sum is 8 more reads plus 16
| add.l/addx.l (91.4) -- 174.8 in total.  Melded, the loads the copy already
| does feed the adds and the second pass over the payload disappears: 133.1
| cycles, 23.9% off the pair.  See docs/BACKLOG.md; feeding the standalone sum
| from movem.l instead was measured and bought nothing, because that pass is
| half ALU and both spellings issue the same eight reads.
|
| a0 = to, a1 = from, d0 = accumulator, d1 = zero, d2 = count, d3 = blocks,
| d4-d7/a2-a5 = the eight longwords in flight.
        .globl  _n68k_copy_sum_longwords
_n68k_copy_sum_longwords:
        movem.l d2-d7/a2-a5,sp@-        | ten registers, 40 bytes
        movea.l sp@(44),a0
        movea.l sp@(48),a1
        move.l  sp@(52),d2

        moveq   #0,d0
        moveq   #0,d1

        move.l  d2,d3
        lsr.l   #3,d3                   | blocks of eight longwords
        beq     .Lcs_tail

.Lcs_block:
        movem.l a1@+,d4-d7/a2-a5
        movem.l d4-d7/a2-a5,a0@
        lea     a0@(32),a0              | MOVEM to memory has no (An)+ mode
        add.l   d4,d0
        addx.l  d1,d0
        add.l   d5,d0
        addx.l  d1,d0
        add.l   d6,d0
        addx.l  d1,d0
        add.l   d7,d0
        addx.l  d1,d0
        add.l   a2,d0
        addx.l  d1,d0
        add.l   a3,d0
        addx.l  d1,d0
        add.l   a4,d0
        addx.l  d1,d0
        add.l   a5,d0
        addx.l  d1,d0
        subq.l  #1,d3
        bne     .Lcs_block

.Lcs_tail:
        andi.l  #7,d2                   | the 0..7 longword remainder
        beq     .Lcs_done

.Lcs_one:
        move.l  a1@+,d4
        move.l  d4,a0@+
        add.l   d4,d0
        addx.l  d1,d0
        subq.l  #1,d2
        bne     .Lcs_one

.Lcs_done:
        movem.l sp@+,d2-d7/a2-a5
        rts

#endif /* AMINETXDUO_NET68K_ASM */
