| AmiNetXDuo -- CPU/memory calibration kernels.
|
| Exact instruction sequences with published 68020 and 68030 cycle costs, so
| an emulator's timing model can be checked against the machine it claims to
| be.  This is deliberately assembly and not C: the whole point is that the
| instruction mix is known, and a compiler is free to change it.
|
| Every kernel runs a body of exactly 16 instructions (or 16 longwords of
| memory traffic) inside a loop, `reps` times.  _cal_empty runs the loop with
| no body at all, so the caller subtracts it and is left with the body.
|
| Published cycle counts, cache case, from the MC68020UM and MC68030UM
| instruction-timing appendices:
|
|   instruction            68020      68030
|   MOVE.L Dn,Dm             2          2
|   ADD.L  Dn,Dm             2          2
|   ADDX.L Dn,Dm             2          2
|   MULU.L Dn,Dm            43         44
|   DIVU.L Dn,Dm            44         44
|   MOVE.L (An)+,Dn          4          4   (+ bus, if it misses the cache)
|   MOVE.L Dn,(An)+          4          4
|   MOVE.L (An)+,(Am)+       5          5
|   MOVEM.L (An)+,<8 regs>  12+8n/2 ... 8 + 4 per register, roughly
|
| The absolute numbers below are only as good as the assumption about the
| clock.  The RATIOS between kernels in one run are not: they need no clock at
| all, and they are what says whether a model charges for the work.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------------------
| VOID cal_empty(ULONG reps)
|
| The loop and nothing else.  Subtracted from every other kernel.
| ---------------------------------------------------------------------------
        .globl  _cal_empty
_cal_empty:
        move.l  sp@(4),d0
        tst.l   d0
        beq     .Lempty_done
.Lempty_loop:
        subq.l  #1,d0
        bne     .Lempty_loop
.Lempty_done:
        rts


| ---------------------------------------------------------------------------
| Register-only kernels.  VOID cal_xxx(ULONG reps); 16 instructions per rep.
|
| The body is 32-64 bytes, so it sits entirely inside the 256-byte instruction
| cache of both the 68020 and the 68030 after the first pass -- which is the
| condition the published "cache case" timings assume.
| ---------------------------------------------------------------------------

        .globl  _cal_add
_cal_add:
        movem.l d2-d3,sp@-
        move.l  sp@(12),d0
        moveq   #17,d2
        moveq   #3,d3
        tst.l   d0
        beq     .Ladd_done
.Ladd_loop:
        .rept   16
        add.l   d2,d3
        .endr
        subq.l  #1,d0
        bne     .Ladd_loop
.Ladd_done:
        movem.l sp@+,d2-d3
        rts

        .globl  _cal_move
_cal_move:
        movem.l d2-d3,sp@-
        move.l  sp@(12),d0
        moveq   #17,d2
        moveq   #3,d3
        tst.l   d0
        beq     .Lmove_done
.Lmove_loop:
        .rept   16
        move.l  d2,d3
        .endr
        subq.l  #1,d0
        bne     .Lmove_loop
.Lmove_done:
        movem.l sp@+,d2-d3
        rts

| ADDX.L is the carry-chain instruction src/crypto68k/ and src/net68k/ are
| built on, so its cost is not academic here.
        .globl  _cal_addx
_cal_addx:
        movem.l d2-d3,sp@-
        move.l  sp@(12),d0
        moveq   #17,d2
        moveq   #3,d3
        tst.l   d0
        beq     .Laddx_done
.Laddx_loop:
        .rept   16
        addx.l  d2,d3
        .endr
        subq.l  #1,d0
        bne     .Laddx_loop
.Laddx_done:
        movem.l sp@+,d2-d3
        rts

| The 68000 has no MULU.L at all -- the assembler rejects it and the machine
| would trap -- so the two multiply kernels are absent from that build and
| cpucal.c prints no row for them.
#if !defined(__mc68020__) && !defined(__mc68030__) && \
    !defined(__mc68040__) && !defined(__mc68060__)
#define CAL_NO_MULU_L 1
#endif

#ifndef CAL_NO_MULU_L

| MULU.L 32x32->32.  44 cycles on a real 68030, i.e. 22x the cost of ADD.L.
| A model that reports anything near 1x is not charging for it -- which is
| exactly what the P-256 work found and why it refused to quote 68030 ratios.
        .globl  _cal_mulu
_cal_mulu:
        movem.l d2-d3,sp@-
        move.l  sp@(12),d0
        move.l  #0x12345,d2
        move.l  #0x6789,d3
        tst.l   d0
        beq     .Lmulu_done
.Lmulu_loop:
        .rept   16
        mulu.l  d2,d3
        .endr
        subq.l  #1,d0
        bne     .Lmulu_loop
.Lmulu_done:
        movem.l sp@+,d2-d3
        rts

| MULU.L 32x32->64, the form the bignum code actually uses.
        .globl  _cal_mulu64
_cal_mulu64:
        movem.l d2-d4,sp@-
        move.l  sp@(16),d0
        move.l  #0x12345,d2
        move.l  #0x6789,d3
        moveq   #0,d4
        tst.l   d0
        beq     .Lmulu64_done
