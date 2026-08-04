| AmiNetXDuo, crypto68k: the Poly1305 block function, 68020 assembly.
|
|   One entry point with the same interface as c68k_poly1305_blocks_c() beside
|   it: h = (h + m) * r mod 2^130 - 5 over some number of whole 16-byte
|   blocks, five limbs of 26 bits, `hibit` supplying the 2^128 term that a
|   full block carries and a short final one does not.  Everything else --
|   the clamp, the buffering, the final reduction, the one-time key, stays
|   in C, as c68k_chacha20.S leaves the AEAD there.
|
|   Written here from RFC 8439 section 2.5, as the C beside it was.  The
|   five-limb radix-2^26 split is the standard 32-bit decomposition --
|   Bernstein described it and poly1305-donna (Andrew Moon, public domain) made
|   it the common spelling, and the clamp constants are the RFC's.  No code
|   or schedule was taken from donna or any other implementation; what is
|   shared is the RFC's mathematics, and nothing below, the register map, the
|   memory-operand multiply, the misaligned limb loads, the loop over the five
|   product columns, has a counterpart there.  The schedule was simulated
|   against RFC 8439 section 2.5.2 before this was written, and the result is
|   checked against 2.5.2, against 2.8.2 and against the C limb for limb over
|   many blocks by tests/crypto68k/crypto68k_bulk.
|
| Why it exists, and what the ceiling is
|
|   docs/RESEARCH.md 58 wrote the cipher and left this: Poly1305 became 64% of
|   an AEAD record.  Its estimate of what was available was 20-30% rather than
|   the third ChaCha20 gave up, because twenty-five MULU.L a block cannot be
|   scheduled away, 1,075 cycles of about 2,900 on real hardware, and this
|   emulator charges 32 for MULU.L where a 68020 charges 43
|   (docs/RESEARCH.md 18.1, 54).
|
|   The other two thirds are the 130-bit accumulate, where the compiler is at a
|   structural disadvantage rather than a scheduling one.  A 32x32 -> 64
|   product lands in a register pair and a 64-bit add is ADD.L plus ADDX.L; C
|   can only reach either through `unsigned long long`, and at -Os GCC gives
|   every one of the twenty-five products a round trip through the frame, two
|   stores and two loads, ~28 cycles, 690 a block, on top of not inlining the
|   little-endian limb load at all.  That is the 300-byte stack frame in the
|   -Os disassembly, and what this file removes.
|
| Fourteen live values, fifteen registers, and MULU.L's <ea>
|
|   A block is
|
|       d_j = sum over i of h_i * K[j][i],   j, i in 0..4
|
|   where K is r0..r4 below the diagonal and 5*r1..5*r4 above it, the
|   reduction of the terms that fall off the top of 2^130, folded into the
|   multiplier instead of into a separate pass.  So the values live across the
|   whole block are five accumulator limbs and nine constants, and the five
|   new limbs have to go somewhere as they are produced: more than eight data
|   registers, and more than fifteen registers.
|
|   It fits because MULU.L takes a data addressing mode, not just a register:
|
|       mulu.l  d16(An),Dh:Dl        Dl * memory -> Dh:Dl
|
|   so the nine constants live in the frame and are never loaded, and the five
|   accumulator limbs live in address registers, MOVE.L An,Dn costs what
|   MOVE.L Dn,Dm costs, and the multiplicand has to be moved into Dl either
|   way.  tests/crypto68k/crypto68k_bulk prices the operand before this file
|   relies on it; it is very nearly free:
|
|       MULU.L Dn,Dh:Dl          31.90 cycles     the register form
|       MULU.L (An),Dh:Dl        31.90            straight out of the frame
|       MULU.L d16(An),Dh:Dl     33.94            ... with a displacement
|
|   Two cycles on a 32-cycle instruction, and nothing for the undisplaced
|   form, so the constant pointer is stepped rather than indexed from the frame
|   pointer.  ADDX.L Dn,Dm costs 1.99 in the same table, so a 64-bit accumulate
|   is 4 cycles on top of the multiply and the whole block is twenty-five
|   products of four instructions, MOVE.L An,Dl, MULU.L, ADD.L, ADDX.L --
|   with no spills.
|
| The loop over j is a cache decision, not a size one
|
|   Fully unrolled, the five columns are 25 products of 12 bytes and the block
|   body comes to about 500 bytes against a 256-byte instruction cache.  Rolled
|   up, the five products unrolled, the five columns a DBRA, the per-block
|   body is 228 bytes and the loop that runs five times inside it is 80, so the
|   whole of it fits.  The price is a DBRA, a pointer step and a store of h'[j]
|   to the frame per column, and reading those five back in the message phase:
|   roughly 125 cycles of about 1,425, a tenth.
|
|   This is the one measurement in the file the emulator argues against.  It
|   does not charge for instruction fetch, section 1's cache sweep reports a
|   2048-byte straight-line body faster per ADD.L (144.8 ns) than a 256-byte
|   one (151.2), so the unrolled form measures better here and would be
|   fetching 500 bytes over a 16-bit bus on the target machine.  Every figure
|   this file is credited with is therefore a lower bound on real hardware, by
|   a wider margin than the unrolled version would be.
|
|   The constants are laid out so that one pointer serves the whole loop:
|
|       C[0..8] = s1 s2 s3 s4 r0 r1 r2 r3 r4      s_i = 5 * r_i
|
|   because K[j][i] is exactly C[4+j-i], check the corners, K[0][0] = C[4] =
|   r0 and K[4][4] = C[4] = r0, K[0][4] = C[0] = s1, K[4][0] = C[8] = r4, so
|   column j reads five descending longwords from &C[4+j], and stepping to the
|   next column is one ADDQ.
|
| The message: five misaligned reads, not four aligned ones
|
|   A 16-byte block splits into limbs at bits 26, 52, 78 and 104, which do not
|   fall on longword boundaries, so the obvious form reads four longwords and
|   reassembles each limb from two of them with shift counts of 26, 20, 14, 12,
|   18 and 6, and LSR.L takes an immediate count of 1..8 only, so every one
|   of those is a register count or a SWAP and a shift.
|
|   Reading the block at byte offsets 0, 3, 6, 9 and 12 instead puts every
|   limb inside one longword, and the shift counts become 0, 2, 4, 6 and 8, all
|   immediate.  The 68020 does misaligned longword reads in hardware, and none
|   of the five reads leaves the block.  It costs one more byte reversal than
|   the four-read form and saves the whole reassembly.
|
| Registers
|
|   d0       the low half of the 64-bit column accumulator, and between
|            columns the carry, it is left there rather than moved
|   d1       the high half
|   d2, d3   MULU.L's Dl:Dh, Dl carries the multiplicand in
|   d4       blocks remaining
|   d5       0x03FFFFFF, the limb mask
|   d6       the column counter
|   d7       63, the six bits of carry that come out of the accumulator low
|   a0-a4    h0..h4, the accumulator, with the message already added
|   a5       &C[4+j], the constant pointer
|   a6       &h'[j], where the reduced limbs go
|   frame    h'[0..4], C[0..8], hibit, the message pointer
|
|   m68k-amigaos: arguments on the stack, d0/d1/a0/a1 scratch, d2-d7/a2-a6
|   preserved.
|
| Constant time: no branch below depends on anything but the block count, and
| there is no table.
|
| SPDX-License-Identifier: MIT

