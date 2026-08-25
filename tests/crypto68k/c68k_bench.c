/*
 * AmiNetXDuo, crypto68k before/after benchmark.
 *
 *   Every figure is a pair run back to back on the same operands: the vendored
 *   routine, then this module.  Only the RATIO of a pair is trustworthy, the
 *   emulator is not cycle exact.  Budget ~300 s of emulated time, run with -t 600.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_support.h"
#include "c68k_timer.h"
#include "aminetxduo/crashguard.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "c68k_vectors.h"

static const char *const c68k_prim_names[3] =
{
    "portable C",
    "68020 assembly",
    "MULU.W assembly, multiply-accumulate only"
};


#define B_MAX_LIMBS         64u
#define B_POWM_SCRATCH      4096u
#define B_HN_SCRATCH        4096u

static c68k_limb    b_work[C68K_MONT_WORK_LIMBS(B_MAX_LIMBS)];
static c68k_limb    b_scratch[B_POWM_SCRATCH];
static c68k_limb    b_hn_scratch[B_HN_SCRATCH];
static c68k_limb    b_result[B_MAX_LIMBS * 2 + 8];
static c68k_limb    b_ref_result[B_MAX_LIMBS * 2 + 8];
static c68k_limb    b_x[B_MAX_LIMBS];
static c68k_limb    b_y[B_MAX_LIMBS];
static c68k_limb    b_m[B_MAX_LIMBS];
static c68k_limb    b_e_copy[B_MAX_LIMBS];
static c68k_limb    b_x_copy[B_MAX_LIMBS];
static c68k_limb    b_p[B_MAX_LIMBS / 2];
static c68k_limb    b_q[B_MAX_LIMBS / 2];

static ULONG        b_failures;


/* ------------------------------------------------------------- timing ---- */

typedef VOID (*B_OP)(VOID);

/*
 * Microseconds per operation, one unit everywhere, so dividing one result by
 * another is always meaningful.  Ticks are accumulated raw and converted once,
 * so the 64-bit divide never happens inside a timed region.
 */
static ULONG b_time(B_OP op)
{

ULONG   hz;
ULONG   start;
ULONG   ticks;
ULONG   n;
ULONG   i;


    hz = c68k_eclock_hz();

    start = c68k_eclock();
    op();
    ticks = c68k_eclock() - start;

    if (ticks >= (hz >> 2))
    {
        return(c68k_eclock_micros(ticks));
    }

    n = (ticks != 0) ? ((hz >> 1) / ticks) : 2000UL;
    if (n == 0)
    {
        n = 1;
    }
    if (n > 20000UL)
    {
        n = 20000UL;
    }

    start = c68k_eclock();
    for (i = 0; i < n; i++)
    {
        op();
    }
    ticks = c68k_eclock() - start;

    return(c68k_eclock_micros(ticks) / n);
}

/* Explicit shared repeat count, for pairs short enough that the calibration
   above would pick different counts for the two halves. */
static ULONG b_time_n(B_OP op, ULONG n)
{

ULONG   start;
ULONG   i;
ULONG   ticks;


    start = c68k_eclock();
    for (i = 0; i < n; i++)
    {
        op();
    }
    ticks = c68k_eclock() - start;

    return(c68k_eclock_micros(ticks) / n);
}

/* Report a pair as microseconds each plus the ratio to one decimal. */
static VOID b_report(const char *name, ULONG ref, ULONG opt)
{

ULONG   ratio;


    ratio = (opt != 0) ? ((ref * 10UL) / opt) : 0UL;

    c68k_log("  %s: ref %lu us -> opt %lu us   = %lu.%lux",
             (LONG)name, ref, opt, ratio / 10UL, ratio % 10UL);
}


/* ------------------------------------------------- 1. the limb primitive -- */

static VOID b_prim_asm(VOID)
{

    (VOID) C68K_ADDMUL_1(b_result, b_y, 64u, 0xDEADBEEFUL);
}

static VOID b_prim_c(VOID)
{

    (VOID) c68k_addmul_1_c(b_result, b_y, 64u, 0xDEADBEEFUL);
}

static VOID b_bench_primitive(VOID)
{

ULONG   ref;
ULONG   opt;


    c68k_log("");
    c68k_log("1. Limb multiply-accumulate, 64 limbs:");

    c68k_rand_limbs(b_y, 64u);
    c68k_rand_limbs(b_result, 64u);

    ref = b_time_n(b_prim_c,   2000UL);
    opt = b_time_n(b_prim_asm, 2000UL);

    b_report("addmul_1, 64 limbs", ref, opt);

    if (!c68k_using_assembly())
    {
        c68k_log("    (built without assembly, both halves are the same code)");
    }
}


/* ---------------------------------------------- 2. one Montgomery step --- */

static NX_CRYPTO_HUGE_NUMBER    b_m_hn, b_x_hn, b_y_hn, b_r_hn;
static c68k_limb                b_n0inv;
static UINT                     b_step_len;

static VOID b_step_ref(VOID)
{

    c68k_hn_set(&b_r_hn, b_ref_result, 0, (B_MAX_LIMBS * 2) + 8);
    _nx_crypto_huge_number_mont(&b_m_hn, b_n0inv, &b_x_hn, &b_y_hn, &b_r_hn);
}

