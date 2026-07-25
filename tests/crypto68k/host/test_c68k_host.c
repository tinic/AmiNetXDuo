/*
 * AmiNetXDuo -- crypto68k correctness gate, HOST tier.
 *
 * The same four checks as tests/crypto68k/c68k_test.c, run on the build
 * machine instead of under FS-UAE:
 *
 *   1. KNOWN ANSWERS from Python's arbitrary-precision integers, including
 *      the full RSA-2048 public and private operations, straight out of the
 *      generated c68k_vectors.h -- the same file the Amiga tier reads.
 *   2. THE LIMB PRIMITIVE against a straight-line C model, over random limb
 *      counts including 0 and 1, operands biased to 0 and 0xFFFFFFFF.
 *   3. MONTGOMERY MULTIPLY AND SQUARE against the unmodified vendored
 *      _nx_crypto_huge_number_mont().
 *   4. WHOLE EXPONENTIATIONS against
 *      _nx_crypto_huge_number_mont_power_modulus(), with exponent top-limb
 *      widths swept across the window boundaries.
 *
 * Same seed and same trial counts as the Amiga tier, so a failure here
 * reproduces there on the same inputs.
 *
 * WHY THIS FILE EXISTS INSTEAD OF JUST BUILDING c68k_test.c FOR THE HOST
 *
 *   c68k_test.c is ILP32 code by construction.  It logs through RawDoFmt(),
 *   whose contract is that every argument is longword sized -- so it passes
 *   strings as `(LONG)ptr`, which is lossless on m68k and truncating on any
 *   LP64 host.  Retargeting it would mean editing the program the emulator
 *   tier runs, to suit a machine it does not run on.  Duplicating the four
 *   checks against the one shared vector header was the smaller price.
 *
 * WHAT THIS TIER DOES NOT COVER
 *
 *   The hand-written 68020 assembly (c68k_prim.S, c68k_p256.S).  It cannot be
 *   assembled here, so the host always exercises the portable C -- which is
 *   why AMINETXDUO_CRYPTO68K_ASM defaults OFF off-target.  The assembly stays
 *   an emulator-tier test, and the two are a matched pair: this one says the
 *   algorithm is right, that one says the assembly agrees with it.
 *
 * ONE KNOWN WART
 *
 *   _nx_crypto_huge_number_mont_power_modulus() compares two pointers by
 *   casting both to ULONG, which is 32 bits by definition here and 64 on the
 *   host -- the compiler says so (-Wpointer-to-int-cast, twice).  The two
 *   pointers are always into the same small static array, so the truncation
 *   is consistent and the comparison is correct in practice; it would only
 *   break for a buffer straddling a 4 GiB boundary, which a static a few
 *   kilobytes wide does not do.  Vendored code, so it stays as it is.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crypto68k.h"

#include <stdio.h>


/* ------------------------------------------------------------- buffers --- */

#define T_MAX_LIMBS         64u             /* RSA-2048 */
#define T_POWM_SCRATCH      3000u           /* > C68K_POWM_SCRATCH_LIMBS(64, 6) */
#define T_HN_SCRATCH        2048u

static c68k_limb    t_m[T_MAX_LIMBS];
static c68k_limb    t_x[T_MAX_LIMBS];
static c68k_limb    t_y[T_MAX_LIMBS];
static c68k_limb    t_exp[T_MAX_LIMBS];
static c68k_limb    t_mine[T_MAX_LIMBS * 2 + 8];
static c68k_limb    t_work[T_MAX_LIMBS * 2 + 8];
static c68k_limb    t_scratch[T_POWM_SCRATCH];
static c68k_limb    t_hn_scratch[T_HN_SCRATCH];
static c68k_limb    t_ref_result[T_MAX_LIMBS * 2 + 8];
static c68k_limb    t_tmp[T_MAX_LIMBS * 2 + 8];

static unsigned long    t_failures;
static unsigned long    t_checks;

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
            t_x[i % T_MAX_LIMBS] = 0;            /* keep the arrays in range */
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
        m_len = (UINT)(t_rand() % 32u) + 1u;            /* 1..32 limbs */

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

int main(void)
{
    printf("AmiNetXDuo -- crypto68k correctness gate (host tier)\n");
    printf("  limb primitives: %s\n",
           c68k_using_assembly() ? "68020 assembly" : "portable C");

    t_rng_seed(0x5A17C0DEUL);

    t_known_answers();
    t_primitive();
    t_mont_differential(400u);
    t_powm_differential(150u);
    t_edge_cases();

    if (t_failures == 0)
    {
        printf("\n%lu checks, 0 failures -- PASS\n", t_checks);
        return(0);
    }

    printf("\n%lu checks, %lu failures -- FAIL\n", t_checks, t_failures);
    return(1);
}
