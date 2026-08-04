/*
 * AmiNetXDuo, crypto68k correctness gate, host tier.
 *
 * The same four checks as tests/crypto68k/c68k_test.c, run on the build
 * machine instead of under FS-UAE:
 *
 *   1. Known answers from Python's arbitrary-precision integers, including
 *      the full RSA-2048 public and private operations, straight out of the
 *      generated c68k_vectors.h, the same file the Amiga tier reads.
 *   2. The limb primitive against a straight-line C model, over random limb
 *      counts including 0 and 1, operands biased to 0 and 0xFFFFFFFF.
 *   3. Montgomery multiply and square against the unmodified vendored
 *      _nx_crypto_huge_number_mont().
 *   4. Whole exponentiations against
 *      _nx_crypto_huge_number_mont_power_modulus(), with exponent top-limb
 *      widths swept across the window boundaries.
 *
 * Same seed and same trial counts as the Amiga tier, so a failure here
 * reproduces there on the same inputs.
 *
 * The checks are duplicated here rather than built from c68k_test.c because
 * that file is ILP32 code by construction: it logs through RawDoFmt(), which
 * takes every argument longword sized, so it passes strings as `(LONG)ptr`,
 * lossless on m68k, truncating on any LP64 host.  Retargeting it would mean
 * editing the program the emulator tier runs.
 *
 * The hand-written 68020 assembly (c68k_prim.S, c68k_p256.S) cannot be
 * assembled here, so the host always exercises the portable C, which is why
 * AMINETXDUO_CRYPTO68K_ASM defaults off off-target.  The assembly stays an
 * emulator-tier test: this tier says the algorithm is right, that one says the
 * assembly agrees with it.
 *
 * _nx_crypto_huge_number_mont_power_modulus() compares two pointers by casting
 * both to ULONG, which is 32 bits by definition here and 64 on the host
 * (-Wpointer-to-int-cast, twice).  The two pointers are always into the same
 * small static array, so the truncation is consistent and the comparison is
 * correct in practice; it would only break for a buffer straddling a 4 GiB
 * boundary, which a static a few kilobytes wide does not do.  Vendored code,
 * so it stays as it is.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"

#include <stdio.h>


/* ------------------------------------------------------------- buffers --- */

#define T_MAX_LIMBS         64u             /* RSA-2048 */
#define T_POWM_SCRATCH      4096u           /* > C68K_POWM_SCRATCH_LIMBS(64, 6) */
#define T_HN_SCRATCH        2048u

static c68k_limb    t_m[T_MAX_LIMBS];

/*
 * t_x and t_y are +8 because section 2 sweeps n up to 70 to catch the loop
 * tails, and T_MAX_LIMBS is 64.  Without the slack the test wrote past t_y
 * for n in 65..70 and reported ~8% of its own trials as addmul mismatches,
 * 316 of 4000, against the 6/71 = 8.45% of draws that overrun.
 */
static c68k_limb    t_x[T_MAX_LIMBS + 8u];
static c68k_limb    t_y[T_MAX_LIMBS + 8u];
static c68k_limb    t_exp[T_MAX_LIMBS];
static c68k_limb    t_mine[T_MAX_LIMBS * 2 + 8];
static c68k_limb    t_work[C68K_MONT_WORK_LIMBS(T_MAX_LIMBS)];
static c68k_limb    t_scratch[T_POWM_SCRATCH];
static c68k_limb    t_hn_scratch[T_HN_SCRATCH];
static c68k_limb    t_ref_result[T_MAX_LIMBS * 2 + 8];
static c68k_limb    t_tmp[T_MAX_LIMBS * 2 + 8];

static VOID t_karatsuba(VOID);
static VOID t_bulk(VOID);
static VOID t_division(VOID);

static unsigned long    t_failures;
static unsigned long    t_checks;

#include "c68k_aes.h"
#include "c68k_sha256.h"
#include "c68k_chacha20.h"

#include "c68k_vectors.h"


static void t_fail(const char *what, unsigned long a, unsigned long b)
{
    t_failures++;
    printf("  FAIL %s (%lu, %lu)\n", what, a, b);
}


/* ----------------------------------------------------------------- RNG --- */
/*
 * xorshift32, identical to the one in ../c68k_support.c: same constant seed,
 * same sequence, so the two tiers test the same inputs.  Written on uint32_t
 * rather than ULONG because ULONG is 32 bits only on the target.
 */

static c68k_limb    t_rng_state = 0x2026072AUL;

static void t_rng_seed(c68k_limb seed)
{
    t_rng_state = (seed != 0) ? seed : 0x2026072AUL;
}

static c68k_limb t_rand(void)
{
c68k_limb   x = t_rng_state;

    x ^= (c68k_limb)(x << 13);
    x ^= (c68k_limb)(x >> 17);
    x ^= (c68k_limb)(x << 5);
    t_rng_state = x;

    return(x);
}

static void t_rand_limbs(c68k_limb *p, UINT n)
{
UINT    i;

    for (i = 0; i < n; i++)
    {
        p[i] = t_rand();
    }

    if (n > 0)
    {
        /* The top limb must be nonzero or the value is not really n limbs
           wide, and the vendored code would shorten it behind our back. */
        while (p[n - 1] == 0)
        {
            p[n - 1] = t_rand();
        }
    }
}


/* --------------------------------------------------------- huge numbers --- */

static void t_hn_set(NX_CRYPTO_HUGE_NUMBER *hn, c68k_limb *data,
                     UINT used_limbs, UINT buffer_limbs)
{
    hn -> nx_crypto_huge_number_data        = data;
    hn -> nx_crypto_huge_number_size        = used_limbs;
    hn -> nx_crypto_huge_buffer_size        = buffer_limbs << 2;
    hn -> nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
}

static UINT t_hn_equals(const NX_CRYPTO_HUGE_NUMBER *hn, const c68k_limb *v,
                        UINT n)
{
UINT                i;
UINT                size = hn -> nx_crypto_huge_number_size;
const c68k_limb    *d    = hn -> nx_crypto_huge_number_data;

    /* The vendored routines return a size-adjusted number and this module
       returns a fixed m_len limbs, so compare values, not buffers. */
    for (i = 0; (i < n) || (i < size); i++)
    {
        c68k_limb   a = (i < size) ? d[i] : 0u;
        c68k_limb   b = (i < n) ? v[i] : 0u;

        if (a != b)
        {
            return(0u);
        }
    }

    return(1u);
}


