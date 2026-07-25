/*
 * AmiNetXDuo -- crypto68k Montgomery multiplication and squaring.
 *
 * CHOICE OF VARIANT
 *
 *   Koc, Acar and Kaliski ("Analyzing and Comparing Montgomery Multiplication
 *   Algorithms", IEEE Micro 16(3), 1996) name five ways to interleave the
 *   multiplication and the reduction.  All five do the same 2s^2 + s limb
 *   multiplications; they differ in memory traffic and temporary space.
 *
 *   c68k_mont_mul() is CIOS: s+2 words of temporary space, and the divide-by-
 *   radix shift falls out of writing the reduction one word lower than it
 *   reads.  On a machine with eight data registers, no data cache (68020) or
 *   256 bytes of it (68030), and a 44-cycle multiply, the variant that touches
 *   memory least and keeps its whole working set in registers is the right
 *   one.  Nothing here is Karatsuba: at 64 limbs the crossover is not reached
 *   on a CPU where a multiply is only ~6x an add.
 *
 *   c68k_mont_sqr() is deliberately NOT CIOS.  A dedicated squaring pass needs
 *   the whole product at once (it computes the off-diagonal terms once and
 *   doubles them), so it uses the separated form -- SOS in the same taxonomy:
 *   full square into 2s words, then reduce in place.  That costs s(s+1)/2 + s
 *   limb multiplies for the square instead of s^2, so a Montgomery square is
 *   about 3/4 of a Montgomery multiply.  In an exponentiation nearly every
 *   operation is a squaring, so this is worth the extra s words of scratch.
 *
 * BIT-FOR-BIT
 *
 *   Both routines produce exactly what _nx_crypto_huge_number_mont() produces
 *   for the same inputs, including the final conditional subtraction.  That is
 *   not a claim, it is what tests/crypto68k/rsa_test checks over thousands of
 *   random operand pairs.
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
     * Newton iteration on inv = m0^-1 mod 2^k: each step doubles the number of
     * correct bits.  Seeding with m0 itself is correct to 3 bits for odd m0
     * (Dusse and Kaliski), so 3 -> 6 -> 12 -> 24 -> 48 covers 32 bits in four
     * steps; a fifth is free insurance and costs one multiply, once per
     * exponentiation.
     */
    inv = m0;
    for (i = 0; i < 5; i++)
    {
        inv = inv * (2u - (m0 * inv));
    }

    /* The reduction wants -m0^-1, i.e. the value u for which r0 + u*m0 == 0. */
    return((c68k_limb)(0u - inv));
}


/* ------------------------------------------------------- final reduction -- */

/*
 * r holds m_len+1 limbs and is known to be < 2*m.  Reduce it to m_len limbs
 * mod m and copy to out.
 */
static VOID c68k_mont_final(c68k_limb *out, c68k_limb *r,
                            const c68k_limb *m, UINT m_len)
{

UINT    i;


    if ((r[m_len] != 0) || (c68k_cmp(r, m, m_len) >= 0))
    {
        (VOID) c68k_sub(r, m, m_len);
    }

    for (i = 0; i < m_len; i++)
    {
        out[i] = r[i];
    }
}


/* -------------------------------------------------------------- CIOS mul -- */

