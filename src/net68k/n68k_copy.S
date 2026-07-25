| AmiNetXDuo -- bulk copy, 68020.
|
| VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);
|
| WHY THIS EXISTS, and what it is measured against.
|
| Three copy loops were timed on the emulated 68020 over 1460 bytes, longword
| aligned (tests/perf/perf_test.c):
|
|   newlib memcpy, libm020        218 ns/B    move.l a1@+,a0@+ x4 + dbf
|   ami_sana2_copy_bytes (C)      243 ns/B    *(ULONG *)to = *(const ULONG *)from
|   byte loop                    ~900 ns/B
|
| The C loop loses to newlib by 11% for one reason: C cannot express a
| memory-to-memory move, so GCC emits a load into a data register and a
| separate store, where the machine has a single addressing-mode pair.  That is
| the same shape of argument src/crypto68k/ makes about the carry flag.
|
| This routine uses movem.l, which moves eight longwords per instruction pair
| and is the only way on a 68020 to reduce the instruction count further --
| there is no move16 until the 68040.
|
| ALIGNMENT.  The 68020 will fetch a longword from any address, but an
| unaligned one costs an extra bus cycle, and the measurements say that is
| worth about 18% (218 ns/B aligned against 254-260 ns/B for every misaligned
| combination -- note that it is 18%, not the order of magnitude a byte-loop
| fallback would cost; libm020's memcpy has no byte-loop cliff).  So this
| brings the destination to a longword boundary and then moves longwords
| whatever the source is doing, exactly as libm020 does.
|
| 68060: add.l/movem.l are implemented everywhere; nothing here traps.  The
| file is nevertheless behind AMINETXDUO_NET68K_ASM, which must stay off for
| 68060 because src/crypto68k/'s MULU.L does trap there.
|
| SPDX-License-Identifier: MIT

#ifdef AMINETXDUO_NET68K_ASM

        .text
        .even
        .globl  _n68k_copy_bytes

| a0 = to, a1 = from, d0 = len
_n68k_copy_bytes:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0

        moveq   #32,d1
        cmp.l   d1,d0
        bcs     .Lbytes                 | under 32 bytes: not worth aligning

        | Bring the destination to a longword boundary.
        move.l  a0,d1
        neg.l   d1
        andi.l  #3,d1
        beq     .Lbulk
        sub.l   d1,d0
.Lalign:
        move.b  a1@+,a0@+
        subq.l  #1,d1
        bne     .Lalign

.Lbulk:
        move.l  d0,d1
        lsr.l   #5,d1                   | 32-byte blocks
        beq     .Llongs
        andi.l  #31,d0

.Lblock:
        movem.l a1@+,d2-d7/a2-a3        | eight longwords in one instruction
        movem.l d2-d7/a2-a3,a0@
        lea     a0@(32),a0              | MOVEM to memory has no (An)+ mode
        subq.l  #1,d1
        bne     .Lblock

.Llongs:
        move.l  d0,d1
        lsr.l   #2,d1
        beq     .Lbytes
        andi.l  #3,d0
.Llong:
        move.l  a1@+,a0@+
        subq.l  #1,d1
        bne     .Llong

.Lbytes:
        tst.l   d0
        beq     .Ldone
.Lbyte:
        move.b  a1@+,a0@+
        subq.l  #1,d0
        bne     .Lbyte

.Ldone:
        movem.l sp@+,d2-d7/a2-a3
        rts

#endif /* AMINETXDUO_NET68K_ASM */
