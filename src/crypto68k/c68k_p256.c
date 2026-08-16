/*
 * AmiNetXDuo, crypto68k: P-256 field and point arithmetic for the 68020.
 *
 * See c68k_p256.h for what this replaces and why.  In short: the vendored
 * nx_crypto_ec.c has the right algorithms, NAF, Solinas reduction, a
 * fixed-base comb, Jacobian coordinates with one inversion, and runs them on
 * a representation that costs more than the arithmetic does.  This file keeps
 * the algorithms and drops the representation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_p256.h"
#include "c68k_variant.h"

#include "nx_crypto_huge_number.h"


/*
 * p = 2^256 - 2^224 + 2^192 + 2^96 - 1, least significant limb first.
 */
static const c68k_limb c68k_p256_p[C68K_P256_LIMBS] =
{
    0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0x00000000UL,
    0x00000000UL, 0x00000000UL, 0x00000001UL, 0xFFFFFFFFUL
};

static const c68k_limb c68k_p256_one[C68K_P256_LIMBS] =
{
    1UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL
};


/* ------------------------------------------------------ limb helpers ----- */

/*
 * One binary for every CPU carries both assemblies of the three routines
 * c68k_p256.S holds.  They differ in one instruction, EXTB.L against ext.w
 * plus ext.l, and both are legal on every part, so unlike the multiply
 * primitives there is no C to fall back to here, only a slower assembly.  The
 * vectors start on that one.
 */
#ifdef C68K_MV

extern c68k_limb c68k_p256_add_raw_mv0(c68k_limb *r, const c68k_limb *a,
                                       const c68k_limb *b);
extern c68k_limb c68k_p256_add_raw_mv20(c68k_limb *r, const c68k_limb *a,
                                        const c68k_limb *b);
extern c68k_limb c68k_p256_sub_raw_mv0(c68k_limb *r, const c68k_limb *a,
                                       const c68k_limb *b);
extern c68k_limb c68k_p256_sub_raw_mv20(c68k_limb *r, const c68k_limb *a,
                                        const c68k_limb *b);
extern INT c68k_p256_reduce_core_mv0(c68k_limb *r, const c68k_limb *t);
extern INT c68k_p256_reduce_core_mv20(c68k_limb *r, const c68k_limb *t);

static c68k_limb (*c68k_vec_p256_add_raw)(c68k_limb *, const c68k_limb *,
                                          const c68k_limb *) =
    c68k_p256_add_raw_mv0;
static c68k_limb (*c68k_vec_p256_sub_raw)(c68k_limb *, const c68k_limb *,
                                          const c68k_limb *) =
    c68k_p256_sub_raw_mv0;
static INT (*c68k_vec_p256_reduce_core)(c68k_limb *, const c68k_limb *) =
    c68k_p256_reduce_core_mv0;

#define C68K_P256_ADD_RAW       (*c68k_vec_p256_add_raw)
#define C68K_P256_SUB_RAW       (*c68k_vec_p256_sub_raw)
#define C68K_P256_REDUCE_CORE   (*c68k_vec_p256_reduce_core)

/* Called by c68k_cpu_select().  The vectors are static to this file. */
VOID c68k_p256_cpu_select(UINT wide)
{

    if (wide != 0u)
    {
        c68k_vec_p256_add_raw     = c68k_p256_add_raw_mv20;
        c68k_vec_p256_sub_raw     = c68k_p256_sub_raw_mv20;
        c68k_vec_p256_reduce_core = c68k_p256_reduce_core_mv20;
    }
    else
    {
        c68k_vec_p256_add_raw     = c68k_p256_add_raw_mv0;
        c68k_vec_p256_sub_raw     = c68k_p256_sub_raw_mv0;
        c68k_vec_p256_reduce_core = c68k_p256_reduce_core_mv0;
    }
}

#else

#define C68K_P256_ADD_RAW       c68k_p256_add_raw
#define C68K_P256_SUB_RAW       c68k_p256_sub_raw
#define C68K_P256_REDUCE_CORE   c68k_p256_reduce_core

#endif

#ifndef C68K_ASM_P256

/*
 * The portable halves of the three routines c68k_p256.S replaces.  Always
 * buildable, and the way to bisect a suspected assembly bug: same algorithm,
 * limb for limb, term for term.
 */

c68k_limb c68k_p256_add_raw(c68k_limb *r, const c68k_limb *a, const c68k_limb *b)
{

UINT        i;
c68k_limb   carry;
c68k_limb   s;
c68k_limb   t;


    carry = 0;

    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        s = a[i] + carry;
        carry = (s < carry) ? 1UL : 0UL;
        t = s + b[i];
        carry = carry + ((t < s) ? 1UL : 0UL);
        r[i] = t;
    }

    return(carry);
}


