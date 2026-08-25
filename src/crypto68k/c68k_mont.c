/*
 * AmiNetXDuo, crypto68k Montgomery multiplication and squaring.
 *
 * SOS (separated operand scanning), not the usually recommended CIOS: every
 * limb product stays in the `ADD.L Dn,(An)+` read-modify-write form that
 * c68k_prim.S is fast at, which CIOS's shifted second loop cannot use.
 * Karatsuba above c68k_karatsuba_limbs, schoolbook below.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"


/* --------------------------------------------------------------- n0inv --- */

c68k_limb c68k_mont_n0inv(c68k_limb m0)
{

c68k_limb   inv;
UINT        i;


    /* Newton iteration for m0^-1 mod 2^k; m0 itself is a 3-bit-correct seed
       for odd m0 (Dusse and Kaliski), so four steps cover 32 bits. */
    inv = m0;
    for (i = 0; i < 5; i++)
    {
        inv = inv * (2u - (m0 * inv));
    }

    /* The reduction wants -m0^-1: the u for which t[0] + u*m[0] == 0 mod 2^32. */
    return((c68k_limb)(0u - inv));
}


/* ------------------------------------------------------- final reduction, */

/* high holds m_len+1 limbs and MUST be < 2m.  Reduce to m_len and copy out. */
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
UINT        stride;
UINT        left;
c68k_limb   u;
c68k_limb   carry;
c68k_limb   sum;


    top = m_len << 1;

    /* The dominant loop of the module: if anything here yields, this must. */
    stride = c68k_yield_stride(m_len);
    left   = stride;

    for (i = 0; i < m_len; i++)
    {
        if (--left == 0u)
        {
            left = stride;
            C68K_YIELD();
        }

        /* u * m zeroes t[i] and adds a multiple of m, preserving the residue. */
        u = (c68k_limb)(t[i] * n0inv);

        carry = C68K_ADDMUL_1(&t[i], m, m_len, u);

        /* The bound is unreachable (the running value stays below 2*m*R) but
           is kept: cheaper than a buffer overrun if that analysis is wrong. */
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
 * Karatsuba.  SUBTRACTIVE for the square, ADDITIVE for the multiply:
 *   x^2 = x1^2*B^2h + (x0^2 + x1^2 - (x1-x0)^2)*B^h + x0^2  -- never negative.
 *   The multiply uses (x0+x1)*(y0+y1), which has no sign but two h-limb sums
 *   that can carry out, handled below as conditional adds.
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
UINT    stride;
UINT    left;


    c68k_zero_n(t, n);

    /* The base case of c68k_mul_n(), so this is where the product half of a
       Montgomery multiply spends its time, one level down from the split. */
    stride = c68k_yield_stride(n);
    left   = stride;

    /* The carry out of row i lands in t[i+n], untouched by any earlier row,
       so a store is the same as an accumulate. */
    for (i = 0; i < n; i++)
    {
        if (--left == 0u)
        {
            left = stride;
            C68K_YIELD();
        }

        t[i + n] = C68K_ADDMUL_1(&t[i], y, n, x[i]);
    }
}

/* The symmetric schoolbook squarer, the base case of the split.  Declared
   here because the recursion above reaches it. */
static VOID c68k_sqr(c68k_limb *t, const c68k_limb *x, UINT n);

static VOID c68k_sqr_n(c68k_limb *t, const c68k_limb *x, UINT n, c68k_limb *s);
static VOID c68k_mul_n(c68k_limb *t, const c68k_limb *x, const c68k_limb *y,
                       UINT n, c68k_limb *s);

/* t[0..2n-1] = x[0..n-1]^2.  s is C68K_KAR_SCRATCH(n) limbs of scratch and
   MUST NOT alias t or x. */
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

    /* mid = L + H - u = 2*x0*x1, never negative; needs n+1 limbs. */
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

/* t[0..2n-1] = x[0..n-1] * y[0..n-1]. */
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

    /* The two conditional adds land at limb offset h and cannot carry past
       p[n]: the whole product is bounded by (2*B^h)^2 = 4*B^n. */
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
 * t[0..2n-1] = x[0..n-1]^2: off-diagonal products accumulated once, doubled,
 * then the x[i]^2 diagonal added.
 */
static VOID c68k_sqr(c68k_limb *t, const c68k_limb *x, UINT n)
{

UINT        i;
UINT        total;
UINT        stride;
UINT        left;
HN_UBASE2   product;


    total = n << 1;

    for (i = 0; i < total; i++)
    {
        t[i] = 0;
    }

    /* Row i is n-i-1 products, so n/2 on average: the stride is sized from
       that rather than from n, or the interval would be half what it says. */
    stride = c68k_yield_stride(n >> 1);
    left   = stride;

    for (i = 0; i < n; i++)
    {
        if (--left == 0u)
        {
            left = stride;
            C68K_YIELD();
        }

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


    /* c68k_sqr() counts down from 2*n-1, which wraps for n == 0, so an empty
       input must be a no-op here. */
    if (m_len == 0u)
    {
        return;
    }

    c68k_sqr_n(t, x, m_len, work + (m_len << 1) + 1u);
    t[m_len << 1] = 0;

    c68k_mont_reduce(t, m, m_len, n0inv);
    c68k_mont_final(r, &t[m_len], m, m_len);
}
