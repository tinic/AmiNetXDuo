| AmiNetXDuo -- bulk copy, 68020.
|
| VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);
|
| Three copy loops were timed on the emulated 68020 over 1460 bytes, longword
| aligned (tests/perf/perf_test.c):
|
|   newlib memcpy, libm020        218 ns/B    move.l a1@+,a0@+ x4 + dbf
|   ami_sana2_copy_bytes (C)      243 ns/B    *(ULONG *)to = *(const ULONG *)from
|   byte loop                    ~900 ns/B
|
| The C loop loses to newlib by 11% because C cannot express a
| memory-to-memory move, so GCC emits a load into a data register and a
| separate store where the machine has a single addressing-mode pair -- the
| same kind of gap src/crypto68k/ finds around the carry flag.
|
| This routine uses movem.l, which moves eight longwords per instruction pair
| and is the only way on a 68020 to reduce the instruction count further --
| there is no move16 until the 68040.
|
| Alignment: the 68020 will fetch a longword from any address, but an unaligned
| one costs an extra bus cycle, measured at about 18% (218 ns/B aligned against
| 254-260 ns/B for every misaligned combination -- 18%, not the order of
| magnitude a byte-loop fallback would cost; libm020's memcpy has no byte-loop
| cliff).  So this brings the destination to a longword boundary and then moves
| longwords whatever the source is doing, as libm020 does.  See the parity
| guard below for what that costs on a 68000, where it is a correctness
| question rather than a performance one.
|
| 68000 and 68060: this file runs on both.  move.l, movem.l and dbf are in the
| original 68000 instruction set and none of them was dropped from the 68060,
| so AMINETXDUO_NET68K_ASM stays on for every target -- unlike
| AMINETXDUO_CRYPTO68K_ASM, which is 68020-only and is gated on the CPU.
|
| SPDX-License-Identifier: MIT

#ifdef AMINETXDUO_NET68K_ASM

        .text
        .even
        .globl  _n68k_copy_bytes

| An odd address is fatal on a 68000.
|
| The routine aligns the destination and then reads longwords from wherever
| the source happens to be, which is right for a 68020 -- it fetches a
| longword from any address and charges one extra bus cycle.  A 68000 does
| not: a word or longword access to an odd address is an address error,
| exception 3, and the machine gurus.  This routine is also the tree's
| memcpy() (AMINETXDUO_NET68K_MEMCPY), so on a 68000 that was every copy in
| the stack with mismatched pointer parity.  It is what the RAM driver test
| died of: `Exception 3 (20d9 ...)`, opcode 20d9 being the move.l a1@+,a0@+
| at .Llong below.  docs/RESEARCH.md §45.
|
| The condition is parity, not longword alignment -- an address of 2 mod 4 is
| legal for move.l on a 68000, it simply becomes two word accesses.  So the
| guard is one test: if the two pointers disagree in bit 0 then no amount of
| aligning the destination will make the source even, and the byte loop is the
| only option.  If they agree, bringing the destination to a longword boundary
| moves the source by the same count and leaves it even, and everything below
| is legal as written.
|
| 68020 and up assemble without this, where the byte loop would be a 4x
| slowdown to avoid an 18% one.
#if !defined(__mc68020__) && !defined(__mc68030__) && \
    !defined(__mc68040__) && !defined(__mc68060__)
#define N68K_ODD_IS_FATAL 1
#endif

| a0 = to, a1 = from, d0 = len
_n68k_copy_bytes:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0

#ifdef N68K_ODD_IS_FATAL
        move.l  a0,d1
        move.l  a1,d2
        eor.l   d1,d2
        btst    #0,d2
        bne     .Lbytes
#endif

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
