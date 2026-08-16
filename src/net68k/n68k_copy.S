| AmiNetXDuo, bulk copy, 68020.
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
| memory-to-memory move.  GCC emits a load into a data register and a separate
| store where the machine has a single addressing-mode pair.  src/crypto68k/
| finds the same kind of gap around the carry flag.
|
| This routine uses movem.l, which moves eight longwords per instruction pair
| and is the only way on a 68020 to cut the instruction count further.  There
| is no move16 until the 68040.  Four of those pairs are unrolled into one
| 128-byte block, which is where the rest of the win is.  See .Lbig.
|
| The 68020 fetches a longword from any address, and a misaligned one costs an
| extra bus cycle, measured at about 18%: 218 ns/B aligned against 254-260 ns/B
| for every misaligned combination.  That is far less than a byte-loop fallback
| costs, and the libm020 memcpy has no byte-loop cliff either.  So this brings
| the destination to a longword boundary and then moves longwords whatever the
| source is doing, as libm020 does.  The parity guard below covers what that
| costs on a 68000, where it is a correctness question and not a performance
| one.
|
| This file runs on the 68000 and on the 68060.  move.l, movem.l and dbf are in
| the original 68000 instruction set and none of them was dropped from the
| 68060, so AMINETXDUO_NET68K_ASM stays on for every target.
| AMINETXDUO_CRYPTO68K_ASM is 68020-only and is gated on the CPU.
|
| SPDX-License-Identifier: MIT

#ifdef AMINETXDUO_NET68K_ASM

#include "n68k_variant.h"

        .text
        .even
        .globl  N68K_MV_SYM(_n68k_copy_bytes)

| An odd address is fatal on a 68000.
|
| The routine aligns the destination and then reads longwords from wherever the
| source happens to be.  That is right for a 68020, which fetches a longword
| from any address and charges one extra bus cycle.  A 68000 does not: a word
| or longword access to an odd address is an address error, exception 3, and
| the machine gurus.  This routine is also the memcpy() of this tree
| (AMINETXDUO_NET68K_MEMCPY), so on a 68000 that covered every copy in the
| stack with mismatched pointer parity.  It is what the RAM driver test died
| of: `Exception 3 (20d9 ...)`, opcode 20d9 being the move.l a1@+,a0@+ at
| .Llong below.  docs/RESEARCH.md §45.
|
| The condition is parity, not longword alignment.  An address of 2 mod 4 is
| legal for move.l on a 68000 and becomes two word accesses.  So the guard is
| one test: if the two pointers disagree in bit 0, no amount of aligning the
| destination makes the source even, and the byte loop is the only option.  If
| they agree, bringing the destination to a longword boundary moves the source
| by the same count and leaves it even, and everything below is legal as
| written.
|
| The 68020 and later assemble without this, where the byte loop costs a 4x
| slowdown to avoid an 18% one.
#if N68K_MV == 0
#define N68K_ODD_IS_FATAL 1
#endif

| A 68060 does not want movem, so this file stops using it there.
|
| Everything measured in this file was measured on a 68020, where movem.l is
| the cheapest way to move eight longwords because the fixed cost is paid once
| for the whole list.  A 68060 is a different machine.  movem is microcoded and
| does not pipeline, while ordinary moves dual-issue, so the instruction that
| wins on an 020 is the one the Motorola 68060 optimisation guidance says to
| avoid.
|
| This is not measured, and it cannot be measured here.  Amiberry and FS-UAE
| turn cycle accounting off above the 68020, so a ns/B figure from an emulated
| 68060 is not a number about a 68060.  This lab has no such hardware.
| docs/RESEARCH.md 87 reached the same conclusion independently about the movem
| chain in the checksum and left it open.  So this is a considered bet on
| documented microarchitecture rather than a result.  If an 060 ever runs
| tests/perf/perf_test, that arm settles it, and the answer can be to take this
| back out.
|
| The odd-address guard above deliberately does not extend here.  A 68060
| fetches a longword from any address.  Routing it to the byte loop the way a
| 68000 must puts eight of the sixteen alignment pairs on a path that measured
| 2638 ns/B against 990 on the part that has no choice.
#if N68K_MV == 60
#define N68K_NO_MOVEM 1
#endif

| a0 = to, a1 = from, d0 = len
N68K_MV_SYM(_n68k_copy_bytes):
#ifndef N68K_NO_MOVEM
| Short copies touch no callee-saved register.
|
| This routine is the memcpy() of this tree, and the calls it gets are mostly
| small: a 20-byte TCP header per received packet is one of the three callers
| Profile WATCH proved over an A600 transfer.  Saving d2-d7/a2-a3 and putting
| them back is sixteen longword accesses, about what twenty bytes of copying
| costs, and the entry below paid it before it had looked at the length.
|
| The threshold is the one the bulk path already uses.  Under 32 bytes the copy
| went to the byte loop in any case, so nothing that used to take the movem
| path stops taking it.  A byte loop needs no alignment or parity argument.
        movea.l sp@(4),a0
        movea.l sp@(8),a1
        move.l  sp@(12),d0

        moveq   #32,d1
        cmp.l   d1,d0
        bcc     .Lfull                  | 32 or more: the routine proper

        tst.l   d0
        beq     .Lshort_out
.Lshort:
        move.b  a1@+,a0@+
        subq.l  #1,d0
        bne     .Lshort
.Lshort_out:
        rts

