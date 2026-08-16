/*
 * AmiNetXDuo, crypto68k modular exponentiation.
 *
 *   The vendored _nx_crypto_huge_number_mont_power_modulus() is bit-at-a-time
 *   square-and-multiply over every bit of every exponent limb.  Two
 *   consequences, both fixed here.
 *
 *   1. It never skips the leading zero bits of the top exponent limb.  For
 *      e = 65537 the exponent is one limb, 0x00010001, so it performs 32
 *      squarings and 2 multiplies where 16 squarings and 1 multiply suffice.
 *      An RSA public operation, the certificate signature check a TLS
 *      client does three times per handshake, therefore costs about twice
 *      what it should.  The largest gain in the module, and not an assembly
 *      change.
 *
 *   2. For a full-length private exponent it does one multiply per set bit,
 *      about 1024 of them for RSA-2048.  Sliding window with w bits needs
 *      2^(w-1) + (b-w)/(w+1) instead.  At b = 1024, w = 6 that is 32 + 145
 *      against 512.  (Formula from the bn_local.h of OpenSSL, which is also
 *      where the window thresholds below come from.)
 *
 *   Squarings are then most of the work, which is what makes the ~24% of
 *   c68k_mont_sqr() over c68k_mont_mul() matter.
 *
 *   The setup, radix^2 mod m, still goes through the vendored long division.
 *   It is O(s^2) and runs once, so a replacement gains little.  It also keeps
 *   the before/after numbers different only in the part under optimisation,
 *   and leaves one less division routine to get subtly wrong.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"


/* --------------------------------------------------------------- helpers, */

static VOID c68k_zero(c68k_limb *p, UINT n)
{

UINT    i;


    for (i = 0; i < n; i++)
    {
        p[i] = 0;
    }
}

static VOID c68k_copy(c68k_limb *dst, const c68k_limb *src, UINT n)
{

UINT    i;


    for (i = 0; i < n; i++)
    {
        dst[i] = src[i];
    }
}

/* Bit i of a little-endian limb array. */
static UINT c68k_bit(const c68k_limb *e, UINT i)
{

    return((UINT)((e[i >> 5] >> (i & 31u)) & 1u));
}

/* Number of significant bits, 0 for a zero value. */
static UINT c68k_bitlen(const c68k_limb *e, UINT e_len)
{

UINT        i;
UINT        bits;
c68k_limb   top;


    while ((e_len > 0) && (e[e_len - 1] == 0))
    {
        e_len--;
    }

    if (e_len == 0)
    {
        return(0);
    }

    top  = e[e_len - 1];
    bits = (e_len - 1) << 5;

    for (i = 0; i < 32u; i++)
    {
        if (top == 0)
        {
            break;
        }
        top >>= 1;
        bits++;
    }

    return(bits);
}

/*
 * BN_window_bits_for_exponent_size from OpenSSL, the standard crossover table.
 * These are the exact algebraic crossovers of 2^(w-1) + (b-w)/(w+1).
 */
static UINT c68k_window_for(UINT bits)
{

    if (bits > 671u)
    {
        return(6u);
    }
    if (bits > 239u)
    {
        return(5u);
    }
    if (bits > 79u)
    {
        return(4u);
    }
    if (bits > 23u)
    {
        return(3u);
    }
    return(1u);
}


/* ---------------------------------------------------- radix^2 mod m setup, */

/*
 * rr = radix^(2*m_len) mod m, that is R^2 mod m.
 *
 * Done the way the vendored routine does it: reduce R, square the remainder,
 * reduce again.  setup needs 3*m_len + 4 limbs.
 */
