| AmiNetXDuo -- instruction-cost kernels for the AES and SHA-256 work.
|
| WHY THESE EXIST SEPARATELY FROM tests/perf/cpucal
|
|   cpucal answers "is this emulator charging for the instructions a real
|   68020 charges for", and its kernels are the ones whose costs the
|   MC68020UM publishes: MOVE, ADD, ADDX, MULU, and the memory sweeps.  The
|   bulk crypto work turns on a different set -- rotates by an immediate
|   count, SWAP, byte moves between registers, and the 68020's scaled indexed
|   addressing mode -- and the design questions in src/crypto68k/c68k_aes.h
|   and c68k_sha256.h are decided by their relative costs.
|
|   Written in assembly for the same reason cpucal's are: a C loop that
|   "should" compile to a rotate is a measurement of the compiler.
|
| WHAT EACH ONE IS FOR
|
|   ror8 / ror1 / rorreg / swap    SHA-256's rotations, and the price of the
|                                  one-table AES variant's three rotates per
|                                  column.  ROR.L takes an immediate count of
|                                  1..8 only; anything larger is either two
|                                  rotates, a SWAP and one rotate, or a count
|                                  in a data register -- and this is what says
|                                  which of the three to reach for.
|   idx1k / idx4k                  the AES table read.  Identical costs is
|                                  the whole argument for four tables over
|                                  one on a machine with no data cache: the
|                                  4 KB footprint buys nothing back, so the
|                                  rotates it removes are pure gain.
|   idxb                           the byte-S-box variant's read.
|   ic32 .. ic2048                 the instruction cache.  It is 256 bytes on
|                                  this part, so a straight-line body larger
|                                  than that costs more per instruction than
|                                  the same body wrapped in a loop -- which is
|                                  the limit on how far an AES round or a
|                                  SHA-256 block may be unrolled.
|
| Every kernel runs a body of exactly 16 instructions per rep, except the
| instruction-cache set, whose whole point is that the body is a different
| size.  _bk_empty is the loop with no body and is subtracted from all of them.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------------------
| VOID bk_empty(ULONG reps)
| ---------------------------------------------------------------------------
        .globl  _bk_empty
_bk_empty:
        move.l  4(sp),d0
        tst.l   d0
        beq     .Lbk_empty_done
.Lbk_empty_loop:
        subq.l  #1,d0
        bne     .Lbk_empty_loop
.Lbk_empty_done:
        rts


| ---------------------------------------------------------------------------
| Register-only kernels.  VOID bk_xxx(ULONG reps).
|
| The 32-byte body sits inside the 256-byte instruction cache, which is the
| condition the published cache-case timings assume.
| ---------------------------------------------------------------------------

        .macro  BK_REG name, op, args
        .globl  _bk_\name
_bk_\name:
        movem.l d2-d3,-(sp)
        move.l  12(sp),d0
        moveq   #13,d2
        move.l  #0x5A3C71E9,d3
        tst.l   d0
        beq     .Lbk_\name\()_done
.Lbk_\name\()_loop:
        .rept   16
        \op     \args
        .endr
        subq.l  #1,d0
        bne     .Lbk_\name\()_loop
.Lbk_\name\()_done:
        movem.l (sp)+,d2-d3
        rts
        .endm

        BK_REG  add,       add.l,  "d2,d3"
        BK_REG  eor,       eor.l,  "d2,d3"
        BK_REG  and,       and.l,  "d2,d3"
        BK_REG  moveb_dd,  move.b, "d2,d3"
        BK_REG  swapd,     swap,   "d3"
        BK_REG  ror1,      ror.l,  "#1,d3"
        BK_REG  ror8,      ror.l,  "#8,d3"
        BK_REG  rorreg,    ror.l,  "d2,d3"
        BK_REG  lsr3,      lsr.l,  "#3,d3"
        BK_REG  addself,   add.l,  "d3,d3"


