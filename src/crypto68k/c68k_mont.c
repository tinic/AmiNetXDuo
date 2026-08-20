/*
 * AmiNetXDuo, crypto68k Montgomery multiplication and squaring.
 *
 *   Koc, Acar and Kaliski ("Analyzing and Comparing Montgomery Multiplication
 *   Algorithms", IEEE Micro 16(3), 1996,
 *   https://www.microsoft.com/en-us/research/wp-content/uploads/1996/01/j37acmon.pdf)
 *   name five ways to interleave the multiplication and the reduction.  All
 *   five perform the same 2s^2 + s limb multiplications.  They differ only in
 *   memory traffic and temporary space.  The measurements in the paper make
 *   CIOS the default recommendation, and SOS a close second at twice the
 *   scratch.
 *
 *   This module uses SOS, full product first, then reduce in place, for a
 *   reason specific to the 68020, recorded here because it contradicts the
 *   main advice of the paper.
 *
 *   The fast multiply-accumulate on this machine (see c68k_prim.S) is the
 *   two-limb loop of GMP, whose speed comes from `ADD.L Dn,(An)+`: a
 *   read-modify-write straight into the accumulator, with the memory carry
 *   folded into the next limb through the X flag.  That instruction only
 *   exists when the destination is the source.  The second inner loop of CIOS
 *   writes one limb below where it reads, and that displacement is how CIOS
 *   gets its divide-by-radix for free, so half of all limb products need a
 *   slower two-pointer loop.  SOS keeps every one of its 2s^2 products in the
 *   read-modify-write form.  The shift disappears because the window of the
 *   reduction into the 2s-limb product moves instead of the data.  The paper
 *   counts memory operations.  This machine also cares which addressing mode
 *   they use.
 *
 *   Karatsuba, above a measured threshold, see the block further down.  The
 *   earlier judgement here was "not Karatsuba", on the grounds that the
 *   reduction is not Karatsuba-able and that one level gains about 5% at the
 *   32-limb halves an RSA-2048 CRT operation runs.  Both of those still hold.
 *   An RSA-2048 public operation is 64 limbs, not 32, and the split removes
 *   more than twice as many limb products there.  A TLS client does three
 *   public operations per handshake, and one private operation only if it
 *   holds a certificate.  The threshold was what needed to be right.
 *
 *   The off-diagonal products of a square each appear twice, so the product
 *   phase costs s(s+1)/2 instead of s^2 (HAC Algorithm 14.16).  The reduction
 *   is unchanged, so a Montgomery square is (s^2 + 3s/2) / (2s^2 + s), about
 *   76% of a Montgomery multiply, not half.  In a sliding-window
 *   exponentiation almost every operation is a squaring, so the 24% saving
 *   applies to nearly all of them.
 *
 *   Both routines produce exactly what _nx_crypto_huge_number_mont() produces
 *   over thousands of random operands, including the final conditional
 *   subtraction, and tests/crypto68k checks it.
 *
 *   They disagree with it for operands very close to the modulus, where the
 *   vendored routine is wrong.  Two cases, both checked against an independent
 *   answer rather than against either implementation.  With m = 2^64 - 1 and
 *   x = m-1, mont(x,x) must be 1 and the vendored routine returns 0.  At 32
 *   limbs with m nearly all ones and x = m-1, its top limb is one less than
 *   the true value.  Random operands never come that close to m, which is why
 *   a 400-trial sweep never caught it, and why no RSA or EC path can reach it.
 *   It does mean the vendored routine cannot be the oracle for exactly the
 *   extreme operands the Karatsuba carry handling most needs.  So that test
 *   diffs the split against the schoolbook of this module instead, which is
 *   what c68k_karatsuba_limbs exists for.
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
     * correct bits.  A seed of m0 itself is correct to 3 bits for odd m0
     * (Dusse and Kaliski, EUROCRYPT'90), so 3 -> 6 -> 12 -> 24 -> 48 covers
     * 32 bits in four steps.  The fifth costs nothing, once per
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


/* ------------------------------------------------------- final reduction, */

