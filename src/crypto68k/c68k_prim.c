/*
 * AmiNetXDuo, crypto68k portable limb primitives.
 *
 * The fallback half of the build option.  When C68K_ASM is not defined these
 * definitions are used, and when it is defined c68k_prim.S replaces them all.
 * Both together let the module build and be checked on any target, and isolate
 * a suspected assembly bug with one build flag instead of a bisect.
 *
 * The C here is written the way that makes GCC generate the best m68k it can:
 * one 64-bit accumulator per limb, no separate high/low bookkeeping.  GCC does
 * emit MULU.L for it.  It carries about nine instructions of register shuffling
 * per limb that the assembly does not need.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"
#include "c68k_variant.h"

/* Declared in crypto68k.h, see the note there. */
VOID (*c68k_yield_hook)(VOID);

UINT c68k_yield_stride(UINT products)
{

UINT    stride;


    /*
     * Nothing hooked.  A count no loop here can reach: 4 billion iterations of
     * a row of limb products is longer than this machine has, so the counter
     * never reaches zero and C68K_YIELD() is never even loaded.
     */
    if (c68k_yield_hook == (VOID (*)(VOID))0)
    {
        return(0xFFFFFFFFu);
    }

    if (products == 0u)
    {
        products = 1u;
    }

    stride = (C68K_YIELD_PRODUCTS + products - 1u) / products;

    return((stride != 0u) ? stride : 1u);
}

/*
 * 32x32 -> 64 without a call and without __muldi3.  Written as
 * (HN_UBASE2)a * (HN_UBASE2)b it asks for a 64x64 multiply and GCC emits
 * __muldi3; the halves must stay USHORT so GCC picks the widening pattern
 * instead of promoting to int and calling __mulsi3.
 */
#define C68K_HAVE_INLINE_WIDE_MUL   1

#if defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
    defined(__mc68060__)

static HN_UBASE2 c68k_wide_mul(c68k_limb a, c68k_limb b)
{

c68k_limb   a_lo = a & 0xFFFFUL;
c68k_limb   a_hi = a >> 16;
c68k_limb   b_lo = b & 0xFFFFUL;
c68k_limb   b_hi = b >> 16;
c68k_limb   ll   = a_lo * b_lo;
c68k_limb   lh   = a_lo * b_hi;
c68k_limb   hl   = a_hi * b_lo;
c68k_limb   hh   = a_hi * b_hi;
c68k_limb   mid  = (ll >> 16) + (lh & 0xFFFFUL) + (hl & 0xFFFFUL);


    return((((HN_UBASE2)(hh + (lh >> 16) + (hl >> 16) + (mid >> 16))) << 32) |
           (HN_UBASE2)((mid << 16) | (ll & 0xFFFFUL)));
}

#else   /* 68000: the same four products, through MULU.W */

static HN_UBASE2 c68k_wide_mul(c68k_limb a, c68k_limb b)
{

unsigned short  a_lo = (unsigned short)a;
unsigned short  a_hi = (unsigned short)(a >> 16);
unsigned short  b_lo = (unsigned short)b;
unsigned short  b_hi = (unsigned short)(b >> 16);
c68k_limb       ll   = (c68k_limb)a_lo * (c68k_limb)b_lo;
c68k_limb       lh   = (c68k_limb)a_lo * (c68k_limb)b_hi;
c68k_limb       hl   = (c68k_limb)a_hi * (c68k_limb)b_lo;
c68k_limb       hh   = (c68k_limb)a_hi * (c68k_limb)b_hi;
c68k_limb       mid  = (ll >> 16) + (lh & 0xFFFFUL) + (hl & 0xFFFFUL);


    return((((HN_UBASE2)(hh + (lh >> 16) + (hl >> 16) + (mid >> 16))) << 32) |
           (HN_UBASE2)((mid << 16) | (ll & 0xFFFFUL)));
}

#endif