c68k_limb c68k_p256_sub_raw(c68k_limb *r, const c68k_limb *a, const c68k_limb *b)
{

UINT        i;
c68k_limb   borrow;
c68k_limb   s;
c68k_limb   t;


    borrow = 0;

    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        s = a[i] - borrow;
        borrow = (s > a[i]) ? 1UL : 0UL;
        t = s - b[i];
        borrow = borrow + ((t > s) ? 1UL : 0UL);
        r[i] = t;
    }

    return(borrow);
}


/*
 * The Solinas reduction of FIPS 186-4 D.2.3, collapsed per output limb.
 *
 *     r = s1 + 2*s2 + 2*s3 + s4 + s5 - s6 - s7 - s8 - s9  (mod p)
 *
 * where each s_k is a permutation of the sixteen product limbs c0..c15.  The
 * vendored implementation materialises each s_k as a 64-byte big-endian byte
 * stream, built one byte at a time, and adds them as sign-carrying huge
 * numbers.  Here the nine terms are collapsed into one pass: limb 0 is
 * c0 + c8 + c9 - c11 - c12 - c13 - c14 and so on, accumulated in a (lo, hi)
 * pair where hi is the signed carry into the next limb.  Nothing is
 * serialised.
 *
 * Returns the leftover coefficient of 2^256, for the caller to fold in.
 *
 * The decomposition was checked against the arbitrary precision arithmetic of
 * Python over 20000 random 512-bit inputs plus the all-zero and all-ones
 * cases, and the leftover was measured to stay in [-4, 4].
 */
INT c68k_p256_reduce_core(c68k_limb *r, const c68k_limb *t)
{

c68k_limb   lo;
INT         hi;


#define C68K_ADD(v)     do { c68k_limb v_ = (v); lo += v_;                  \
                             if (lo < v_) { hi++; } } while (0)
#define C68K_SUB(v)     do { c68k_limb v_ = (v);                            \
                             if (lo < v_) { hi--; } lo -= v_; } while (0)
#define C68K_NEXT()     do { lo = (c68k_limb)hi;                            \
                             hi = (hi < 0) ? -1 : 0; } while (0)

    lo = 0;
    hi = 0;

    /* limb 0: c0 + c8 + c9 - c11 - c12 - c13 - c14 */
    C68K_ADD(t[0]);  C68K_ADD(t[8]);  C68K_ADD(t[9]);
    C68K_SUB(t[11]); C68K_SUB(t[12]); C68K_SUB(t[13]); C68K_SUB(t[14]);
    r[0] = lo; C68K_NEXT();

    /* limb 1: c1 + c9 + c10 - c12 - c13 - c14 - c15 */
    C68K_ADD(t[1]);  C68K_ADD(t[9]);  C68K_ADD(t[10]);
    C68K_SUB(t[12]); C68K_SUB(t[13]); C68K_SUB(t[14]); C68K_SUB(t[15]);
    r[1] = lo; C68K_NEXT();

    /* limb 2: c2 + c10 + c11 - c13 - c14 - c15 */
    C68K_ADD(t[2]);  C68K_ADD(t[10]); C68K_ADD(t[11]);
    C68K_SUB(t[13]); C68K_SUB(t[14]); C68K_SUB(t[15]);
    r[2] = lo; C68K_NEXT();

    /* limb 3: c3 + 2*c11 + 2*c12 + c13 - c15 - c8 - c9 */
    C68K_ADD(t[3]);  C68K_ADD(t[11]); C68K_ADD(t[11]);
    C68K_ADD(t[12]); C68K_ADD(t[12]); C68K_ADD(t[13]);
    C68K_SUB(t[15]); C68K_SUB(t[8]);  C68K_SUB(t[9]);
    r[3] = lo; C68K_NEXT();

    /* limb 4: c4 + 2*c12 + 2*c13 + c14 - c9 - c10 */
    C68K_ADD(t[4]);  C68K_ADD(t[12]); C68K_ADD(t[12]);
    C68K_ADD(t[13]); C68K_ADD(t[13]); C68K_ADD(t[14]);
    C68K_SUB(t[9]);  C68K_SUB(t[10]);
    r[4] = lo; C68K_NEXT();

    /* limb 5: c5 + 2*c13 + 2*c14 + c15 - c10 - c11 */
    C68K_ADD(t[5]);  C68K_ADD(t[13]); C68K_ADD(t[13]);
    C68K_ADD(t[14]); C68K_ADD(t[14]); C68K_ADD(t[15]);
    C68K_SUB(t[10]); C68K_SUB(t[11]);
    r[5] = lo; C68K_NEXT();

    /* limb 6: c6 + 3*c14 + 2*c15 + c13 - c8 - c9 */
    C68K_ADD(t[6]);  C68K_ADD(t[14]); C68K_ADD(t[14]); C68K_ADD(t[14]);
    C68K_ADD(t[15]); C68K_ADD(t[15]); C68K_ADD(t[13]);
    C68K_SUB(t[8]);  C68K_SUB(t[9]);
    r[6] = lo; C68K_NEXT();

    /* limb 7: c7 + 3*c15 + c8 - c10 - c11 - c12 - c13 */
    C68K_ADD(t[7]);  C68K_ADD(t[15]); C68K_ADD(t[15]); C68K_ADD(t[15]);
    C68K_ADD(t[8]);
    C68K_SUB(t[10]); C68K_SUB(t[11]); C68K_SUB(t[12]); C68K_SUB(t[13]);
    r[7] = lo;

