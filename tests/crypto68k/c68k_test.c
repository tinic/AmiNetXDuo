/*
 * AmiNetXDuo, crypto68k correctness gate.
 *
 * Bignum bugs are input dependent: a carry that only propagates when a limb is
 * 0xFFFFFFFF, a modulus whose top limb is small, an exponent whose leading
 * bits are zero.  A handful of test vectors does not find those.  So this
 * program checks three things, in increasing order of how much they can hide:
 *
 *   1. Known answers from Python's arbitrary-precision integers
 *      (tests/crypto68k/gen_vectors.py).  Python shares no ancestry with
 *      either nx_crypto or crypto68k, so agreement is independent evidence
 *      rather than two implementations making the same mistake.  Includes the
 *      full RSA-2048 public and private operations.
 *
 *   2. The limb primitive against a straight-line C model, over random limb
 *      counts including 0 and 1, and with operands biased towards 0 and
 *      0xFFFFFFFF where the carry logic breaks.
 *
 *   3. The whole thing against the unmodified vendored implementation, over
 *      random moduli, random operands and random exponent lengths.  A
 *      differential test against the code this module is a drop-in for, so any
 *      disagreement is a bug in one of them.
 *
 * Everything is seeded from a constant.  A failure is reproducible.
 *
 * Run:
 *   AMINETXDUO_RUN_TAG=rsa20 ./tools/fsuae-run.sh        -t 300 build/cm/tests/crypto68k/crypto68k_test
 *   AMINETXDUO_RUN_TAG=rsa30 ./tools/fsuae-run.sh -c 68030 -t 300 build/cm/tests/crypto68k/crypto68k_test
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_support.h"
#include "c68k_timer.h"
#include "aminetxduo/crashguard.h"

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "c68k_vectors.h"


/* ------------------------------------------------------------- buffers --- */

#define T_MAX_LIMBS         64u             /* RSA-2048 */
#define T_POWM_SCRATCH      4096u           /* > C68K_POWM_SCRATCH_LIMBS(64, 6) */
#define T_HN_SCRATCH        2048u

static c68k_limb    t_m[T_MAX_LIMBS];
static c68k_limb    t_x[T_MAX_LIMBS];
static c68k_limb    t_y[T_MAX_LIMBS];
static c68k_limb    t_exp[T_MAX_LIMBS];
static c68k_limb    t_mine[T_MAX_LIMBS * 2 + 8];
static c68k_limb    t_work[C68K_MONT_WORK_LIMBS(T_MAX_LIMBS)];
static c68k_limb    t_scratch[T_POWM_SCRATCH];
static c68k_limb    t_hn_scratch[T_HN_SCRATCH];
static c68k_limb    t_ref_result[T_MAX_LIMBS * 2 + 8];
static c68k_limb    t_tmp[T_MAX_LIMBS * 2 + 8];

static ULONG        t_failures;
static ULONG        t_checks;


static VOID t_fail(const char *what, ULONG a, ULONG b)
{

    t_failures++;
    if (t_failures <= 20u)
    {
        c68k_log("  FAIL %s (%lu, %lu)", (LONG)what, a, b);
    }
}


/* --------------------------------------------------- 1. known answers ---- */

static VOID t_known_answers(VOID)
{

UINT    i;
UINT    status;


    c68k_log("");
    c68k_log("1. Known answers (Python arbitrary precision):");

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
    c68k_log("  %lu small modexp vectors", (ULONG)i);

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
        c68k_log("  RSA-2048 public (e=65537)  OK");
    }

    /* RSA-2048 private, plain: m^d mod n.  The slow one, and the operation the
       module exists for, so it gets a KAT. */
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
        c68k_log("  RSA-2048 private (d, 2048 bit)  OK");
    }
}


/* ----------------------------------------------- 2. the limb primitive --- */

/*
 * A model of c68k_addmul_1 written plainly enough that it cannot share a bug
 * with either the assembly or the optimised C: one 64-bit accumulator.
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
static c68k_limb t_extreme(VOID)
{

c68k_limb   v = c68k_rand();


    switch (v & 7u)
    {
    case 0:  return(0xFFFFFFFFUL);
    case 1:  return(0xFFFFFFFEUL);
    case 2:  return(0u);
    case 3:  return(1u);
    case 4:  return(0x80000000UL);
    default: return(c68k_rand());
    }
}

static VOID t_primitive(VOID)
{

UINT        trial;
UINT        i;
UINT        n;
c68k_limb   a;
c68k_limb   c_mine;
c68k_limb   c_model;
UINT        mismatch = 0;


    c68k_log("");
    c68k_log("2. c68k_addmul_1 against a straight-line model:");

    for (trial = 0; trial < 4000u; trial++)
    {
        n = (UINT)(c68k_rand() % 71u);          /* 0..70, including 0 and 1 */
        a = t_extreme();

        for (i = 0; i < n; i++)
        {
            t_x[i % T_MAX_LIMBS] = 0;           /* keep the arrays in range */
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

    c68k_log("  4000 trials, n = 0..70, extreme-biased operands: %lu mismatches",
             (ULONG)mismatch);
}