static VOID b_step_mul(VOID)
{

    c68k_mont_mul(b_result, b_x, b_y, b_m, b_step_len, b_n0inv, b_work);
}

static VOID b_step_sqr(VOID)
{

    c68k_mont_sqr(b_result, b_x, b_m, b_step_len, b_n0inv, b_work);
}

static VOID b_step_ref_sqr(VOID)
{

    c68k_hn_set(&b_r_hn, b_ref_result, 0, (B_MAX_LIMBS * 2) + 8);
    _nx_crypto_huge_number_mont(&b_m_hn, b_n0inv, &b_x_hn, &b_x_hn, &b_r_hn);
}

static VOID b_bench_step(UINT m_len, ULONG reps)
{

ULONG   ref;
ULONG   opt;


    b_step_len = m_len;

    c68k_rand_limbs(b_m, m_len);
    b_m[0] |= 1u;
    b_m[m_len - 1] |= 0x80000000UL;

    c68k_rand_limbs(b_x, m_len);
    c68k_rand_limbs(b_y, m_len);
    b_x[m_len - 1] &= 0x3FFFFFFFUL;         /* comfortably below the modulus */
    b_y[m_len - 1] &= 0x3FFFFFFFUL;

    c68k_hn_set(&b_m_hn, b_m, m_len, m_len);
    c68k_hn_set(&b_x_hn, b_x, m_len, B_MAX_LIMBS);
    c68k_hn_set(&b_y_hn, b_y, m_len, B_MAX_LIMBS);
    b_n0inv = c68k_mont_n0inv(b_m[0]);

    /* Correctness before speed, every time. */
    b_step_ref();
    b_step_mul();
    if (!c68k_hn_equals(&b_r_hn, b_result, m_len))
    {
        c68k_log("  FAIL mont_mul disagrees at %lu limbs", (ULONG)m_len);
        b_failures++;
    }
    b_step_ref_sqr();
    b_step_sqr();
    if (!c68k_hn_equals(&b_r_hn, b_result, m_len))
    {
        c68k_log("  FAIL mont_sqr disagrees at %lu limbs", (ULONG)m_len);
        b_failures++;
    }

    ref = b_time_n(b_step_ref, reps);
    opt = b_time_n(b_step_mul, reps);
    c68k_log("  %lu limbs (%lu bit modulus):", (ULONG)m_len, (ULONG)(m_len << 5));
    b_report("    Montgomery multiply", ref, opt);

    ref = b_time_n(b_step_ref_sqr, reps);
    opt = b_time_n(b_step_sqr, reps);
    b_report("    Montgomery square  ", ref, opt);
}

static VOID b_bench_steps(VOID)
{

    c68k_log("");
    c68k_log("2. One Montgomery operation:");
    b_bench_step(32u, 200UL);
    b_bench_step(64u, 200UL);
}


/* ------------------------------------------------------ 3. RSA-2048 ------ */

static NX_CRYPTO_HUGE_NUMBER    b_e_hn, b_p_hn, b_q_hn;
static const c68k_limb         *b_exp;
static UINT                     b_exp_len;

static VOID b_rsa_ref(VOID)
{

UINT    i;


    for (i = 0; i < 64u; i++)
    {
        b_x_copy[i] = t_msg[i];
        b_e_copy[i] = (i < b_exp_len) ? b_exp[i] : 0u;
    }

    c68k_hn_set(&b_x_hn, b_x_copy, 64u, B_MAX_LIMBS);
    c68k_hn_set(&b_e_hn, b_e_copy, b_exp_len, B_MAX_LIMBS);
    c68k_hn_set(&b_m_hn, b_m, 64u, 64u);
    c68k_hn_set(&b_r_hn, b_ref_result, 0, (B_MAX_LIMBS * 2) + 8);

    _nx_crypto_huge_number_mont_power_modulus(&b_x_hn, &b_e_hn, &b_m_hn,
                                              &b_r_hn, b_hn_scratch);
}

static VOID b_rsa_opt(VOID)
{

    (VOID) c68k_mont_power_modulus(b_result, t_msg, 64u, b_exp, b_exp_len,
                                   b_m, 64u, b_scratch, B_POWM_SCRATCH);
}

static VOID b_bench_rsa(const char *name, const c68k_limb *e, UINT e_len,
                        const c68k_limb *expected)
{

ULONG   ref;
ULONG   opt;
UINT    i;


    b_exp     = e;
    b_exp_len = e_len;

    for (i = 0; i < 64u; i++)
    {
        b_m[i] = t_n[i];
    }

    ref = b_time(b_rsa_ref);
    if (!c68k_hn_equals(&b_r_hn, expected, 64u))
    {
        c68k_log("  FAIL reference %s is wrong", (LONG)name);
        b_failures++;
    }

    opt = b_time(b_rsa_opt);
    if (c68k_cmp(b_result, expected, 64u) != 0)
    {
        c68k_log("  FAIL optimised %s is wrong", (LONG)name);
        b_failures++;
    }

    b_report(name, ref, opt);
}


