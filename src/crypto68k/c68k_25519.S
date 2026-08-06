| AmiNetXDuo, crypto68k 2^255-19 field multiply, 68000/68020/68060 assembly.
|
|   docs/RESEARCH.md 35.4 shipped c68k_25519.c with "No assembly", on the
|   grounds that 18.4 had established this GCC leaves nothing on the table for
|   a loop whose whole content is a multiply and an add, and it named the
|   condition for revisiting that: "Assembly is a separate question and should
|   be asked against a measurement of this, not instead of one."
|
|   The measurement now exists.  Profiling one real ssh handshake from inside
|   the guest (tools/profiler, 7,655 samples at 1000 Hz on a 13.9 MHz 68020)
|   puts 45.8% of the whole connection in fe_mul and 19.7% in fe_sqr.  The
|   field multiply is not a hot loop in a program, it very nearly IS the
|   program.
|
| WHY THIS BEATS THE C, WHICH IS NOT THE USUAL REASON
|
|   Not instruction selection.  GCC does emit MULU.L for the 68020 inner loop,
|   exactly as 35.4 said.  Three other things cost it:
|
|     - t[16] is zeroed with memset.  It shows up in the profile under its own
|       name at 4.3%, which is more than sha256 and chacha20 together.  Row 0
|       here has nothing to add to, so the low half needs no clearing at all
|       and the high half is written by the rows as they go.
|     - the accumulator round trip.  The C carries `v` as a uint64_t through
|       an array subscript on t[i+j], so every limb reloads and restores it;
|       below, the carry lives in a register and ADDX folds the previous
|       limb's memory carry in for free.
|     - the 38-fold is a second pass over sixteen words in the C.  Here it is
|       the same row primitive with a multiplier of 38.
|
| THE TWO PARTS, WHICH NEED DIFFERENT MULTIPLIES
|
|   68020/68040   MULU.L Dn,Dh:Dl, 32x32 -> 64 in one instruction.
|   68000/68060   that form does not exist.  The 68000 never had it; the 68060
|                 dropped it and traps to vector 61, where 68060.library
|                 emulates it at a cost far worse than doing it by hand.  Both
|                 have MULU.W, so the product is built from four of them, the
|                 way c68k_prim_mulw.S builds it.
|
|   Both rows below are transcribed from the two proven files rather than
|   invented: the MULU.L row is c68k_prim.S's, the MULU.W row is
|   c68k_prim_mulw.S's, including its T-split and its rule about what may
|   touch X.  The commentary there is the explanation for both; what is new
|   here is only the 8x8 structure around them and the 38-fold.
|
| THE ROW IS FULLY UNROLLED, and that is forced rather than chosen.  The
| MULU.W row needs all eight data registers, so there is no register left to
| count a loop in, and the loop cannot be closed on a SUB/BNE anyway because
| nothing between the deferred X and the ADDX that consumes it may disturb it.
| Eight limbs is a fixed length here, so the counter simply goes away.
|
| Register rules (m68k-amigaos, stack calling convention)
|
|   Arguments on the stack from 4(sp) upward; return value in d0; d0/d1/a0/a1
|   are scratch and d2-d7/a2-a6 must be preserved.
|
| SPDX-License-Identifier: MIT

#if defined(C68K_ASM) || defined(C68K_ASM_MULW)

        .text
        .align  2

#ifdef C68K_ASM_MULW

| One limb, four MULU.W.  See c68k_prim_mulw.S for why the T split is shaped
| this way and why SUBX/AND/OR is the cheapest route from X to bit 48.
|
|   CIN   the carry register this limb reads
|   COUT  the one it leaves for the next
|
| d1 is a hard zero throughout, d2 is a's low half and d5 is a's high half.

