| AmiNetXDuo -- crypto68k: the SHA-256 compression function, 68020 assembly.
|
| WHY THIS IS WORTH WRITING BY HAND
|
|   SHA-256 is rotations, and the 68020 has real ones.  ROR.L and ROL.L take
|   an immediate count of 1 to 8; SWAP is a 16-bit rotate for four cycles; and
|   every rotation the algorithm asks for is one or two of those:
|
|       ROTR(x,11) = SWAP then ROL.L #5      ROTR(x,17) = SWAP then ROR.L #1
|       ROTR(x,19) = SWAP then ROR.L #3      ROTR(x,20) = SWAP then ROR.L #4
|
|   A compiler cannot use that.  For a count above eight it must put the count
|   in a data register first -- and there are eight of those, all of them
|   holding live state.  So the portable C spills, and the spills are the
|   difference this file is trying to collect.
|
|   The rotations are also FACTORED, because rotation distributes over
|   exclusive or:
|
|       Sigma0(x) = ROTR2(x ^ ROTR11(x) ^ ROTR20(x))
|       Sigma1(x) = ROTR6(x ^ ROTR5(x)  ^ ROTR19(x))
|
|   Written out, Sigma0 is ROTR2 ^ ROTR13 ^ ROTR22, and the cheapest chaining
|   of those is four rotates and one SWAP; factored it is three rotates and
|   two SWAPs, and a SWAP is half a rotate on this part.  It is worth a few
|   cycles a round and no more -- see docs/RESEARCH.md 16 for what the whole
|   file is worth against what GCC 15.2 emits, which is less than it looks
|   like it should be.
|
| THE REGISTER PROBLEM, AND WHAT SOLVES IT
|
|   Eight state variables, eight data registers, and a round needs two
|   scratch.  Two of the eight are pure addends in any given round -- `d` is
|   only ever `d = d + T1` and `h` is only ever summed into T1 -- and an
|   address register can do ADD and MOVE even though it cannot do AND, EOR or
|   a rotate.  So `d` and `h` live in a4 and a5, six state variables live in
|   d2-d7, and d0 and d1 are scratch.  `g` moves to a5 the instant Ch has
|   finished with it, which frees a THIRD scratch for the second half of the
|   round.
|
|   The names then rotate, and they are rotated with seven MOVEs rather than
|   by unrolling eight rounds with eight register assignments.  That is
|   deliberate: seven MOVEs are fourteen cycles, and the eight-round unroll
|   that would remove them is 770 bytes of straight-line code against a
|   256-byte instruction cache.  The loop below is 96 bytes and is fetched
|   once.  tests/crypto68k/crypto68k_bulk sweeps a straight-line body from 32
|   bytes to 2 KB so that this is a measured limit and not a remembered one.
|
| REGISTERS, in the round loop
|
|   d2 a   d3 b   d4 c   d5 e   d6 f   d7 g       a4 d   a5 h
|   d0, d1 scratch     a0 the message schedule, advancing     a1 K, advancing
|   a3 the input, advancing        a6 the state
|
| SPDX-License-Identifier: MIT

#ifdef C68K_ASM

        .text
        .even

| The stack frame: 64 longwords of message schedule, then the block counter,
| then eleven saved registers.  The counter gets a slot of its own rather than
| living in the caller's argument area -- decrementing an incoming argument in
| place works today and is exactly the kind of thing that stops working when a
| compiler decides to reuse the push.
        .set    SHA_W,          256
        .set    SHA_COUNT,      256
        .set    SHA_LOCAL,      264
        .set    SHA_SAVE,       44
        .set    SHA_ARG,        (SHA_LOCAL + SHA_SAVE)

| ---------------------------------------------------------------------------
| VOID c68k_sha256_blocks_asm(ULONG *state, const UCHAR *data, ULONG blocks)
| ---------------------------------------------------------------------------
        .globl  _c68k_sha256_blocks_asm
_c68k_sha256_blocks_asm:
        movem.l d2-d7/a2-a6,-(sp)
        lea     -SHA_LOCAL(sp),sp

        move.l  SHA_ARG+12(sp),d0       | blocks
        beq     .Lsha_done
        move.l  d0,SHA_COUNT(sp)
        movea.l SHA_ARG+4(sp),a6        | state
        movea.l SHA_ARG+8(sp),a3        | data

.Lsha_block:

| ---- W[0..15]: sixteen big-endian longwords, straight off the wire ---------
|
| One MOVE.L each, on any address.  The 68020 does misaligned accesses in
| hardware and a TLS record's payload starts 21 bytes into the packet buffer,
| so the aligned case is the rare one.  nx_crypto_sha2.c's W0() macro builds
| each of these from four byte loads, three shifts and three ORs.

        movea.l sp,a0
        .rept   16
        move.l  (a3)+,(a0)+
        .endr

