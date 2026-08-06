| Round three.  Round two left two winners, one per part, and both load with
| movem: the 68000 wants movem stores, the 68020 wants post-incrementing ones.
| So the remaining questions are how deep to unroll each, and whether the ADDX
| wants to sit between the stores or after them.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------------------
| v_addx14 -- the movem-pair form, fourteen longwords a block.
| ---------------------------------------------------------------------------

        .globl  _v_addx14
_v_addx14:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d1

        moveq   #0,d0
        tst.l   d1
        beq     .La14_out

        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a1,a2
        adda.l  d2,a2

        moveq   #14,d3
        cmp.l   d3,d1
        bcs     .La14_short

        lea     a2@(-56),a3

        moveq   #0,d1
        add.l   d1,d1

.La14_block:
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
        cmpa.l  a3,a1
        bls     .La14_block

        bra     .La14_tail

.La14_short:
        moveq   #0,d1
        add.l   d1,d1

.La14_tail:
        cmpa.l  a2,a1
        bcc     .La14_fold

.La14_one:
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        cmpa.l  a2,a1
        bcs     .La14_one

.La14_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.La14_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

| ---------------------------------------------------------------------------
| v_lm14 -- movem load, post-incrementing stores, fourteen a block.
| ---------------------------------------------------------------------------

        .globl  _v_lm14
_v_lm14:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d1

        moveq   #0,d0
        tst.l   d1
        beq     .Llm14_out

        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a1,a2
        adda.l  d2,a2

        moveq   #14,d3
        cmp.l   d3,d1
        bcs     .Llm14_short

        lea     a2@(-56),a3

        moveq   #0,d1
        add.l   d1,d1

.Llm14_block:
        movem.l a1@+,d1-d7
        move.l  d1,a0@+
        addx.l  d1,d0
        move.l  d2,a0@+
        addx.l  d2,d0
        move.l  d3,a0@+
        addx.l  d3,d0
        move.l  d4,a0@+
        addx.l  d4,d0
        move.l  d5,a0@+
        addx.l  d5,d0
        move.l  d6,a0@+
        addx.l  d6,d0
        move.l  d7,a0@+
        addx.l  d7,d0
        movem.l a1@+,d1-d7
        move.l  d1,a0@+
        addx.l  d1,d0
        move.l  d2,a0@+
        addx.l  d2,d0
        move.l  d3,a0@+
        addx.l  d3,d0
        move.l  d4,a0@+
        addx.l  d4,d0
        move.l  d5,a0@+
        addx.l  d5,d0
        move.l  d6,a0@+
        addx.l  d6,d0
        move.l  d7,a0@+
        addx.l  d7,d0
        cmpa.l  a3,a1
        bls     .Llm14_block

        bra     .Llm14_tail

.Llm14_short:
        moveq   #0,d1
        add.l   d1,d1

.Llm14_tail:
        cmpa.l  a2,a1
        bcc     .Llm14_fold

.Llm14_one:
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        cmpa.l  a2,a1
        bcs     .Llm14_one

.Llm14_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Llm14_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

| ---------------------------------------------------------------------------
| v_lmsep -- movem load, all seven stores, then all seven ADDX.
|
| Interleaving puts a store between each pair of ADDX.  Separating them says
| whether that costs anything: the ADDX chain is serial on X either way, so if
| the two are the same, the store placement is free.
| ---------------------------------------------------------------------------

        .globl  _v_lmsep
_v_lmsep:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d1

        moveq   #0,d0
        tst.l   d1
        beq     .Lsep_out

        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a1,a2
        adda.l  d2,a2

        moveq   #7,d3
        cmp.l   d3,d1
        bcs     .Lsep_short

        lea     a2@(-28),a3

        moveq   #0,d1
        add.l   d1,d1

.Lsep_block:
        movem.l a1@+,d1-d7
        move.l  d1,a0@+
        move.l  d2,a0@+
        move.l  d3,a0@+
        move.l  d4,a0@+
        move.l  d5,a0@+
        move.l  d6,a0@+
        move.l  d7,a0@+
        addx.l  d1,d0
        addx.l  d2,d0
        addx.l  d3,d0
        addx.l  d4,d0
        addx.l  d5,d0
        addx.l  d6,d0
        addx.l  d7,d0
        cmpa.l  a3,a1
        bls     .Lsep_block

        bra     .Lsep_tail

.Lsep_short:
        moveq   #0,d1
        add.l   d1,d1

.Lsep_tail:
        cmpa.l  a2,a1
        bcc     .Lsep_fold

.Lsep_one:
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        cmpa.l  a2,a1
        bcs     .Lsep_one

.Lsep_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Lsep_out:
        movem.l sp@+,d2-d7/a2-a3
        rts