#define FEROW(CIN, COUT)                        \
        move.l  (a1)+,d3                      ; \
        move.l  d3,d6                         ; \
        move.l  d3,d4                         ; \
        swap    d4                            ; \
        move.l  d4,COUT                       ; \
        mulu.w  d2,d3                         ; \
        mulu.w  d5,d6                         ; \
        mulu.w  d2,d4                         ; \
        mulu.w  d5,COUT                       ; \
        addx.l  CIN,d3                        ; \
        addx.l  d1,COUT                       ; \
        add.l   d6,d4                         ; \
        subx.l  CIN,CIN                       ; \
        and.l   #0x10000,CIN                  ; \
        swap    d4                            ; \
        or.w    d4,CIN                        ; \
        clr.w   d4                            ; \
        add.l   d4,d3                         ; \
        addx.l  CIN,COUT                      ; \
        add.l   d3,(a0)+

#else   /* C68K_ASM: 68020/68040 */

| One limb, one MULU.L.  c68k_prim.S's row.

#define FEROW(CIN, COUT)                        \
        move.l  (a1)+,d3                      ; \
        mulu.l  d2,COUT:d3                    ; \
        addx.l  CIN,d3                        ; \
        addx.l  d5,COUT                       ; \
        add.l   d3,(a0)+

#endif

| ---------------------------------------------------------------------------
| Lfe_row  --  r[0..7] += a * b[0..7], eight limbs, carry out in d0.
|
| Internal, NOT the C ABI: a0 = r, a1 = b, d2 = a.  Clobbers d0-d7, a0, a1.
| The carry ping-pongs between d0 and d7 so that no limb spends an instruction
| moving it into place; eight limbs is an even number, so it ends in d0.
| ---------------------------------------------------------------------------

c68k_fe_row:
#ifdef C68K_ASM_MULW
        move.l  d2,d5
        swap    d5                      | d5's low word is a_hi
        moveq   #0,d1                   | the hard zero the ADDX sinks need
#else
        moveq   #0,d5                   | the hard zero, and a_hi is unused
#endif
        moveq   #0,d0                   | carry in
        moveq   #0,d7                   | the other carry
        moveq   #0,d3
        add.l   d3,d3                   | X = 0

        FEROW(d0, d7)
        FEROW(d7, d0)
        FEROW(d0, d7)
        FEROW(d7, d0)
        FEROW(d0, d7)
        FEROW(d7, d0)
        FEROW(d0, d7)
        FEROW(d7, d0)

        moveq   #0,d3                   | MOVEQ leaves X alone
        addx.l  d3,d0                   | fold the last memory carry in
        rts

| ---------------------------------------------------------------------------
| void c68k_fe_mul_asm(fe r, const fe a, const fe b)
|
|   r = a * b mod 2^255-19, lazily reduced, exactly as c68k_25519.c's fe_mul:
|   the schoolbook 8x8 into t[16], then r = t_low + 38 * t_high, then the same
|   carry fold.  r may alias a or b -- r is not written until the product is
|   complete, which is what makes that safe.
|
| Eleven registers saved, then 64 bytes of t, so the return address is at
| 108(sp) and the arguments follow it.
| ---------------------------------------------------------------------------

        .globl  _c68k_fe_mul_asm
_c68k_fe_mul_asm:
        movem.l d2-d7/a2-a6,-(sp)       | 11 longs
        suba.l  #64,sp                  | t[16]

        move.l  112(sp),a2              | r
        move.l  116(sp),a3              | a
        move.l  120(sp),a4              | b
        lea     32(a3),a6               | one past a[7]
        move.l  sp,a5                   | t, and the row destination

        | t[0..7] = 0.  Only the low half: row i writes t[i+8] itself, so
        | every word the rows read above t[7] has already been stored.
        move.l  a5,a0
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+

Lfe_mul_outer:
        move.l  (a3)+,d2                | a[i]
        move.l  a5,a0                   | t + i
        move.l  a4,a1                   | b
        bsr     c68k_fe_row
        move.l  d0,32(a5)               | t[i+8] = the carry out
        addq.l  #4,a5
        cmpa.l  a6,a3
        bne     Lfe_mul_outer

        move.l  sp,a4                   | t; a2 is still r from the top
        bsr     c68k_fe_reduce

        adda.l  #64,sp
        movem.l (sp)+,d2-d7/a2-a6
        rts