/* ------------------------------- 3. differential vs the vendored code ---- */

/*
 * One Montgomery multiply and one Montgomery square, checked against
 * _nx_crypto_huge_number_mont() with the same n0inv.  At this level a carry bug
 * shows up as a single wrong limb rather than smeared over an exponentiation,
 * so it gets the most trials.
 */
static VOID t_mont_differential(UINT trials)
{

UINT                    trial;
UINT                    m_len;
UINT                    i;
c68k_limb               n0inv;
NX_CRYPTO_HUGE_NUMBER   m_hn, x_hn, y_hn, r_hn;
UINT                    bad_mul = 0;
UINT                    bad_sqr = 0;


    c68k_log("");
    c68k_log("3. c68k_mont_mul / c68k_mont_sqr vs _nx_crypto_huge_number_mont:");

    for (trial = 0; trial < trials; trial++)
    {
        m_len = (UINT)(c68k_rand() % 32u) + 1u;         /* 1..32 limbs */

        c68k_rand_limbs(t_m, m_len);
        t_m[0] |= 1u;                                   /* Montgomery needs odd */

        c68k_rand_limbs(t_x, m_len);
        c68k_rand_limbs(t_y, m_len);

        /* Reduce the operands below the modulus with the vendored divider, so
           the inputs are exactly what a real Montgomery step would see. */
        c68k_hn_set(&m_hn, t_m, m_len, m_len);
        c68k_hn_set(&x_hn, t_x, m_len, T_MAX_LIMBS);
        c68k_hn_set(&y_hn, t_y, m_len, T_MAX_LIMBS);
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
        c68k_hn_set(&x_hn, t_x, m_len, T_MAX_LIMBS);
        c68k_hn_set(&y_hn, t_y, m_len, T_MAX_LIMBS);

        n0inv = c68k_mont_n0inv(t_m[0]);

        /* multiply */
        c68k_hn_set(&r_hn, t_ref_result, 0, T_MAX_LIMBS * 2);
        _nx_crypto_huge_number_mont(&m_hn, n0inv, &x_hn, &y_hn, &r_hn);
        c68k_mont_mul(t_mine, t_x, t_y, t_m, m_len, n0inv, t_work);

        t_checks++;
        if (!c68k_hn_equals(&r_hn, t_mine, m_len))
        {
            bad_mul++;
            t_fail("mont_mul", trial, m_len);
        }

        /* square: mont(x, x) in the vendored code, c68k_mont_sqr here */
        c68k_hn_set(&r_hn, t_ref_result, 0, T_MAX_LIMBS * 2);
        _nx_crypto_huge_number_mont(&m_hn, n0inv, &x_hn, &x_hn, &r_hn);
        c68k_mont_sqr(t_mine, t_x, t_m, m_len, n0inv, t_work);

        t_checks++;
        if (!c68k_hn_equals(&r_hn, t_mine, m_len))
        {
            bad_sqr++;
            t_fail("mont_sqr", trial, m_len);
        }
    }

    c68k_log("  %lu trials, m 1..32 limbs: %lu mul, %lu sqr mismatches",
             (ULONG)trials, (ULONG)bad_mul, (ULONG)bad_sqr);
}


/*
 * Full exponentiations against _nx_crypto_huge_number_mont_power_modulus, with
 * the exponent length varied across the boundaries where the window logic
 * changes (1, 24, 80, 240 bits) and with exponents whose top limb has leading
 * zeros, the case the vendored code handles differently from this one.
 */