#undef C68K_ADD
#undef C68K_SUB
#undef C68K_NEXT

    return(hi);
}

#endif /* !C68K_ASM */


static INT fe_cmp(const c68k_limb *a, const c68k_limb *b)
{

UINT    i;


    for (i = C68K_P256_LIMBS; i > 0; i--)
    {
        if (a[i - 1] != b[i - 1])
        {
            return((a[i - 1] > b[i - 1]) ? 1 : -1);
        }
    }

    return(0);
}


static UINT fe_is_zero(const c68k_limb *a)
{

UINT    i;


    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        if (a[i] != 0)
        {
            return(0u);
        }
    }

    return(1u);
}


static UINT fe_is_one(const c68k_limb *a)
{

UINT    i;


    if (a[0] != 1UL)
    {
        return(0u);
    }

    for (i = 1; i < C68K_P256_LIMBS; i++)
    {
        if (a[i] != 0)
        {
            return(0u);
        }
    }

    return(1u);
}


static VOID fe_copy(c68k_limb *r, const c68k_limb *a)
{

UINT    i;


    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        r[i] = a[i];
    }
}


static VOID fe_zero(c68k_limb *r)
{

UINT    i;


    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        r[i] = 0;
    }
}


/* r >>= 1, with `top` shifted into the most significant bit. */
static VOID fe_rshift1(c68k_limb *r, c68k_limb top)
{

UINT    i;


    for (i = 0; i < C68K_P256_LIMBS - 1u; i++)
    {
        r[i] = (r[i] >> 1) | (r[i + 1] << 31);
    }

    r[C68K_P256_LIMBS - 1u] = (r[C68K_P256_LIMBS - 1u] >> 1) | (top << 31);
}


/* ------------------------------------------------------- field: core ---- */

VOID c68k_p256_fe_reduce(c68k_p256_fe r, const c68k_limb t[16])
{

INT     hi;


    hi = C68K_P256_REDUCE_CORE(r, t);

    /*
     * hi is the leftover coefficient of 2^256.  An addition of p raises the
     * value by very nearly 2^256, so each addition moves hi towards zero by
     * one, and each subtraction does the same.  A handful of iterations.
     */
    while (hi < 0)
    {
        hi = hi + (INT)C68K_P256_ADD_RAW(r, r, c68k_p256_p);
    }

    while (hi > 0)
    {
        hi = hi - (INT)C68K_P256_SUB_RAW(r, r, c68k_p256_p);
    }

    while (fe_cmp(r, c68k_p256_p) >= 0)
    {
        (VOID) C68K_P256_SUB_RAW(r, r, c68k_p256_p);
    }
}


static VOID c68k_p256_fe_add(c68k_p256_fe r, const c68k_p256_fe a, const c68k_p256_fe b)
{

c68k_limb   carry;


    carry = C68K_P256_ADD_RAW(r, a, b);

    /* a + b < 2p < 2^257, so at most one subtraction is ever needed. */
    if ((carry != 0) || (fe_cmp(r, c68k_p256_p) >= 0))
    {
        (VOID) C68K_P256_SUB_RAW(r, r, c68k_p256_p);
    }
}


static VOID c68k_p256_fe_sub(c68k_p256_fe r, const c68k_p256_fe a, const c68k_p256_fe b)
{

    if (C68K_P256_SUB_RAW(r, a, b) != 0)
    {
        (VOID) C68K_P256_ADD_RAW(r, r, c68k_p256_p);
    }
}


VOID c68k_p256_fe_mul(c68k_p256_fe r, const c68k_p256_fe a, const c68k_p256_fe b)
{

c68k_limb   t[16];
UINT        i;


    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        t[i] = 0;
    }

    /*
     * Schoolbook, one c68k_addmul_1 per limb of a.  That routine is the 68020
     * assembly the RSA work produced, so the ~1.4x limb-loop gain carries over
     * to elliptic curves with no new assembly.  t[i + 8] has not been written
     * when row i runs, so the carry-out is a store, not an add.
     */
    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        t[i + C68K_P256_LIMBS] = C68K_ADDMUL_1(&t[i], b, C68K_P256_LIMBS, a[i]);
    }

    c68k_p256_fe_reduce(r, t);
}