| ---------------------------------------------------------------------------
| c68k_fe_rowv  --  the same row for a VARIABLE number of limbs.
|
| a0 = r, a1 = b, a2 = one past the last b limb, d2 = a; carry out in d0, and
| a0 is left one past the last limb written, which is where the squaring wants
| to put it.  Clobbers d0-d7, a0, a1.
|
| fe_mul's rows are all eight limbs and take the unrolled c68k_fe_row above.
| The squaring's are 7, 6, 5, 4, 3, 2, 1 and 0, so this one closes on the end
| pointer instead of a counter -- there is no register to count in on the
| MULU.W path, and CMPA is one of the few instructions that leaves X alone.
| An odd length enters at the second phase, so the carry still finishes in d0.
| ---------------------------------------------------------------------------

c68k_fe_rowv:
        move.l  a2,d3
        sub.l   a1,d3                   | length in bytes
        beq     Lrowv_zero
        move.l  d3,d4                   | bit 2 is the limb count's bit 0
#ifdef C68K_ASM_MULW
        move.l  d2,d5
        swap    d5                      | d5's low word is a_hi
        moveq   #0,d1                   | the hard zero the ADDX sinks need
#else
        moveq   #0,d5                   | the hard zero
#endif
        moveq   #0,d0
        moveq   #0,d7
        moveq   #0,d3
        add.l   d3,d3                   | X = 0
        btst    #2,d4                   | BTST leaves X alone
        bne     Lrowv_half

Lrowv_loop:
        FEROW(d0, d7)
Lrowv_half:
        FEROW(d7, d0)
        cmpa.l  a2,a1                   | CMPA leaves X alone
        bne     Lrowv_loop

        moveq   #0,d3
        addx.l  d3,d0
        rts

Lrowv_zero:
        moveq   #0,d0
        rts

| ---------------------------------------------------------------------------
| c68k_fe_reduce  --  r = t_low + 38 * t_high, then the carry fold.
|
| a4 = t[16], a2 = r.  Shared by the multiply and the squaring, which reduce
| identically.  Clobbers d0-d7, a0, a1.
| ---------------------------------------------------------------------------

c68k_fe_reduce:
        move.l  a4,a0
        lea     32(a4),a1
        moveq   #38,d2
        bsr     c68k_fe_row             | d0 = the carry out, below 39

        | Copy t[0..7] out.  Writing r only here is what lets r alias a or b.
        move.l  a4,a0
        move.l  a2,a1
        move.l  (a0)+,(a1)+
        move.l  (a0)+,(a1)+
        move.l  (a0)+,(a1)+
        move.l  (a0)+,(a1)+
        move.l  (a0)+,(a1)+
        move.l  (a0)+,(a1)+
        move.l  (a0)+,(a1)+
        move.l  (a0)+,(a1)+

        move.l  d0,d1
        bsr     c68k_fe_foldc
        rts

| ---------------------------------------------------------------------------
| c68k_fe_foldc  --  fold a carry out of the top back into r.
|
| a2 = r, d1 = the carry, which may be 0.  2^256 = 38 (mod p), so a carry of c
| is worth 38c at the bottom.  Clobbers d1, d3, d4, d5, a0.
|
| The LOOP is required, and c68k_25519.c's fe_fold comment is the reason: with
| lazy reduction 0 is routinely carried as 2^256-38 and 1 as 2^256-37, so
| adding 38 to either carries straight through all eight limbs.  Dropping that
| second carry loses exactly 38, which is the bug that made an Ed25519
| doubling return 37 where it owed -1 and passed every published vector on the
| way.  A second pass always terminates: a carry out means every limb is now
| small.
| ---------------------------------------------------------------------------

c68k_fe_foldc:
        tst.l   d1
        beq     Lfoldc_done

Lfoldc_loop:
        mulu.w  #38,d1                  | c * 38; c < 39, so this fits a word
        move.l  a2,a0
        add.l   d1,(a0)+                | r[0] += it; X = the carry
        moveq   #0,d3
        moveq   #6,d4                   | seven limbs left to ripple through
