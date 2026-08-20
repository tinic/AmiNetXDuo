/*
 * AmiNetXDuo, crypto68k long division, 32-bit limbs.
 *
 *   Measured: on an RSA-2048 public operation through crypto68k, 36.6 ms of
 *   163.9 went into one routine, the R^2 mod m setup in c68k_powm.c, which
 *   reduces through the vendored _nx_crypto_huge_number_modulus().  AmiSSL's
 *   equivalent is 15.9 ms.  With the exponentiation itself level at 0.997x
 *   after Karatsuba, that setup was 98% of the remaining gap to OpenSSL on
 *   that operation.
 *
 *   The vendored divider is a correct implementation of the right algorithm,
 *   traditional long division with a two-digit quotient estimate, done in
 *   16-bit half-limbs.  Its own declarations say so ("In number of USHORT
 *   words") and it takes its estimate from `>> (HN_SHIFT >> 1)`.  A halved
 *   digit size doubles the number of quotient digits and doubles the length of
 *   the multiply-subtract pass under each one, so it is about four times the
 *   inner work of the same algorithm over 32-bit limbs.  OpenSSL gets its
 *   quotient digit from bn_div_words, which on this target is one DIVU.L out
 *   of the bn_m68k.s of Howard Chu.  This file is the same algorithm at the
 *   word size of the machine, not a better one.
 *
 *   Rejected: a cache of R^2 against the modulus, which avoids the division
 *   rather than makes it faster.  It gains nothing for a TLS client.  The
 *   three RSA public operations in a handshake check the leaf with the key of
 *   the intermediate, the intermediate with the key of the root, and the
 *   ServerKeyExchange with the key of the leaf, three different moduli, so a
 *   cache keyed on the modulus never hits inside a handshake.  Across
 *   handshakes to one host, session resumption (docs/RESEARCH.md 13) does no
 *   public-key work, so the cache is not consulted there either.
 *
 *   Also rejected: R^2 mod m by repeated modular doubling from R mod m is 2048
 *   shift-and-subtract passes over 64 limbs, and by a Montgomery-squaring
 *   ladder is eleven Montgomery squares.  Priced at the measured figures of
 *   this module, those are ~42 ms and ~72 ms against the 36.6 ms in question.
 *   Division is the right primitive, in 32-bit digits.
 *
 *   The one place DIVU.L can trap: algorithm D's quotient estimate divides a
 *   two-limb prefix of the partial remainder by the top limb of the divisor,
 *   and the result fits in one limb except when those two top limbs are equal,
 *   where the true quotient digit is B-1.  A 68020 DIVU.L traps on that
 *   overflow rather than saturating, so the case is tested for and never
 *   reaches the instruction.
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
