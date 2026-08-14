/*
 * AmiNetXDuo, pick this machine's crypto primitives.
 *
 * The module used to be one all-or-nothing switch: AMINETXDUO_CRYPTO68K_ASM was
 * ON for a 68020 or 68040 build and OFF for a 68000 or 68060, so the archive
 * carried a tls.library per CPU and a machine that installed the wrong one
 * either ran the portable C for everything or trapped every MULU.L into
 * 68060.library.  Here the choice is AttnFlags at library init, once, and one
 * binary serves the family.
 *
 * WHAT MOVES AND WHAT DOES NOT.  Most of the assembly in this directory is
 * legal on every 68k and is simply called by name: c68k_chacha20.S, and four of
 * c68k_prim.S's six routines (add, sub, cmp, add_carry).  Only two primitives
 * cannot be one routine, and they are the two the 68060 lost:
 *
 *   addmul_1   MULU.L 32x32 -> 64 in c68k_prim.S, and the same function
 *              rewritten in MULU.W in c68k_prim_mulw.S, which every part
 *              implements.  Both are assembled now; this picks.  It is the
 *              hottest routine in the module, so it is also the one where
 *              picking wrong costs the most.
 *   div_2by1   DIVU.L 64/32 in c68k_prim.S, against the portable 64-bit divide
 *              that reaches src/common/ami_udivdi3.c.
 *
 * poly1305_blocks is the third: its inner loop is a 32x32 -> 64 product, so it
 * is 68020-to-68040 assembly or the C, with nothing in between.
 *
 * c68k_variant.h has the two hardware facts this reads and why they are two.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"
#include "c68k_poly1305.h"
#include "c68k_aes.h"

#ifdef C68K_MV

#include <exec/execbase.h>

extern c68k_limb c68k_addmul_1_mulu(c68k_limb *r, const c68k_limb *b, UINT n,
                                    c68k_limb a);
extern c68k_limb c68k_addmul_1_mulw(c68k_limb *r, const c68k_limb *b, UINT n,
                                    c68k_limb a);

extern c68k_limb c68k_div_2by1_mulu(c68k_limb hi, c68k_limb lo, c68k_limb d,
                                    c68k_limb *rem);
extern c68k_limb c68k_div_2by1_c(c68k_limb hi, c68k_limb lo, c68k_limb d,
                                 c68k_limb *rem);

extern VOID c68k_poly1305_blocks_asm(C68K_POLY1305 *ctx, const UCHAR *m,
                                     ULONG blocks, ULONG hibit);

/* X25519's four field routines are set where their portable twins are, which
   is inside c68k_25519.c: they are static there. */
extern void c68k_25519_cpu_select(unsigned int mul_ul);

/*
 * The portable C to begin with, exactly as src/net68k does it: this file is
 * linked by programs that never select -- a bench, a test, a Shell command --
 * and the answer they get has to be a correct one.
 */
c68k_limb (*c68k_vec_addmul_1)(c68k_limb *, const c68k_limb *, UINT,
                               c68k_limb) = c68k_addmul_1_c;
c68k_limb (*c68k_vec_div_2by1)(c68k_limb, c68k_limb, c68k_limb,
                               c68k_limb *) = c68k_div_2by1_c;
VOID (*c68k_vec_poly1305_blocks)(C68K_POLY1305 *, const UCHAR *, ULONG,
                                 ULONG) = c68k_poly1305_blocks_c;

static UINT c68k_selected = C68K_ASM_NONE;

VOID c68k_cpu_select(ULONG attnflags)
{
    /*
     * The 68060 raises AFF_68040 as well, so "has the 64-bit MULU.L" is
     * 68020-and-up AND NOT 68060, which is why this is two tests and not a
     * class number.  AFF_68060 itself comes from 68060.library rather than
     * from any ROM (src/net68k/n68k_cpu.c says more): a 68060 without it reads
     * as a 68040 and takes the MULU.L path, which 68060.library then emulates.
     * That is the one case here where a wrong answer is slow rather than
     * merely slower, and it cannot be told apart from a real 68040 from user
     * mode.
     */
    UINT wide   = ((attnflags & AFF_68020) != 0UL) ? 1u : 0u;
    UINT mul_ul = (wide != 0u && (attnflags & AFF_68060) == 0UL) ? 1u : 0u;

    /* AES wants the first fact and not the second: its kernels are built on
       the scaled index, which the 68060 kept.  c68k_aes_variant is already a
       run-time choice -- tests/crypto68k/crypto68k_bulk moves it to time the
       five forms against each other -- so this only sets the default. */
    c68k_aes_variant = (wide != 0u) ? C68K_AES_V_T4_ASM : C68K_AES_V_T4_C;

    if (mul_ul != 0u)
    {
        c68k_vec_addmul_1        = c68k_addmul_1_mulu;
        c68k_vec_div_2by1        = c68k_div_2by1_mulu;
        c68k_vec_poly1305_blocks = c68k_poly1305_blocks_asm;
        c68k_selected            = C68K_ASM_68020;
        c68k_25519_cpu_select(1u);
    }
    else
    {
        /* A 68000 and a 68060 want the same thing here for opposite reasons:
           one never had the instruction, the other gave it back. */
        c68k_vec_addmul_1        = c68k_addmul_1_mulw;
        c68k_vec_div_2by1        = c68k_div_2by1_c;
        c68k_vec_poly1305_blocks = c68k_poly1305_blocks_c;
        c68k_selected            = C68K_ASM_68060;
        c68k_25519_cpu_select(0u);
    }
}

UINT c68k_cpu_class(VOID)
{

    return (c68k_selected);
}

#else

VOID c68k_cpu_select(ULONG attnflags)
{

    (VOID)attnflags;
}

/* One build, one answer, and c68k_using_assembly() already knows it. */
UINT c68k_cpu_class(VOID)
{

    return (c68k_using_assembly());
}

#endif /* C68K_MV */