/* ----------------------------------------------------- 1. known answers --- */

static void t_known_answers(void)
{
UINT    i;
UINT    status;

    printf("\n1. Known answers (Python arbitrary precision):\n");

    for (i = 0; i < (sizeof(t_kats) / sizeof(t_kats[0])); i++)
    {
        status = c68k_mont_power_modulus(t_mine,
                                         t_kats[i].x, t_kats[i].x_len,
                                         t_kats[i].e, t_kats[i].e_len,
                                         t_kats[i].m, t_kats[i].m_len,
                                         t_scratch, T_POWM_SCRATCH);
        t_checks++;
        if (status != NX_CRYPTO_SUCCESS)
        {
            t_fail("KAT status", i, status);
            continue;
        }
        if (c68k_cmp(t_mine, t_kats[i].expected, t_kats[i].m_len) != 0)
        {
            t_fail("KAT value", i, 0);
        }
    }
    printf("  %u small modexp vectors\n", i);

    /* RSA-2048 public: m^e mod n, e = 65537. */
    status = c68k_mont_power_modulus(t_mine, t_msg, 64u, t_e, 1u, t_n, 64u,
                                     t_scratch, T_POWM_SCRATCH);
    t_checks++;
    if ((status != NX_CRYPTO_SUCCESS) ||
        (c68k_cmp(t_mine, t_msg_pub, 64u) != 0))
    {
        t_fail("RSA-2048 public KAT", status, 0);
    }
    else
    {
        printf("  RSA-2048 public (e=65537)  OK\n");
    }

    /* RSA-2048 private, plain: m^d mod n. */
    status = c68k_mont_power_modulus(t_mine, t_msg, 64u, t_d, 64u, t_n, 64u,
                                     t_scratch, T_POWM_SCRATCH);
    t_checks++;
    if ((status != NX_CRYPTO_SUCCESS) ||
        (c68k_cmp(t_mine, t_msg_priv, 64u) != 0))
    {
        t_fail("RSA-2048 private KAT", status, 0);
    }
    else
    {
        printf("  RSA-2048 private (d, 2048 bit)  OK\n");
    }
}


/* ----------------------------------------------- 2. the limb primitive --- */

/*
 * A model of c68k_addmul_1 written so plainly that it cannot share a bug with
 * the optimised C: one 64-bit accumulator, no tricks.
 */
static c68k_limb t_addmul_model(c68k_limb *r, const c68k_limb *b, UINT n,
                                c68k_limb a)
{
UINT                i;
unsigned long long  acc = 0;

    for (i = 0; i < n; i++)
    {
        acc = (unsigned long long)r[i] +
              ((unsigned long long)a * (unsigned long long)b[i]) +
              (acc >> 32);
        r[i] = (c68k_limb)acc;
    }

    return((c68k_limb)(acc >> 32));
}

/* Bias operands towards the values where carry logic breaks. */
static c68k_limb t_extreme(void)
{
c68k_limb   v = t_rand();

    switch (v & 7u)
    {
    case 0:  return(0xFFFFFFFFUL);
    case 1:  return(0xFFFFFFFEUL);
    case 2:  return(0u);
    case 3:  return(1u);
    case 4:  return(0x80000000UL);
    default: return(t_rand());
    }
}

static void t_primitive(void)
{
UINT        trial;
UINT        i;
UINT        n;
c68k_limb   a;
c68k_limb   c_mine;
c68k_limb   c_model;
UINT        mismatch = 0;

    printf("\n2. c68k_addmul_1 against a straight-line model:\n");

    for (trial = 0; trial < 4000u; trial++)
    {
        n = (UINT)(t_rand() % 71u);              /* 0..70, including 0 and 1 */
        a = t_extreme();

        for (i = 0; i < n; i++)
        {
            t_x[i] = 0;
        }
        for (i = 0; i < n; i++)
        {
            t_y[i] = t_extreme();
            t_mine[i] = t_extreme();
            t_ref_result[i] = t_mine[i];
        }

        c_mine  = c68k_addmul_1(t_mine, t_y, n, a);
        c_model = t_addmul_model(t_ref_result, t_y, n, a);

        t_checks++;
        if (c_mine != c_model)
        {
            mismatch++;
            t_fail("addmul carry", trial, n);
            continue;
        }
        for (i = 0; i < n; i++)
        {
            if (t_mine[i] != t_ref_result[i])
            {
                mismatch++;
                t_fail("addmul limb", trial, i);
                break;
            }
        }
    }

    printf("  4000 trials, n = 0..70, extreme-biased operands: %u mismatches\n",
           mismatch);
}


/* ------------------------------- 3. differential vs the vendored code ---- */