VOID c68k_p256_fe_sqr(c68k_p256_fe r, const c68k_p256_fe a)
{

c68k_limb   t[16];
HN_UBASE2   acc;
c68k_limb   carry;
c68k_limb   v;
UINT        i;


    for (i = 0; i < 16u; i++)
    {
        t[i] = 0;
    }

    /*
     * Off-diagonal half first: 28 limb products instead of 64, because
     * a[i]*a[j] and a[j]*a[i] are computed once and the sum doubled.
     */
    for (i = 0; i < C68K_P256_LIMBS - 1u; i++)
    {
        t[i + C68K_P256_LIMBS] = C68K_ADDMUL_1(&t[(i << 1) + 1u], &a[i + 1u],
                                               C68K_P256_LIMBS - 1u - i, a[i]);
    }

    /* Double.  The off-diagonal sum is strictly below 2^511, so this fits. */
    carry = 0;
    for (i = 0; i < 16u; i++)
    {
        v = t[i];
        t[i] = (v << 1) | carry;
        carry = v >> 31;
    }

    /* Add the diagonal a[i]^2 at limb 2i. */
    acc = 0;
    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        acc = (acc >> 32) + (HN_UBASE2)t[i << 1] +
              ((HN_UBASE2)a[i] * (HN_UBASE2)a[i]);
        t[i << 1] = (c68k_limb)acc;

        acc = (acc >> 32) + (HN_UBASE2)t[(i << 1) + 1u];
        t[(i << 1) + 1u] = (c68k_limb)acc;
    }

    c68k_p256_fe_reduce(r, t);
}


/*
 * x = x / 2 mod p.  If x is odd, x + p is even and congruent, so the addition
 * comes first.  It can carry out of 256 bits, and that bit is shifted back in.
 */
static VOID fe_half(c68k_p256_fe x)
{

c68k_limb   top;


    top = 0;

    if ((x[0] & 1UL) != 0)
    {
        top = C68K_P256_ADD_RAW(x, x, c68k_p256_p);
    }

    fe_rshift1(x, top);
}


/*
 * Binary extended Euclid, Hankerson Algorithm 2.22.  Chosen over Fermat
 * (a^(p-2), 255 squarings) because it uses no multiplications: on this machine
 * an inversion costs about four field multiplications, so one per scalar
 * multiplication plus one per batch inversion does not need optimisation.
 */
VOID c68k_p256_fe_inv(c68k_p256_fe r, const c68k_p256_fe a)
{

c68k_p256_fe    u;
c68k_p256_fe    v;
c68k_p256_fe    x1;
c68k_p256_fe    x2;


    if (fe_is_zero(a))
    {
        fe_zero(r);
        return;
    }

    fe_copy(u, a);
    fe_copy(v, c68k_p256_p);
    fe_copy(x1, c68k_p256_one);
    fe_zero(x2);

    while ((fe_is_one(u) == 0u) && (fe_is_one(v) == 0u))
    {
        while ((u[0] & 1UL) == 0)
        {
            fe_rshift1(u, 0);
            fe_half(x1);
        }

        while ((v[0] & 1UL) == 0)
        {
            fe_rshift1(v, 0);
            fe_half(x2);
        }

        if (fe_cmp(u, v) >= 0)
        {
            (VOID) C68K_P256_SUB_RAW(u, u, v);
            c68k_p256_fe_sub(x1, x1, x2);
        }
        else
        {
            (VOID) C68K_P256_SUB_RAW(v, v, u);
            c68k_p256_fe_sub(x2, x2, x1);
        }
    }

    fe_copy(r, fe_is_one(u) ? x1 : x2);
}


/* ------------------------------------------------------------- points ---- */

/*
 * dbl-2001-b from the EFD, valid because a == -3 for every NIST prime curve:
 *
 *     delta = Z^2 ; gamma = Y^2 ; beta = X * gamma
 *     alpha = 3 * (X - delta) * (X + delta)
 *     X3 = alpha^2 - 8*beta
 *     Z3 = (Y + Z)^2 - gamma - delta
 *     Y3 = alpha * (4*beta - X3) - 8*gamma^2
 *
 * 3M + 5S, against the vendored 4M + 4S.  A gain only because squaring here is
 * cheaper than a multiply, 28 limb products against 64.
 */