#ifdef C68K_ASM

        .text
        .even

| Frame, after `movem.l d2-d7/a2-a6,-(sp)` (44 bytes) and `lea -68(sp),sp`.
        .set    PB_H0,    0
        .set    PB_H1,    4
        .set    PB_H2,    8
        .set    PB_H3,   12
        .set    PB_H4,   16
        .set    PB_C,    20             | C[0..8]: s1 s2 s3 s4 r0 r1 r2 r3 r4
        .set    PB_C4,   36             | &C[4], where column 0 starts
        .set    PB_HI,   56
        .set    PB_MSG,  60
        .set    PB_END,  68

        .set    PB_CTX, 116             | 68 + 44 + 4
        .set    PB_M,   120
        .set    PB_BLK, 124
        .set    PB_HIBIT, 128

| C68K_POLY1305, from c68k_poly1305.h.  c68k_poly1305.c fails the build if
| either of these two moves.
        .set    PB_CTX_R, 0             | ULONG r[5]
        .set    PB_CTX_H, 20            | ULONG h[5]

| ---------------------------------------------------------------------------
| One little-endian limb: the longword at `off` bytes into the block, shifted
| down by `sh` so that the limb sits at bit 0.  The 68020 reads the misaligned
| longword in hardware; the three-instruction reversal is the endianness tax
| docs/RESEARCH.md 54.3 priced.
|
| \msk is `d5` where the shift leaves more than 26 bits and blank where it does
| not, offset 9 shifted down 6 has exactly 26 left, and offset 12 shifted
| down 8 has 24 and takes the 2^128 bit instead.
| ---------------------------------------------------------------------------
        .macro  PB_LIMB off, sh, msk
        move.l  \off(a5),d2
        rol.w   #8,d2
        swap    d2
        rol.w   #8,d2
        .ifne   \sh
        lsr.l   #\sh,d2
        .endif
        .ifnb   \msk
        and.l   \msk,d2
        .endif
        .endm