/*
 * high holds m_len+1 limbs and is known to be < 2m.  Reduce to m_len limbs and
 * copy to out.  The one conditional branch on secret data that Montgomery
 * multiplication always has.
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
 * is zero.  The answer is then t[m_len .. 2*m_len], which is < 2m.
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

        carry = C68K_ADDMUL_1(&t[i], m, m_len, u);

        /*
         * Propagate into the limbs above.  The first addition almost always
         * absorbs it, and the loop covers the rest.  The bound cannot be
         * reached, because the running value stays below 2*m*R, so t[top] is
         * 0 or 1.  An unreachable bound is cheaper than a buffer overrun if
         * that analysis is ever wrong.
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


/* ----------------------------------------------------------- Karatsuba ---- */

/*
 * docs/RESEARCH.md 9 rejected Karatsuba at 32 limbs, where it measured ~5%.
 * That measurement stands.  An RSA-2048 CRT half is 32 limbs.  An RSA-2048
 * public operation is 64, and the split is worth much more there:
 *
 *     limbs      8      16      32      64
 *     squaring   1.00x  1.26x   1.63x   2.14x     (fewer limb products)
 *     multiply   1.00x  1.33x   1.78x   2.37x
 *
 * So the threshold is the whole design, set from measurement on this machine
 * rather than copied out of another project.  Below it, schoolbook, which at
 * 8 limbs is exactly as good (36 products either way).
 *
 * The delivered gain is less than that table suggests.  A Montgomery step is a
 * product and a reduction, and the reduction is a chain of scalar-by-vector
 * c68k_addmul_1 calls Karatsuba cannot touch.  It costs s^2 + s regardless.  So
 * 2.14x on the squaring of 64 limbs dilutes to 6240 -> 5132 on the Montgomery
 * square, 1.22x, and the whole e=65537 exponentiation goes 124,608 -> 99,776
 * limb products.
 *
 * Subtractive for the square, additive for the multiply:
 *
 *   Squaring:  x^2 = x1^2*B^2h + (x0^2 + x1^2 - (x1-x0)^2)*B^h + x0^2.
 *   The middle is 2*x0*x1, so it is never negative, and |x1-x0| fits in h
 *   limbs with no carry out.  Nothing to sign-track and nothing to carry.
 *
 *   Multiplying:  the matching subtractive form is (x1-x0)*(y0-y1), whose sign
 *   depends on the operands, the term that the bn_mul_recursive of OpenSSL
 *   carries a `neg` flag for, and where this kind of code goes wrong.  The
 *   additive form
 *   (x0+x1)*(y0+y1) has no sign, at the price of two h-limb sums that can
 *   carry out.  Those carries are handled below as two conditional adds and
 *   one conditional increment.
 */

UINT c68k_karatsuba_limbs = C68K_KARATSUBA_DEFAULT;

static VOID c68k_zero_n(c68k_limb *p, UINT n)
{

UINT    i;


    for (i = 0; i < n; i++)
    {
        p[i] = 0;
    }
}

static VOID c68k_copy_n(c68k_limb *dst, const c68k_limb *src, UINT n)
{

UINT    i;


    for (i = 0; i < n; i++)
    {
        dst[i] = src[i];
    }
}

/* Schoolbook product, the base case and the below-threshold path. */
static VOID c68k_mul_school(c68k_limb *t, const c68k_limb *x,
                            const c68k_limb *y, UINT n)
{

UINT    i;


    c68k_zero_n(t, n);

    /*
     * The carry out of row i lands in t[i+n], which no earlier row has
     * touched, because row i' writes at most t[i'+n].  A store is therefore
     * the same as an accumulate, so this stores.
     */
    for (i = 0; i < n; i++)
    {
        t[i + n] = C68K_ADDMUL_1(&t[i], y, n, x[i]);
    }
}