VOID c68k_p256_jac_double(c68k_p256_jac *r, const c68k_p256_jac *p1)
{

c68k_p256_fe    delta;
c68k_p256_fe    gamma;
c68k_p256_fe    beta;
c68k_p256_fe    alpha;
c68k_p256_fe    t1;
c68k_p256_fe    t2;
c68k_p256_fe    x3;
c68k_p256_fe    y3;
c68k_p256_fe    z3;


    if (fe_is_zero(p1 -> z))
    {
        fe_zero(r -> z);
        return;
    }

    c68k_p256_fe_sqr(delta, p1 -> z);
    c68k_p256_fe_sqr(gamma, p1 -> y);
    c68k_p256_fe_mul(beta, p1 -> x, gamma);

    c68k_p256_fe_sub(t1, p1 -> x, delta);
    c68k_p256_fe_add(t2, p1 -> x, delta);
    c68k_p256_fe_mul(alpha, t1, t2);
    c68k_p256_fe_add(t1, alpha, alpha);
    c68k_p256_fe_add(alpha, t1, alpha);         /* alpha = 3 * (X-d)(X+d) */

    /* Z3 first: it is the only place the input Y and Z are still needed. */
    c68k_p256_fe_add(z3, p1 -> y, p1 -> z);
    c68k_p256_fe_sqr(z3, z3);
    c68k_p256_fe_sub(z3, z3, gamma);
    c68k_p256_fe_sub(z3, z3, delta);

    c68k_p256_fe_add(t1, beta, beta);
    c68k_p256_fe_add(t1, t1, t1);               /* t1 = 4 * beta */
    c68k_p256_fe_add(t2, t1, t1);               /* t2 = 8 * beta */

    c68k_p256_fe_sqr(x3, alpha);
    c68k_p256_fe_sub(x3, x3, t2);

    c68k_p256_fe_sub(t2, t1, x3);               /* 4*beta - X3 */
    c68k_p256_fe_mul(y3, alpha, t2);

    c68k_p256_fe_sqr(gamma, gamma);             /* gamma^2 = Y^4 */
    c68k_p256_fe_add(t1, gamma, gamma);
    c68k_p256_fe_add(t1, t1, t1);
    c68k_p256_fe_add(t1, t1, t1);               /* 8 * Y^4 */
    c68k_p256_fe_sub(y3, y3, t1);

    fe_copy(r -> x, x3);
    fe_copy(r -> y, y3);
    fe_copy(r -> z, z3);
}


/*
 * madd-2007-bl: Jacobian + affine, 7M + 4S against the vendored 8M + 3S.
 *
 * Every degenerate case is handled explicitly.  A scalar multiplication that
 * mishandles an accumulator equal to a table entry returns a valid curve point
 * that is wrong, and no sanity check catches that.
 */
VOID c68k_p256_jac_add_affine(c68k_p256_jac *r, const c68k_p256_jac *p1,
                              const c68k_p256_aff *q)
{

c68k_p256_fe    z1z1;
c68k_p256_fe    u2;
c68k_p256_fe    s2;
c68k_p256_fe    h;
c68k_p256_fe    rr;
c68k_p256_fe    hh;
c68k_p256_fe    ii;
c68k_p256_fe    jj;
c68k_p256_fe    vv;
c68k_p256_fe    t1;
c68k_p256_fe    x3;
c68k_p256_fe    y3;
c68k_p256_fe    z3;


    /* q at infinity is (0, 0), the representation the vendored code uses. */
    if (fe_is_zero(q -> x) && fe_is_zero(q -> y))
    {
        if (r != p1)
        {
            *r = *p1;
        }
        return;
    }

    if (fe_is_zero(p1 -> z))
    {
        fe_copy(r -> x, q -> x);
        fe_copy(r -> y, q -> y);
        fe_copy(r -> z, c68k_p256_one);
        return;
    }

    c68k_p256_fe_sqr(z1z1, p1 -> z);
    c68k_p256_fe_mul(u2, q -> x, z1z1);
    c68k_p256_fe_mul(s2, p1 -> z, z1z1);
    c68k_p256_fe_mul(s2, q -> y, s2);

    c68k_p256_fe_sub(h, u2, p1 -> x);
    c68k_p256_fe_sub(rr, s2, p1 -> y);
    c68k_p256_fe_add(rr, rr, rr);

    if (fe_is_zero(h))
    {
        if (fe_is_zero(rr))
        {
            /* p1 == q: the addition formula degenerates, double instead. */
            c68k_p256_jac_double(r, p1);
        }
        else
        {
            /* p1 == -q. */
            fe_zero(r -> z);
        }
        return;
    }

    c68k_p256_fe_sqr(hh, h);
    c68k_p256_fe_add(ii, hh, hh);
    c68k_p256_fe_add(ii, ii, ii);               /* I = 4 * HH */
    c68k_p256_fe_mul(jj, h, ii);
    c68k_p256_fe_mul(vv, p1 -> x, ii);

    /* Z3 = (Z1 + H)^2 - Z1Z1 - HH, while Z1 is still readable. */
    c68k_p256_fe_add(z3, p1 -> z, h);
    c68k_p256_fe_sqr(z3, z3);
    c68k_p256_fe_sub(z3, z3, z1z1);
    c68k_p256_fe_sub(z3, z3, hh);

    c68k_p256_fe_sqr(x3, rr);
    c68k_p256_fe_sub(x3, x3, jj);
    c68k_p256_fe_sub(x3, x3, vv);
    c68k_p256_fe_sub(x3, x3, vv);               /* X3 = r^2 - J - 2V */

    c68k_p256_fe_sub(t1, vv, x3);
    c68k_p256_fe_mul(y3, rr, t1);
    c68k_p256_fe_mul(t1, p1 -> y, jj);
    c68k_p256_fe_add(t1, t1, t1);               /* 2 * Y1 * J */
    c68k_p256_fe_sub(y3, y3, t1);

    fe_copy(r -> x, x3);
    fe_copy(r -> y, y3);
    fe_copy(r -> z, z3);
}


