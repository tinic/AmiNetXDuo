/*
 * AmiNetXDuo -- crypto68k portable limb primitives.
 *
 * This is the fallback half of the build option: when C68K_ASM is not defined
 * these definitions are used, and when it is they are replaced wholesale by
 * c68k_prim.S.  Keeping both means the module builds and can be cross-checked
 * on any target, and that a suspected assembly bug can be isolated with one
 * build flag instead of a bisect.
 *
 * The C here is written the way that makes GCC generate the best m68k it can:
 * one 64-bit accumulator per limb, no separate high/low bookkeeping.  It is
 * not slow -- GCC really does emit MULU.L for it -- it just carries about nine
 * instructions of register shuffling per limb that the assembly does not need.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"

/*
 * Always compiled, under its own name, even in an assembly build: the
 * benchmark measures the two against each other in the same run, which is the
 * only comparison the emulator does not distort.
 */
c68k_limb c68k_addmul_1_c(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a)
{

UINT        j;
HN_UBASE2   product;


    /*
     * product holds the running 64-bit sum; its high half is the carry into
     * the next limb.  The identity that makes a separate carry bit
     * unnecessary is that
     *
     *     (2^32-1) * (2^32-1) + (2^32-1) + (2^32-1) == 2^64 - 1
     *
     * exactly, so a limb product plus two limb addends never overflows 64
     * bits.
     */
    product = 0;

    for (j = 0; j < n; j++)
    {
        product = (product >> 32) + (HN_UBASE2)r[j] +
                  ((HN_UBASE2)a * (HN_UBASE2)b[j]);
        r[j] = (c68k_limb)product;
    }

    return((c68k_limb)(product >> 32));
}


#ifndef C68K_ASM

c68k_limb c68k_addmul_1(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a)
{

    return(c68k_addmul_1_c(r, b, n, a));
}


c68k_limb c68k_add_carry(c68k_limb *dst, const c68k_limb *src, UINT n,
                         c68k_limb carry)
{

UINT        j;
HN_UBASE2   sum;


    sum = (HN_UBASE2)carry;

    for (j = 0; j < n; j++)
    {
        sum = (sum >> 32) + (HN_UBASE2)src[j];
        dst[j] = (c68k_limb)sum;
    }

    return((c68k_limb)(sum >> 32));
}


c68k_limb c68k_sub(c68k_limb *r, const c68k_limb *b, UINT n)
{

UINT        j;
c68k_limb   borrow;
c68k_limb   left;
c68k_limb   right;


    borrow = 0;

    for (j = 0; j < n; j++)
    {
        left  = r[j];
        right = b[j];
        r[j]  = left - right - borrow;

        /*
         * Borrow out.  Two comparisons rather than a 64-bit subtract, because
         * the 68020 has no 64-bit compare and GCC would synthesise one.
         */
        if (borrow != 0)
        {
            borrow = (left <= right) ? 1u : 0u;
        }
        else
        {
            borrow = (left < right) ? 1u : 0u;
        }
    }

    return(borrow);
}


INT c68k_cmp(const c68k_limb *a, const c68k_limb *b, UINT n)
{

UINT    j;


    for (j = n; j > 0; j--)
    {
        if (a[j - 1] != b[j - 1])
        {
            return((a[j - 1] > b[j - 1]) ? 1 : -1);
        }
    }

    return(0);
}

#endif /* !C68K_ASM */


UINT c68k_using_assembly(VOID)
{

#ifdef C68K_ASM
    return(1u);
#else
    return(0u);
#endif
}