/* The symmetric schoolbook squarer, further down: the base case of the split,
   and optimal at 8 limbs (36 products, what a Comba square costs).  Declared
   here because the recursion reaches it. */
static VOID c68k_sqr(c68k_limb *t, const c68k_limb *x, UINT n);

static VOID c68k_sqr_n(c68k_limb *t, const c68k_limb *x, UINT n, c68k_limb *s);
static VOID c68k_mul_n(c68k_limb *t, const c68k_limb *x, const c68k_limb *y,
                       UINT n, c68k_limb *s);

/*
 * t[0..2n-1] = x[0..n-1]^2.
 *
 * s is scratch, C68K_KAR_SCRATCH(n) limbs, and must not alias t or x.
 */
static VOID c68k_sqr_n(c68k_limb *t, const c68k_limb *x, UINT n, c68k_limb *s)
{

UINT        h;
c68k_limb  *d;
c68k_limb  *u;
c68k_limb  *mid;
c68k_limb  *sub;
c68k_limb   carry;
c68k_limb   borrow;


    /* Odd sizes fall back rather than take a special case.  No RSA or EC
       size in this tree is odd. */
    if ((n < c68k_karatsuba_limbs) || (n < 2u) || ((n & 1u) != 0u))
    {
        c68k_sqr(t, x, n);
        return;
    }

    h   = n >> 1;
    d   = s;                            /* h      */
    u   = d + h;                        /* 2h     */
    mid = u + n;                        /* 2h + 1 */
    sub = mid + n + 1u;

    /* d = |x1 - x0|, h limbs, no carry out either way. */
    if (c68k_cmp(&x[h], x, h) >= 0)
    {
        c68k_copy_n(d, &x[h], h);
        (VOID) c68k_sub(d, x, h);
    }
    else
    {
        c68k_copy_n(d, x, h);
        (VOID) c68k_sub(d, &x[h], h);
    }

    c68k_sqr_n(t,      x,     h, sub);          /* L = x0^2 -> t[0 .. n-1]   */
    c68k_sqr_n(&t[n],  &x[h], h, sub);          /* H = x1^2 -> t[n .. 2n-1]  */
    c68k_sqr_n(u,      d,     h, sub);          /* u = |x1-x0|^2             */

    /*
     * mid = L + H - u, which is 2*x0*x1 and therefore never negative.  It
     * needs n+1 limbs because L + H can carry out of n.
     */
    c68k_copy_n(mid, t, n);
    mid[n] = c68k_add(mid, &t[n], n);
    borrow = c68k_sub(mid, u, n);
    mid[n] = (c68k_limb)(mid[n] - borrow);

    /* t += mid * B^h.  mid is n+1 limbs, so it reaches t[h .. h+n], which is
       t[h .. 3h], inside the 4h limbs of t.  The carry goes above that. */
    carry = c68k_add(&t[h], mid, n + 1u);
    if (carry != 0u)
    {
        (VOID) c68k_add_carry(&t[h + n + 1u], &t[h + n + 1u],
                              (n << 1) - (h + n + 1u), carry);
    }
}

/*
 * t[0..2n-1] = x[0..n-1] * y[0..n-1].
 */