static VOID c68k_p256_to_affine(c68k_p256_aff *r, const c68k_p256_jac *p1)
{

c68k_p256_fe    zi;
c68k_p256_fe    z2;


    if (fe_is_zero(p1 -> z))
    {
        fe_zero(r -> x);
        fe_zero(r -> y);
        return;
    }

    c68k_p256_fe_inv(zi, p1 -> z);
    c68k_p256_fe_sqr(z2, zi);
    c68k_p256_fe_mul(r -> x, p1 -> x, z2);
    c68k_p256_fe_mul(z2, z2, zi);
    c68k_p256_fe_mul(r -> y, p1 -> y, z2);
}


/* ---------------------------------------------------- fixed base: comb --- */

/* Bit i of a 256-bit scalar.  Indices at or above 256 read as zero, which is
   what lets the top row of the comb run past the end of the scalar. */
static c68k_limb k_bit(const c68k_limb *k, UINT i)
{

    if (i >= 256u)
    {
        return(0);
    }

    return((k[i >> 5] >> (i & 31u)) & 1UL);
}


VOID c68k_p256_mul_g(c68k_p256_aff *r, const c68k_limb k[C68K_P256_LIMBS])
{

c68k_p256_jac   acc;
c68k_p256_aff   entry;
UINT            c;
UINT            t;
UINT            digit;
UINT            row;


    fe_zero(acc.z);
    fe_zero(acc.x);
    fe_zero(acc.y);

    /*
     * Lim-Lee: k = sum_c 2^c * T1[digit(c)] over c in 0..d-1, split at e so
     * that columns e..d-1 are served by T2 = 2^e * T1 in the same iteration.
     * 26 doublings and 52 additions, against 256 doublings for a generic
     * point, which is why ECDH key generation measures at 1.52 s while an
     * ECDH shared secret measures at 5.18 s.
     */
    for (c = C68K_P256_COMB_E; c > 0u; c--)
    {
        C68K_YIELD();

        c68k_p256_jac_double(&acc, &acc);

        digit = 0;
        for (t = 0; t < C68K_P256_COMB_W; t++)
        {
            digit |= (UINT)k_bit(k, (t * C68K_P256_COMB_D) + c - 1u) << t;
        }
        if (digit != 0)
        {
            fe_copy(entry.x, &c68k_p256_comb[digit - 1u][0]);
            fe_copy(entry.y, &c68k_p256_comb[digit - 1u][8]);
            c68k_p256_jac_add_affine(&acc, &acc, &entry);
        }

        digit = 0;
        for (t = 0; t < C68K_P256_COMB_W; t++)
        {
            digit |= (UINT)k_bit(k, (t * C68K_P256_COMB_D) + c - 1u +
                                    C68K_P256_COMB_E) << t;
        }
        if (digit != 0)
        {
            row = C68K_P256_COMB_HALF + digit - 1u;
            fe_copy(entry.x, &c68k_p256_comb[row][0]);
            fe_copy(entry.y, &c68k_p256_comb[row][8]);
            c68k_p256_jac_add_affine(&acc, &acc, &entry);
        }
    }

    c68k_p256_to_affine(r, &acc);
}


/* ------------------------------------------------ generic point: wNAF ---- */

/*
 * Width-5 non-adjacent form.  Digits are odd and in [-15, 15], one nonzero in
 * six on average, so 256 bits cost about 43 additions, against 85 for the
 * vendored width-2 NAF.  Returns the digit count, at most 257.
 */
static UINT p256_wnaf(signed char *naf, const c68k_limb k[C68K_P256_LIMBS])
{

c68k_limb   kk[C68K_P256_LIMBS + 1u];
UINT        len;
UINT        i;
UINT        nonzero;
INT         d;
c68k_limb   borrow;
c68k_limb   carry;
c68k_limb   v;


    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        kk[i] = k[i];
    }
    kk[C68K_P256_LIMBS] = 0;

    len = 0;

    for (;;)
    {
        nonzero = 0;
        for (i = 0; i <= C68K_P256_LIMBS; i++)
        {
            if (kk[i] != 0)
            {
                nonzero = 1u;
                break;
            }
        }
        if (nonzero == 0u)
        {
            break;
        }

        d = 0;
        if ((kk[0] & 1UL) != 0)
        {
            d = (INT)(kk[0] & 31UL);
            if (d >= 16)
            {
                d -= 32;
            }

            if (d > 0)
            {
                borrow = (c68k_limb)d;
                for (i = 0; i <= C68K_P256_LIMBS; i++)
                {
                    v = kk[i];
                    kk[i] = v - borrow;
                    borrow = (kk[i] > v) ? 1UL : 0UL;
                    if (borrow == 0)
                    {
                        break;
                    }
                }
            }
            else
            {
                carry = (c68k_limb)(-d);
                for (i = 0; i <= C68K_P256_LIMBS; i++)
                {
                    v = kk[i];
                    kk[i] = v + carry;
                    carry = (kk[i] < v) ? 1UL : 0UL;
                    if (carry == 0)
                    {
                        break;
                    }
                }
            }
        }

        naf[len] = (signed char)d;
        len++;

        for (i = 0; i < C68K_P256_LIMBS; i++)
        {
            kk[i] = (kk[i] >> 1) | (kk[i + 1u] << 31);
        }
        kk[C68K_P256_LIMBS] >>= 1;
    }

    return(len);
}