.Lmulu64_loop:
        .rept   16
        mulu.l  d2,d4:d3
        .endr
        subq.l  #1,d0
        bne     .Lmulu64_loop
.Lmulu64_done:
        movem.l sp@+,d2-d4
        rts

#endif /* CAL_NO_MULU_L */


| ---------------------------------------------------------------------------
| Memory kernels.
|
|   VOID cal_read (APTR buf, ULONG longs, ULONG reps)
|   VOID cal_write(APTR buf, ULONG longs, ULONG reps)
|   VOID cal_m2m  (APTR dst, APTR src, ULONG longs, ULONG reps)
|   VOID cal_movem(APTR dst, APTR src, ULONG longs, ULONG reps)
|
| Each sweeps `longs` longwords (rounded down to a multiple of 16) `reps`
| times.  Calling them with a 64-byte window measures the data cache; calling
| them with a window far larger than 256 bytes measures the bus.  On a 68020
| there is no data cache at all, so the two must agree; on a 68030 they must
| not.  THAT is the discriminator, and it needs no clock either.
| ---------------------------------------------------------------------------

        .globl  _cal_read
_cal_read:
        movem.l d2-d3/a2,sp@-           | 12 bytes
        movea.l sp@(16),a2              | buf
        move.l  sp@(20),d1              | longs
        move.l  sp@(24),d0              | reps
        lsr.l   #4,d1                   | 16-longword blocks
        tst.l   d0
        beq     .Lrd_done
        tst.l   d1
        beq     .Lrd_done
.Lrd_outer:
        movea.l a2,a0
        move.l  d1,d2
.Lrd_inner:
        .rept   16
        move.l  a0@+,d3
        .endr
        subq.l  #1,d2
        bne     .Lrd_inner
        subq.l  #1,d0
        bne     .Lrd_outer
.Lrd_done:
        movem.l sp@+,d2-d3/a2
        rts

        .globl  _cal_write
_cal_write:
        movem.l d2-d3/a2,sp@-
        movea.l sp@(16),a2
        move.l  sp@(20),d1
        move.l  sp@(24),d0
        lsr.l   #4,d1
        tst.l   d0
        beq     .Lwr_done
        tst.l   d1
        beq     .Lwr_done
        moveq   #0x5A,d3
.Lwr_outer:
        movea.l a2,a0
        move.l  d1,d2
.Lwr_inner:
        .rept   16
        move.l  d3,a0@+
        .endr
        subq.l  #1,d2
        bne     .Lwr_inner
        subq.l  #1,d0
        bne     .Lwr_outer
.Lwr_done:
        movem.l sp@+,d2-d3/a2
        rts

        .globl  _cal_m2m
_cal_m2m:
        movem.l d2/a2-a3,sp@-           | 12 bytes
        movea.l sp@(16),a2              | dst
        movea.l sp@(20),a3              | src
        move.l  sp@(24),d1              | longs
        move.l  sp@(28),d0              | reps
        lsr.l   #4,d1
        tst.l   d0
        beq     .Lm2m_done
        tst.l   d1
        beq     .Lm2m_done
.Lm2m_outer:
        movea.l a2,a0
        movea.l a3,a1
        move.l  d1,d2
.Lm2m_inner:
        .rept   16
        move.l  a1@+,a0@+
        .endr
        subq.l  #1,d2
        bne     .Lm2m_inner
        subq.l  #1,d0
        bne     .Lm2m_outer
.Lm2m_done:
        movem.l sp@+,d2/a2-a3
        rts

| The movem.l burst n68k_copy_bytes() is built on: eight longwords per
| instruction pair, two pairs per inner iteration = the same 16 longwords the
| other memory kernels move.
| Eight registers per MOVEM is 32 bytes, and two pairs per inner iteration is
| the same 16 longwords the other memory kernels move -- so the rows are
| directly comparable.  d2-d7/a2-a3 IS the movem list, which leaves nothing to
| hold the base pointers in: they are re-read from the argument slots on every
| outer pass instead.
        .globl  _cal_movem
_cal_movem:
        movem.l d2-d7/a2-a4,sp@-        | 36 bytes
        move.l  sp@(52),d0              | reps
        tst.l   d0
        beq     .Lmvm_done
        move.l  sp@(48),d1              | longs
        lsr.l   #4,d1                   | 16-longword (64 B) blocks
        tst.l   d1
        beq     .Lmvm_done
        lsl.l   #6,d1                   | ... in bytes
        movea.l sp@(44),a4
        adda.l  d1,a4                   | a4 = one past the last source byte
.Lmvm_outer:
        movea.l sp@(40),a0              | dst
        movea.l sp@(44),a1              | src
.Lmvm_inner:
        movem.l a1@+,d2-d7/a2-a3        | eight longwords in one instruction
        movem.l d2-d7/a2-a3,a0@
        lea     a0@(32),a0
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@
        lea     a0@(32),a0
        cmpa.l  a1,a4
        bne     .Lmvm_inner
        subq.l  #1,d0
        bne     .Lmvm_outer
.Lmvm_done:
        movem.l sp@+,d2-d7/a2-a4
        rts