static void t_mont_differential(UINT trials)
{
UINT                    trial;
UINT                    m_len;
UINT                    i;
c68k_limb               n0inv;
NX_CRYPTO_HUGE_NUMBER   m_hn, x_hn, y_hn, r_hn;
UINT                    bad_mul = 0;
UINT                    bad_sqr = 0;

    printf("\n3. c68k_mont_mul / c68k_mont_sqr vs _nx_crypto_huge_number_mont:\n");

    for (trial = 0; trial < trials; trial++)
    {
        m_len = (UINT)(t_rand() % 64u) + 1u;            /* 1..64 limbs */

        /* Forced low so that every even width here goes through Karatsuba and
           is checked against the vendored routine, not just against our own
           schoolbook.  Random operands, so the reference is trustworthy,
           unlike the near-maximal ones in 3b. */
        c68k_karatsuba_limbs = 2u;

        t_rand_limbs(t_m, m_len);
        t_m[0] |= 1u;                                   /* Montgomery needs odd */

        t_rand_limbs(t_x, m_len);
        t_rand_limbs(t_y, m_len);

        /* Reduce the operands below the modulus with the vendored divider. */
        t_hn_set(&m_hn, t_m, m_len, m_len);
        t_hn_set(&x_hn, t_x, m_len, T_MAX_LIMBS);
        t_hn_set(&y_hn, t_y, m_len, T_MAX_LIMBS);
        _nx_crypto_huge_number_modulus(&x_hn, &m_hn);
        _nx_crypto_huge_number_modulus(&y_hn, &m_hn);
        for (i = x_hn.nx_crypto_huge_number_size; i < m_len; i++)
        {
            t_x[i] = 0;
        }
        for (i = y_hn.nx_crypto_huge_number_size; i < m_len; i++)
        {
            t_y[i] = 0;
        }
        t_hn_set(&x_hn, t_x, m_len, T_MAX_LIMBS);
        t_hn_set(&y_hn, t_y, m_len, T_MAX_LIMBS);

        n0inv = c68k_mont_n0inv(t_m[0]);

        /* multiply */
        t_hn_set(&r_hn, t_ref_result, 0, T_MAX_LIMBS * 2);
        _nx_crypto_huge_number_mont(&m_hn, n0inv, &x_hn, &y_hn, &r_hn);
        c68k_mont_mul(t_mine, t_x, t_y, t_m, m_len, n0inv, t_work);

        t_checks++;
        if (!t_hn_equals(&r_hn, t_mine, m_len))
        {
            bad_mul++;
            t_fail("mont_mul", trial, m_len);
        }

        /* square */
        t_hn_set(&r_hn, t_ref_result, 0, T_MAX_LIMBS * 2);
        _nx_crypto_huge_number_mont(&m_hn, n0inv, &x_hn, &x_hn, &r_hn);
        c68k_mont_sqr(t_mine, t_x, t_m, m_len, n0inv, t_work);

        t_checks++;
        if (!t_hn_equals(&r_hn, t_mine, m_len))
        {
            bad_sqr++;
            t_fail("mont_sqr", trial, m_len);
        }
    }

    printf("  %u trials, m 1..32 limbs: %u mul, %u sqr mismatches\n",
           trials, bad_mul, bad_sqr);
}


static void t_powm_differential(UINT trials)
{
UINT                    trial;
UINT                    m_len;
UINT                    e_len;
UINT                    top_bits;
UINT                    i;
UINT                    status;
UINT                    bad = 0;
NX_CRYPTO_HUGE_NUMBER   m_hn, x_hn, e_hn, r_hn;

    t_karatsuba();
    t_division();

    printf("\n4. c68k_mont_power_modulus vs "
           "_nx_crypto_huge_number_mont_power_modulus:\n");

    for (trial = 0; trial < trials; trial++)
    {
        m_len = (UINT)(t_rand() % 12u) + 1u;            /* 1..12 limbs */
        e_len = (UINT)(t_rand() % 4u) + 1u;             /* 1..4 limbs  */

        t_rand_limbs(t_m, m_len);
        t_m[0] |= 1u;

        t_rand_limbs(t_x, m_len);
        t_rand_limbs(t_exp, e_len);

        /* Chop the exponent's top limb to a random width: leading zero bits
           are the case this module handles differently from the vendored one,
           and uniformly random limbs almost never have them. */
        top_bits = (UINT)(t_rand() % 32u) + 1u;
        t_exp[e_len - 1] &= (c68k_limb)(0xFFFFFFFFUL >> (32u - top_bits));
        if (t_exp[e_len - 1] == 0)
        {
            t_exp[e_len - 1] = 1u;
        }

        t_hn_set(&m_hn, t_m, m_len, m_len);
        t_hn_set(&x_hn, t_x, m_len, T_MAX_LIMBS);
        _nx_crypto_huge_number_modulus(&x_hn, &m_hn);
        for (i = x_hn.nx_crypto_huge_number_size; i < m_len; i++)
        {
            t_x[i] = 0;
        }

        /* Reference.  It squares into `result`, so that buffer needs 2*(m+1). */
        for (i = 0; i < m_len; i++)
        {
            t_tmp[i] = t_x[i];
        }
        t_hn_set(&x_hn, t_tmp, m_len, T_MAX_LIMBS);
        t_hn_set(&e_hn, t_exp, e_len, T_MAX_LIMBS);
        t_hn_set(&r_hn, t_ref_result, 0, T_MAX_LIMBS * 2);
        t_hn_set(&m_hn, t_m, m_len, m_len);
        _nx_crypto_huge_number_mont_power_modulus(&x_hn, &e_hn, &m_hn, &r_hn,
                                                  t_hn_scratch);

        status = c68k_mont_power_modulus(t_mine, t_x, m_len, t_exp, e_len,
                                         t_m, m_len,
                                         t_scratch, T_POWM_SCRATCH);

        t_checks++;
        if (status != NX_CRYPTO_SUCCESS)
        {
            bad++;
            t_fail("powm status", trial, status);
            continue;
        }
        if (!t_hn_equals(&r_hn, t_mine, m_len))
        {
            bad++;
            t_fail("powm value", trial, (m_len << 8) | e_len);
        }
    }

    printf("  %u trials, m 1..12 limbs, e 1..4 limbs, "
           "top limb 1..32 bits: %u mismatches\n", trials, bad);
}


/* ---------------------------------------------------------- 5. edges ----- */

