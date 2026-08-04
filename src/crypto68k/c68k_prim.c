/*
 * AmiNetXDuo, crypto68k portable limb primitives.
 *
 * The fallback half of the build option: when C68K_ASM is not defined these
 * definitions are used, and when it is they are replaced wholesale by
 * c68k_prim.S.  Keeping both lets the module build and be cross-checked on any
 * target, and isolates a suspected assembly bug with one build flag instead of
 * a bisect.
 *
 * The C here is written the way that makes GCC generate the best m68k it can:
 * one 64-bit accumulator per limb, no separate high/low bookkeeping.  GCC does
 * emit MULU.L for it; it carries about nine instructions of register shuffling
 * per limb that the assembly does not need.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"

/*
 * Always compiled, under its own name, even in an assembly build: the
 * benchmark measures the two against each other in the same run, the only
 * comparison the emulator does not distort.
 */
c68k_limb c68k_addmul_1_c(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a)
{

UINT        j;
HN_UBASE2   product;


    /*
     * product holds the running 64-bit sum; its high half is the carry into
     * the next limb.  No separate carry bit is needed because
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


/*
 * r -= a * b, the inner loop of long division.
 *
 * Always compiled: unlike c68k_addmul_1 there is no assembly twin.  GCC emits
 * MULU.L for the product here as it does there, and the division's cost is
 * dominated by the quotient estimate rather than by this pass, so
 * hand-writing it gains little.
 */
c68k_limb c68k_submul_1(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a)
{

UINT        j;
HN_UBASE2   product;
c68k_limb   borrow;
c68k_limb   lo;
c68k_limb   old;


    product = 0;
    borrow  = 0;

    for (j = 0; j < n; j++)
    {
        product = (product >> 32) + ((HN_UBASE2)a * (HN_UBASE2)b[j]);
        lo      = (c68k_limb)product;

        old  = r[j];
        r[j] = (c68k_limb)(old - lo - borrow);

        /*
         * Two comparisons rather than a 64-bit subtract, for the same reason
         * c68k_sub uses them: the 68020 has no 64-bit compare and GCC
         * synthesises a poor one.
         */
        if (borrow != 0u)
        {
            borrow = (old <= lo) ? 1u : 0u;
        }
        else
        {
            borrow = (old < lo) ? 1u : 0u;
        }
    }

    /*
     * The high half of the last product plus any final borrow.  It cannot
     * overflow a limb: a*b + carry <= (2^32-1)^2 + (2^32-1), whose high half
     * is at most 2^32-2, and the borrow adds at most one.
     */
    return((c68k_limb)((product >> 32) + borrow));
}


#ifndef C68K_ASM

/*
 * The portable quotient estimate.  On target this is replaced by a single
 * DIVU.L; here it is a 64-bit divide, which on m68k reaches __udivdi3 in
 * src/common/ami_udivdi3.c because this toolchain ships an empty libgcc.
 */
c68k_limb c68k_div_2by1(c68k_limb hi, c68k_limb lo, c68k_limb d,
                        c68k_limb *rem)
{

HN_UBASE2   num;


    num  = ((HN_UBASE2)hi << 32) | (HN_UBASE2)lo;
    *rem = (c68k_limb)(num % (HN_UBASE2)d);

    return((c68k_limb)(num / (HN_UBASE2)d));
}


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


c68k_limb c68k_add(c68k_limb *r, const c68k_limb *b, UINT n)
{

UINT        j;
HN_UBASE2   sum;


    /*
     * The 64-bit running accumulator, as in c68k_add_carry and
     * c68k_addmul_1_c above: its high half is the carry, so there is no
     * separate carry bookkeeping.  c68k_sub below cannot use the same form
     * because a borrow needs a 64-bit compare, which this machine lacks.
     */
    sum = 0;

    for (j = 0; j < n; j++)
    {
        sum  = (sum >> 32) + (HN_UBASE2)r[j] + (HN_UBASE2)b[j];
        r[j] = (c68k_limb)sum;
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