| ---------------------------------------------------------------------------
| Data-to-address register traffic.  VOID bk_xxx(ULONG reps).
|
| ChaCha20 is sixteen 32-bit words against eight data registers, so eight of
| them are somewhere else at any moment and the question is where.  A spill to
| the stack is MOVE.L Dm,d16(An) out and MOVE.L d16(An),Dm back, which the
| rows above price at 8.8 and 5.0; an address register is a third home, and
| EXG Dn,An is the exchange the schedule actually wants -- one instruction that
| retires a live word and produces the next one in the same data register.
|
| These three rows decide src/crypto68k/c68k_chacha20.S's register allocation.
| ---------------------------------------------------------------------------

        .macro  BK_AREG name, op, args
        .globl  _bk_\name
_bk_\name:
        movem.l d2-d3/a2,-(sp)
        move.l  16(sp),d0
        move.l  #0x5A3C71E9,d3
        movea.l d3,a2
        tst.l   d0
        beq     .Lbk_\name\()_done
.Lbk_\name\()_loop:
        .rept   16
        \op     \args
        .endr
        subq.l  #1,d0
        bne     .Lbk_\name\()_loop
.Lbk_\name\()_done:
        movem.l (sp)+,d2-d3/a2
        rts
        .endm

        BK_AREG exg_da,    exg,     "d3,a2"
        BK_AREG movel_ad,  move.l,  "a2,d3"
        BK_AREG movel_da,  move.l,  "d3,a2"


| ---------------------------------------------------------------------------
| The 32x32 -> 64 multiply, and what its operand costs.  VOID bk_xxx(ULONG).
|
| Poly1305 is twenty-five MULU.L a block against nine constants and five
| accumulator limbs, which is fourteen live values for fifteen registers --
| and MULU.L's <ea> operand may be MEMORY, which is the only reason it fits
| at all.  These three rows are the price of that: if a longword read costs
| what MOVE.L d16(An),Dm costs then the constants live in the frame and the
| limbs live in registers, and if it costs much more the whole allocation has
| to be rethought.  ADDX.L is here because the 130-bit accumulate is one
| ADD.L and one ADDX.L per product and nothing else prices it.
|
| The multiplicand is d4 and the product overwrites d5:d4, so d4 walks a
| pseudo-random sequence rather than settling -- both seeds are odd and an odd
| product stays odd, so it never reaches zero and no early-out in the model
| can flatter the row.
| ---------------------------------------------------------------------------

        .macro  BK_MUL name, src
        .globl  _bk_\name
_bk_\name:
        movem.l d2-d5,-(sp)
        movea.l 20(sp),a0               | 4 KB scratch, used by the mem rows
        move.l  24(sp),d0
        move.l  #0x5A3C71E9,d2
        move.l  d2,(a0)
        move.l  d2,8(a0)
        move.l  #0x2F6B41D7,d4
        tst.l   d0
        beq     .Lbk_\name\()_done
.Lbk_\name\()_loop:
        .rept   16
        mulu.l  \src,d5:d4
        .endr
        subq.l  #1,d0
        bne     .Lbk_\name\()_loop
.Lbk_\name\()_done:
        movem.l (sp)+,d2-d5
        rts
        .endm

        BK_MUL  mulu_dd,   "d2"
        BK_MUL  mulu_an,   "(a0)"
        BK_MUL  mulu_d16,  "8(a0)"

        BK_REG  addx,      addx.l, "d2,d3"
        BK_REG  andi,      and.l,  "#0x03FFFFFF,d3"
        BK_REG  rol6,      rol.l,  "#6,d3"


| ---------------------------------------------------------------------------
| Table kernels.  VOID bk_xxx(APTR table, ULONG reps).
|
| a0..a3 point at the four 1 KB quarters of the caller's 4 KB buffer, so
| bk_idx4k reads from four different kilobytes and bk_idx1k reads from one.
| On a part with a data cache those two differ; on this one they must not.
| ---------------------------------------------------------------------------

        .globl  _bk_idx1k
_bk_idx1k:
        movem.l d2-d3/a2,-(sp)
        movea.l 16(sp),a0
        move.l  20(sp),d0
        moveq   #37,d2
        tst.l   d0
        beq     .Lbk_i1_done
.Lbk_i1_loop:
        .rept   16
        move.l  (a0,d2.w*4),d3
        .endr
        subq.l  #1,d0
        bne     .Lbk_i1_loop