c68k_limb c68k_addmul_1_c(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a)
{

UINT        j;
HN_UBASE2   product;


    /*
     * product holds the running 64-bit sum.  Its high half is the carry into
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
#ifdef C68K_HAVE_INLINE_WIDE_MUL
        product = (product >> 32) + (HN_UBASE2)r[j] + c68k_wide_mul(a, b[j]);
#else
        product = (product >> 32) + (HN_UBASE2)r[j] +
                  ((HN_UBASE2)a * (HN_UBASE2)b[j]);
#endif
        r[j] = (c68k_limb)product;
    }

    return((c68k_limb)(product >> 32));
}


/*
 * r -= a * b, the inner loop of long division.
 *
 * Always compiled: unlike c68k_addmul_1 there is no assembly twin.  GCC emits
 * MULU.L for the product here as it does there, and the cost of the division
 * is dominated by the quotient estimate rather than by this pass, so a
 * hand-written form gains little.
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


/*
 * The portable quotient estimate.  On target this is replaced by a single
 * DIVU.L.  Here it is a 64-bit divide, which on m68k reaches __udivdi3 in
 * src/common/ami_udivdi3.c because this toolchain ships an empty libgcc.
 *
 * It is compiled in the assembly build too when the binary serves every CPU.
 * DIVU.L 64/32 is 68020-to-68040 only, so a 68000 or a 68060 needs this at run
 * time and not only as a build option (c68k_cpu.c).  It takes the _c name
 * there, beside c68k_addmul_1_c, and the plain name stays with the per-CPU
 * build.
 */
#if defined(C68K_MV)
#define c68k_div_2by1 c68k_div_2by1_c
#endif

#if !defined(C68K_ASM) || defined(C68K_MV)
c68k_limb c68k_div_2by1(c68k_limb hi, c68k_limb lo, c68k_limb d,
                        c68k_limb *rem)
{

HN_UBASE2   num;


    num  = ((HN_UBASE2)hi << 32) | (HN_UBASE2)lo;
    *rem = (c68k_limb)(num % (HN_UBASE2)d);

    return((c68k_limb)(num / (HN_UBASE2)d));
}
#endif /* !C68K_ASM || C68K_MV */

#ifdef C68K_MV
#undef c68k_div_2by1
#endif

#ifndef C68K_ASM_PRIM

/*
 * C68K_ASM_MULW is the 68060 build, where c68k_prim_mulw.S supplies this one
 * function and everything else in this file stands.  See src/crypto68k/
 * CMakeLists.txt for why it is a separate option from C68K_ASM.
 */
#ifndef C68K_ASM_MULW
c68k_limb c68k_addmul_1(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a)
{

    return(c68k_addmul_1_c(r, b, n, a));
}
#endif


c68k_limb c68k_add_carry(c68k_limb *dst, const c68k_limb *src, UINT n,
                         c68k_limb carry)
{

UINT        j;
HN_UBASE2   sum;


    /*
     * The carry lives in the high half between iterations, which is what the
     * shift at the top of the loop reads, so an incoming carry has to be
     * seeded there.  A carry seeded in the low half was discarded on the first
     * shift and returned 0 for n == 0, where the answer is the carry itself.
     * c68k_prim.S branches straight to its exit and returns d0 untouched.
     * Nothing called this with a carry in until the huge-number add did, so
     * the two implementations never disagreed anywhere it showed.
     */
    sum = ((HN_UBASE2)carry) << 32;

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
         * the 68020 has no 64-bit compare and GCC synthesises one.
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

#if defined(C68K_MV)
    /* One binary for every CPU: what is in use is what was selected, not what
       was compiled, and c68k_cpu.c is the only place that knows. */
    return(c68k_cpu_class());
#elif defined(C68K_ASM_PRIM)
    return(C68K_ASM_68020);
#elif defined(C68K_ASM_MULW)
    return(C68K_ASM_68060);
#else
    return(C68K_ASM_NONE);
#endif
}
