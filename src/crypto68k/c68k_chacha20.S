| AmiNetXDuo, crypto68k: the ChaCha20 block function, 68020 assembly.
|
|   One entry point with the same interface as c68k_chacha20_core_c() beside
|   it: twenty rounds over a copy of a sixteen-word state, then the original
|   state added back in.  Everything else, the buffering, the counter, the
|   little-endian serialisation, the AEAD, stays in C, as c68k_aes.S leaves
|   the key schedule and CBC there.
|
|   Written here from RFC 8439 section 2.3, as the C beside it was.  The
|   register schedule below was derived and checked against the 2.3.2 block of
|   RFC 8439 before this was written.  The result is checked against 2.4.2,
|   2.5.2 and 2.8.2 by tests/crypto68k/crypto68k_bulk, and against the C core
|   block for block.
|
|   The tree moved to -Os and this cipher regressed by 22.3%
|   (docs/RESEARCH.md 57).  That section settled on a hand-written kernel
|   rather than a higher optimisation level over a whole stack to suit one
|   file.  ChaCha20 suits a hand-written kernel because it is add, rotate and
|   exclusive-or over sixteen 32-bit words with no table and no carry, so the
|   only thing left to decide is which words are in registers when.  A compiler
|   answers that with a spill heuristic, and a hand-written schedule answers it
|   exactly.
|
| Sixteen words, eight data registers
|
|   A quarter-round is
|
|       a += b; d ^= a; d = rotl(d,16);   c += d; b ^= c; b = rotl(b,12);
|       a += b; d ^= a; d = rotl(d, 8);   c += d; b ^= c; b = rotl(b, 7);
|
|   and all four operands have to be in data registers, not three and an
|   address register: EOR has no An destination and no An source, and the
|   rotates have neither, so `b`, `c` and `d` are excluded outright, and `a`,
|   which is only ever added to, is still read by `d ^= a`.  Four data
|   registers per quarter-round, no temporary needed, so the eight of the part
|   hold exactly two quarter-rounds at a time.
|
|   That fixes the schedule.  A double round is eight quarter-rounds, taken
|   two at a time:
|
|       G1  QR(0,4,8,12)  QR(1,5,9,13)      the column round
|       G2  QR(2,6,10,14) QR(3,7,11,15)
|       G3  QR(0,5,10,15) QR(1,6,11,12)     the diagonal round
|       G4  QR(2,7,8,13)  QR(3,4,9,14)
|
|   G1 and G2 are disjoint, so are G3 and G4, and every diagonal quarter-round
|   takes exactly one word from each column quarter-round, so a group of two
|   column quarter-rounds and a group of two diagonal ones always share four
|   words, whichever way the four are paired.  Eight words change at G1->G2,
|   four at G2->G3, eight at G3->G4, four at G4->G1: 24 word exchanges a double
|   round, and no pairing of the quarter-rounds does better.  A property of the
|   cipher, not of the schedule.
|
| Where the other eight words live, and why EXG
|
|   tests/crypto68k/crypto68k_bulk prices the three candidate homes:
|
|       MOVE.L Dm,d16(An) / MOVE.L d16(An),Dm    8.80 + 4.98 cycles
|       MOVEA.L Dn,An     / MOVE.L An,Dn         2.00 + 2.00
|       EXG    Dn,An                             4.01
|
|   A spill to the stack costs 13.8 cycles a round trip and an address register
|   costs 4, so the seven address registers are the store, but the pair of
|   MOVEs cannot be used, because there is no ninth data register to stage the
|   outgoing word through.  EXG is the whole exchange in one instruction, at
|   the price of the two MOVEs and with no temporary: it retires the word this
|   data register no longer needs and produces the next one in the same
|   register.
|
|   Eight data registers plus seven address registers is fifteen homes for
|   sixteen words, so one word is on the stack at any moment, and the choice of
|   which word is deliberate.  A word resident in adjacent groups is exchanged
|   once a double round.  A word resident in G1 and G3, or G2 and G4, is
|   exchanged twice.  The
|   stack word must be one of the once-only kind, and there are two to place:
|   x15, in G2 and G3, sits on the stack across G4 and G1; x9, in G4 and G1,
|   sits there across G2 and G3.  They change over in d6, the one data register
|   that touches memory inside the loop.
|
|   The round loop therefore costs 22 EXG, one store and one load a double
|   round, 115.6 cycles against 336 of arithmetic, so state traffic is a
|   quarter of the work rather than the half it was.
|
| The map, which repeats every double round
|
|       d0 x0   d1 x4   d2 x8   d3 x12   d4 x1  d5 x5  d6 x9  d7 x13
|       a0 x2   a1 x3   a2 x6   a3 x7    a4 x10 a5 x11 a6 x14   stack x15
|
|   It has to repeat, or the body cannot be a loop.  The repeat after one
|   double round with 22 exchanges rather than 24 follows from the two stack
|   words above.  Both SWAP8s use the same seven EXG pairs and both
|   SWAP4s the same four, because the column-to-diagonal relabelling is the
|   same permutation both times.
|
| Size
|
|   The body is 270 bytes against the 68020's 256-byte instruction cache, so it
|   overhangs by the size of one cache line.  Not unrolled further for that
|   reason: two double rounds are 540 bytes and are fetched from the bus every
|   pass.  A body under 256 bytes costs the immediate rotate counts, and the
|   rotates are the arithmetic.
|
| Registers
|
|   d0-d7    eight of the sixteen state words, two quarter-rounds' worth
|   a0-a6    seven more; a0 is the input pointer during the prologue only
|   0(sp)    x9 across G2 and G3, then the parking space of x11 in the epilogue
|   4(sp)    x15 across G4 and G1
|   8(sp)    the parking space of x14 in the epilogue
|   12(sp)   the double-round counter
|
|   m68k-amigaos: arguments on the stack, d0/d1/a0/a1 scratch, d2-d7/a2-a6
|   preserved.
|
| SPDX-License-Identifier: MIT