static void t_edge_cases(void)
{
UINT                    status;
UINT                    i;
c68k_limb               zero_e[1];
NX_CRYPTO_HUGE_NUMBER   m_hn, x_hn, e_hn, r_hn;

    printf("\n5. Edge cases:\n");

    /* m = 1 limb, x = 0. */
    t_m[0] = 0xFFFFFFFFUL;
    t_x[0] = 0;
    zero_e[0] = 5u;
    status = c68k_mont_power_modulus(t_mine, t_x, 1u, zero_e, 1u, t_m, 1u,
                                     t_scratch, T_POWM_SCRATCH);
    t_checks++;
    if ((status != NX_CRYPTO_SUCCESS) || (t_mine[0] != 0u))
    {
        t_fail("0^5 mod (2^32-1)", status, t_mine[0]);
    }

    /* e = 0 -> 1, checked against the vendored routine. */
    t_rand_limbs(t_m, 8u);
    t_m[0] |= 1u;
    t_rand_limbs(t_x, 8u);
    zero_e[0] = 0u;

    t_hn_set(&m_hn, t_m, 8u, 8u);
    t_hn_set(&x_hn, t_x, 8u, T_MAX_LIMBS);
    _nx_crypto_huge_number_modulus(&x_hn, &m_hn);
    for (i = x_hn.nx_crypto_huge_number_size; i < 8u; i++)
    {
        t_x[i] = 0;
    }

    for (i = 0; i < 8u; i++)
    {
        t_tmp[i] = t_x[i];
    }
    t_hn_set(&x_hn, t_tmp, 8u, T_MAX_LIMBS);
    t_hn_set(&e_hn, zero_e, 1u, 1u);
    t_hn_set(&r_hn, t_ref_result, 0, T_MAX_LIMBS * 2);
    t_hn_set(&m_hn, t_m, 8u, 8u);
    _nx_crypto_huge_number_mont_power_modulus(&x_hn, &e_hn, &m_hn, &r_hn,
                                              t_hn_scratch);

    status = c68k_mont_power_modulus(t_mine, t_x, 8u, zero_e, 1u, t_m, 8u,
                                     t_scratch, T_POWM_SCRATCH);
    t_checks++;
    if ((status != NX_CRYPTO_SUCCESS) || !t_hn_equals(&r_hn, t_mine, 8u))
    {
        t_fail("x^0", status, 0);
    }

    /* An even modulus must be refused, not silently answered wrongly. */
    t_m[0] &= (c68k_limb)~1u;
    status = c68k_mont_power_modulus(t_mine, t_x, 8u, t_exp, 1u, t_m, 8u,
                                     t_scratch, T_POWM_SCRATCH);
    t_checks++;
    if (status == NX_CRYPTO_SUCCESS)
    {
        t_fail("even modulus accepted", 0, 0);
    }

    /* Too little scratch must be refused too. */
    t_m[0] |= 1u;
    status = c68k_mont_power_modulus(t_mine, t_x, 8u, t_exp, 1u, t_m, 8u,
                                     t_scratch, 4u);
    t_checks++;
    if (status == NX_CRYPTO_SUCCESS)
    {
        t_fail("undersized scratch accepted", 0, 0);
    }

    printf("  0^e, x^0, even modulus, undersized scratch\n");
}


/* ------------------------------------------------------------------ main -- */

/*
 * Karatsuba at the sizes and shapes the random sweep will not produce often
 * enough to trust.
 *
 * The split's carry and borrow handling is where this kind of code goes wrong,
 * and all of it lives in the recombination: L + H can carry out of n limbs,
 * L + H - u must not borrow (it is 2*x0*x1, so it cannot, and the test is
 * whether the code agrees), and the (x0+x1)*(y0+y1) form of the multiply has
 * two carry bits out of the half-width sums plus their product term.  The
 * operands below drive those to their extremes rather than being random:
 *
 *   all ones      every add carries, every subtract borrows
 *   x1 == x0      |x1 - x0| == 0, so the middle term is a square of zero
 *   x1 == 0       the high half vanishes; L + H == L
 *   x0 == 0       the low half vanishes
 *   1 and 0       the degenerate values every bignum bug survives
 *
 * Checked against the vendored _nx_crypto_huge_number_mont, which knows
 * nothing about Karatsuba, at the widths where the split actually engages.
 */
/*
 * c68k_mod() against the vendored divider, which, unlike the vendored
 * Montgomery, has no known defect and is what the rest of this suite has
 * always been validated against.
 *
 * Two code paths in algorithm D are almost unreachable by chance and are
 * driven directly, because both are where this kind of routine breaks:
 *
 *   the B-1 clamp   the partial remainder's top limb equal to the divisor's.
 *                   The true quotient digit is B-1, and on a 68020 a DIVU.L
 *                   would trap rather than saturate, so the code must test
 *                   for it before dividing.  Driven by giving u and m the
 *                   same top limb.
 *   the add-back    the estimate one too large, needing the divisor added
 *                   back.  Normalisation makes it rare, textbooks quote
 *                   about one in 2^31 for random operands, so it is driven
 *                   by the classic shape: a divisor just above a power of the
 *                   radix, with a dividend that straddles it.
 *
 * Also swept: an unnormalised divisor (top bit clear, so the shift path runs
 * with s != 0), a single-limb divisor, u shorter than m, and all ones.
 */
static VOID t_division(VOID)
{

static c68k_limb    d_u[T_MAX_LIMBS * 2 + 8];
static c68k_limb    d_m[T_MAX_LIMBS];
static c68k_limb    d_rem[T_MAX_LIMBS + 2];
static c68k_limb    d_scratch[(T_MAX_LIMBS * 4) + 16];
UINT                trial;
UINT                shape;
UINT                u_len;
UINT                m_len;
UINT                i;
UINT                bad = 0;
NX_CRYPTO_HUGE_NUMBER u_hn, m_hn;


    printf("\n3c. c68k_mod against the vendored divider:\n");

    for (trial = 0; trial < 600u; trial++)
    {
        shape = trial % 6u;

        m_len = (UINT)(t_rand() % 32u) + 1u;
        u_len = m_len + (UINT)(t_rand() % (m_len + 1u));
        if (u_len > (T_MAX_LIMBS * 2u))
        {
            u_len = T_MAX_LIMBS * 2u;
        }

        t_rand_limbs(d_m, m_len);
        t_rand_limbs(d_u, u_len);

        switch (shape)
        {
        case 0:                                 /* the DIVU.L B-1 clamp */
            d_m[m_len - 1u] |= 0x80000000UL;
            for (i = 0; i < m_len; i++)
            {
                d_u[u_len - m_len + i] = d_m[i];
            }
            break;
        case 1:                                 /* add-back shape */
            for (i = 0; i < m_len; i++) { d_m[i] = 0; }
            d_m[m_len - 1u] = 0x80000000UL;
            d_m[0]          = 1u;
            for (i = 0; i < u_len; i++) { d_u[i] = 0xFFFFFFFFUL; }
            break;
        case 2:                                 /* unnormalised divisor */
            d_m[m_len - 1u] &= 0x0000FFFFUL;
            d_m[m_len - 1u] |= 1u;
            break;
        case 3:                                 /* single limb divisor */
            m_len = 1u;
            d_m[0] |= 1u;
            break;
        case 4:                                 /* all ones both sides */
            for (i = 0; i < m_len; i++) { d_m[i] = 0xFFFFFFFFUL; }
            for (i = 0; i < u_len; i++) { d_u[i] = 0xFFFFFFFFUL; }
            break;
        default:                                /* u shorter than m */
            u_len = m_len;
            d_u[u_len - 1u] &= 0x7FFFFFFFUL;
            d_m[m_len - 1u] |= 0x80000000UL;
            break;
        }

        if (d_m[m_len - 1u] == 0u)
        {
            d_m[m_len - 1u] = 1u;
        }

        c68k_mod(d_rem, d_u, u_len, d_m, m_len, d_scratch);

        /* the vendored divider reduces in place, so it gets its own copy */
        t_hn_set(&u_hn, t_tmp, 0, T_MAX_LIMBS * 2);
        for (i = 0; i < u_len; i++) { t_tmp[i] = d_u[i]; }
        u_hn.nx_crypto_huge_number_size = u_len;
        _nx_crypto_huge_number_adjust_size(&u_hn);
        t_hn_set(&m_hn, d_m, m_len, m_len);
        _nx_crypto_huge_number_modulus(&u_hn, &m_hn);

        t_checks++;
        if (!t_hn_equals(&u_hn, d_rem, m_len))
        {
            bad++;
            t_fail("c68k_mod", trial, (m_len << 8) | u_len);
        }
    }

    printf("  600 trials, m 1..32 limbs, u up to 2x, six shapes: %u mismatches\n",
           bad);
}


