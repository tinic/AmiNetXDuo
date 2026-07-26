/*
 * AmiNetXDuo -- crypto68k long division, 32-bit limbs.
 *
 * WHY THIS EXISTS
 *
 *   Measured, not guessed: on an RSA-2048 public operation through
 *   crypto68k, 36.6 ms of 163.9 went into ONE routine -- the R^2 mod m setup
 *   in c68k_powm.c, which reduces through the vendored
 *   _nx_crypto_huge_number_modulus().  AmiSSL's equivalent is 15.9 ms.  With
 *   the exponentiation itself level at 0.997x after Karatsuba, that setup was
 *   98% of everything still separating us from OpenSSL on that operation.
 *
 *   The vendored divider is a correct implementation of the right algorithm
 *   -- traditional long division with a two-digit quotient estimate -- done in
 *   SIXTEEN-BIT half-limbs.  Its own declarations say so ("In number of USHORT
 *   words") and it takes its estimate from `>> (HN_SHIFT >> 1)`.  Halving the
 *   digit size doubles the number of quotient digits AND doubles the length of
 *   the multiply-subtract pass under each one, so it is about four times the
 *   inner work of the same algorithm over 32-bit limbs.  OpenSSL gets its
 *   quotient digit from bn_div_words, which on this target is one DIVU.L out
 *   of Howard Chu's bn_m68k.s.
 *
 *   So this is not a better algorithm.  It is the same algorithm at the
 *   machine's own word size.
 *
 * WHAT WAS COSTED AND REJECTED FIRST
 *
 *   Caching R^2 against the modulus, which would have avoided the division
 *   rather than speeding it up.  It buys nothing for a TLS client: the three
 *   RSA public operations in a handshake verify the leaf with the
 *   intermediate's key, the intermediate with the root's, and the
 *   ServerKeyExchange with the leaf's -- three DIFFERENT moduli, so a cache
 *   keyed on the modulus never hits inside a handshake.  Across handshakes to
 *   one host, session resumption (docs/RESEARCH.md 13) does no public-key work
 *   at all, so it would not be consulted there either.
 *
 *   Also rejected, and worth recording because they look attractive: R^2 mod m
 *   by repeated modular doubling from R mod m is 2048 shift-and-subtract
 *   passes over 64 limbs, and by a Montgomery-squaring ladder is eleven
 *   Montgomery squares.  Priced at this module's own measured figures those
 *   are ~42 ms and ~72 ms against the 36.6 ms being replaced.  Division is the
 *   right primitive; it just has to be done in 32-bit digits.
 *
 * THE ONE PLACE DIVU.L CAN TRAP
 *
 *   Algorithm D's quotient estimate divides a two-limb prefix of the partial
 *   remainder by the top limb of the divisor, and the result fits in one limb
 *   EXCEPT when those two top limbs are equal, where the true quotient digit
 *   is B-1.  A 68020 DIVU.L traps on that overflow rather than saturating, so
 *   the case is tested for and never reaches the instruction.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"


UINT c68k_fast_modulus = 1u;


/* ------------------------------------------------------------- helpers --- */

static VOID d_zero(c68k_limb *p, UINT n)
{

UINT    i;


    for (i = 0; i < n; i++)
    {
        p[i] = 0;
    }
}

/* dst[0..n-1] = src[0..n-1] << s, returning the bits shifted out of the top.
   s must be 0..31; s == 0 is a copy, because a 32-bit shift is undefined. */
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
            (VOID) c68k_div_2by1(rhat, u[(UINT)j], m[0], &rhat);
        }
        rem[0] = rhat;
        return;
    }

    un = scratch;                       /* u_len + 1 */
    vn = un + u_len + 1u;               /* m_len     */

    /* Normalise so the divisor's top bit is set, which is what bounds the
       quotient estimate's error to at most 2. */
    s = d_clz(m[n - 1u]);
    (VOID) d_shl(vn, m, n, s);
    un[u_len] = d_shl(un, u, u_len, s);

    for (j = (INT)(u_len - n); j >= 0; j--)
    {
        c68k_limb top = un[(UINT)j + n];

        /*
         * The estimate.  top <= vn[n-1] always, because the running remainder
         * stays below the divisor; equality is the one case whose quotient is
         * B-1 and would overflow DIVU.L, so it never reaches the instruction.
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
            qhat = c68k_div_2by1(top, un[(UINT)j + n - 1u], vn[n - 1u], &rhat);
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
             * The estimate was one too large -- which normalisation makes
             * rare but not impossible.  Add the divisor back once.
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
