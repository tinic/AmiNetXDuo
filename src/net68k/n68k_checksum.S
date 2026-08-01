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

| a0 = p, d1 = count, d0 = accumulator, d2 = zero, d3 = scratch,
| d4-d7/a2-a5 = the eight longwords movem.l brings in at a time.
_n68k_sum_longwords:
        movem.l d2-d7/a2-a5,sp@-        | ten registers, 40 bytes
        movea.l sp@(44),a0
        move.l  sp@(48),d1

        moveq   #0,d0
        moveq   #0,d2

        move.l  d1,d3
        andi.l  #7,d3                   | the 0..7 longword tail
        lsr.l   #3,d1                   | groups of eight
        beq     .Ltail

| The loads come through movem.l rather than the add's own addressing mode.
| add.l a0@+,d0 pays a full operand fetch per longword; movem.l fetches eight
| for about half that each, which is why the copy next door uses it and is why
| a read-and-write copy measured CHEAPER per byte than this read-only sum did
| (183.65 vs 201.39 ns/B, tests/perf on a 68020).  The adds then run
| register-to-register.  add.l An,Dn is a valid long add and sets X, so a3-a5
| carry data here like any other register.
.Loop8:
        movem.l a0@+,d4-d7/a2-a5
        add.l   d4,d0
        addx.l  d2,d0
        add.l   d5,d0
        addx.l  d2,d0
        add.l   d6,d0
        addx.l  d2,d0
        add.l   d7,d0
        addx.l  d2,d0
        add.l   a2,d0
        addx.l  d2,d0
        add.l   a3,d0
        addx.l  d2,d0
        add.l   a4,d0
        addx.l  d2,d0
        add.l   a5,d0
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
        movem.l sp@+,d2-d7/a2-a5
        rts

#endif /* AMINETXDUO_NET68K_ASM */