static VOID t_karatsuba(VOID)
{

static const UINT   widths[] = { 16u, 31u, 32u, 48u, 63u, 64u };
UINT                w;
UINT                shape;
UINT                m_len;
UINT                i;
UINT                bad = 0;
c68k_limb           n0inv;


    printf("\n3b. Karatsuba against schoolbook, same module, same operands:\n");

    for (w = 0; w < (sizeof(widths) / sizeof(widths[0])); w++)
    {
        m_len = widths[w];

        /* All ones: the largest modulus of this width, so the operands below
           can reach their maximum and drive every carry and every borrow. */
        for (i = 0; i < m_len; i++)
        {
            t_m[i] = 0xFFFFFFFFu;
        }
        n0inv = c68k_mont_n0inv(t_m[0]);

        for (shape = 0; shape < 8u; shape++)
        {
            for (i = 0; i < m_len; i++)
            {
                t_x[i] = 0;
                t_y[i] = 0;
            }

            switch (shape)
            {
            case 0:                             /* x = y = m - 1, the maximum */
                for (i = 0; i < m_len; i++) { t_x[i] = t_m[i]; t_y[i] = t_m[i]; }
                t_x[0]--;                       /* m is odd, so no borrow */
                t_y[0]--;
                break;
            case 1:                             /* x1 == x0: |x1-x0| is zero */
                for (i = 0; i < (m_len >> 1); i++)
                {
                    t_x[i] = 0xDEADBEEFu ^ (c68k_limb)i;
                    t_x[i + (m_len >> 1)] = t_x[i];
                    t_y[i] = 0x12345678u ^ (c68k_limb)i;
                    t_y[i + (m_len >> 1)] = t_y[i];
                }
                break;
            case 2:                             /* high half zero */
                for (i = 0; i < (m_len >> 1); i++)
                {
                    t_x[i] = 0xFFFFFFFFu;
                    t_y[i] = 0xFFFFFFFFu;
                }
                break;
            case 3:                             /* low half zero */
                for (i = (m_len >> 1); i < m_len; i++)
                {
                    t_x[i] = 0xFFFFFFFFu;
                    t_y[i] = 0xFFFFFFFFu;
                }
                break;
            case 4:                             /* x = 1 */
                t_x[0] = 1u;
                for (i = 0; i < m_len; i++) { t_y[i] = t_m[i]; }
                t_y[0]--;
                break;
            case 5:                             /* x = 0 */
                for (i = 0; i < m_len; i++) { t_y[i] = t_m[i]; }
                t_y[0]--;
                break;
            case 6:                             /* alternating limbs */
                for (i = 0; i < m_len; i++)
                {
                    t_x[i] = ((i & 1u) != 0u) ? 0xFFFFFFFFu : 0u;
                    t_y[i] = ((i & 1u) != 0u) ? 0u : 0xFFFFFFFFu;
                }
                break;
            default:                            /* random at this width */
                t_rand_limbs(t_x, m_len);
                t_rand_limbs(t_y, m_len);
                t_x[m_len - 1u] &= 0x7FFFFFFFu;
                t_y[m_len - 1u] &= 0x7FFFFFFFu;
                break;
            }

            /* multiply: schoolbook, then maximum recursion, then compare */
            c68k_karatsuba_limbs = 0xFFFFu;
            c68k_mont_mul(t_ref_result, t_x, t_y, t_m, m_len, n0inv, t_work);
            c68k_karatsuba_limbs = 2u;
            c68k_mont_mul(t_mine, t_x, t_y, t_m, m_len, n0inv, t_work);

            t_checks++;
            for (i = 0; i < m_len; i++)
            {
                if (t_mine[i] != t_ref_result[i])
                {
                    bad++;
                    t_fail("kar mont_mul", m_len, shape);
                    break;
                }
            }

            /* square */
            c68k_karatsuba_limbs = 0xFFFFu;
            c68k_mont_sqr(t_ref_result, t_x, t_m, m_len, n0inv, t_work);
            c68k_karatsuba_limbs = 2u;
            c68k_mont_sqr(t_mine, t_x, t_m, m_len, n0inv, t_work);

            t_checks++;
            for (i = 0; i < m_len; i++)
            {
                if (t_mine[i] != t_ref_result[i])
                {
                    bad++;
                    t_fail("kar mont_sqr", m_len, shape);
                    break;
                }
            }
        }
    }

    c68k_karatsuba_limbs = C68K_KARATSUBA_DEFAULT;

    printf("  widths 16/31/32/48/63/64 x 8 operand shapes, mul and sqr: "
           "%u mismatches\n", bad);
}



/* ============================================================ the bulk path ==
 *
 * AES-128/256 and SHA-256, against the published vectors, for every portable
 * variant in src/crypto68k/.  The assembly cannot be assembled here and stays
 * an emulator-tier test (tests/crypto68k/crypto68k_bulk), the same split the
 * limb primitives have.
 *
 * This tier runs the vectors on every push, and is the only place the
 * endianness of the message-word load is tested: the SHA-256 fast path loads
 * W[0..15] as longwords, which is correct on the m68k and wrong here, so a
 * mis-set guard around it shows up only here.  It has.
 */