VOID c68k_mont_setup_rr(c68k_limb *rr, const c68k_limb *m, UINT m_len,
                        c68k_limb *setup)
{

NX_CRYPTO_HUGE_NUMBER   m_hn;
NX_CRYPTO_HUGE_NUMBER   temp_hn;
NX_CRYPTO_HUGE_NUMBER   sq_hn;
c68k_limb              *temp = setup;
c68k_limb              *sq   = setup + (m_len + 1);
UINT                    i;


    m_hn.nx_crypto_huge_number_data        = (HN_UBASE *)m;
    m_hn.nx_crypto_huge_number_size        = m_len;
    m_hn.nx_crypto_huge_buffer_size        = m_len << 2;
    m_hn.nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
    _nx_crypto_huge_number_adjust_size(&m_hn);

    /* temp = R = radix^m_len, then temp = R mod m. */
    c68k_zero(temp, m_len);
    temp[m_len] = 1;
    temp_hn.nx_crypto_huge_number_data        = temp;
    temp_hn.nx_crypto_huge_number_size        = m_len + 1;
    temp_hn.nx_crypto_huge_buffer_size        = (m_len + 1) << 2;
    temp_hn.nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;

    if (c68k_fast_modulus != 0u)
    {
        /*
         * R mod m and then (R mod m)^2 mod m, both through the 32-bit divider
         * of this module.  That is all the fast path changes.  The square in
         * between is the vendored one either way: 2080 limb products against
         * the tens of thousands of limb operations the two reductions cost.
         */
        c68k_mod(temp, temp, m_len + 1u, m, m_len, &setup[m_len + 1u]);

        temp_hn.nx_crypto_huge_number_size = m_len;
        _nx_crypto_huge_number_adjust_size(&temp_hn);

        sq_hn.nx_crypto_huge_number_data        = sq;
        sq_hn.nx_crypto_huge_number_size        = 0;
        sq_hn.nx_crypto_huge_buffer_size        = ((m_len << 1) + 2) << 2;
        sq_hn.nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
        _nx_crypto_huge_number_square(&temp_hn, &sq_hn);

        for (i = sq_hn.nx_crypto_huge_number_size; i < (m_len << 1); i++)
        {
            sq[i] = 0;
        }

        c68k_mod(rr, sq, m_len << 1, m, m_len, &setup[m_len + 1u]);
        return;
    }

    _nx_crypto_huge_number_modulus(&temp_hn, &m_hn);

    /* sq = temp^2, then sq = sq mod m == R^2 mod m. */
    sq_hn.nx_crypto_huge_number_data        = sq;
    sq_hn.nx_crypto_huge_number_size        = 0;
    sq_hn.nx_crypto_huge_buffer_size        = ((m_len << 1) + 2) << 2;
    sq_hn.nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
    _nx_crypto_huge_number_square(&temp_hn, &sq_hn);
    _nx_crypto_huge_number_modulus(&sq_hn, &m_hn);

    c68k_zero(rr, m_len);
    for (i = 0; (i < m_len) && (i < sq_hn.nx_crypto_huge_number_size); i++)
    {
        rr[i] = sq[i];
    }
}


/* -------------------------------------------------------- exponentiation, */