VOID c68k_mont_mul(c68k_limb *r,
                   const c68k_limb *x, const c68k_limb *y,
                   const c68k_limb *m, UINT m_len, c68k_limb n0inv,
                   c68k_limb *work)
{

UINT        i;
c68k_limb   xi;
c68k_limb   u;
c68k_limb   carry;
HN_UBASE2   product;
c68k_limb  *t = work;


    for (i = 0; i <= (m_len + 1); i++)
    {
        t[i] = 0;
    }

    for (i = 0; i < m_len; i++)
    {
        xi = x[i];

        /* t[0..m_len-1] += xi * y, carry out into t[m_len]. */
        carry = c68k_mul_acc(t, t, y, m_len, xi, 0);

        /*
         * t[m_len] += carry, into t[m_len+1] if it overflows.
         *
         * The loop invariant is t <= 2m-1 at the top of every iteration, so
         * the value only needs m_len+1 limbs there -- but BETWEEN the product
         * and the reduction it can reach m*(2^32+2), which needs one more.
         * The vendored routine has only m_len+1 limbs and drops the overflow;
         * that is unreachable unless the top limb of the modulus is
         * 0xFFFFFFFF (probability 2^-32 for an RSA modulus), which is why it
         * has never been seen.  Carrying the extra limb costs one add per
         * outer iteration, so this keeps it rather than reproducing the edge.
         */
        product  = (HN_UBASE2)t[m_len] + (HN_UBASE2)carry;
        t[m_len] = (c68k_limb)product;
        t[m_len + 1] += (c68k_limb)(product >> 32);

        /*
         * u = t[0] * n0inv mod 2^32 makes t[0] + u*m[0] a multiple of the
         * radix, so the low limb of the reduction is discarded rather than
         * stored -- which is where the free shift comes from.
         */
        u = (c68k_limb)(t[0] * n0inv);

        product = (HN_UBASE2)t[0] + ((HN_UBASE2)u * (HN_UBASE2)m[0]);
        carry   = (c68k_limb)(product >> 32);

        /*
         * t[j-1] = t[j] + u*m[j] + carry, for j = 1..m_len-1.  dst is src-1:
         * the divide by the radix, done by the store address.
         */
        carry = c68k_mul_acc(t, t + 1, m + 1, m_len - 1, u, carry);

        /* Top limbs: no m limb left to multiply, just the carry. */
        product      = (HN_UBASE2)t[m_len] + (HN_UBASE2)carry;
        t[m_len - 1] = (c68k_limb)product;
        product      = (product >> 32) + (HN_UBASE2)t[m_len + 1];
        t[m_len]     = (c68k_limb)product;
        t[m_len + 1] = (c68k_limb)(product >> 32);
    }

    c68k_mont_final(r, t, m, m_len);
}


/* -------------------------------------------------------------- SOS sqr --- */

/*
 * t[0..2n-1] = x[0..n-1]^2.
 *
 * The off-diagonal products x[i]*x[j] with i < j each appear twice in the
 * square, so they are accumulated once, the whole thing is doubled, and the
 * diagonal x[i]^2 terms are added afterwards.  n(n-1)/2 + n multiplies instead
 * of n^2.  This is the same algorithm the vendored _nx_crypto_huge_number_square
 * uses, which is why the two agree limb for limb.
 */
static VOID c68k_sqr(c68k_limb *t, const c68k_limb *x, UINT n)
{

UINT        i;
UINT        total;
c68k_limb   carry;
HN_UBASE2   product;


    total = n << 1;

    for (i = 0; i < total; i++)
    {
        t[i] = 0;
    }

    /* Upper triangle: t[i+j] += x[i]*x[j] for j > i. */
    for (i = 0; i < n; i++)
    {
        carry = c68k_mul_acc(&t[(i << 1) + 1], &t[(i << 1) + 1],
                             &x[i + 1], n - i - 1, x[i], 0);
        t[i + n] = carry;
    }

    /* Double it.  The top limb of the triangle sum is always 0, so nothing
       falls off the end. */
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

UINT        i;
UINT        j;
c68k_limb   u;
c68k_limb   carry;
c68k_limb  *t = work;          /* 2*m_len + 1 limbs */


    c68k_sqr(t, x, m_len);
    t[m_len << 1] = 0;

    /*
     * Montgomery reduction in place.  For each low limb, add a multiple of m
     * that zeroes it; after m_len rounds the low half is zero and the answer
     * is the high half.  Unlike CIOS there is no shifting -- the window into
     * t moves instead.
     */
    for (i = 0; i < m_len; i++)
    {
        u = (c68k_limb)(t[i] * n0inv);

        carry = c68k_mul_acc(&t[i], &t[i], m, m_len, u, 0);

        /*
         * Propagate into the limbs above.  The first addition almost always
         * absorbs it; the loop is here because "almost always" is how
         * multi-precision bugs are born.
         */
        j = i + m_len;
        while ((carry != 0) && (j <= (m_len << 1)))
        {
            c68k_limb   sum = t[j] + carry;

            carry = (sum < carry) ? 1u : 0u;
            t[j]  = sum;
            j++;
        }
    }

    c68k_mont_final(r, &t[m_len], m, m_len);
}
