/*
 * AmiNetXDuo, crypto68k long division, 32-bit limbs.
 *
 * A 68020 DIVU.L traps on quotient overflow rather than saturating.  Algorithm
 * D's quotient estimate overflows exactly when the two top limbs are equal, so
 * that case is tested for and never reaches the instruction.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"


UINT c68k_fast_modulus = 1u;


/* ------------------------------------------------------------- helpers --- */

/* dst[0..n-1] = src[0..n-1] << s.  Returns the bits shifted out of the top.
   s must be 0..31.  s == 0 is a copy, because a 32-bit shift is undefined. */
static c68k_limb d_shl(c68k_limb *dst, const c68k_limb *src, UINT n, UINT s)
{

UINT        i;
c68k_limb   carry = 0;
c68k_limb   v;


    if (s == 0u)
    {
        for (i = 0; i < n; i++)
        {
            dst[i] = src[i];
        }
        return(0);
    }

    for (i = 0; i < n; i++)
    {
        v      = src[i];
        dst[i] = (c68k_limb)((v << s) | carry);
        carry  = (c68k_limb)(v >> (32u - s));
    }

    return(carry);
}

/* dst[0..n-1] = src[0..n-1] >> s, s in 0..31. */
static VOID d_shr(c68k_limb *dst, const c68k_limb *src, UINT n, UINT s)
{

UINT    i;


    if (s == 0u)
    {
        for (i = 0; i < n; i++)
        {
            dst[i] = src[i];
        }
        return;
    }

    for (i = 0; i < n; i++)
    {
        dst[i] = (c68k_limb)((src[i] >> s) |
                             ((i + 1u < n) ? (src[i + 1u] << (32u - s)) : 0u));
    }
}

static UINT d_clz(c68k_limb v)
{

UINT    n = 0;


    if (v == 0u)
    {
        return(32u);
    }
    while ((v & 0x80000000UL) == 0u)
    {
        v <<= 1;
        n++;
    }

    return(n);
}


/* ------------------------------------------------------------- the mod --- */

VOID c68k_mod(c68k_limb *rem, const c68k_limb *u, UINT u_len,
              const c68k_limb *m, UINT m_len, c68k_limb *scratch)
{

c68k_limb  *un;
c68k_limb  *vn;
UINT        n = m_len;
UINT        s;
UINT        i;
UINT        stride;
UINT        left;
INT         j;
c68k_limb   qhat;
c68k_limb   rhat;
c68k_limb   borrow;
c68k_limb   carry;
HN_UBASE2   num;


    if ((n == 0u) || (u_len < n))
    {
        /* u < m already, or a degenerate modulus: the remainder is u. */
        for (i = 0; i < n; i++)
        {
            rem[i] = (i < u_len) ? u[i] : 0u;
        }
        return;
    }

    /* Single-limb divisor: no estimate needed, one DIVU.L per limb. */
    if (n == 1u)
    {
        rhat = 0;
        for (j = (INT)u_len - 1; j >= 0; j--)
        {
            /* rhat < m[0] always, so the quotient fits and DIVU.L is safe. */
            (VOID) C68K_DIV_2BY1(rhat, u[(UINT)j], m[0], &rhat);
        }
        rem[0] = rhat;
        return;
    }

    un = scratch;                       /* u_len + 1 */
    vn = un + u_len + 1u;               /* m_len     */

    /* Normalise so the top bit of the divisor is set, which bounds the error
       of the quotient estimate to at most 2. */
    s = d_clz(m[n - 1u]);
    (VOID) d_shl(vn, m, n, s);
    un[u_len] = d_shl(un, u, u_len, s);

    /*
     * R^2 mod m is one call to this with a 2s-limb numerator, so it is s rows
     * of s limb products in the c68k_submul_1 below: the same size as a
     * Montgomery reduction, once per exponentiation, and it used to be the
     * longest silent stretch left after the Montgomery loops were covered.
     */
    stride = c68k_yield_stride(n);
    left   = stride;

    for (j = (INT)(u_len - n); j >= 0; j--)
    {
        c68k_limb top = un[(UINT)j + n];

        if (--left == 0u)
        {
            left = stride;
            C68K_YIELD();
        }

        /*
         * The estimate.  top <= vn[n-1] always, because the running remainder
         * stays below the divisor.  Equality is the one case whose quotient is
         * B-1, which overflows DIVU.L, so it never reaches the instruction.
         */
        if (top >= vn[n - 1u])
        {
            qhat = 0xFFFFFFFFUL;
            num  = (HN_UBASE2)un[(UINT)j + n - 1u] + (HN_UBASE2)vn[n - 1u];
            rhat = (num >> 32) != 0 ? 0xFFFFFFFFUL : (c68k_limb)num;
            if ((num >> 32) != 0)
            {
                goto subtract;          /* rhat >= B: no correction possible */
            }
        }
        else
        {
            qhat = C68K_DIV_2BY1(top, un[(UINT)j + n - 1u], vn[n - 1u], &rhat);
        }

        /* At most two corrections, by the normalisation above. */
        for (;;)
        {
            if (((HN_UBASE2)qhat * (HN_UBASE2)vn[n - 2u]) <=
                (((HN_UBASE2)rhat << 32) | (HN_UBASE2)un[(UINT)j + n - 2u]))
            {
                break;
            }

            qhat--;
            num = (HN_UBASE2)rhat + (HN_UBASE2)vn[n - 1u];
            if ((num >> 32) != 0)
            {
                break;                  /* rhat overflowed: stop correcting */
            }
            rhat = (c68k_limb)num;
        }

subtract:
        borrow = c68k_submul_1(&un[(UINT)j], vn, n, qhat);

        if (un[(UINT)j + n] < borrow)
        {
            /*
             * The estimate was one too large, rare after normalisation, but
             * possible.  Add the divisor back once.
             */
            un[(UINT)j + n] = (c68k_limb)(un[(UINT)j + n] - borrow);
            carry = c68k_add(&un[(UINT)j], vn, n);
            un[(UINT)j + n] = (c68k_limb)(un[(UINT)j + n] + carry);
        }
        else
        {
            un[(UINT)j + n] = (c68k_limb)(un[(UINT)j + n] - borrow);
        }
    }

    /* The remainder is the low n limbs, denormalised. */
    d_shr(rem, un, n, s);
}