VOID c68k_p256_mul_point(c68k_p256_aff *r, const c68k_p256_aff *q,
                         const c68k_limb k[C68K_P256_LIMBS],
                         c68k_p256_work *work)
{

c68k_p256_jac   acc;
c68k_p256_jac   dbl;
c68k_p256_aff   q2;
c68k_p256_aff   entry;
c68k_p256_fe    inv;
c68k_p256_fe    zt;
UINT            len;
UINT            i;
INT             d;


    if ((fe_is_zero(q -> x) && fe_is_zero(q -> y)))
    {
        fe_zero(r -> x);
        fe_zero(r -> y);
        return;
    }

    /*
     * Table: q, 3q, 5q, ..., 15q, affine so that every addition in the main
     * loop is the cheap mixed one.
     *
     * 2q goes to affine (one inversion), then seven mixed additions chain
     * from it, then a single batch inversion converts all eight results
     * (Montgomery's trick: one inversion and 3*(n-1) multiplies instead of n
     * inversions).  Two inversions in total for the whole table.
     */
    fe_copy(dbl.x, q -> x);
    fe_copy(dbl.y, q -> y);
    fe_copy(dbl.z, c68k_p256_one);
    c68k_p256_jac_double(&dbl, &dbl);
    c68k_p256_to_affine(&q2, &dbl);

    fe_copy(work -> tab[0].x, q -> x);
    fe_copy(work -> tab[0].y, q -> y);
    fe_copy(work -> tab[0].z, c68k_p256_one);

    for (i = 1; i < C68K_P256_WNAF_POINTS; i++)
    {
        c68k_p256_jac_add_affine(&work -> tab[i], &work -> tab[i - 1u], &q2);
    }

    /* acc[i] = z_0 * z_1 * ... * z_i */
    fe_copy(work -> acc[0], work -> tab[0].z);
    for (i = 1; i < C68K_P256_WNAF_POINTS; i++)
    {
        c68k_p256_fe_mul(work -> acc[i], work -> acc[i - 1u], work -> tab[i].z);
    }

    c68k_p256_fe_inv(inv, work -> acc[C68K_P256_WNAF_POINTS - 1u]);

    for (i = C68K_P256_WNAF_POINTS; i > 0u; i--)
    {
        if (i > 1u)
        {
            c68k_p256_fe_mul(zt, inv, work -> acc[i - 2u]);
            c68k_p256_fe_mul(inv, inv, work -> tab[i - 1u].z);
        }
        else
        {
            fe_copy(zt, inv);
        }

        /* zt = z_i^-1; convert in place, the z field is dead afterwards. */
        c68k_p256_fe_sqr(work -> tab[i - 1u].z, zt);
        c68k_p256_fe_mul(work -> tab[i - 1u].x, work -> tab[i - 1u].x,
                         work -> tab[i - 1u].z);
        c68k_p256_fe_mul(work -> tab[i - 1u].z, work -> tab[i - 1u].z, zt);
        c68k_p256_fe_mul(work -> tab[i - 1u].y, work -> tab[i - 1u].y,
                         work -> tab[i - 1u].z);
    }

    len = p256_wnaf(work -> naf, k);

    fe_zero(acc.x);
    fe_zero(acc.y);
    fe_zero(acc.z);

    for (i = len; i > 0u; i--)
    {
        C68K_YIELD();

        c68k_p256_jac_double(&acc, &acc);

        d = (INT)work -> naf[i - 1u];
        if (d > 0)
        {
            fe_copy(entry.x, work -> tab[d >> 1].x);
            fe_copy(entry.y, work -> tab[d >> 1].y);
            c68k_p256_jac_add_affine(&acc, &acc, &entry);
        }
        else if (d < 0)
        {
            fe_copy(entry.x, work -> tab[(-d) >> 1].x);
            c68k_p256_fe_sub(entry.y, c68k_p256_p, work -> tab[(-d) >> 1].y);
            c68k_p256_jac_add_affine(&acc, &acc, &entry);
        }
    }

    c68k_p256_to_affine(r, &acc);
}


/* --------------------------------------------------------- integration --- */

/* Copy a huge number into eight limbs, zero padded.  Fails only if the value
   does not fit, which tells the caller to fall back. */
