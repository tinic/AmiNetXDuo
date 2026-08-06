| AmiNetXDuo, one's-complement longword sum, 68k.
|
| ULONG n68k_sum_longwords(const ULONG *p, ULONG count);
|
| This was two instructions per longword, add.l a0@+,d0 to sum and set X,
| addx.l d2,d0 with d2 zero to fold the carry back in.  C reaches neither: it
| has no carry flag and cannot fold the load into the add's addressing mode.
|
| addx.l Dn,d0 adds the source AND the X flag, and X is the end-around carry
| that second instruction existed to fold.  So chain the addx instructions and
| let the carry ride X from one longword to the next:
|
|       movem.l a0@+,d1-d7      | seven longwords in one instruction
|       addx.l  d1,d0           | and one instruction each to fold them in
|       ...
|
| Eight instructions per seven longwords rather than sixteen per eight.  Not
| the earlier movem experiment that bought nothing, that one still ran
| add.l/addx.l on each loaded register, 19 instructions per 32 bytes against
| 18, and measured 201.27 against 201.39.  movem.l pays only when what it feeds
| needs one instruction per longword, not two.
|
| Nothing between two addx may disturb X.  movem, cmpa, lea, movea, adda, suba,
| moveq and the branches leave it alone; add, sub, the shifts and addx do not.
| Hence the setup shifts before the chain is armed, and cmpa against a
| precomputed limit for loop control rather than subq, which writes X.  dbf
| does NOT write X, DBcc leaves the condition codes alone, so it is barred
| for the other reason instead: it counts in a data register, and all eight are
| spoken for, d0 the accumulator and d1-d7 the movem payload.  Seven is the
| ceiling: d0 is the accumulator, addx takes only data registers, and folding an
| address register in with add.l would drop the pending carry.
|
| The chain leaves one carry in X.  Folding it can carry again, d0 =
| 0xFFFFFFFF with X set is reachable here, unlike in the add/addx form where a
| carry out of the add bounds the result at 0xFFFFFFFE, so the fold runs
| twice.  Twice is enough: the first can only carry by taking d0 to zero, and
| 0 + 1 does not.  That keeps what n68k_checksum.c relies on: congruent to the
| 16-bit sum modulo 0xFFFF, and the zero spelling only when every longword was
| zero.
|
| The chain costs seven registers saved and restored.  Over 1460 bytes that
| vanishes; over the 20-byte IP header this stack checksums on every packet in
| both directions it was a 24-32% loss, measured.  So 64 longwords and under
| take a second shape that saves nothing: a computed jump into 64 unrolled
| move.l/addx.l pairs, entered so exactly `count` of them run.  Straight-line
| code run once, so its 256 bytes cost nothing in the instruction cache, a
| cache charges only for code that repeats.
|
| A1200 profile, ns/B, best of five, every candidate in one binary so no
| cross-run drift reads as a difference.  Floor, this loop assembled twice at
| two addresses, is 1.3-3.3% at 128 B and up, 5% at 20-40 B, where placement
| alone moves the number.
|
|   bytes    1460   1024    512    400    296    260    200    128     40     20
|   was     180.6  182.0  191.9  203.9  211.9  214.4  229.5  252.0  464.7  852.4
|   now     128.6  137.5  155.1  173.4  185.0  191.9  215.6  253.1  458.9  669.5
|
| Between 128 and 256 B the table runs the same two instructions per longword
| the old loop did and the difference is placement noise.
|
| Losers, so they are not tried again.  add/addx unrolled 16/32/64 longwords:
| 166-175 at 1460 B, the subq/bne being 11% of the budget and all there was;
| unrolled 128: 184, worse than eight.  dbf for loop control: identical within
| the floor.  Two accumulators: flat, the addx reads the X the add just set,
| so the flag serialises the pairs whatever the data does.  Movem chain 7x13
| and 7x16: match 7x8 at 1460 B, lose 5-11% at 260-512 B where the deeper block
| no longer fits the packet.  7x32: 7% worse everywhere.  A middle loop level:
| 189.5 at 296 B against 185.0 without.  docs/RESEARCH.md 87 has the rest,
| including what FS-UAE cannot say about a 68040 or 68060.
|
| 68000 and 68060: this file runs on both.  movem.l, addx.l, the computed jump
| and everything else here are original 68000 instructions and none was dropped
| from the 68060, so AMINETXDUO_NET68K_ASM stays on for every target, unlike
| AMINETXDUO_CRYPTO68K_ASM, which is 68020-only because its MULU.L 32x32 traps
| to vector 61 on an 060.
|
| SPDX-License-Identifier: MIT

#ifdef AMINETXDUO_NET68K_ASM

        .text
        .even
        .globl  _n68k_sum_longwords