#ifdef C68K_ASM

        .text
        .even

| Stack after `movem.l d2-d7/a2-a6,-(sp)` (44 bytes) and a 16-byte frame.
        .set    CC_S0,   0
        .set    CC_S1,   4
        .set    CC_S2,   8
        .set    CC_CNT, 12
        .set    CC_IN,  64
        .set    CC_OUT, 68

| ---------------------------------------------------------------------------
| One quarter-round, RFC 8439 section 2.1, on four data registers.
|
| ROTL 16 is SWAP, at 3.99 cycles the cheapest rotate the part has.  ROTL 12
| is SWAP and then ROR.L #4, 9.97 cycles, because ROR.L takes an immediate
| count of 1..8 only and the register-count form costs 7.97 plus a data
| register this schedule does not have.  ROTL 8 and ROTL 7 are single
| instructions at 5.99.  Thirteen instructions, 26 bytes, ~42 cycles.
| ---------------------------------------------------------------------------
        .macro  CC_QR   a, b, c, d
        add.l   \b,\a
        eor.l   \a,\d
        swap    \d
        add.l   \d,\c
        eor.l   \c,\b
        swap    \b
        ror.l   #4,\b
        add.l   \b,\a
        eor.l   \a,\d
        rol.l   #8,\d
        add.l   \d,\c
        eor.l   \c,\b
        rol.l   #7,\b
        .endm

| ---------------------------------------------------------------------------
| The two shuffles.  \spill and \fill are the stack slots d6 changes over
| through.  Every other exchange is one EXG.
| ---------------------------------------------------------------------------
        .macro  CC_SWAP8  spill, fill
        exg     d0,a0
        exg     d1,a2
        exg     d2,a4
        exg     d3,a6
        exg     d4,a1
        exg     d5,a3
        exg     d7,a5
        move.l  d6,\spill(sp)
        move.l  \fill(sp),d6
        .endm

        .macro  CC_SWAP4
        exg     d0,a0
        exg     d3,a3
        exg     d4,a1
        exg     d5,a6
        .endm

| ---------------------------------------------------------------------------
| VOID c68k_chacha20_core_asm(const ULONG *in, ULONG *out)
|
| out[i] = round20(in)[i] + in[i], sixteen words.  `in` and `out` can be the
| same array: the epilogue reads and writes one index at a time and never
| reads an index it already wrote.
| ---------------------------------------------------------------------------
        .globl  _c68k_chacha20_core_asm
_c68k_chacha20_core_asm:
        movem.l d2-d7/a2-a6,-(sp)
        lea     -16(sp),sp

        movea.l CC_IN(sp),a0
        move.w  #10,CC_CNT(sp)

        move.l  (a0),d0                 | x0
        move.l  16(a0),d1               | x4
        move.l  32(a0),d2               | x8
        move.l  48(a0),d3               | x12
        move.l  4(a0),d4                | x1
        move.l  20(a0),d5               | x5
        move.l  36(a0),d6               | x9
        move.l  52(a0),d7               | x13
        movea.l 12(a0),a1               | x3
        movea.l 24(a0),a2               | x6
        movea.l 28(a0),a3               | x7
        movea.l 40(a0),a4               | x10
        movea.l 44(a0),a5               | x11
        movea.l 56(a0),a6               | x14
        move.l  60(a0),CC_S1(sp)        | x15
        movea.l 8(a0),a0                | x2, last, this was the pointer