static VOID c68k_mul_n(c68k_limb *t, const c68k_limb *x, const c68k_limb *y,
                       UINT n, c68k_limb *s)
{

UINT        h;
c68k_limb  *a;
c68k_limb  *b;
c68k_limb  *p;
c68k_limb  *sub;
c68k_limb   cx;
c68k_limb   cy;
c68k_limb   carry;
c68k_limb   borrow;


    if ((n < c68k_karatsuba_limbs) || (n < 2u) || ((n & 1u) != 0u))
    {
        c68k_mul_school(t, x, y, n);
        return;
    }

    h   = n >> 1;
    a   = s;                            /* h      */
    b   = a + h;                        /* h      */
    p   = b + h;                        /* 2h + 1 */
    sub = p + n + 1u;

    /* a = x0 + x1 and b = y0 + y1, each h limbs plus a carry bit. */
    c68k_copy_n(a, x, h);
    cx = c68k_add(a, &x[h], h);
    c68k_copy_n(b, y, h);
    cy = c68k_add(b, &y[h], h);

    c68k_mul_n(t,     x,     y,     h, sub);    /* z0 = x0*y0 -> t[0..n-1]  */
    c68k_mul_n(&t[n], &x[h], &y[h], h, sub);    /* z2 = x1*y1 -> t[n..2n-1] */
    c68k_mul_n(p,     a,     b,     h, sub);    /* a*b, low n limbs         */
    p[n] = 0;

    /*
     * (cx*B^h + a) * (cy*B^h + b) = cx*cy*B^n + (cx*b + cy*a)*B^h + a*b.
     * The two conditional adds land at limb offset h and cannot carry past
     * p[n], because the whole product is bounded by (2*B^h)^2 = 4*B^n.
     */
    if (cx != 0u)
    {
        p[n] = (c68k_limb)(p[n] + c68k_add(&p[h], b, h));
    }
    if (cy != 0u)
    {
        p[n] = (c68k_limb)(p[n] + c68k_add(&p[h], a, h));
    }
    if ((cx != 0u) && (cy != 0u))
    {
        p[n] = (c68k_limb)(p[n] + 1u);
    }

    /* p -= z0 + z2, leaving the middle term, which is never negative. */
    borrow = c68k_sub(p, t, n);
    p[n]   = (c68k_limb)(p[n] - borrow);
    borrow = c68k_sub(p, &t[n], n);
    p[n]   = (c68k_limb)(p[n] - borrow);

    carry = c68k_add(&t[h], p, n + 1u);
    if (carry != 0u)
    {
        (VOID) c68k_add_carry(&t[h + n + 1u], &t[h + n + 1u],
                              (n << 1) - (h + n + 1u), carry);
    }
}


/* ------------------------------------------------------------ multiply ---- */

VOID c68k_mont_mul(c68k_limb *r,
                   const c68k_limb *x, const c68k_limb *y,
                   const c68k_limb *m, UINT m_len, c68k_limb n0inv,
                   c68k_limb *work)
{

UINT        total;
c68k_limb  *t = work;


    /* Match c68k_mont_sqr(): the public empty-input primitive is a no-op.
       Without this guard the empty multiply still writes work[0]. */
    if (m_len == 0u)
    {
        return;
    }

    total = m_len << 1;

    c68k_mul_n(t, x, y, m_len, work + total + 1u);
    t[total] = 0;

    c68k_mont_reduce(t, m, m_len, n0inv);
    c68k_mont_final(r, &t[m_len], m, m_len);
}


/* -------------------------------------------------------------- square ---- */

/*
 * t[0..2n-1] = x[0..n-1]^2.
 *
 * The off-diagonal products x[i]*x[j], i < j, each appear twice, so they are
 * accumulated once, the whole of it is doubled, and the x[i]^2 diagonal is
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
        t[i + n] = C68K_ADDMUL_1(&t[(i << 1) + 1], &x[i + 1], n - i - 1, x[i]);
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


    /* c68k_sqr() doubles from 2*n-1 down to one.  For n == 0 that unsigned
       starting index wraps, so make the public primitive's empty input a
       no-op.  The exponentiation caller already refuses an empty modulus,
       but direct callers should not acquire a hidden non-zero precondition. */
    if (m_len == 0u)
    {
        return;
    }

    c68k_sqr_n(t, x, m_len, work + (m_len << 1) + 1u);
    t[m_len << 1] = 0;

    c68k_mont_reduce(t, m, m_len, n0inv);
    c68k_mont_final(r, &t[m_len], m, m_len);
}