| THREE PARTS, THREE ANSWERS.  The seven-longword step below is the whole
| inner loop, and which form of it is fastest is a question about the machine
| rather than about the arithmetic.
|
|   68020/68030   movem.l feeding an addx chain.  Measured, and it is why this
|                 routine exists in this shape: 138 ns/B against 201 for the
|                 add/addx form it replaced (docs/RESEARCH.md 87).  movem pays
|                 because its fixed cost is amortised over seven registers.
|
|   68060         NOT movem.  It is microcoded there and does not pipeline,
|                 while ordinary instructions dual-issue, so the thing that
|                 wins on an 020 is the thing to avoid on an 060.  The
|                 add/addx form also needs no register loads at all -- the
|                 memory operand goes straight into the add -- so it is two
|                 simple instructions per longword against movem plus seven.
|                 UNMEASURED, and unmeasurable here: cycle accounting is off
|                 above the 68020, so an emulated 060 ns/B is not a number
|                 about an 060.  Same considered bet as n68k_copy.S.
|
|   68000         movem, and MEASURED rather than inherited.  Both forms were
|                 built and run on an A600, ns/B:
|
|                                    1460 B    8192 B chain
|                   movem             1001.6         921.8
|                   add/addx          1191.8        1115.1
|
|                 movem wins by 19%, so the 020 answer happens to be right
|                 here too -- but it is right by measurement now, and the
|                 alternative is ruled out rather than untried.  A 68000 reads
|                 a longword as two word cycles whichever instruction asks for
|                 it, so movem's amortised fixed cost still pays.
|
| Nothing between two addx may disturb X.  movem, cmpa, lea, movea, adda, suba
| and the add/addx pair itself all leave it alone; only the rest of the
| add/subtract/shift group touches it.

| An 060 takes the add/addx form; everything else takes movem.  Overridable
| by hand for the A/B, which is how the 68000 row below was taken.
#if defined(__mc68060__) && !defined(N68K_SUM_NO_MOVEM)
#define N68K_SUM_NO_MOVEM 1
#endif

#ifdef N68K_SUM_NO_MOVEM
        | d1 is the hard zero the addx sinks read; a0 walks the buffer.
        .macro  N68K_SUM7
        add.l   a0@+,d0
        addx.l  d1,d0
        add.l   a0@+,d0
        addx.l  d1,d0
        add.l   a0@+,d0
        addx.l  d1,d0
        add.l   a0@+,d0
        addx.l  d1,d0
        add.l   a0@+,d0
        addx.l  d1,d0
        add.l   a0@+,d0
        addx.l  d1,d0
        add.l   a0@+,d0
        addx.l  d1,d0
        .endm
#else
        .macro  N68K_SUM7
        movem.l a0@+,d1-d7
        addx.l  d1,d0
        addx.l  d2,d0
        addx.l  d3,d0
        addx.l  d4,d0
        addx.l  d5,d0
        addx.l  d6,d0
        addx.l  d7,d0
        .endm
#endif

| The table has to reach at least as far as the deep block, or the do-while
| below would read past the end of a buffer that never entered it.
#define N68K_TABLE      64
#define N68K_BLOCKS     8

_n68k_sum_longwords:
        movea.l sp@(4),a0
        move.l  sp@(8),d1

        cmpi.l  #N68K_TABLE,d1
        bhi     .Lbulk

| 0..64 longwords: nothing saved, no loop, one computed jump.
        lsl.l   #2,d1                   | four bytes per pair
        lea     .Lsdone(pc),a1
        suba.l  d1,a1                   | count pairs back from the end
        moveq   #0,d0
        add.l   d0,d0                   | d0 = 0 and X = 0
        jmp     a1@

        .rept   N68K_TABLE
        move.l  a0@+,d1
        addx.l  d1,d0
        .endr

.Lsdone:
        moveq   #0,d1
        addx.l  d1,d0                   | the carry still pending in X
        addx.l  d1,d0                   | and the one folding it can make
        rts

| More than 64: movem.l feeding the chain.
.Lbulk:
        movem.l d2-d7/a2,sp@-
        move.l  d1,d2
        lsl.l   #2,d2
        movea.l a0,a1
        adda.l  d2,a1                   | one past the last longword
        moveq   #0,d0
#ifdef N68K_SUM_NO_MOVEM
        moveq   #0,d1                   | the hard zero the addx sinks read
#endif
        add.l   d0,d0                   | from here to .Lfold nothing may
        |                                 touch X

        lea     a1@(-N68K_BLOCKS*28),a2
.Lblock:
        .rept   N68K_BLOCKS
        N68K_SUM7
        .endr
        cmpa.l  a2,a0
        bls     .Lblock                 | count > 64 >= 56, so this needs no
        |                                 entry test

        lea     a1@(-28),a2             | 0..55 left, seven at a time
        cmpa.l  a2,a0
        bhi     .Lfold
.Lseven:
        N68K_SUM7
        cmpa.l  a2,a0
        bls     .Lseven

.Lfold:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

        move.l  a1,d1                   | 0..6 longwords left, same jump again
        sub.l   a0,d1
        lsr.l   #2,d1
        beq     .Ldone
        lsl.l   #2,d1
        lea     .Ltdone(pc),a2
        suba.l  d1,a2
        moveq   #0,d2
        add.l   d2,d2                   | X = 0 again, the shifts cleared it
        jmp     a2@

        .rept   6
        move.l  a0@+,d1
        addx.l  d1,d0
        .endr

.Ltdone:
        moveq   #0,d1
        addx.l  d1,d0
        addx.l  d1,d0

.Ldone:
        movem.l sp@+,d2-d7/a2
        rts

#endif /* AMINETXDUO_NET68K_ASM */