| --------------------------------------------------------- the double round
.Lcc_round:
        CC_QR   d0,d1,d2,d3             | QR(x0, x4, x8,  x12)
        CC_QR   d4,d5,d6,d7             | QR(x1, x5, x9,  x13)
        CC_SWAP8 CC_S0, CC_S1           | x9 out, x15 in
        CC_QR   d0,d1,d2,d3             | QR(x2, x6, x10, x14)
        CC_QR   d4,d5,d7,d6             | QR(x3, x7, x11, x15)
        CC_SWAP4
        CC_QR   d0,d3,d2,d6             | QR(x0, x5, x10, x15)
        CC_QR   d4,d1,d7,d5             | QR(x1, x6, x11, x12)
        CC_SWAP8 CC_S1, CC_S0           | x15 out, x9 in
        CC_QR   d0,d3,d2,d7             | QR(x2, x7, x8,  x13)
        CC_QR   d4,d1,d6,d5             | QR(x3, x4, x9,  x14)
        CC_SWAP4

        subq.w  #1,CC_CNT(sp)
        bne     .Lcc_round

| ------------------------------------------------- the original added back in
| Two address registers have to become the two pointers, so x11 and x14 are
| parked in the two frame slots the round loop no longer needs.  The eight in
| data registers go first, which frees d0 as the staging register for the seven
| that are not.
        move.l  a5,CC_S0(sp)            | x11
        move.l  a6,CC_S2(sp)            | x14
        movea.l CC_IN(sp),a5
        movea.l CC_OUT(sp),a6

        add.l   (a5),d0
        move.l  d0,(a6)                 | x0
        add.l   16(a5),d1
        move.l  d1,16(a6)               | x4
        add.l   32(a5),d2
        move.l  d2,32(a6)               | x8
        add.l   48(a5),d3
        move.l  d3,48(a6)               | x12
        add.l   4(a5),d4
        move.l  d4,4(a6)                | x1
        add.l   20(a5),d5
        move.l  d5,20(a6)               | x5
        add.l   36(a5),d6
        move.l  d6,36(a6)               | x9
        add.l   52(a5),d7
        move.l  d7,52(a6)               | x13

        move.l  a0,d0
        add.l   8(a5),d0
        move.l  d0,8(a6)                | x2
        move.l  a1,d0
        add.l   12(a5),d0
        move.l  d0,12(a6)               | x3
        move.l  a2,d0
        add.l   24(a5),d0
        move.l  d0,24(a6)               | x6
        move.l  a3,d0
        add.l   28(a5),d0
        move.l  d0,28(a6)               | x7
        move.l  a4,d0
        add.l   40(a5),d0
        move.l  d0,40(a6)               | x10
        move.l  CC_S0(sp),d0
        add.l   44(a5),d0
        move.l  d0,44(a6)               | x11
        move.l  CC_S2(sp),d0
        add.l   56(a5),d0
        move.l  d0,56(a6)               | x14
        move.l  CC_S1(sp),d0
        add.l   60(a5),d0
        move.l  d0,60(a6)               | x15

        lea     16(sp),sp
        movem.l (sp)+,d2-d7/a2-a6
        rts


| ---------------------------------------------------------------------------
| VOID c68k_chacha20_xor_block_asm(const ULONG *ks, const UCHAR *in,
|                                  UCHAR *out)
|
| out = in XOR the little-endian serialisation of one 64-byte keystream block.
|
| Here because it measured a sixth of the cipher.  The C beside it is already
| the right seven instructions a word, load, the three-instruction byte
| reversal, load, EOR, store, and at -O3 that is what it compiled to; at -Os
| GCC gave every word two LEAs, a reloaded `moveq #64` and the 68020's
| full-extension double-indexed addressing for both the load and the store,
| about 64 cycles a word where 37 buys the same four bytes.  Post-increment
| addressing is the whole fix; there is nothing to schedule.
|
| Unrolled four words, so the DBRA is amortised and the body is 60 bytes.
| `in` and `out` need no alignment: the 68020 does misaligned longwords in
| hardware, and a TLS record's payload starts 21 bytes into the packet.
| ---------------------------------------------------------------------------
        .globl  _c68k_chacha20_xor_block_asm
_c68k_chacha20_xor_block_asm:
        movem.l d2/a2,-(sp)
        movea.l 12(sp),a0               | the keystream, longword aligned
        movea.l 16(sp),a1               | plaintext, any alignment
        movea.l 20(sp),a2               | ciphertext, any alignment
        moveq   #3,d2
.Lccx_four:
        .rept   4
        move.l  (a0)+,d0
        rol.w   #8,d0
        swap    d0
        rol.w   #8,d0
        move.l  (a1)+,d1
        eor.l   d1,d0
        move.l  d0,(a2)+
        .endr
        dbra    d2,.Lccx_four
        movem.l (sp)+,d2/a2
        rts

#endif /* C68K_ASM */
