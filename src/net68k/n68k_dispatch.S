| AmiNetXDuo, the three published net68k entry points when the inner loops are
| multiversioned.
|
| Each is two instructions: load the vector n68k_cpu.c filled in and jump.  The
| callers -- the C in this directory and in src/sana2, and every memcpy() in
| the stack through n68k_memcpy_hook.c -- keep calling the same names they
| always did, and nothing in the vendored tree learns that the routine it
| reaches was chosen from AttnFlags.
|
| a0 is the caller's to lose in this ABI, and every variant reloads it from the
| stack frame before touching anything, so the trampoline needs no register of
| its own.  It costs a movea.l from absolute memory and a jmp: 20 cycles on a
| 68000, and less anywhere the vector stays in a cache.  That is the whole
| price of one binary.  The A1200 loopback arm in
| cmake/toolchain-m68k-amigaos.cmake is the measurement it comes out of.
|
| SPDX-License-Identifier: MIT

#ifdef AMINETXDUO_NET68K_ASM
#ifdef N68K_MV_MULTI

        .text
        .even

        .globl  _n68k_sum_longwords
_n68k_sum_longwords:
        movea.l _n68k_vec_sum,a0
        jmp     a0@

        .globl  _n68k_copy_sum_longwords
_n68k_copy_sum_longwords:
        movea.l _n68k_vec_copy_sum,a0
        jmp     a0@

        .globl  _n68k_copy_bytes
_n68k_copy_bytes:
        movea.l _n68k_vec_copy,a0
        jmp     a0@

#endif /* N68K_MV_MULTI */
#endif /* AMINETXDUO_NET68K_ASM */