Lfoldc_ripple:
        move.l  (a0),d5                 | MOVE leaves X alone
        addx.l  d3,d5
        move.l  d5,(a0)+
        dbra    d4,Lfoldc_ripple        | DBRA leaves CCR alone
        subx.l  d1,d1                   | d1 = -X
        neg.l   d1                      | d1 = X, so the next pass folds 38
        bne     Lfoldc_loop

Lfoldc_done:
        rts

| ---------------------------------------------------------------------------
| void c68k_fe_add_asm(fe r, const fe a, const fe b)
| void c68k_fe_sub_asm(fe r, const fe a, const fe b)
|
|   r = a + b and r = a - b, both lazily reduced, exactly as c68k_25519.c's
|   fe_add and fe_sub.  Nothing clever: eight limbs of ADDX or SUBX with the
|   carry in X where the C had to carry it in a uint64_t through an array
|   subscript, reloading and restoring it every limb.
|
|   These are here because they stopped being rounding.  With fe_mul and
|   fe_sqr in assembly, fe_sub was the largest routine left in an ssh
|   handshake at 9.6% and fe_add was 5.1%.
|
|   Five registers saved, so the arguments sit at 24, 28 and 32.
| ---------------------------------------------------------------------------

        .globl  _c68k_fe_add_asm
_c68k_fe_add_asm:
        movem.l d2-d5/a2,-(sp)          | 5 longs
        move.l  24(sp),a0               | r
        move.l  28(sp),a1               | a
        move.l  32(sp),a2               | b
        moveq   #0,d0
        add.l   d0,d0                   | X = 0
        moveq   #7,d1

Ladd_loop:
        move.l  (a1)+,d2                | MOVE leaves X alone
        move.l  (a2)+,d3
        addx.l  d3,d2
        move.l  d2,(a0)+
        dbra    d1,Ladd_loop            | DBRA leaves CCR alone

        moveq   #0,d1
        addx.l  d1,d1                   | d1 = the carry out, 0 or 1
        move.l  24(sp),a2               | r, for the fold
        bsr     c68k_fe_foldc
        movem.l (sp)+,d2-d5/a2
        rts

        .globl  _c68k_fe_sub_asm
_c68k_fe_sub_asm:
        movem.l d2-d5/a2,-(sp)          | 5 longs
        move.l  24(sp),a0               | r
        move.l  28(sp),a1               | a
        move.l  32(sp),a2               | b
        moveq   #0,d0
        sub.l   d0,d0                   | X = 0
        moveq   #7,d1

Lsub_loop:
        move.l  (a1)+,d2
        move.l  (a2)+,d3
        subx.l  d3,d2
        move.l  d2,(a0)+
        dbra    d1,Lsub_loop

        | A borrow out means the true value is a-b+2^256, and 2^256 = 38
        | (mod p), so 38 has to come back OFF.  Same trap as the fold above and
        | for the mirror-image reason: dropping the borrow silently adds 38.
        moveq   #0,d1
        subx.l  d1,d1                   | d1 = -X, so 0 or -1
        tst.l   d1
        beq     Lsub_done

Lsub_fold:
        move.l  24(sp),a0               | r
        moveq   #0,d0                   | the hard zero the SUBX sinks need
        moveq   #38,d2
        move.l  (a0),d3
        sub.l   d2,d3                   | r[0] -= 38; X = the borrow
        move.l  d3,(a0)+
        moveq   #6,d4
Lsub_ripple:
        move.l  (a0),d3                 | MOVE leaves X alone
        subx.l  d0,d3
        move.l  d3,(a0)+
        dbra    d4,Lsub_ripple
        moveq   #0,d3
        subx.l  d3,d3                   | d3 = -X
        tst.l   d3
        bne     Lsub_fold

Lsub_done:
        movem.l (sp)+,d2-d5/a2
        rts