.Lbk_i1_done:
        movem.l (sp)+,d2-d3/a2
        rts

        .globl  _bk_idx4k
_bk_idx4k:
        movem.l d2-d3/a2-a3,-(sp)
        movea.l 20(sp),a0
        move.l  24(sp),d0
        lea     1024(a0),a1
        lea     2048(a0),a2
        lea     3072(a0),a3
        moveq   #37,d2
        tst.l   d0
        beq     .Lbk_i4_done
.Lbk_i4_loop:
        .rept   4
        move.l  (a0,d2.w*4),d3
        move.l  (a1,d2.w*4),d3
        move.l  (a2,d2.w*4),d3
        move.l  (a3,d2.w*4),d3
        .endr
        subq.l  #1,d0
        bne     .Lbk_i4_loop
.Lbk_i4_done:
        movem.l (sp)+,d2-d3/a2-a3
        rts

        .globl  _bk_idxb
_bk_idxb:
        movem.l d2-d3,-(sp)
        movea.l 12(sp),a0
        move.l  16(sp),d0
        moveq   #37,d2
        tst.l   d0
        beq     .Lbk_ib_done
.Lbk_ib_loop:
        .rept   16
        move.b  (a0,d2.w),d3
        .endr
        subq.l  #1,d0
        bne     .Lbk_ib_loop
.Lbk_ib_done:
        movem.l (sp)+,d2-d3
        rts

        .globl  _bk_movel_d16
_bk_movel_d16:
        movem.l d2-d3,-(sp)
        movea.l 12(sp),a0
        move.l  16(sp),d0
        tst.l   d0
        beq     .Lbk_md_done
.Lbk_md_loop:
        .rept   16
        move.l  8(a0),d3
        .endr
        subq.l  #1,d0
        bne     .Lbk_md_loop
.Lbk_md_done:
        movem.l (sp)+,d2-d3
        rts

        .globl  _bk_moveb_d16
_bk_moveb_d16:
        movem.l d2-d3,-(sp)
        movea.l 12(sp),a0
        move.l  16(sp),d0
        tst.l   d0
        beq     .Lbk_mb_done
.Lbk_mb_loop:
        .rept   16
        move.b  5(a0),d3
        .endr
        subq.l  #1,d0
        bne     .Lbk_mb_loop
.Lbk_mb_done:
        movem.l (sp)+,d2-d3
        rts

        .globl  _bk_movel_store
_bk_movel_store:
        movem.l d2-d3,-(sp)
        movea.l 12(sp),a0
        move.l  16(sp),d0
        move.l  #0x5A3C71E9,d3
        tst.l   d0
        beq     .Lbk_ms_done
.Lbk_ms_loop:
        .rept   16
        move.l  d3,8(a0)
        .endr
        subq.l  #1,d0
        bne     .Lbk_ms_loop
.Lbk_ms_done:
        movem.l (sp)+,d2-d3
        rts


| ---------------------------------------------------------------------------
| Instruction-cache sweep.  VOID bk_icNNNN(ULONG reps), where NNNN is the size
| of the straight-line body in bytes.  ADD.L Dn,Dm is two bytes, so the body
| is `size / 2` of them and the reported cost is per ADD.L.
|
| The 68020's instruction cache is 256 bytes, direct mapped, 64 entries of one
| longword.  A body that fits runs at the published two cycles; a body that
| does not pays a fetch for every instruction, and the difference is the limit
| on unrolling anything in src/crypto68k/.
| ---------------------------------------------------------------------------

        .macro  BK_IC size
        .globl  _bk_ic\size
_bk_ic\size:
        movem.l d2-d3,-(sp)
        move.l  12(sp),d0
        moveq   #13,d2
        moveq   #3,d3
        tst.l   d0
        beq     .Lbk_ic\size\()_done
.Lbk_ic\size\()_loop:
        .rept   (\size / 2)
        add.l   d2,d3
        .endr
        subq.l  #1,d0
        bne     .Lbk_ic\size\()_loop
.Lbk_ic\size\()_done:
        movem.l (sp)+,d2-d3
        rts
        .endm

        BK_IC   32
        BK_IC   128
        BK_IC   256
        BK_IC   512
        BK_IC   1024
        BK_IC   2048