| ---- W[16..63] -------------------------------------------------------------
|
|   W[t] = sigma1(W[t-2]) + W[t-7] + sigma0(W[t-15]) + W[t-16]
|
| a0 is &W[t], so the four inputs are at -8, -28, -60 and -64.  Computed in
| one pass up front rather than interleaved with the rounds: interleaved, the
| round's two scratch registers have to serve both and the whole thing spills.

        lea     SHA_W(sp),a2            | one past W[63]
.Lsha_sched:
        move.l  -8(a0),d0
        swap    d0
        ror.l   #1,d0                   | ROTR17
        move.l  d0,d1
        ror.l   #2,d1                   | ROTR19
        eor.l   d1,d0
        move.l  -8(a0),d1
        lsr.l   #8,d1
        lsr.l   #2,d1                   | >> 10
        eor.l   d1,d0                   | sigma1
        add.l   -28(a0),d0
        add.l   -64(a0),d0

        move.l  -60(a0),d1
        ror.l   #7,d1                   | ROTR7
        move.l  -60(a0),d2
        swap    d2
        ror.l   #2,d2                   | ROTR18
        eor.l   d2,d1
        move.l  -60(a0),d2
        lsr.l   #3,d2
        eor.l   d2,d1                   | sigma0

        add.l   d1,d0
        move.l  d0,(a0)+
        cmpa.l  a2,a0
        bne     .Lsha_sched

| ---- the sixty-four rounds -------------------------------------------------

        movea.l sp,a0                   | W[0]
        lea     _c68k_sha256_k,a1
        move.l  (a6),d2                 | a
        move.l  4(a6),d3                | b
        move.l  8(a6),d4                | c
        movea.l 12(a6),a4               | d
        move.l  16(a6),d5               | e
        move.l  20(a6),d6               | f
        move.l  24(a6),d7               | g
        movea.l 28(a6),a5               | h

.Lsha_round:
        | ---- T1 = h + Sigma1(e) + Ch(e,f,g) + K[t] + W[t] ----
        move.l  d5,d0
        move.l  d5,d1
        ror.l   #5,d1
        eor.l   d1,d0
        move.l  d5,d1
        swap    d1
        ror.l   #3,d1                   | ROTR19(e)
        eor.l   d1,d0
        ror.l   #6,d0                   | Sigma1(e)

        move.l  d6,d1
        eor.l   d7,d1
        and.l   d5,d1
        eor.l   d7,d1                   | Ch(e,f,g) = g ^ (e & (f ^ g))
        add.l   d1,d0
        add.l   a5,d0                   | + h -- a5 is free from here
        movea.l d7,a5                   | the next round's h is this g;
                                        | d7 is now a third scratch
        add.l   (a1)+,d0                | + K[t]
        add.l   (a0)+,d0                | + W[t]        d0 = T1
        adda.l  d0,a4                   | d = d + T1, which is the next e

        | ---- T2 = Sigma0(a) + Maj(a,b,c), accumulated into T1 ----
        move.l  d2,d1
        eor.l   d3,d1                   | a ^ b
        move.l  d3,d7
        eor.l   d4,d7                   | b ^ c
        and.l   d7,d1
        eor.l   d3,d1                   | Maj = b ^ ((a^b) & (b^c))
        add.l   d1,d0

        move.l  d2,d1
        move.l  d2,d7
        swap    d7
        rol.l   #5,d7                   | ROTR11(a)
        eor.l   d7,d1
        move.l  d2,d7
        swap    d7
        ror.l   #4,d7                   | ROTR20(a)
        eor.l   d7,d1
        ror.l   #2,d1                   | Sigma0(a)
        add.l   d1,d0                   | d0 = T1 + T2, the next a

        | ---- rotate the names: a,b,c,d,e,f,g,h <- h',a,b,c,d',e,f,g ----
        move.l  a4,d1                   | d' out of the way first
        movea.l d4,a4                   | the next d is this c
        move.l  d6,d7                   | g  <- f
        move.l  d5,d6                   | f  <- e
        move.l  d1,d5                   | e  <- d'
        move.l  d3,d4                   | c  <- b
        move.l  d2,d3                   | b  <- a
        move.l  d0,d2                   | a  <- h'

        cmpa.l  #(_c68k_sha256_k + 256),a1
        bne     .Lsha_round

| ---- fold into the state ---------------------------------------------------

        add.l   d2,(a6)
        add.l   d3,4(a6)
        add.l   d4,8(a6)
        move.l  a4,d0
        add.l   d0,12(a6)
        add.l   d5,16(a6)
        add.l   d6,20(a6)
        add.l   d7,24(a6)
        move.l  a5,d0
        add.l   d0,28(a6)

        subq.l  #1,SHA_COUNT(sp)
        bne     .Lsha_block

.Lsha_done:
        lea     SHA_LOCAL(sp),sp
        movem.l (sp)+,d2-d7/a2-a6
        rts

#endif /* C68K_ASM */
