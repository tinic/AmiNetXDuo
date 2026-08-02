| AmiNetXDuo -- calibration kernels for the Moira evaluation.
|
| Each label below marks ONE instruction.  The harness points the reset vector
| at the label, resets, seeds the registers and calls execute() exactly once,
| so the cycle count it reads back is that instruction's cost and nothing
| else -- no loop, no bracket subtraction, no best-of-n.  That is the whole
| claim under test: tests/perf/cpucal.c has to do all three because it measures
| on an emulated machine with an operating system underneath it.
|
| The published costs these are checked against are in tests/moira/moiracal.cpp
| next to the expectation, taken from the M68000PRM instruction-timing
| appendix (68000) and the MC68020UM one.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------- registers --

        .globl  _k_add_l
_k_add_l:
        add.l   d1,d0

        .globl  _k_add_w
_k_add_w:
        add.w   d1,d0

        .globl  _k_move_l
_k_move_l:
        move.l  d1,d0

        .globl  _k_move_w
_k_move_w:
        move.w  d1,d0

        .globl  _k_addx_l
_k_addx_l:
        addx.l  d1,d0

        .globl  _k_mulu_w
_k_mulu_w:
        mulu.w  d1,d0

        .globl  _k_muls_w
_k_muls_w:
        muls.w  d1,d0

        .globl  _k_lsl_l
_k_lsl_l:
        lsl.l   #2,d0

        .globl  _k_moveq
_k_moveq:
        moveq   #0,d0

| ------------------------------------------------------------------ memory --
|
| a0 and a1 point into the harness scratch buffer.

        .globl  _k_move_l_pi_d
_k_move_l_pi_d:
        move.l  a0@+,d0

        .globl  _k_move_l_d_pi
_k_move_l_d_pi:
        move.l  d0,a0@+

        .globl  _k_move_l_pi_pi
_k_move_l_pi_pi:
        move.l  a0@+,a1@+

        .globl  _k_move_w_pi_pi
_k_move_w_pi_pi:
        move.w  a0@+,a1@+

| ------------------------------------------------------------------- movem --
|
| The instruction both net68k primitives are built on, at the register counts
| they actually use.  n68k_sum_longwords loads seven; n68k_copy_bytes moves
| eight at a time.

        .globl  _k_movem_l_pi_1
_k_movem_l_pi_1:
        movem.l a0@+,d1

        .globl  _k_movem_l_pi_2
_k_movem_l_pi_2:
        movem.l a0@+,d1-d2

        .globl  _k_movem_l_pi_4
_k_movem_l_pi_4:
        movem.l a0@+,d1-d4

        .globl  _k_movem_l_pi_7
_k_movem_l_pi_7:
        movem.l a0@+,d1-d7

        .globl  _k_movem_l_pi_8
_k_movem_l_pi_8:
        movem.l a0@+,d0-d7

        .globl  _k_movem_l_pi_12
_k_movem_l_pi_12:
        movem.l a0@+,d0-d7/a2-a5

        .globl  _k_movem_l_pd_8
_k_movem_l_pd_8:
        movem.l d0-d7,a1@-

        .globl  _k_movem_l_ea_8
_k_movem_l_ea_8:
        movem.l d0-d7,a1@

        .globl  _k_movem_w_pi_8
_k_movem_w_pi_8:
        movem.w a0@+,d0-d7

| ---------------------------------------------------------------- branching --

        .globl  _k_bra
_k_bra:
        bra     _k_bra_t
_k_bra_t:
        nop

        .globl  _k_bcc_taken
_k_bcc_taken:
        beq     _k_bcc_t
_k_bcc_t:
        nop

        .globl  _k_dbf
_k_dbf:
        dbf     d0,_k_dbf

        .globl  _k_jsr_abs
_k_jsr_abs:
        jsr     _k_rts

        .globl  _k_rts
_k_rts:
        rts

        .globl  _k_nop
_k_nop:
        nop

| ------------------------------------------------------- the trampoline ---
|
| The harness pushes arguments, points the reset vector here, patches the
| longword at _tramp_target and runs until the PC reaches _tramp_end.

        .globl  _tramp
        .globl  _tramp_target
        .globl  _tramp_end
_tramp:
        jsr     0x00000000:l
_tramp_target = _tramp + 2
_tramp_end:
        illegal

| --------------------------------------------------- the I-cache probe ---
|
| One loop whose body is assembled at several sizes, so the harness can ask
| whether anything changes as the body crosses the 68020's 256-byte
| instruction cache.  Each body is `count` iterations of a straight-line run
| of move.l/addx.l pairs -- the same two instructions n68k_sum_longwords'
| table runs -- followed by dbf.
|
| d0 = iteration count - 1, a0 = buffer, a1 = buffer (reset each iteration).
|
| ICACHE_BODY n emits n pairs, i.e. 8n bytes of body.

        .macro  ICACHE_LOOP name, pairs
        .globl  \name
\name:
        movem.l d2-d7/a2-a3,sp@-
        move.l  sp@(36),d0
        movea.l sp@(40),a0
        movea.l a0,a1
        move.l  #0,d1
9:      movea.l a1,a0
        .rept   \pairs
        move.l  a0@+,d1
        addx.l  d1,d2
        .endr
        dbf     d0,9b
        movem.l sp@+,d2-d7/a2-a3
        rts
        .endm

        ICACHE_LOOP _ic_8,   8         | 64 bytes of body
        ICACHE_LOOP _ic_16, 16         | 128
        ICACHE_LOOP _ic_18, 18         | 144
        ICACHE_LOOP _ic_24, 24         | 192
        ICACHE_LOOP _ic_30, 30         | 240
        ICACHE_LOOP _ic_32, 32         | 256   <- the 68020 cache size
        ICACHE_LOOP _ic_34, 34         | 272
        ICACHE_LOOP _ic_40, 40         | 320
        ICACHE_LOOP _ic_48, 48         | 384
        ICACHE_LOOP _ic_64, 64         | 512
        ICACHE_LOOP _ic_80, 80         | 640