static VOID t_powm_differential(UINT trials)
{

UINT                    trial;
UINT                    m_len;
UINT                    e_len;
UINT                    top_bits;
UINT                    i;
UINT                    status;
UINT                    bad = 0;
NX_CRYPTO_HUGE_NUMBER   m_hn, x_hn, e_hn, r_hn;


    c68k_log("");
    c68k_log("4. c68k_mont_power_modulus vs _nx_crypto_huge_number_mont_power_modulus:");

    for (trial = 0; trial < trials; trial++)
    {
        m_len = (UINT)(c68k_rand() % 12u) + 1u;         /* 1..12 limbs */
        e_len = (UINT)(c68k_rand() % 4u) + 1u;          /* 1..4 limbs  */

        c68k_rand_limbs(t_m, m_len);
        t_m[0] |= 1u;

        c68k_rand_limbs(t_x, m_len);
        c68k_rand_limbs(t_exp, e_len);

        /*
         * Chop the exponent's top limb to a random width.  Leading zero bits
         * of the top limb cost this module nothing, and uniformly random limbs
         * almost never have them, so those cases are generated here.
         */
        top_bits = (UINT)(c68k_rand() % 32u) + 1u;
        t_exp[e_len - 1] &= (c68k_limb)(0xFFFFFFFFUL >> (32u - top_bits));
        if (t_exp[e_len - 1] == 0)
        {
            t_exp[e_len - 1] = 1u;
        }

        c68k_hn_set(&m_hn, t_m, m_len, m_len);
        c68k_hn_set(&x_hn, t_x, m_len, T_MAX_LIMBS);
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
        c68k_hn_set(&x_hn, t_tmp, m_len, T_MAX_LIMBS);
        c68k_hn_set(&e_hn, t_exp, e_len, T_MAX_LIMBS);
        c68k_hn_set(&r_hn, t_ref_result, 0, T_MAX_LIMBS * 2);
        c68k_hn_set(&m_hn, t_m, m_len, m_len);
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
        if (!c68k_hn_equals(&r_hn, t_mine, m_len))
        {
            bad++;
            t_fail("powm value", trial, (m_len << 8) | e_len);
        }
    }

    c68k_log("  %lu trials, m 1..12 limbs, e 1..4 limbs, top limb 1..32 bits: %lu mismatches",
             (ULONG)trials, (ULONG)bad);
}


/*
 * Two cases the random generator will never produce but a caller might.
 */
static VOID t_edge_cases(VOID)
{

UINT                    status;
UINT                    i;
c68k_limb               zero_e[1];
NX_CRYPTO_HUGE_NUMBER   m_hn, x_hn, e_hn, r_hn;


    c68k_log("");
    c68k_log("5. Edge cases:");

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

    /* e = 0 -> 1, checked against the vendored routine, which also has to
       cope with an exponent whose only limb is zero. */
    c68k_rand_limbs(t_m, 8u);
    t_m[0] |= 1u;
    c68k_rand_limbs(t_x, 8u);
    zero_e[0] = 0u;

    c68k_hn_set(&m_hn, t_m, 8u, 8u);
    c68k_hn_set(&x_hn, t_x, 8u, T_MAX_LIMBS);
    _nx_crypto_huge_number_modulus(&x_hn, &m_hn);
    for (i = x_hn.nx_crypto_huge_number_size; i < 8u; i++)
    {
        t_x[i] = 0;
    }

    for (i = 0; i < 8u; i++)
    {
        t_tmp[i] = t_x[i];
    }
    c68k_hn_set(&x_hn, t_tmp, 8u, T_MAX_LIMBS);
    c68k_hn_set(&e_hn, zero_e, 1u, 1u);
    c68k_hn_set(&r_hn, t_ref_result, 0, T_MAX_LIMBS * 2);
    c68k_hn_set(&m_hn, t_m, 8u, 8u);
    _nx_crypto_huge_number_mont_power_modulus(&x_hn, &e_hn, &m_hn, &r_hn,
                                              t_hn_scratch);

    status = c68k_mont_power_modulus(t_mine, t_x, 8u, zero_e, 1u, t_m, 8u,
                                     t_scratch, T_POWM_SCRATCH);
    t_checks++;
    if ((status != NX_CRYPTO_SUCCESS) || !c68k_hn_equals(&r_hn, t_mine, 8u))
    {
        t_fail("x^0", status, 0);
    }

    /* An even modulus must be refused, not silently answered wrongly. */
    t_m[0] &= ~1u;
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

    c68k_log("  0^e, x^0, even modulus, undersized scratch");
}


/* ------------------------------------------------------------------ main -- */

int main(VOID)
{

ULONG   start;


    ami_crash_set_reference((APTR)main, "crypto68k_test");
    if (!ami_crash_install())
    {
        c68k_log("CRASHED -- see the serial log and DH0:crash.txt");
        c68k_flush();
        ami_crash_remove();
        return(20);
    }

    c68k_log("AmiNetXDuo -- crypto68k correctness gate");
    c68k_log("  limb primitives: %s",
             (LONG)(c68k_using_assembly() ? "68020 assembly" : "portable C"));

    (VOID) c68k_timer_open();
    start = c68k_eclock();

    c68k_rng_seed(0x5A17C0DEUL);

    t_known_answers();
    t_primitive();
    t_mont_differential(400u);
    t_powm_differential(150u);
    t_edge_cases();

    c68k_log("");
    c68k_log("Wall time: %lu ms", c68k_eclock_millis(c68k_eclock() - start));

    if (t_failures == 0)
    {
        c68k_log("%lu checks, 0 failures -- PASS", t_checks);
    }
    else
    {
        c68k_log("%lu checks, %lu failures -- FAIL", t_checks, t_failures);
    }

    c68k_timer_close();
    c68k_flush();
    ami_crash_remove();

    return((t_failures == 0) ? 0 : 20);
}