.Lfull:
#endif
| The path that does not use movem touches no callee-saved register.  It works
| in a0, a1, d0 and d1, all of which are the caller's to lose.  Saving eight
| registers and restoring them is then pure overhead on every call.  This
| routine is also the memcpy() of this tree, so that includes many short calls
| where a 16-longword round trip to the stack is a large fraction of the work.
#ifdef N68K_NO_MOVEM
        movea.l sp@(4),a0
        movea.l sp@(8),a1
        move.l  sp@(12),d0
#else
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0
#endif

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
        lsr.l   #7,d1                   | 128-byte blocks
        beq     .Lblocks
        andi.l  #127,d0

| Four movem pairs per iteration.  The pointer arithmetic and the branch are
| the reason: at one pair per iteration they are three instructions per 32
| bytes, and on this machine an instruction costs about what a longword access
| does.  Measured on the A1200 profile over 1460 bytes, ns/B, source offset
| relative to an aligned destination:
|
|                       s0      s1      s2      s3
|   one pair          179.7   227.2   228.3   229.5
|   two pairs         167.8   216.5   218.0   215.6
|   four pairs        160.2   211.8   212.0   209.8
|   eight pairs       161.0   206.5   212.3   210.8
|
| Eight is not better than four and doubles the loop, so the code uses four.
| dbf in place of subq/bne measured the same and needs a 65536-block guard.
|
| At a source offset of 2 mod 4 the offset is exactly a word, so the reads can
| be moved to the aligned side and put back with swap and move.w instead of
| paying the split bus cycle.  That loses badly, and so does every other
| word-granularity idea.  Same machine, same run, s2:
|
|   movem.l, dest aligned (this)       233     | the split read costs ~3 cycles
|   movem.w, both sides word aligned   349     | per longword.  swap + move.w
|   aligned movem.l + swap recombine   359     | costs ~7, so there is no win.
|   move.w a1@+,a0@+                   419
|
| Aligning the source instead of the destination, which moves the split from
| the read to the write, measured 232.8 against 229.8.  Nothing in it.
.Lbig:
#ifdef N68K_NO_MOVEM
        | Thirty-two longwords, one at a time.  (a0)+ does the pointer
        | arithmetic movem needed a lea for, so the loop tail is two
        | instructions rather than three.
        .rept   32
        move.l  a1@+,a0@+
        .endr
        subq.l  #1,d1
        bne     .Lbig
#else
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@(32)
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@(64)
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@(96)
        lea     a0@(128),a0
        subq.l  #1,d1
        bne     .Lbig
#endif

.Lblocks:
        move.l  d0,d1
        lsr.l   #5,d1                   | 32-byte blocks
        beq     .Llongs
        andi.l  #31,d0

.Lblock:
#ifdef N68K_NO_MOVEM
        .rept   8
        move.l  a1@+,a0@+
        .endr
        subq.l  #1,d1
        bne     .Lblock
#else
        movem.l a1@+,d2-d7/a2-a3        | eight longwords in one instruction
        movem.l d2-d7/a2-a3,a0@
        lea     a0@(32),a0              | MOVEM to memory has no (An)+ mode
        subq.l  #1,d1
        bne     .Lblock
#endif

.Llongs:
        move.l  d0,d1
        lsr.l   #2,d1
        beq     .Lbytes
        andi.l  #3,d0
.Llong:
        move.l  a1@+,a0@+
        subq.l  #1,d1
        bne     .Llong

| The byte loop, unrolled by eight.
|
| On a 68020 this is a tail: at most three bytes after the longword path, and
| the loop overhead against it does not matter.  On a 68000 it is not a tail.
| It is the whole copy whenever the two pointers disagree in bit 0, because an
| odd word or longword access is an address error there and no amount of
| aligning the destination fixes an odd source.  That is eight of the sixteen
| alignment pairs.
|
| Measured on the A600 profile, 1460 bytes, before this unroll:
|
|             s0      s1      s2      s3
|   d0       961    5427     961    5438
|   d1      5449     995    5459     987
|
| 5.5x, and it was three instructions per byte with two of them loop overhead:
| move.b memory to memory is 12 cycles on a 68000, subq.l is 8 and a taken bne
| is 10, so 30 cycles a byte of which 18 bought nothing.  Eight moves to one
| subq/bne is 14.25.
|
| Reading longwords from the even side and shifting bytes out to the odd one
| was considered and is worse.  The load is two bus cycles on a 16-bit bus, and
| each byte then costs a move.b and a shift, about 20 cycles a byte against 12
| for the memory-to-memory move that needs no register at all.
.Lbytes:
        tst.l   d0
        beq     .Ldone

        move.l  d0,d1
        lsr.l   #3,d1                   | eight-byte runs
        beq     .Lbyte
        andi.l  #7,d0

.Lbyte8:
        move.b  a1@+,a0@+
        move.b  a1@+,a0@+
        move.b  a1@+,a0@+
        move.b  a1@+,a0@+
        move.b  a1@+,a0@+
        move.b  a1@+,a0@+
        move.b  a1@+,a0@+
        move.b  a1@+,a0@+
        subq.l  #1,d1
        bne     .Lbyte8

        tst.l   d0
        beq     .Ldone

.Lbyte:
        move.b  a1@+,a0@+
        subq.l  #1,d0
        bne     .Lbyte

.Ldone:
#ifndef N68K_NO_MOVEM
        movem.l sp@+,d2-d7/a2-a3
#endif
        rts

#endif /* AMINETXDUO_NET68K_ASM */
