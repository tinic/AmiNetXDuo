| AmiNetXDuo, copy-and-sum variants for the bench harness.
|
| Each entry has the signature of n68k_copy_sum_longwords():
|
|     ULONG v(ULONG *to, const ULONG *from, ULONG count)
|
| copy `count` longwords and return the ones-complement accumulator, carries
| folded end-around, which is what the C reference in n68k_checksum.c returns.
|
| They differ only in how the sum is taken out of the copy, which is the whole
| question: the movem pair and the bus traffic are identical.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------------------
| v_addadd -- what the tree ships today.
|
| Loads d4-d7/a2-a5.  Four of those are address registers and ADDX will not
| take one, so every longword costs an ADD and an ADDX: two instructions.
| ---------------------------------------------------------------------------

        .globl  _v_addadd
_v_addadd:
        movem.l d2-d7/a2-a5,sp@-        | 10 longs
        movea.l sp@(44),a0
        movea.l sp@(48),a1
        move.l  sp@(52),d2

        moveq   #0,d0
        moveq   #0,d1

        move.l  d2,d3
        lsr.l   #3,d3
        beq     .Laa_tail

.Laa_block:
        movem.l a1@+,d4-d7/a2-a5
        movem.l d4-d7/a2-a5,a0@
        lea     a0@(32),a0
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
        bne     .Laa_block

.Laa_tail:
        andi.l  #7,d2
        beq     .Laa_done

.Laa_one:
        move.l  a1@+,d4
        move.l  d4,a0@+
        add.l   d4,d0
        addx.l  d1,d0
        subq.l  #1,d2
        bne     .Laa_one

.Laa_done:
        movem.l sp@+,d2-d7/a2-a5
        rts

| ---------------------------------------------------------------------------
| v_addx -- seven data registers feeding an ADDX chain.
|
| RESEARCH 87's form, applied here: ADDX carries through X by itself, so a
| longword costs ONE instruction to sum.  Seven is the limit -- d0 holds the
| accumulator and ADDX cannot reach an address register.
|
| Nothing between two ADDX may disturb X, so the loop closes on CMPA against a
| precomputed end pointer rather than on a SUBQ.
| ---------------------------------------------------------------------------

        .globl  _v_addx
_v_addx:
        movem.l d2-d7/a2-a3,sp@-        | 8 longs
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d1

        moveq   #0,d0
        tst.l   d1
        beq     .Lax_out

        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a1,a2
        adda.l  d2,a2                   | one past the last source longword

        moveq   #7,d3
        cmp.l   d3,d1
        bcs     .Lax_short

        lea     a2@(-28),a3

        moveq   #0,d1
        add.l   d1,d1                   | X = 0

.Lax_block:
        movem.l a1@+,d1-d7
        movem.l d1-d7,a0@
        lea     a0@(28),a0
        addx.l  d1,d0
        addx.l  d2,d0
        addx.l  d3,d0
        addx.l  d4,d0
        addx.l  d5,d0
        addx.l  d6,d0
        addx.l  d7,d0
        cmpa.l  a3,a1                   | CMPA leaves X alone
        bls     .Lax_block

        bra     .Lax_tail

.Lax_short:
        moveq   #0,d1
        add.l   d1,d1                   | X = 0

.Lax_tail:
        cmpa.l  a2,a1
        bcc     .Lax_fold

.Lax_one:
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        cmpa.l  a2,a1
        bcs     .Lax_one

.Lax_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Lax_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

| ---------------------------------------------------------------------------
| v_discrete -- no movem, same ADDX chain.
|
| For a 68060, where movem is microcoded and does not pipeline while ordinary
| moves dual-issue.  Three instructions a longword against the movem form's
| ten per seven, but none of them is microcoded.
| ---------------------------------------------------------------------------

        .globl  _v_discrete
_v_discrete:
        movem.l d2-d3/a2-a3,sp@-        | 4 longs
        movea.l sp@(20),a0
        movea.l sp@(24),a1
        move.l  sp@(28),d1

        moveq   #0,d0
        tst.l   d1
        beq     .Ldi_out

        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a1,a2
        adda.l  d2,a2

        moveq   #8,d3
        cmp.l   d3,d1
        bcs     .Ldi_short

        lea     a2@(-32),a3

        moveq   #0,d1
        add.l   d1,d1                   | X = 0

.Ldi_block:
        .rept   8
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        .endr
        cmpa.l  a3,a1
        bls     .Ldi_block

        bra     .Ldi_tail

.Ldi_short:
        moveq   #0,d1
        add.l   d1,d1                   | X = 0

.Ldi_tail:
        cmpa.l  a2,a1
        bcc     .Ldi_fold

.Ldi_one:
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        cmpa.l  a2,a1
        bcs     .Ldi_one

.Ldi_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Ldi_out:
        movem.l sp@+,d2-d3/a2-a3
        rts
