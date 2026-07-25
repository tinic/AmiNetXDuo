/*
 * AmiNetXDuo -- crypto68k Montgomery multiplication and squaring.
 *
 * CHOICE OF VARIANT
 *
 *   Koc, Acar and Kaliski ("Analyzing and Comparing Montgomery Multiplication
 *   Algorithms", IEEE Micro 16(3), 1996,
 *   https://www.microsoft.com/en-us/research/wp-content/uploads/1996/01/j37acmon.pdf)
 *   name five ways to interleave the multiplication and the reduction.  All
 *   five perform the same 2s^2 + s limb multiplications; they differ only in
 *   memory traffic and temporary space.  Their measurements make CIOS the
 *   default recommendation, and SOS a close second at twice the scratch.
 *
 *   This module uses SOS -- full product first, then reduce in place -- for a
 *   reason specific to the 68020, and it is worth writing down because it
 *   contradicts the paper's headline advice.
 *
 *   The fast multiply-accumulate on this machine (see c68k_prim.S) is GMP's
 *   two-limb loop, whose speed comes from `ADD.L Dn,(An)+`: a read-modify-
 *   write straight into the accumulator, with the memory carry folded into the
 *   next limb through the X flag.  That instruction only exists when the
 *   destination IS the source.  CIOS's second inner loop writes one limb below
 *   where it reads -- that displacement is how CIOS gets its divide-by-radix
 *   for free -- so half of all limb products would have to use a slower
 *   two-pointer loop.  SOS keeps every one of its 2s^2 products in the
 *   read-modify-write form; the shift disappears because the reduction's
 *   window into the 2s-limb product moves instead of the data.
 *
 *   So: CIOS wins on paper and SOS wins here, because the paper counts memory
 *   operations and this machine cares which addressing mode they use.
 *
 *   Not Karatsuba.  GMP's own tuning file for this architecture puts the
 *   crossover at 14 limbs on a 68040, but the Montgomery reduction is a chain
 *   of scalar-by-vector products and is not Karatsuba-able at all, so only
 *   half the work is eligible; at the 32-limb halves an RSA-2048 CRT operation
 *   actually runs, one level buys about 5%.  Not worth the scratch or the risk.
 *
 * WHY SQUARING IS A SEPARATE ROUTINE
 *
 *   The off-diagonal products of a square each appear twice, so the product
 *   phase costs s(s+1)/2 instead of s^2 (HAC Algorithm 14.16).  The reduction
 *   is unchanged, so a Montgomery square is (s^2 + 3s/2) / (2s^2 + s), about
 *   76% of a Montgomery multiply -- not half.  In a sliding-window
 *   exponentiation almost every operation is a squaring, which is what makes
 *   a 24% saving on it worth having.
 *
 * BIT-FOR-BIT
 *
 *   Both routines produce exactly what _nx_crypto_huge_number_mont() produces,
 *   including the final conditional subtraction.  That is not a claim; it is
 *   what tests/crypto68k/rsa_test checks over thousands of random operands.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"


/* --------------------------------------------------------------- n0inv --- */

c68k_limb c68k_mont_n0inv(c68k_limb m0)
{

c68k_limb   inv;
UINT        i;


    /*
     * Newton iteration for m0^-1 mod 2^k: each step doubles the number of
     * correct bits.  Seeding with m0 itself is correct to 3 bits for odd m0
     * (Dusse and Kaliski, EUROCRYPT'90), so 3 -> 6 -> 12 -> 24 -> 48 covers
     * 32 bits in four steps; the fifth is free insurance, once per
     * exponentiation.
     */
    inv = m0;
    for (i = 0; i < 5; i++)
    {
        inv = inv * (2u - (m0 * inv));
    }

    /* The reduction wants -m0^-1: the u for which t[0] + u*m[0] == 0 mod 2^32. */
    return((c68k_limb)(0u - inv));
}


/* ------------------------------------------------------- final reduction -- */

/*
 * high holds m_len+1 limbs and is known to be < 2m.  Reduce to m_len limbs and
 * copy to out.  This is the one conditional branch on secret data that
 * Montgomery multiplication always has.
 */
static VOID c68k_mont_final(c68k_limb *out, c68k_limb *high,
                            const c68k_limb *m, UINT m_len)
{

UINT    i;


    if ((high[m_len] != 0) || (c68k_cmp(high, m, m_len) >= 0))
    {
        (VOID) c68k_sub(high, m, m_len);
    }

    for (i = 0; i < m_len; i++)
    {
        out[i] = high[i];
    }
}