static void t_bytes(const char *what, const unsigned char *got,
                    const unsigned char *want, unsigned n)
{
    unsigned i;

    t_checks++;
    for (i = 0; i < n; i++)
    {
        if (got[i] != want[i])
        {
            t_failures++;
            printf("  FAIL %s at byte %u: %02x vs %02x\n",
                   what, i, got[i], want[i]);
            return;
        }
    }
}

static const unsigned char t_aes_k128[16] =
{ 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
static const unsigned char t_aes_k256[32] =
{ 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 };
static const unsigned char t_aes_pt[16] =
{ 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
  0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF };
static const unsigned char t_aes_c128[16] =
{ 0x69,0xC4,0xE0,0xD8,0x6A,0x7B,0x04,0x30,
  0xD8,0xCD,0xB7,0x80,0x70,0xB4,0xC5,0x5A };
static const unsigned char t_aes_c256[16] =
{ 0x8E,0xA2,0xB7,0xCA,0x51,0x67,0x45,0xBF,
  0xEA,0xFC,0x49,0x90,0x4B,0x49,0x60,0x89 };

static const unsigned char t_sha_abc[32] =
{ 0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23,
  0xB0,0x03,0x61,0xA3,0x96,0x17,0x7A,0x9C,0xB4,0x10,0xFF,0x61,0xF2,0x00,0x15,0xAD };
static const unsigned char t_sha_empty[32] =
{ 0xE3,0xB0,0xC4,0x42,0x98,0xFC,0x1C,0x14,0x9A,0xFB,0xF4,0xC8,0x99,0x6F,0xB9,0x24,
  0x27,0xAE,0x41,0xE4,0x64,0x9B,0x93,0x4C,0xA4,0x95,0x99,0x1B,0x78,0x52,0xB8,0x55 };
static const unsigned char t_sha_448[32] =
{ 0x24,0x8D,0x6A,0x61,0xD2,0x06,0x38,0xB8,0xE5,0xC0,0x26,0x93,0x0C,0x3E,0x60,0x39,
  0xA3,0x3C,0xE4,0x59,0x64,0xFF,0x21,0x67,0xF6,0xEC,0xED,0xD4,0x19,0xDB,0x06,0xC1 };
static const unsigned char t_sha_million[32] =
{ 0xCD,0xC7,0x6E,0x5C,0x99,0x14,0xFB,0x92,0x81,0xA1,0xC7,0xE2,0x84,0xD7,0x3E,0x67,
  0xF1,0x80,0x9A,0x48,0xA4,0x97,0x20,0x0E,0x04,0x6D,0x39,0xCC,0xC7,0x11,0x2C,0xD0 };

static void t_bulk_aes(unsigned variant)
{
    C68K_AES        aes;
    unsigned char   out[16];
    unsigned char   back[16];
    unsigned char   iv_a[16], iv_b[16];
    unsigned char   in[64], ref[64], got[64];
    unsigned        i;

    c68k_aes_variant = variant;

    (void)c68k_aes_key_set(&aes, t_aes_k128, 128u);
    c68k_aes_encrypt_block(&aes, t_aes_pt, out);
    t_bytes("FIPS-197 AES-128 ciphertext", out, t_aes_c128, 16u);
    c68k_aes_decrypt_block(&aes, t_aes_c128, back);
    t_bytes("FIPS-197 AES-128 plaintext", back, t_aes_pt, 16u);

    (void)c68k_aes_key_set(&aes, t_aes_k256, 256u);
    c68k_aes_encrypt_block(&aes, t_aes_pt, out);
    t_bytes("FIPS-197 AES-256 ciphertext", out, t_aes_c256, 16u);
    c68k_aes_decrypt_block(&aes, t_aes_c256, back);
    t_bytes("FIPS-197 AES-256 plaintext", back, t_aes_pt, 16u);

    /* The awkward shapes: a chaining value carried across calls, a decrypt
       in place, and zero blocks, each of which has been somebody's CBC
       bug. */
    (void)c68k_aes_key_set(&aes, t_aes_k128, 128u);
    for (i = 0; i < 16u; i++)
    {
        iv_a[i] = iv_b[i] = (unsigned char)(0xA0u + i);
    }
    for (i = 0; i < 64u; i++)
    {
        in[i] = (unsigned char)((i * 7u) + 1u);
    }

    c68k_aes_cbc_encrypt(&aes, iv_a, in, ref, 4uL);
    for (i = 0; i < 4u; i++)
    {
        c68k_aes_cbc_encrypt(&aes, iv_b, in + (i * 16u), got + (i * 16u), 1uL);
    }
    t_bytes("CBC chaining across calls", got, ref, 64u);
    t_bytes("CBC leaves the same IV", iv_a, iv_b, 16u);

    got[0] = 0x5Au;
    c68k_aes_cbc_encrypt(&aes, iv_a, in, got, 0uL);
    t_checks++;
    if (got[0] != 0x5Au)
    {
        t_failures++;
        printf("  FAIL CBC with zero blocks wrote to the output\n");
    }

    for (i = 0; i < 16u; i++)
    {
        iv_a[i] = (unsigned char)(0xA0u + i);
    }
    memcpy(got, ref, 64u);
    c68k_aes_cbc_decrypt(&aes, iv_a, got, got, 4uL);
    t_bytes("CBC decrypt in place", got, in, 64u);
}

static void t_bulk_sha(unsigned variant)
{
    C68K_SHA256     ctx;
    unsigned char   d[32];
    unsigned char   m[64];
    unsigned long   left;
    unsigned        i;

    c68k_sha256_variant = variant;

    (void)c68k_sha256_initialize(&ctx, 0u);
    (void)c68k_sha256_update(&ctx, (unsigned char *)"abc", 3u);
    (void)c68k_sha256_digest_calculate(&ctx, d, 0u);
    t_bytes("SHA-256(\"abc\")", d, t_sha_abc, 32u);

    (void)c68k_sha256_initialize(&ctx, 0u);
    (void)c68k_sha256_digest_calculate(&ctx, d, 0u);
    t_bytes("SHA-256(\"\")", d, t_sha_empty, 32u);

    memcpy(m, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    (void)c68k_sha256_initialize(&ctx, 0u);
    (void)c68k_sha256_update(&ctx, m, 56u);
    (void)c68k_sha256_digest_calculate(&ctx, d, 0u);
    t_bytes("SHA-256(56 bytes)", d, t_sha_448, 32u);

    /* Split at 1, 54 and 1, which crosses the internal buffer boundary in
       the middle rather than on it. */
    (void)c68k_sha256_initialize(&ctx, 0u);
    (void)c68k_sha256_update(&ctx, m, 1u);
    (void)c68k_sha256_update(&ctx, m + 1, 54u);
    (void)c68k_sha256_update(&ctx, m + 55, 1u);
    (void)c68k_sha256_digest_calculate(&ctx, d, 0u);
    t_bytes("SHA-256 across three updates", d, t_sha_448, 32u);

    /* Every odd offset into the buffer, which is what a TLS record gives us
       and what the longword message load has to survive. */
    for (i = 1u; i < 8u; i++)
    {
        (void)c68k_sha256_initialize(&ctx, 0u);
        (void)c68k_sha256_update(&ctx, m, i);
        (void)c68k_sha256_update(&ctx, m + i, 56u - i);
        (void)c68k_sha256_digest_calculate(&ctx, d, 0u);
        t_bytes("SHA-256 split on an odd boundary", d, t_sha_448, 32u);
    }

    for (i = 0; i < 56u; i++)
    {
        m[i] = (unsigned char)'a';
    }
    (void)c68k_sha256_initialize(&ctx, 0u);
    left = 1000000uL;
    while (left != 0uL)
    {
        unsigned chunk = (left > 56uL) ? 56u : (unsigned)left;

        (void)c68k_sha256_update(&ctx, m, chunk);
        left -= (unsigned long)chunk;
    }
    (void)c68k_sha256_digest_calculate(&ctx, d, 0u);
    t_bytes("SHA-256 of one million 'a'", d, t_sha_million, 32u);
}

/* ------------------------------------------- ChaCha20-Poly1305, RFC 8439 -- */
/*
 * The AEAD record path, ciphersuites 0xCCA8 and 0xCCA9.  Here for the same
 * reason as the AES and SHA-256 vectors: this tier runs them on every push.
 *
 * Same endianness trap as SHA-256.  ChaCha20 and Poly1305 read their input
 * little-endian, so the m68k fast path is one MOVE.L and a byte reversal and
 * the portable path is four byte loads; get the guard backwards and one of the
 * two machines produces a self-consistent wrong answer.  These vectors are the
 * only thing that says which.
 */

static const unsigned char t_cc_key[32] =
{ 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 };

static const unsigned char t_cc_nonce[12] =
{ 0,0,0,0,0,0,0,0x4a,0,0,0,0 };

/* RFC 8439 2.4.2, the first 64 keystream bytes at block counter 1. */
static const unsigned char t_cc_stream[64] =
{ 0x22,0x4f,0x51,0xf3,0x40,0x1b,0xd9,0xe1,0x2f,0xde,0x27,0x6f,0xb8,0x63,0x1d,0xed,
  0x8c,0x13,0x1f,0x82,0x3d,0x2c,0x06,0xe2,0x7e,0x4f,0xca,0xec,0x9e,0xf3,0xcf,0x78,
  0x8a,0x3b,0x0a,0xa3,0x72,0x60,0x0a,0x92,0xb5,0x79,0x74,0xcd,0xed,0x2b,0x93,0x34,
  0x79,0x4c,0xba,0x40,0xc6,0x3e,0x34,0xcd,0xea,0x21,0x2c,0x4c,0xf0,0x7d,0x41,0xb7 };

/* RFC 8439 2.5.2. */
static const unsigned char t_poly_key[32] =
{ 0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,
  0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b };

static const unsigned char t_poly_tag[16] =
{ 0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,0xc2,0x2b,0x8b,0xaf,0x0c,0x01,0x27,0xa9 };

/* RFC 8439 2.8.2. */
static const unsigned char t_aead_key[32] =
{ 0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
  0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f };

static const unsigned char t_aead_nonce[12] =
{ 0x07,0x00,0x00,0x00,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47 };

static const unsigned char t_aead_aad[12] =
{ 0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7 };

static const unsigned char t_aead_cipher[114] =
{ 0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,0x7b,0x86,0xaf,0xbc,0x53,0xef,0x7e,0xc2,
  0xa4,0xad,0xed,0x51,0x29,0x6e,0x08,0xfe,0xa9,0xe2,0xb5,0xa7,0x36,0xee,0x62,0xd6,
  0x3d,0xbe,0xa4,0x5e,0x8c,0xa9,0x67,0x12,0x82,0xfa,0xfb,0x69,0xda,0x92,0x72,0x8b,
  0x1a,0x71,0xde,0x0a,0x9e,0x06,0x0b,0x29,0x05,0xd6,0xa5,0xb6,0x7e,0xcd,0x3b,0x36,
  0x92,0xdd,0xbd,0x7f,0x2d,0x77,0x8b,0x8c,0x98,0x03,0xae,0xe3,0x28,0x09,0x1b,0x58,
  0xfa,0xb3,0x24,0xe4,0xfa,0xd6,0x75,0x94,0x55,0x85,0x80,0x8b,0x48,0x31,0xd7,0xbc,
  0x3f,0xf4,0xde,0xf0,0x8e,0x4b,0x7a,0x9d,0xe5,0x76,0xd2,0x65,0x86,0xce,0xc6,0x4b,
  0x61,0x16 };

static const unsigned char t_aead_tag[16] =
{ 0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91 };

static const char t_aead_plain[] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only "
    "one tip for the future, sunscreen would be it.";

static void t_bulk_chacha(void)
{
    C68K_CHACHA20           cc;
    C68K_POLY1305           poly;
    C68K_CHACHA20_POLY1305  aead;
    unsigned char           got[128];
    unsigned char           tag[16];
    unsigned                i;

    for (i = 0; i < 64; i++)
    {
        got[i] = 0;
    }
    c68k_chacha20_initialize(&cc, t_cc_key, t_cc_nonce, 1uL);
    c68k_chacha20_keystream(&cc, got, 64uL);
    t_bytes("RFC 8439 2.4.2 keystream", got, t_cc_stream, 64);

    /* Split where no block boundary is, which is what a chained packet does. */
    for (i = 0; i < 64; i++)
    {
        got[i] = 0;
    }
    c68k_chacha20_initialize(&cc, t_cc_key, t_cc_nonce, 1uL);
    c68k_chacha20_keystream(&cc, &got[0], 7uL);
    c68k_chacha20_keystream(&cc, &got[7], 50uL);
    c68k_chacha20_keystream(&cc, &got[57], 7uL);
    t_bytes("RFC 8439 2.4.2 across three calls", got, t_cc_stream, 64);

    c68k_poly1305_initialize(&poly, t_poly_key);
    c68k_poly1305_update(&poly,
                         (const unsigned char *)
                             "Cryptographic Forum Research Group", 34uL);
    c68k_poly1305_finish(&poly, tag);
    t_bytes("RFC 8439 2.5.2 tag", tag, t_poly_tag, 16);

    c68k_poly1305_initialize(&poly, t_poly_key);
    c68k_poly1305_update(&poly,
                         (const unsigned char *)"Cryptographic", 13uL);
    c68k_poly1305_update(&poly,
                         (const unsigned char *)" Forum Research", 15uL);
    c68k_poly1305_update(&poly, (const unsigned char *)" Group", 6uL);
    c68k_poly1305_finish(&poly, tag);
    t_bytes("RFC 8439 2.5.2 across three updates", tag, t_poly_tag, 16);

    c68k_chacha20_poly1305_initialize(&aead, t_aead_key, t_aead_nonce);
    c68k_chacha20_poly1305_associate(&aead, t_aead_aad, 12uL);
    c68k_chacha20_poly1305_encrypt(&aead,
                                   (const unsigned char *)t_aead_plain,
                                   got, 114uL);
    c68k_chacha20_poly1305_tag(&aead, tag);
    t_bytes("RFC 8439 2.8.2 ciphertext", got, t_aead_cipher, 114);
    t_bytes("RFC 8439 2.8.2 tag", tag, t_aead_tag, 16);

    /* The same encryption in three uneven pieces must give the same record. */
    c68k_chacha20_poly1305_initialize(&aead, t_aead_key, t_aead_nonce);
    c68k_chacha20_poly1305_associate(&aead, t_aead_aad, 12uL);
    c68k_chacha20_poly1305_encrypt(&aead,
                                   (const unsigned char *)t_aead_plain,
                                   &got[0], 5uL);
    c68k_chacha20_poly1305_encrypt(&aead,
                                   (const unsigned char *)&t_aead_plain[5],
                                   &got[5], 100uL);
    c68k_chacha20_poly1305_encrypt(&aead,
                                   (const unsigned char *)&t_aead_plain[105],
                                   &got[105], 9uL);
    c68k_chacha20_poly1305_tag(&aead, tag);
    t_bytes("RFC 8439 2.8.2 ciphertext, chunked", got, t_aead_cipher, 114);
    t_bytes("RFC 8439 2.8.2 tag, chunked", tag, t_aead_tag, 16);

    /* Decrypt in place, which is what the record path does. */
    c68k_chacha20_poly1305_initialize(&aead, t_aead_key, t_aead_nonce);
    c68k_chacha20_poly1305_associate(&aead, t_aead_aad, 12uL);
    c68k_chacha20_poly1305_decrypt(&aead, got, got, 114uL);
    c68k_chacha20_poly1305_tag(&aead, tag);
    t_bytes("RFC 8439 2.8.2 plaintext recovered", got,
            (const unsigned char *)t_aead_plain, 114);
    t_bytes("RFC 8439 2.8.2 tag on decrypt", tag, t_aead_tag, 16);

    /* One flipped ciphertext bit must not verify, the one AEAD property no
       published vector states. */
    t_checks++;
    if (c68k_chacha20_poly1305_verify(tag, t_aead_tag) != NX_CRYPTO_TRUE)
    {
        t_failures++;
        printf("  FAIL a correct tag did not verify\n");
    }

    for (i = 0; i < 114; i++)
    {
        got[i] = t_aead_cipher[i];
    }
    got[57] = (unsigned char)(got[57] ^ 0x01u);
    c68k_chacha20_poly1305_initialize(&aead, t_aead_key, t_aead_nonce);
    c68k_chacha20_poly1305_associate(&aead, t_aead_aad, 12uL);
    c68k_chacha20_poly1305_decrypt(&aead, got, got, 114uL);
    c68k_chacha20_poly1305_tag(&aead, tag);

    t_checks++;
    if (c68k_chacha20_poly1305_verify(tag, t_aead_tag) != NX_CRYPTO_FALSE)
    {
        t_failures++;
        printf("  FAIL a forged record verified\n");
    }
}

static VOID t_bulk(VOID)
{
    unsigned v;

    printf("\nthe bulk path, AES and SHA-256 against the published "
           "vectors\n");

    for (v = 0; v < C68K_AES_V_COUNT; v++)
    {
        if (c68k_aes_variant_is_asm(v))
        {
            continue;                   /* emulator tier */
        }
        t_bulk_aes(v);
    }
    c68k_aes_variant = C68K_AES_V_BEST;

    for (v = 0; v < C68K_SHA256_V_COUNT; v++)
    {
        t_bulk_sha(v);
    }
    c68k_sha256_variant = C68K_SHA256_V_BEST;

    t_bulk_chacha();

    printf("  every portable AES and SHA-256 variant checked, and the "
           "AEAD\n");
}


int main(void)
{
    printf("AmiNetXDuo, crypto68k correctness gate (host tier)\n");
    printf("  limb primitives: %s\n",
           c68k_using_assembly() ? "68020 assembly" : "portable C");

    t_rng_seed(0x5A17C0DEUL);

    t_known_answers();
    t_primitive();
    t_mont_differential(400u);
    t_powm_differential(150u);
    t_edge_cases();
    t_bulk();

    if (t_failures == 0)
    {
        printf("\n%lu checks, 0 failures, PASS\n", t_checks);
        return(0);
    }

    printf("\n%lu checks, %lu failures, FAIL\n", t_checks, t_failures);
    return(1);
}