| ---------------------------------------------------------------------------
| void c68k_fe_sqr_asm(fe r, const fe a)
|
|   r = a * a mod 2^255-19, the same lazily-reduced result as c68k_25519.c's
|   fe_sqr and by the same route: THIRTY-SIX products rather than sixty-four,
|   because every off-diagonal appears twice.  Sum the upper triangle once,
|   double the lot, then add the eight diagonal squares.
|
|   Row i is a[i] * a[i+1..7] landing at t[2i+1], so the rows shorten as i
|   grows and the last is empty -- which is not a special case here: a zero
|   length falls straight through c68k_fe_rowv and stores the 0 that the C
|   writes as t[15].
|
|   A transcription error in here would still produce plausible output, which
|   is why tests/crypto68k checks it against the portable C on random inputs
|   rather than only against published vectors.
| ---------------------------------------------------------------------------

        .globl  _c68k_fe_sqr_asm
_c68k_fe_sqr_asm:
        movem.l d2-d7/a2-a6,-(sp)       | 11 longs
        suba.l  #64,sp                  | t[16]

        move.l  116(sp),a3              | a
        lea     32(a3),a2               | one past a[7]; every row ends here
        move.l  sp,a4                   | t

        | t[0..7] = 0.  t[8..15] are written by the rows as their carries,
        | t[15] by the empty last row.
        move.l  a4,a0
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+
        clr.l   (a0)+

        lea     4(a4),a5                | row 0 lands at t[1]

Lsqr_outer:
        move.l  (a3)+,d2                | a[i]; a3 is now a + i + 1
        move.l  a5,a0
        move.l  a3,a1                   | the row runs a[i+1..7]
        bsr     c68k_fe_rowv
        move.l  d0,(a0)                 | t[i+8], where the row stopped
        addq.l  #8,a5                   | the next row starts two limbs on
        cmpa.l  a2,a3
        bne     Lsqr_outer

        | Double.  The top word cannot carry out: the off-diagonal sum is
        | below 2^(2*256-1).
        move.l  a4,a0
        moveq   #0,d3
        add.l   d3,d3                   | X = 0
        moveq   #15,d1
Lsqr_dbl:
        move.l  (a0),d2                 | MOVE leaves X alone
        roxl.l  #1,d2                   | in from X, out to X
        move.l  d2,(a0)+
        dbra    d1,Lsqr_dbl             | DBRA leaves CCR alone

        | Add the diagonal, a[i]^2 at t[2i].  The running carry lives in X
        | rather than a register, so d0 is a hard zero the ADDX reads.
        move.l  116(sp),a1              | a
        move.l  a4,a0                   | t
        moveq   #0,d0
        moveq   #0,d5
        moveq   #7,d4
        moveq   #0,d3
        add.l   d3,d3                   | X = 0

Lsqr_diag:
        move.l  (a1)+,d3                | a[i]
#ifdef C68K_ASM_MULW
        move.l  d3,d6
        swap    d6                      | d6's low word is a_hi
        move.l  d6,d2
        mulu.w  d3,d2                   | ah*al
        move.l  d6,d1
        mulu.w  d6,d1                   | ah*ah
        mulu.w  d3,d3                   | al*al
        addx.l  d0,d3                   | + the carry in X, before T touches it
        addx.l  d5,d1
        add.l   d2,d2                   | T = 2*ah*al; X = T's bit 32
        subx.l  d7,d7                   | d7 = -X
        and.l   #0x10000,d7             | d7 = X << 16
        swap    d2                      | d2 = T_lo:T_hi
        or.w    d2,d7                   | d7 = T's whole high-limb share
        clr.w   d2                      | d2 = T_lo << 16
        add.l   d2,d3
        addx.l  d7,d1
#else
        move.l  d3,d6
        mulu.l  d6,d1:d3                | d1:d3 = a[i]^2
        addx.l  d0,d3                   | + the carry in X
        addx.l  d5,d1
#endif
        add.l   d3,(a0)+                | t[2i]   += the low half
        move.l  (a0),d3
        addx.l  d1,d3                   | t[2i+1] += the high half + carry
        move.l  d3,(a0)+
        dbra    d4,Lsqr_diag

        move.l  112(sp),a2              | r.  a2 was the rows end pointer until
                                        | here, so it has to be reloaded before
                                        | the reduction, which writes through it
        bsr     c68k_fe_reduce

        adda.l  #64,sp
        movem.l (sp)+,d2-d7/a2-a6
        rts

#endif /* C68K_ASM || C68K_ASM_MULW */
