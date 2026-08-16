| AmiNetXDuo, crypto68k: the AES round, 68020 assembly.
|
|   Four entry points, all with the same interface as the C round cores in
|   c68k_aes.c: (round keys, number of rounds, a four-longword state in
|   memory).  Two are the four-table layout and two the one-table-and-rotates
|   layout, so the two can be measured against each other on a machine with no
|   data cache.
|
|   The buffering, the key schedule and CBC stay in C.  They are 9% of a
|   block, they are where the awkward cases live, and the one instruction that
|   mattered in them, a misaligned MOVE.L for a big-endian longword, is
|   already inline assembly there.
|
| The state is kept in memory
|
|   A round needs four state words, four accumulators, an index and a
|   temporary: ten values against the 68020's eight data registers.  The two
|   candidates:
|
|     state in registers   16 byte extractions a round, each a MOVE.B out of
|                          the low byte plus a ROL.L to bring the next byte
|                          down, since only the low byte of a register can be
|                          moved out.  Two instructions a byte, and the four
|                          accumulators no longer fit either.
|     state in memory      16 MOVE.B d16(An),Dn a round.  One instruction a
|                          byte, and it frees d2-d5 to hold the accumulators,
|                          so the round ends with a single MOVEM.L.
|
|   The second is fewer instructions.  On a part with a data cache the sixteen
|   byte reads are nearly free and the gap is wide.  On this part they are bus
|   cycles and it is still ahead, because the rotates they replace are not free
|   either.  tests/crypto68k/crypto68k_bulk measures both forms of
|   the rotate and the byte read.
|
| The round loop is not unrolled
|
|   The 68020's instruction cache is 256 bytes.  The four-table round body
|   below is 176 bytes and the one-table body is 200, so each sits inside it
|   and is fetched once.  A two-round unroll puts the body at 350-400 bytes,
|   and every instruction fetch then goes to the bus.  The same benchmark
|   sweeps a straight-line body from 32 bytes to 2 KB and shows where that
|   turns.
|
| Registers
|
|   a0  round keys, advancing          d0  round counter
|   a1  Te0 / Td0, and the S-box in    d1  the value just looked up
|       the final round                d2-d5  the four accumulators
|   a2  Te1 / Td1                      d6, d7  index registers, kept
|   a3  Te2 / Td2                      zero-extended: nothing but MOVE.B
|   a4  Te3 / Td3                      ever writes them
|   a5  the state
|
|   m68k-amigaos: arguments on the stack from 4(sp), d0/d1/a0/a1 scratch,
|   d2-d7/a2-a6 preserved.
|
| SPDX-License-Identifier: MIT

#ifdef C68K_ASM

        .text
        .even

| Stack layout after `movem.l d2-d7/a2-a6,-(sp)`, eleven longwords.
        .set    AES_ARG,        44

| ---------------------------------------------------------------------------
| One output word, four tables.  \acc accumulates.  \o0..\o3 are the byte
| offsets into the state, which encode ShiftRows.
| ---------------------------------------------------------------------------
        .macro  AES_W4  acc, o0, o1, o2, o3
        move.l  (a0)+,\acc
        move.b  \o0(a5),d6
        move.l  (a1,d6.w*4),d1
        eor.l   d1,\acc
        move.b  \o1(a5),d7
        move.l  (a2,d7.w*4),d1
        eor.l   d1,\acc
        move.b  \o2(a5),d6
        move.l  (a3,d6.w*4),d1
        eor.l   d1,\acc
        move.b  \o3(a5),d7
        move.l  (a4,d7.w*4),d1
        eor.l   d1,\acc
        .endm

| ---------------------------------------------------------------------------
| The same word from one table, with the three rotations that recover the
| other three.  T1 = ROR 8, T2 = ROR 16 (which is SWAP), T3 = ROR 24 (ROL 8).
| ---------------------------------------------------------------------------
        .macro  AES_W1  acc, o0, o1, o2, o3
        move.l  (a0)+,\acc
        move.b  \o0(a5),d6
        move.l  (a1,d6.w*4),d1
        eor.l   d1,\acc
        move.b  \o1(a5),d7
        move.l  (a1,d7.w*4),d1
        ror.l   #8,d1
        eor.l   d1,\acc
        move.b  \o2(a5),d6
        move.l  (a1,d6.w*4),d1
        swap    d1
        eor.l   d1,\acc
        move.b  \o3(a5),d7
        move.l  (a1,d7.w*4),d1
        rol.l   #8,d1
        eor.l   d1,\acc
        .endm

| ---------------------------------------------------------------------------
| The last round: SubBytes, ShiftRows and AddRoundKey, no MixColumns.  a1 is
| the 256-byte S-box by now, so this is byte reads and a shift, which is
| cheaper than a mask over four table entries the way OpenSSL does it.
| ---------------------------------------------------------------------------
        .macro  AES_WL  acc, o0, o1, o2, o3
        moveq   #0,\acc
        move.b  \o0(a5),d6
        move.b  (a1,d6.w),\acc
        lsl.l   #8,\acc
        move.b  \o1(a5),d6
        move.b  (a1,d6.w),\acc
        lsl.l   #8,\acc
        move.b  \o2(a5),d6
        move.b  (a1,d6.w),\acc
        lsl.l   #8,\acc
        move.b  \o3(a5),d6
        move.b  (a1,d6.w),\acc
        move.l  (a0)+,d1
        eor.l   d1,\acc
        .endm

