| AmiNetXDuo -- A/B subjects for the 68000 measurement.
|
| Four routines, all in one binary so no cross-run drift can be mistaken for a
| difference:
|
|   _ab_copy_old   src/net68k/n68k_copy.S at 3c6aaeb~1     (one movem pair)
|   _ab_copy_new2  src/net68k/n68k_copy.S at 3c6aaeb       (four movem pairs)
|   _ab_sum_old    src/net68k/n68k_checksum.S at b342ba7~1 (add.l/addx.l x8)
|   _ab_sum_new2   src/net68k/n68k_checksum.S at b342ba7   (movem/addx chain)
|
| The two _new2 routines are byte-for-byte the shipped instruction sequences,
| assembled a second time at a different address.  Timed against the library's
| own copy they measure nothing but code placement, which is the noise floor
| every other row here has to clear.
|
| SPDX-License-Identifier: MIT

        .text
        .even
        .globl  _ab_copy_old
        .globl  _ab_copy_new2
        .globl  _ab_sum_old
        .globl  _ab_sum_new2

#if !defined(__mc68020__) && !defined(__mc68030__) && \
    !defined(__mc68040__) && !defined(__mc68060__)
#define N68K_ODD_IS_FATAL 1
#endif


| ------------------------------------------------- copy, before 3c6aaeb ----

_ab_copy_old:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0

#ifdef N68K_ODD_IS_FATAL
        move.l  a0,d1
        move.l  a1,d2
        eor.l   d1,d2
        btst    #0,d2
        bne     .Lo_bytes
#endif

        moveq   #32,d1
        cmp.l   d1,d0
        bcs     .Lo_bytes

        move.l  a0,d1
        neg.l   d1
        andi.l  #3,d1
        beq     .Lo_bulk
        sub.l   d1,d0
.Lo_align:
        move.b  a1@+,a0@+
        subq.l  #1,d1
        bne     .Lo_align

.Lo_bulk:
        move.l  d0,d1
        lsr.l   #5,d1
        beq     .Lo_longs
        andi.l  #31,d0

.Lo_block:
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@
        lea     a0@(32),a0
        subq.l  #1,d1
        bne     .Lo_block

.Lo_longs:
        move.l  d0,d1
        lsr.l   #2,d1
        beq     .Lo_bytes
        andi.l  #3,d0
.Lo_long:
        move.l  a1@+,a0@+
        subq.l  #1,d1
        bne     .Lo_long

.Lo_bytes:
        tst.l   d0
        beq     .Lo_done
.Lo_byte:
        move.b  a1@+,a0@+
        subq.l  #1,d0
        bne     .Lo_byte

.Lo_done:
        movem.l sp@+,d2-d7/a2-a3
        rts


| ------------------------------- copy, 3c6aaeb, assembled a second time ----

_ab_copy_new2:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0

#ifdef N68K_ODD_IS_FATAL
        move.l  a0,d1
        move.l  a1,d2
        eor.l   d1,d2
        btst    #0,d2
        bne     .Ln_bytes
#endif

        moveq   #32,d1
        cmp.l   d1,d0
        bcs     .Ln_bytes

        move.l  a0,d1
        neg.l   d1
        andi.l  #3,d1
        beq     .Ln_bulk
        sub.l   d1,d0
.Ln_align:
        move.b  a1@+,a0@+
        subq.l  #1,d1
        bne     .Ln_align

.Ln_bulk:
        move.l  d0,d1
        lsr.l   #7,d1
        beq     .Ln_blocks
        andi.l  #127,d0

.Ln_big:
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
        bne     .Ln_big

.Ln_blocks:
        move.l  d0,d1
        lsr.l   #5,d1
        beq     .Ln_longs
        andi.l  #31,d0

.Ln_block:
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@
        lea     a0@(32),a0
        subq.l  #1,d1
        bne     .Ln_block

.Ln_longs:
        move.l  d0,d1
        lsr.l   #2,d1
        beq     .Ln_bytes
        andi.l  #3,d0
.Ln_long:
        move.l  a1@+,a0@+
        subq.l  #1,d1
        bne     .Ln_long

.Ln_bytes:
        tst.l   d0
        beq     .Ln_done
.Ln_byte:
        move.b  a1@+,a0@+
        subq.l  #1,d0
        bne     .Ln_byte

.Ln_done:
        movem.l sp@+,d2-d7/a2-a3
        rts


| --------------------------------------------- checksum, before b342ba7 ----

_ab_sum_old:
        movem.l d2-d3,sp@-
        movea.l sp@(12),a0
        move.l  sp@(16),d1

        moveq   #0,d0
        moveq   #0,d2

        move.l  d1,d3
        andi.l  #7,d3
        lsr.l   #3,d1
        beq     .Ls_tail

.Ls_loop8:
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
        bne     .Ls_loop8

.Ls_tail:
        tst.l   d3
        beq     .Ls_done

.Ls_loop1:
        add.l   a0@+,d0
        addx.l  d2,d0
        subq.l  #1,d3
        bne     .Ls_loop1

.Ls_done:
        movem.l sp@+,d2-d3
        rts


| --------------------------- checksum, b342ba7, assembled a second time ----

        .macro  AB_SUM7
        movem.l a0@+,d1-d7
        addx.l  d1,d0
        addx.l  d2,d0
        addx.l  d3,d0
        addx.l  d4,d0
        addx.l  d5,d0
        addx.l  d6,d0
        addx.l  d7,d0
        .endm

#define AB_TABLE        64
#define AB_BLOCKS       8

_ab_sum_new2:
        movea.l sp@(4),a0
        move.l  sp@(8),d1

        cmpi.l  #AB_TABLE,d1
        bhi     .Lt_bulk

        lsl.l   #2,d1
        lea     .Lt_sdone(pc),a1
        suba.l  d1,a1
        moveq   #0,d0
        add.l   d0,d0
        jmp     a1@

        .rept   AB_TABLE
        move.l  a0@+,d1
        addx.l  d1,d0
        .endr

.Lt_sdone:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0
        rts

.Lt_bulk:
        movem.l d2-d7/a2,sp@-
        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a0,a1
        adda.l  d2,a1
        moveq   #0,d0
        add.l   d0,d0

        lea     a1@(-AB_BLOCKS*28),a2
.Lt_block:
        .rept   AB_BLOCKS
        AB_SUM7
        .endr
        cmpa.l  a2,a0
        bls     .Lt_block

        lea     a1@(-28),a2
        cmpa.l  a2,a0
        bhi     .Lt_fold
.Lt_seven:
        AB_SUM7
        cmpa.l  a2,a0
        bls     .Lt_seven

.Lt_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

        move.l  a1,d1
        sub.l   a0,d1
        lsr.l   #2,d1
        beq     .Lt_done
        lsl.l   #2,d1
        lea     .Lt_tdone(pc),a2
        suba.l  d1,a2
        moveq   #0,d2
        add.l   d2,d2
        jmp     a2@

        .rept   6
        move.l  a0@+,d1
        addx.l  d1,d0
        .endr

.Lt_tdone:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Lt_done:
        movem.l sp@+,d2-d7/a2
        rts