/* ------------------------------------------------------ 4. RSA-2048 CRT -- */

static VOID b_crt_ref(VOID)
{

UINT    i;


    for (i = 0; i < 64u; i++)
    {
        b_x_copy[i] = t_msg[i];
        b_e_copy[i] = t_d[i];
    }

    c68k_hn_set(&b_x_hn, b_x_copy, 64u, B_MAX_LIMBS);
    c68k_hn_set(&b_e_hn, b_e_copy, 64u, B_MAX_LIMBS);
    c68k_hn_set(&b_m_hn, b_m, 64u, 64u);
    c68k_hn_set(&b_p_hn, b_p, 32u, 32u);
    c68k_hn_set(&b_q_hn, b_q, 32u, 32u);
    c68k_hn_set(&b_r_hn, b_ref_result, 0, (B_MAX_LIMBS * 2) + 8);

    _nx_crypto_huge_number_crt_power_modulus(&b_x_hn, &b_e_hn, &b_p_hn,
                                             &b_q_hn, &b_m_hn, &b_r_hn,
                                             b_hn_scratch);
}

static VOID b_crt_opt(VOID)
{

UINT    i;


    for (i = 0; i < 64u; i++)
    {
        b_x_copy[i] = t_msg[i];
        b_e_copy[i] = t_d[i];
    }

    c68k_hn_set(&b_x_hn, b_x_copy, 64u, B_MAX_LIMBS);
    c68k_hn_set(&b_e_hn, b_e_copy, 64u, B_MAX_LIMBS);
    c68k_hn_set(&b_m_hn, b_m, 64u, 64u);
    c68k_hn_set(&b_p_hn, b_p, 32u, 32u);
    c68k_hn_set(&b_q_hn, b_q, 32u, 32u);
    c68k_hn_set(&b_r_hn, b_result, 0, (B_MAX_LIMBS * 2) + 8);

    c68k_crt_power_modulus(&b_x_hn, &b_e_hn, &b_p_hn, &b_q_hn, &b_m_hn,
                           &b_r_hn, b_hn_scratch,
                           b_scratch, B_POWM_SCRATCH);
}

static VOID b_bench_crt(VOID)
{

ULONG   ref;
ULONG   opt;
UINT    i;


    for (i = 0; i < 64u; i++)
    {
        b_m[i] = t_n[i];
    }
    for (i = 0; i < 32u; i++)
    {
        b_p[i] = t_p[i];
        b_q[i] = t_q[i];
    }

    ref = b_time(b_crt_ref);
    if (!c68k_hn_equals(&b_r_hn, t_msg_priv, 64u))
    {
        c68k_log("  FAIL reference CRT is wrong");
        b_failures++;
    }

    opt = b_time(b_crt_opt);
    if (!c68k_hn_equals(&b_r_hn, t_msg_priv, 64u))
    {
        c68k_log("  FAIL optimised CRT is wrong");
        b_failures++;
    }

    b_report("RSA-2048 private, CRT  ", ref, opt);
}


/* ------------------------------------------------------------------ main -- */

int main(VOID)
{

ULONG   start;


    /* As a shipped library does at init, so the rows below are labelled
       with what would actually run (src/crypto68k/c68k_cpu.c). */
    c68k_cpu_select((ULONG)SysBase->AttnFlags);

    ami_crash_set_reference((APTR)main, "crypto68k_bench");
    if (!ami_crash_install())
    {
        c68k_log("CRASHED, see the serial log and DH0:crash.txt");
        c68k_flush();
        ami_crash_remove();
        return(20);
    }

    c68k_log("AmiNetXDuo, crypto68k before/after benchmark");
    c68k_log("  limb primitives: %s",
             (LONG)c68k_prim_names[c68k_using_assembly() % 3u]);

    if (!c68k_timer_open())
    {
        c68k_log("FATAL: timer.device UNIT_ECLOCK would not open");
        c68k_flush();
        ami_crash_remove();
        return(20);
    }
    c68k_log("  E-Clock %lu Hz", c68k_eclock_hz());
    c68k_log("  Ratios are the trustworthy figure; absolutes are 'the");
    c68k_log("  emulator's 68020', not a 14 MHz A1200.");

    c68k_rng_seed(0x0B1EC0DEUL);
    start = c68k_eclock();

    b_bench_primitive();
    b_bench_steps();

    c68k_log("");
    c68k_log("3. RSA-2048, whole operation:");
    b_bench_rsa("RSA-2048 public       ", t_e, 1u, t_msg_pub);
    b_bench_crt();
    b_bench_rsa("RSA-2048 private, plain", t_d, 64u, t_msg_priv);

    c68k_log("");
    c68k_log("Wall time: %lu ms", c68k_eclock_millis(c68k_eclock() - start));

    if (b_failures == 0)
    {
        c68k_log("0 correctness failures, every timed result was checked");
    }
    else
    {
        c68k_log("%lu CORRECTNESS FAILURES, timings are meaningless",
                 b_failures);
    }

    c68k_timer_close();
    c68k_flush();
    ami_crash_remove();

    return((b_failures == 0) ? 0 : 20);
}