static UINT hn_to_fe(c68k_limb *r, const NX_CRYPTO_HUGE_NUMBER *h)
{

UINT    i;


    if ((h -> nx_crypto_huge_number_size > C68K_P256_LIMBS) ||
        (h -> nx_crypto_huge_number_is_negative != NX_CRYPTO_FALSE))
    {
        return(0u);
    }

    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        r[i] = (i < h -> nx_crypto_huge_number_size) ?
               h -> nx_crypto_huge_number_data[i] : 0UL;
    }

    return(1u);
}


static VOID fe_to_hn(NX_CRYPTO_HUGE_NUMBER *h, const c68k_limb *a)
{

UINT    i;


    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        h -> nx_crypto_huge_number_data[i] = a[i];
    }

    h -> nx_crypto_huge_number_size = C68K_P256_LIMBS;
    h -> nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;

    _nx_crypto_huge_number_adjust_size(h);
}


VOID c68k_p256_ec_multiple(NX_CRYPTO_EC *curve,
                           NX_CRYPTO_EC_POINT *g,
                           NX_CRYPTO_HUGE_NUMBER *d,
                           NX_CRYPTO_EC_POINT *r,
                           HN_UBASE *scratch)
{

c68k_p256_aff   q;
c68k_p256_aff   out;
c68k_limb       k[C68K_P256_LIMBS];
c68k_p256_work *work;


    /*
     * Everything this cannot do falls through to the vendored routine, so the
     * substitution is safe under every caller.  _nx_crypto_ec_precomputation
     * does pass scalars wider than 256 bits, which are otherwise silently
     * truncated.
     */
    if ((curve -> nx_crypto_ec_id != NX_CRYPTO_EC_SECP256R1) ||
        (r -> nx_crypto_ec_point_x.nx_crypto_huge_buffer_size <
         (C68K_P256_LIMBS << HN_SIZE_SHIFT)) ||
        (r -> nx_crypto_ec_point_y.nx_crypto_huge_buffer_size <
         (C68K_P256_LIMBS << HN_SIZE_SHIFT)) ||
        (hn_to_fe(k, d) == 0u))
    {
        _nx_crypto_ec_fp_projective_multiple(curve, g, d, r, scratch);
        return;
    }

    if (g == &curve -> nx_crypto_ec_g)
    {
        c68k_p256_mul_g(&out, k);
    }
    else
    {
        if ((hn_to_fe(q.x, &g -> nx_crypto_ec_point_x) == 0u) ||
            (hn_to_fe(q.y, &g -> nx_crypto_ec_point_y) == 0u))
        {
            _nx_crypto_ec_fp_projective_multiple(curve, g, d, r, scratch);
            return;
        }

        /*
         * The wNAF table lives in the scratch of the caller, not on the
         * stack.  This runs under a 4 KB AmigaOS process stack, and every
         * caller already carries a scratch area sized for the vendored tables
         * (ECDSA 3016 bytes, ECDH 2464, of which roughly 450 are spent before
         * this point, against the C68K_P256_WORK_LIMBS needed here).
         */
        work = (c68k_p256_work *)(VOID *)scratch;
        c68k_p256_mul_point(&out, &q, k, work);
    }

    fe_to_hn(&r -> nx_crypto_ec_point_x, out.x);
    fe_to_hn(&r -> nx_crypto_ec_point_y, out.y);
    r -> nx_crypto_ec_point_type = NX_CRYPTO_EC_POINT_AFFINE;
}


UINT c68k_p256_self_check(VOID)
{

static c68k_p256_work   check_work;
c68k_p256_aff           g;
c68k_p256_aff           got;
c68k_limb               k[C68K_P256_LIMBS];
UINT                    i;


    /* T1[0] must be G itself. */
    fe_copy(g.x, &c68k_p256_comb[0][0]);
    fe_copy(g.y, &c68k_p256_comb[0][8]);

    /* T2[0] must be 2^e * G.  Recompute it the slow way and compare. */
    for (i = 0; i < C68K_P256_LIMBS; i++)
    {
        k[i] = 0;
    }
    k[C68K_P256_COMB_E >> 5] = 1UL << (C68K_P256_COMB_E & 31u);

    c68k_p256_mul_point(&got, &g, k, &check_work);

    if ((fe_cmp(got.x, &c68k_p256_comb[C68K_P256_COMB_HALF][0]) != 0) ||
        (fe_cmp(got.y, &c68k_p256_comb[C68K_P256_COMB_HALF][8]) != 0))
    {
        return(NX_CRYPTO_NOT_SUCCESSFUL);
    }

    /* And the comb itself must agree with the generic routine on 2^e. */
    c68k_p256_mul_g(&got, k);

    if ((fe_cmp(got.x, &c68k_p256_comb[C68K_P256_COMB_HALF][0]) != 0) ||
        (fe_cmp(got.y, &c68k_p256_comb[C68K_P256_COMB_HALF][8]) != 0))
    {
        return(NX_CRYPTO_NOT_SUCCESSFUL);
    }

    return(NX_CRYPTO_SUCCESS);
}