/* ------------------------------------------------------ SOS reduction ----- */

/*
 * t holds 2*m_len+1 limbs and is < m*R.  Add multiples of m until the low half
 * is zero; the answer is then t[m_len .. 2*m_len], which is < 2m.
 */
static VOID c68k_mont_reduce(c68k_limb *t, const c68k_limb *m, UINT m_len,
                             c68k_limb n0inv)
{

UINT        i;
UINT        j;
UINT        top;
c68k_limb   u;
c68k_limb   carry;
c68k_limb   sum;


    top = m_len << 1;

    for (i = 0; i < m_len; i++)
    {
        /* u * m zeroes t[i] and adds a multiple of m, preserving the residue. */
        u = (c68k_limb)(t[i] * n0inv);

        carry = c68k_addmul_1(&t[i], m, m_len, u);

        /*
         * Propagate into the limbs above.  The first addition almost always
         * absorbs it; the loop is here because "almost always" is how
         * multi-precision bugs are born.  The bound cannot be reached -- the
         * running value stays below 2*m*R, so t[top] is 0 or 1 -- but a bound
         * that is never hit still beats a buffer overrun if the analysis is
         * ever wrong.
         */
        j = i + m_len;
        while ((carry != 0) && (j <= top))
        {
            sum   = t[j] + carry;
            carry = (sum < carry) ? 1u : 0u;
            t[j]  = sum;
            j++;
        }
    }
}


/* ------------------------------------------------------------ multiply ---- */

VOID c68k_mont_mul(c68k_limb *r,
                   const c68k_limb *x, const c68k_limb *y,
                   const c68k_limb *m, UINT m_len, c68k_limb n0inv,
                   c68k_limb *work)
{

UINT        i;
UINT        total;
c68k_limb  *t = work;


    total = m_len << 1;

    for (i = 0; i <= total; i++)
    {
        t[i] = 0;
    }

    /*
     * t = x * y, one row at a time.  The carry out of row i lands in
     * t[i+m_len], which no earlier row has touched -- row i' writes at most
     * t[i'+m_len] -- so storing it is the same as adding it.
     */
    for (i = 0; i < m_len; i++)
    {
        t[i + m_len] = c68k_addmul_1(&t[i], y, m_len, x[i]);
    }

    c68k_mont_reduce(t, m, m_len, n0inv);
    c68k_mont_final(r, &t[m_len], m, m_len);
}


/* -------------------------------------------------------------- square ---- */

/*
 * t[0..2n-1] = x[0..n-1]^2.
 *
 * The off-diagonal products x[i]*x[j], i < j, each appear twice, so they are
 * accumulated once, the whole thing is doubled, and the x[i]^2 diagonal is
 * added afterwards.  Same algorithm as the vendored
 * _nx_crypto_huge_number_square, which is why the two agree limb for limb.
 */
static VOID c68k_sqr(c68k_limb *t, const c68k_limb *x, UINT n)
{

UINT        i;
UINT        total;
HN_UBASE2   product;


    total = n << 1;

    for (i = 0; i < total; i++)
    {
        t[i] = 0;
    }

    for (i = 0; i < n; i++)
    {
        t[i + n] = c68k_addmul_1(&t[(i << 1) + 1], &x[i + 1], n - i - 1, x[i]);
    }

    /* Double.  The top limb of the triangle sum is 0, so nothing falls off. */
    for (i = total - 1; i > 0; i--)
    {
        t[i] = (c68k_limb)((t[i] << 1) | (t[i - 1] >> 31));
    }
    t[0] = (c68k_limb)(t[0] << 1);

    /* Diagonal. */
    product = 0;
    for (i = 0; i < n; i++)
    {
        product = (product >> 32) + (HN_UBASE2)t[i << 1] +
                  ((HN_UBASE2)x[i] * (HN_UBASE2)x[i]);
        t[i << 1] = (c68k_limb)product;

        product = (product >> 32) + (HN_UBASE2)t[(i << 1) + 1];
        t[(i << 1) + 1] = (c68k_limb)product;
    }
}


VOID c68k_mont_sqr(c68k_limb *r,
                   const c68k_limb *x,
                   const c68k_limb *m, UINT m_len, c68k_limb n0inv,
                   c68k_limb *work)
{

c68k_limb  *t = work;


    c68k_sqr(t, x, m_len);
    t[m_len << 1] = 0;

    c68k_mont_reduce(t, m, m_len, n0inv);
    c68k_mont_final(r, &t[m_len], m, m_len);
}
