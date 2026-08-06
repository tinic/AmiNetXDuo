| Round two of copy-and-sum variants.
|
| Round one said movem+ADDX wins on a 68000 and discrete moves win on a 68020,
| which means the cost is not where the instruction count says it is.  These
| three take that apart: how much of the discrete form's win is the loop tail,
| and whether the load and the store want different treatment.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------------------
| v_disc16 -- discrete moves, sixteen a block instead of eight.
|
| If the discrete form wins because movem is expensive rather than because the
| loop is tight, doubling the block should buy almost nothing.  If it buys
| something, the tail was the cost.
| ---------------------------------------------------------------------------

        .globl  _v_disc16
_v_disc16:
        movem.l d2-d3/a2-a3,sp@-
        movea.l sp@(20),a0
        movea.l sp@(24),a1
        move.l  sp@(28),d1

        moveq   #0,d0
        tst.l   d1
        beq     .Ld16_out

        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a1,a2
        adda.l  d2,a2

        moveq   #16,d3
        cmp.l   d3,d1
        bcs     .Ld16_short

        lea     a2@(-64),a3

        moveq   #0,d1
        add.l   d1,d1                   | X = 0

.Ld16_block:
        .rept   16
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        .endr
        cmpa.l  a3,a1
        bls     .Ld16_block

        bra     .Ld16_tail

.Ld16_short:
        moveq   #0,d1
        add.l   d1,d1

.Ld16_tail:
        cmpa.l  a2,a1
        bcc     .Ld16_fold

.Ld16_one:
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        cmpa.l  a2,a1
        bcs     .Ld16_one

.Ld16_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Ld16_out:
        movem.l sp@+,d2-d3/a2-a3
        rts

| ---------------------------------------------------------------------------
| v_ldmovem -- movem for the LOAD, discrete stores.
|
| MOVEM to memory has no (An)+ mode, so the movem-pair form pays a LEA per
| block and cannot post-increment the destination.  This keeps the cheap half,
| seven loads in one instruction, and stores with move.l (An)+ so the pointer
| walks itself.
| ---------------------------------------------------------------------------

        .globl  _v_ldmovem
_v_ldmovem:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d1

        moveq   #0,d0
        tst.l   d1
        beq     .Llm_out

        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a1,a2
        adda.l  d2,a2

        moveq   #7,d3
        cmp.l   d3,d1
        bcs     .Llm_short

        lea     a2@(-28),a3

        moveq   #0,d1
        add.l   d1,d1                   | X = 0

.Llm_block:
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
        bls     .Llm_block

        bra     .Llm_tail

.Llm_short:
        moveq   #0,d1
        add.l   d1,d1

.Llm_tail:
        cmpa.l  a2,a1
        bcc     .Llm_fold

.Llm_one:
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        cmpa.l  a2,a1
        bcs     .Llm_one

.Llm_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Llm_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

| ---------------------------------------------------------------------------
| v_disc4 -- discrete, four a block.
|
| The other end of the unroll curve, so the three discrete rows say where the
| tail stops mattering.
| ---------------------------------------------------------------------------

        .globl  _v_disc4
_v_disc4:
        movem.l d2-d3/a2-a3,sp@-
        movea.l sp@(20),a0
        movea.l sp@(24),a1
        move.l  sp@(28),d1

        moveq   #0,d0
        tst.l   d1
        beq     .Ld4_out

        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a1,a2
        adda.l  d2,a2

        moveq   #4,d3
        cmp.l   d3,d1
        bcs     .Ld4_short

        lea     a2@(-16),a3

        moveq   #0,d1
        add.l   d1,d1

.Ld4_block:
        .rept   4
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        .endr
        cmpa.l  a3,a1
        bls     .Ld4_block

        bra     .Ld4_tail

.Ld4_short:
        moveq   #0,d1
        add.l   d1,d1

.Ld4_tail:
        cmpa.l  a2,a1
        bcc     .Ld4_fold

.Ld4_one:
        move.l  a1@+,d1
        move.l  d1,a0@+
        addx.l  d1,d0
        cmpa.l  a2,a1
        bcs     .Ld4_one

.Ld4_fold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Ld4_out:
        movem.l sp@+,d2-d3/a2-a3
        rts