| ---------------------------------------------------------------------------
| One product: h_i * C[4+j-i], accumulated into d1:d0.
| ---------------------------------------------------------------------------
        .macro  PB_MUL h, k
        move.l  \h,d2
        mulu.l  \k(a5),d3:d2
        add.l   d2,d0
        addx.l  d3,d1
        .endm

| ---------------------------------------------------------------------------
| VOID c68k_poly1305_blocks_asm(C68K_POLY1305 *ctx, const UCHAR *m,
|                               ULONG blocks, ULONG hibit)
| ---------------------------------------------------------------------------
        .globl  _c68k_poly1305_blocks_asm
_c68k_poly1305_blocks_asm:
        movem.l d2-d7/a2-a6,-(sp)
        lea     -PB_END(sp),sp

        move.l  PB_BLK(sp),d4                   | blocks remaining
        beq     .Lpb_out
        move.l  PB_M(sp),PB_MSG(sp)
        move.l  PB_HIBIT(sp),PB_HI(sp)

        movea.l PB_CTX(sp),a0

| C[4] = r0, C[5..8] = r1..r4, C[0..3] = 5*r1..5*r4.
        move.l  PB_CTX_R(a0),PB_C+16(sp)
        lea     PB_CTX_R+4(a0),a1
        lea     PB_C(sp),a2                     | &C[0], the five-times half
        lea     PB_C+20(sp),a3                  | &C[5], r1..r4
        moveq   #3,d6
.Lpb_five:
        move.l  (a1)+,d0
        move.l  d0,(a3)+
        move.l  d0,d1
        lsl.l   #2,d0
        add.l   d1,d0
        move.l  d0,(a2)+
        dbra    d6,.Lpb_five

| The accumulator arrives reduced and leaves reduced, so it goes to the frame
| where the column loop writes it, not to the address registers, the first
| thing the block loop does is fold and add the message, and that reads it
| from there.
        lea     PB_CTX_H(a0),a1
        move.l  (a1)+,PB_H0(sp)
        move.l  (a1)+,PB_H1(sp)
        move.l  (a1)+,PB_H2(sp)
        move.l  (a1)+,PB_H3(sp)
        move.l  (a1),PB_H4(sp)

        move.l  #0x03FFFFFF,d5
        moveq   #63,d7
        moveq   #0,d0                           | no carry into the first fold

