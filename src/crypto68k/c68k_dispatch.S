| AmiNetXDuo, the two multiversioned primitives under their published names.
|
| src/net68k/n68k_dispatch.S is the same two instructions for the same reason,
| and here there is a caller that leaves no choice: the fork's
| nx_crypto_huge_number.c calls c68k_addmul_1 by name when
| NX_CRYPTO_AMIGA_C68K_LIMBS is set.  Vendored code is not edited to suit us
| (docs/RESEARCH.md 13.2), so the name has to exist and has to dispatch.
|
| Our own callers go through C68K_ADDMUL_1 and C68K_DIV_2BY1 instead, which are
| the vector itself, and pay nothing for this.  What it costs the vendored path
| is a movea.l from absolute memory and a jmp, ~20 cycles on a 68000, against a
| routine that is entered once per limb VECTOR and multiplies n limbs inside.
|
| a0 is the caller's to lose in this ABI and both variants reload their
| arguments from the stack frame, so the trampoline needs no register of its
| own.
|
| SPDX-License-Identifier: MIT

#ifdef C68K_MV

        .text
        .even

        .globl  _c68k_addmul_1
_c68k_addmul_1:
        movea.l _c68k_vec_addmul_1,a0
        jmp     a0@

        .globl  _c68k_div_2by1
_c68k_div_2by1:
        movea.l _c68k_vec_div_2by1,a0
        jmp     a0@

#endif /* C68K_MV */