| ---------------------------------------------------------------------------
| The initial AddRoundKey, common to all four entry points.
| ---------------------------------------------------------------------------
        .macro  AES_ARK
        move.l  (a0)+,d1
        eor.l   d1,(a5)
        move.l  (a0)+,d1
        eor.l   d1,4(a5)
        move.l  (a0)+,d1
        eor.l   d1,8(a5)
        move.l  (a0)+,d1
        eor.l   d1,12(a5)
        .endm

        .macro  AES_ENTER
        movem.l d2-d7/a2-a6,-(sp)
        movea.l AES_ARG+4(sp),a0
        move.l  AES_ARG+8(sp),d0
        movea.l AES_ARG+12(sp),a5
        moveq   #0,d6
        moveq   #0,d7
        .endm

        .macro  AES_LEAVE
        movem.l (sp)+,d2-d7/a2-a6
        rts
        .endm


| ---------------------------------------------------------------------------
| VOID c68k_aes_core_enc_t4_asm(const ULONG *rk, UINT nr, ULONG *st)
|
| ShiftRows for encryption picks bytes 0,5,10,15 / 4,9,14,3 / 8,13,2,7 /
| 12,1,6,11, the diagonals of the state read column by column.
| ---------------------------------------------------------------------------
        .globl  _c68k_aes_core_enc_t4_asm
_c68k_aes_core_enc_t4_asm:
        AES_ENTER
        lea     _c68k_aes_te,a1
        lea     1024(a1),a2
        lea     2048(a1),a3
        lea     3072(a1),a4
        AES_ARK
        subq.l  #1,d0                   | nr - 1 full rounds
.Le4_round:
        AES_W4  d2, 0, 5, 10, 15
        AES_W4  d3, 4, 9, 14, 3
        AES_W4  d4, 8, 13, 2, 7
        AES_W4  d5, 12, 1, 6, 11
        movem.l d2-d5,(a5)
        subq.l  #1,d0
        bne     .Le4_round

        lea     _c68k_aes_sbox,a1
        AES_WL  d2, 0, 5, 10, 15
        AES_WL  d3, 4, 9, 14, 3
        AES_WL  d4, 8, 13, 2, 7
        AES_WL  d5, 12, 1, 6, 11
        movem.l d2-d5,(a5)
        AES_LEAVE


| ---------------------------------------------------------------------------
| VOID c68k_aes_core_dec_t4_asm(const ULONG *rk, UINT nr, ULONG *st)
|
| The equivalent inverse cipher.  InvShiftRows walks the other way round:
| 0,13,10,7 / 4,1,14,11 / 8,5,2,15 / 12,9,6,3.
| ---------------------------------------------------------------------------
        .globl  _c68k_aes_core_dec_t4_asm
_c68k_aes_core_dec_t4_asm:
        AES_ENTER
        lea     _c68k_aes_td,a1
        lea     1024(a1),a2
        lea     2048(a1),a3
        lea     3072(a1),a4
        AES_ARK
        subq.l  #1,d0
.Ld4_round:
        AES_W4  d2, 0, 13, 10, 7
        AES_W4  d3, 4, 1, 14, 11
        AES_W4  d4, 8, 5, 2, 15
        AES_W4  d5, 12, 9, 6, 3
        movem.l d2-d5,(a5)
        subq.l  #1,d0
        bne     .Ld4_round

        lea     _c68k_aes_isbox,a1
        AES_WL  d2, 0, 13, 10, 7
        AES_WL  d3, 4, 1, 14, 11
        AES_WL  d4, 8, 5, 2, 15
        AES_WL  d5, 12, 9, 6, 3
        movem.l d2-d5,(a5)
        AES_LEAVE


| ---------------------------------------------------------------------------
| VOID c68k_aes_core_enc_t1_asm(const ULONG *rk, UINT nr, ULONG *st)
|
| The same cipher out of Te0 alone.  Three rotates a column, twelve a round,
| in exchange for 3 KB of table that this machine has no cache to keep it out
| of.  Built to be measured against the four-table form above.
| ---------------------------------------------------------------------------
        .globl  _c68k_aes_core_enc_t1_asm
_c68k_aes_core_enc_t1_asm:
        AES_ENTER
        lea     _c68k_aes_te,a1
        AES_ARK
        subq.l  #1,d0
.Le1_round:
        AES_W1  d2, 0, 5, 10, 15
        AES_W1  d3, 4, 9, 14, 3
        AES_W1  d4, 8, 13, 2, 7
        AES_W1  d5, 12, 1, 6, 11
        movem.l d2-d5,(a5)
        subq.l  #1,d0
        bne     .Le1_round

        lea     _c68k_aes_sbox,a1
        AES_WL  d2, 0, 5, 10, 15
        AES_WL  d3, 4, 9, 14, 3
        AES_WL  d4, 8, 13, 2, 7
        AES_WL  d5, 12, 1, 6, 11
        movem.l d2-d5,(a5)
        AES_LEAVE


| ---------------------------------------------------------------------------
| VOID c68k_aes_core_dec_t1_asm(const ULONG *rk, UINT nr, ULONG *st)
| ---------------------------------------------------------------------------
        .globl  _c68k_aes_core_dec_t1_asm
_c68k_aes_core_dec_t1_asm:
        AES_ENTER
        lea     _c68k_aes_td,a1
        AES_ARK
        subq.l  #1,d0
.Ld1_round:
        AES_W1  d2, 0, 13, 10, 7
        AES_W1  d3, 4, 1, 14, 11
        AES_W1  d4, 8, 5, 2, 15
        AES_W1  d5, 12, 9, 6, 3
        movem.l d2-d5,(a5)
        subq.l  #1,d0
        bne     .Ld1_round

        lea     _c68k_aes_isbox,a1
        AES_WL  d2, 0, 13, 10, 7
        AES_WL  d3, 4, 1, 14, 11
        AES_WL  d4, 8, 5, 2, 15
        AES_WL  d5, 12, 9, 6, 3
        movem.l d2-d5,(a5)
        AES_LEAVE

#endif /* C68K_ASM */