UINT c68k_mont_power_modulus(c68k_limb *result,
                             const c68k_limb *x, UINT x_len,
                             const c68k_limb *e, UINT e_len,
                             const c68k_limb *m, UINT m_len,
                             c68k_limb *scratch, UINT scratch_limbs)
{

c68k_limb  *xm;
c68k_limb  *acc;
c68k_limb  *one;
c68k_limb  *rr;
c68k_limb  *xpad;
c68k_limb  *work;
c68k_limb  *setup;
c68k_limb  *table;
c68k_limb   n0inv;
UINT        w;
UINT        table_size;
UINT        bits;
UINT        i;
INT         pos;
INT         low;
UINT        window_value;
UINT        window_len;
UINT        started;


    if ((m_len == 0) || ((m[0] & 1u) == 0) || (x_len > m_len))
    {
        return(NX_CRYPTO_NOT_SUCCESSFUL);
    }

    /* Pick the largest window this scratch buffer can hold. */
    bits = c68k_bitlen(e, e_len);
    w    = c68k_window_for(bits);
    if (w > C68K_POWM_MAX_WINDOW)
    {
        w = C68K_POWM_MAX_WINDOW;
    }
    while ((w > 1u) && (C68K_POWM_SCRATCH_LIMBS(m_len, w) > scratch_limbs))
    {
        w--;
    }
    if (C68K_POWM_SCRATCH_LIMBS(m_len, 1u) > scratch_limbs)
    {
        return(NX_CRYPTO_SIZE_ERROR);
    }

    table_size = 1u << (w - 1u);

    xm    = scratch;
    acc   = xm    + m_len;
    one   = acc   + m_len;
    rr    = one   + m_len;
    xpad  = rr    + m_len;
    work  = xpad  + m_len;                  /* 8*m_len + 2 */
    setup = work  + C68K_MONT_WORK_LIMBS(m_len);    /* 7*m_len + 8 */
    table = setup + (7u * m_len) + 8u;      /* table_size * m_len */

    n0inv = c68k_mont_n0inv(m[0]);

    c68k_mont_setup_rr(rr, m, m_len, setup);

    /* x, zero padded to the modulus width. */
    c68k_zero(xpad, m_len);
    c68k_copy(xpad, x, x_len);

    /* one == 1, used both as the Montgomery-one source and to leave the domain. */
    c68k_zero(one, m_len);
    one[0] = 1;

    /* xm = x * R mod m. */
    c68k_mont_mul(xm, xpad, rr, m, m_len, n0inv, work);

    if (bits == 0)
    {
        /* x^0 == 1. */
        c68k_copy(result, one, m_len);
        return(NX_CRYPTO_SUCCESS);
    }

    /*
     * Odd-power table: table[k] = xm^(2k+1).  Built with one squaring plus
     * table_size-1 multiplies, which is the whole precomputation cost.
     */
    c68k_copy(table, xm, m_len);
    if (table_size > 1u)
    {
        /* acc doubles as the running xm^2 here; it is reset before the scan. */
        c68k_mont_sqr(acc, xm, m, m_len, n0inv, work);
        for (i = 1; i < table_size; i++)
        {
            C68K_YIELD();
            c68k_mont_mul(&table[i * m_len], &table[(i - 1u) * m_len], acc,
                          m, m_len, n0inv, work);
        }
    }

    /*
     * Left-to-right sliding window.  `started` stays false until the first set
     * bit, so the leading zeros of the top exponent limb cost nothing, the
     * change that halves the RSA public operation.
     */
    started = 0;
    pos     = (INT)bits - 1;

    while (pos >= 0)
    {
        C68K_YIELD();

        if (c68k_bit(e, (UINT)pos) == 0)
        {
            if (started != 0)
            {
                c68k_mont_sqr(acc, acc, m, m_len, n0inv, work);
            }
            pos--;
            continue;
        }

        /*
         * Longest window ending at a set bit, at most w bits wide.  The value
         * is therefore odd, so only odd powers are tabulated.
         */
        low = pos - (INT)(w - 1u);
        if (low < 0)
        {
            low = 0;
        }
        while (c68k_bit(e, (UINT)low) == 0)
        {
            low++;
        }

        window_len   = (UINT)(pos - low + 1);
        window_value = 0;
        for (i = 0; i < window_len; i++)
        {
            window_value = (window_value << 1) |
                           c68k_bit(e, (UINT)pos - i);
        }

        if (started == 0)
        {
            c68k_copy(acc, &table[((window_value - 1u) >> 1) * m_len], m_len);
            started = 1;
        }
        else
        {
            for (i = 0; i < window_len; i++)
            {
                c68k_mont_sqr(acc, acc, m, m_len, n0inv, work);
            }
            c68k_mont_mul(acc, acc, &table[((window_value - 1u) >> 1) * m_len],
                          m, m_len, n0inv, work);
        }

        pos = low - 1;
    }

    /* Leave the Montgomery domain: mont(acc, 1) == acc * R^-1 == x^e mod m. */
    c68k_mont_mul(result, acc, one, m, m_len, n0inv, work);

    return(NX_CRYPTO_SUCCESS);
}


/* ------------------------------------------------- nx_crypto-shaped entry, */

VOID c68k_huge_number_mont_power_modulus(NX_CRYPTO_HUGE_NUMBER *x,
                                         NX_CRYPTO_HUGE_NUMBER *e,
                                         NX_CRYPTO_HUGE_NUMBER *m,
                                         NX_CRYPTO_HUGE_NUMBER *result,
                                         HN_UBASE *scratch,
                                         UINT scratch_limbs)
{

UINT    m_len;
UINT    status;


    _nx_crypto_huge_number_adjust_size(x);
    _nx_crypto_huge_number_adjust_size(e);
    _nx_crypto_huge_number_adjust_size(m);

    m_len = m -> nx_crypto_huge_number_size;

    status = c68k_mont_power_modulus(result -> nx_crypto_huge_number_data,
                                     x -> nx_crypto_huge_number_data,
                                     x -> nx_crypto_huge_number_size,
                                     e -> nx_crypto_huge_number_data,
                                     e -> nx_crypto_huge_number_size,
                                     m -> nx_crypto_huge_number_data,
                                     m_len,
                                     scratch, scratch_limbs);

    if (status != NX_CRYPTO_SUCCESS)
    {
        /*
         * Unrecoverable here: the caller gave a buffer too small or a modulus
         * this module cannot use (even).  Fall back to the vendored routine
         * rather than returning a wrong answer.
         */
        _nx_crypto_huge_number_mont_power_modulus(x, e, m, result, scratch);
        return;
    }

    result -> nx_crypto_huge_number_size        = m_len;
    result -> nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
    _nx_crypto_huge_number_adjust_size(result);
}