| ------------------------------------------------------------ one block ----
| h += m, with the previous block's carry folded into h0 on the way through.
| The fold is here rather than at the end of the column loop because it is the
| same six instructions either way and this way the loop has one exit.
.Lpb_block:
        movea.l PB_MSG(sp),a5

        move.l  d0,d1
        lsl.l   #2,d0
        add.l   d1,d0                           | 5c
        add.l   PB_H0(sp),d0
        rol.l   #6,d0
        move.l  d0,d1
        lsr.l   #6,d1                           | h0, back under 2^26
        and.l   d7,d0                           | and what it carried out

        PB_LIMB 0, 0, d5
        add.l   d2,d1
        movea.l d1,a0

        move.l  PB_H1(sp),d1
        add.l   d0,d1                           | the fold's carry lands here
        PB_LIMB 3, 2, d5
        add.l   d2,d1
        movea.l d1,a1

        move.l  PB_H2(sp),d1
        PB_LIMB 6, 4, d5
        add.l   d2,d1
        movea.l d1,a2

        move.l  PB_H3(sp),d1
        PB_LIMB 9, 6,                           | 26 bits already, no mask
        add.l   d2,d1
        movea.l d1,a3

        move.l  PB_H4(sp),d1
        PB_LIMB 12, 8,
        or.l    PB_HI(sp),d2                    | 2^128, or nothing at the end
        add.l   d2,d1
        movea.l d1,a4

        lea     16(a5),a5                       | ADDQ tops out at 8
        move.l  a5,PB_MSG(sp)

| ---------------------------------------------------------- h = h * r ------
| Five columns, five products each.  a5 walks C[4] .. C[8] one longword a
| column and the five reads inside a column walk back down from it.
        lea     PB_C4(sp),a5
        movea.l sp,a6                           | &h'[0]
        moveq   #4,d6
        moveq   #0,d0                           | column 0 has no carry in
.Lpb_column:
        moveq   #0,d1
        PB_MUL  a0, 0
        PB_MUL  a1, -4
        PB_MUL  a2, -8
        PB_MUL  a3, -12
        PB_MUL  a4, -16
        addq.l  #4,a5

| d1:d0 is under 2^57, so the carry out is under 2^31 and fits a longword: it
| is the accumulator's top six bits ORed onto its high half shifted up six.
| The ROL puts those six at the bottom and leaves the limb above them, so one
| rotate serves both halves of the split.
        rol.l   #6,d0
        move.l  d0,d2
        lsr.l   #6,d2
        move.l  d2,(a6)+                        | h'[j]
        and.l   d7,d0
        lsl.l   #6,d1
        or.l    d1,d0                           | the carry, left in d0
        dbra    d6,.Lpb_column

        subq.l  #1,d4
        bne     .Lpb_block

| ------------------------------------------------------------- and out ----
| The last column's carry has nowhere to go but round the top, where 2^130 is
| 5, so it is the same fold the block loop opens with, without a message.
        move.l  d0,d1
        lsl.l   #2,d0
        add.l   d1,d0
        add.l   PB_H0(sp),d0
        rol.l   #6,d0
        move.l  d0,d1
        lsr.l   #6,d1                           | h0
        and.l   d7,d0
        add.l   PB_H1(sp),d0                    | h1

        movea.l PB_CTX(sp),a0
        move.l  d1,PB_CTX_H(a0)
        move.l  d0,PB_CTX_H+4(a0)
        move.l  PB_H2(sp),PB_CTX_H+8(a0)
        move.l  PB_H3(sp),PB_CTX_H+12(a0)
        move.l  PB_H4(sp),PB_CTX_H+16(a0)

.Lpb_out:
        lea     PB_END(sp),sp
        movem.l (sp)+,d2-d7/a2-a6
        rts

#endif /* C68K_ASM */
