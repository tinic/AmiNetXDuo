/*
 * AmiNetXDuo, crypto68k P-256 before/after benchmark.
 *
 * Same method as c68k_bench.c: every figure is a pair, the unmodified vendored
 * routine and this module, run back to back in one process on the same
 * operands, and only the ratio of a pair is trustworthy.  FS-UAE's 68020 is
 * not a 14 MHz A1200, and its 68030 model is not a timing model at all, it
 * does not charge for MULU.L, so it flatters anything that moves work into
 * multiplies.  Read the 68020 column; the 68030 column is printed so the
 * figure is attributable.
 *
 * The three headline operations are the ones docs/RESEARCH.md 9 measured:
 * ECDHE key generation, ECDHE shared secret, ECDSA verify.  They are timed
 * through the real vendored APIs, with nothing changed but one function
 * pointer in a copy of the curve structure, so the ratio is what a TLS
 * handshake would see.
 *
 * The layers below are timed too, to show which layer costs the time.
 *
 * Budget about 120 s of emulated time; run it with -t 400.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_support.h"
#include "c68k_timer.h"
#include "c68k_p256.h"
#include "aminetxduo/crashguard.h"

#include "nx_crypto_ec.h"
#include "nx_crypto_ecdh.h"
#include "nx_crypto_ecdsa.h"

#include <exec/types.h>
#include <proto/exec.h>

#include "c68k_ec_vectors.h"


static ULONG                b_failures;
static ULONG                b_sum_ref;
static ULONG                b_sum_opt;

static NX_CRYPTO_EC        *b_ref_curve;
static NX_CRYPTO_EC         b_opt_curve;
static NX_CRYPTO_EC        *b_curve;         /* which one the timed op uses */

static HN_UBASE             b_scratch[2048];
static HN_UBASE             b_store[512];

static NX_CRYPTO_ECDH       b_ecdh;
static HN_UBASE             b_ecdsa_scratch[NX_CRYPTO_ECDSA_SCRATCH_BUFFER_SIZE >> HN_SIZE_SHIFT];

static NX_CRYPTO_EC_POINT   b_point;
static NX_CRYPTO_EC_POINT   b_base;
static NX_CRYPTO_HUGE_NUMBER b_d;

static c68k_p256_fe         b_fa;
static c68k_p256_fe         b_fb;
static c68k_p256_fe         b_fr;
static c68k_limb            b_prod[16];
static NX_CRYPTO_HUGE_NUMBER b_ahn, b_bhn, b_rhn;
static c68k_p256_aff        b_aff_q;
static c68k_limb            b_k[8];


/* ------------------------------------------------------------- timing ---- */

typedef VOID (*B_OP)(VOID);

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


static VOID b_report(const char *name, ULONG ref, ULONG opt)
{

ULONG   ratio;


    ratio = (opt != 0) ? ((ref * 10UL) / opt) : 0UL;

    c68k_log("  %s: ref %lu us -> opt %lu us   = %lu.%lux",
             (LONG)name, ref, opt, ratio / 10UL, ratio % 10UL);
}


/* --------------------------------------------------------- 1. the field -- */

static VOID b_ref_mul(VOID)
{

    _nx_crypto_huge_number_multiply(&b_ahn, &b_bhn, &b_rhn);
    b_ref_curve -> nx_crypto_ec_reduce(b_ref_curve, &b_rhn, b_scratch);
}

static VOID b_opt_mul(VOID)
{

    c68k_p256_fe_mul(b_fr, b_fa, b_fb);
}

static VOID b_ref_sqr(VOID)
{

    _nx_crypto_huge_number_square(&b_ahn, &b_rhn);
    b_ref_curve -> nx_crypto_ec_reduce(b_ref_curve, &b_rhn, b_scratch);
}

static VOID b_opt_sqr(VOID)
{

    c68k_p256_fe_sqr(b_fr, b_fa);
}

static VOID b_ref_reduce(VOID)
{

UINT    i;


    for (i = 0; i < 16u; i++)
    {
        b_rhn.nx_crypto_huge_number_data[i] = b_prod[i];
    }
    b_rhn.nx_crypto_huge_number_size = 16u;
    b_rhn.nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
    b_ref_curve -> nx_crypto_ec_reduce(b_ref_curve, &b_rhn, b_scratch);
}

static VOID b_opt_reduce(VOID)
{

    c68k_p256_fe_reduce(b_fr, b_prod);
}

/* The multiply on its own, so the reduction's share of a field multiply can be
   read off. */
static VOID b_ref_mul_only(VOID)
{

    _nx_crypto_huge_number_multiply(&b_ahn, &b_bhn, &b_rhn);
}

static VOID b_bench_field(VOID)
{

HN_UBASE   *sp;
UINT        i;
ULONG       ref;
ULONG       opt;
ULONG       mul_only;


    c68k_log("");
    c68k_log("1. Field arithmetic (one 256-bit modular operation):");

    sp = b_store;
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&b_ahn, sp, 32);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&b_bhn, sp, 32);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&b_rhn, sp, 96);

    for (i = 0; i < 8u; i++)
    {
        b_fa[i] = c68k_rand();
        b_fb[i] = c68k_rand();
    }
    b_fa[7] &= 0xFFFFFFFEUL;
    b_fb[7] &= 0xFFFFFFFEUL;

    for (i = 0; i < 8u; i++)
    {
        b_ahn.nx_crypto_huge_number_data[i] = b_fa[i];
        b_bhn.nx_crypto_huge_number_data[i] = b_fb[i];
    }
    b_ahn.nx_crypto_huge_number_size = 8u;
    b_bhn.nx_crypto_huge_number_size = 8u;

    for (i = 0; i < 16u; i++)
    {
        b_prod[i] = c68k_rand();
    }

    ref = b_time_n(b_ref_mul, 400UL);
    opt = b_time_n(b_opt_mul, 400UL);
    b_report("multiply + reduce", ref, opt);

    mul_only = b_time_n(b_ref_mul_only, 400UL);
    c68k_log("    of which the vendored multiply alone is %lu us, so the",
             mul_only);
    c68k_log("    vendored REDUCTION is %lu us -- %lu%% of a field multiply",
             (ref > mul_only) ? (ref - mul_only) : 0UL,
             (ref != 0) ? (((ref - mul_only) * 100UL) / ref) : 0UL);

    ref = b_time_n(b_ref_sqr, 400UL);
    opt = b_time_n(b_opt_sqr, 400UL);
    b_report("square + reduce  ", ref, opt);

    ref = b_time_n(b_ref_reduce, 400UL);
    opt = b_time_n(b_opt_reduce, 400UL);
    b_report("reduce alone     ", ref, opt);
}


/* -------------------------------------------------- 2. point operations -- */

static NX_CRYPTO_EC_POINT   b_proj;
static c68k_p256_jac        b_jac;

static VOID b_ref_double(VOID)
{

    _nx_crypto_ec_fp_projective_double(b_ref_curve, &b_proj, b_scratch);
}

static VOID b_opt_double(VOID)
{

    c68k_p256_jac_double(&b_jac, &b_jac);
}

static VOID b_ref_add(VOID)
{

    _nx_crypto_ec_fp_projective_add(b_ref_curve, &b_proj, &b_base, b_scratch);
}

static VOID b_opt_add(VOID)
{

    c68k_p256_jac_add_affine(&b_jac, &b_jac, &b_aff_q);
}

static VOID b_bench_points(VOID)
{

HN_UBASE   *sp;
UINT        i;
ULONG       ref;
ULONG       opt;


    c68k_log("");
    c68k_log("2. One point operation:");

    sp = b_store;
    NX_CRYPTO_EC_POINT_INITIALIZE(&b_proj, NX_CRYPTO_EC_POINT_PROJECTIVE, sp, 32);
    NX_CRYPTO_EC_POINT_INITIALIZE(&b_base, NX_CRYPTO_EC_POINT_AFFINE, sp, 32);

    NX_CRYPTO_HUGE_NUMBER_COPY(&b_proj.nx_crypto_ec_point_x,
                               &b_ref_curve -> nx_crypto_ec_g.nx_crypto_ec_point_x);
    NX_CRYPTO_HUGE_NUMBER_COPY(&b_proj.nx_crypto_ec_point_y,
                               &b_ref_curve -> nx_crypto_ec_g.nx_crypto_ec_point_y);
    _nx_crypto_ec_point_fp_affine_to_projective(&b_proj);

    NX_CRYPTO_HUGE_NUMBER_COPY(&b_base.nx_crypto_ec_point_x,
                               &b_ref_curve -> nx_crypto_ec_g.nx_crypto_ec_point_x);
    NX_CRYPTO_HUGE_NUMBER_COPY(&b_base.nx_crypto_ec_point_y,
                               &b_ref_curve -> nx_crypto_ec_g.nx_crypto_ec_point_y);

    for (i = 0; i < 8u; i++)
    {
        b_jac.x[i] = c68k_p256_comb[1][i];
        b_jac.y[i] = c68k_p256_comb[1][i + 8u];
        b_jac.z[i] = (i == 0u) ? 1UL : 0UL;
        b_aff_q.x[i] = c68k_p256_comb[0][i];
        b_aff_q.y[i] = c68k_p256_comb[0][i + 8u];
    }

    ref = b_time_n(b_ref_double, 200UL);
    opt = b_time_n(b_opt_double, 200UL);
    b_report("projective double         ", ref, opt);

    ref = b_time_n(b_ref_add, 200UL);
    opt = b_time_n(b_opt_add, 200UL);
    b_report("projective + affine add   ", ref, opt);
}


/* ------------------------------------------- 3. scalar multiplication ---- */

static VOID b_mul_ref_fixed(VOID)
{

    _nx_crypto_ec_fp_projective_multiple(b_ref_curve,
                                         &b_ref_curve -> nx_crypto_ec_g,
                                         &b_d, &b_point, b_scratch);
}

static VOID b_mul_opt_fixed(VOID)
{

    c68k_p256_ec_multiple(&b_opt_curve, &b_opt_curve.nx_crypto_ec_g,
                          &b_d, &b_point, b_scratch);
}

static VOID b_mul_ref_generic(VOID)
{

    _nx_crypto_ec_fp_projective_multiple(b_ref_curve, &b_base, &b_d, &b_point,
                                         b_scratch);
}

static VOID b_mul_opt_generic(VOID)
{

    c68k_p256_ec_multiple(&b_opt_curve, &b_base, &b_d, &b_point, b_scratch);
}

static VOID b_bench_scalar(VOID)
{

HN_UBASE   *sp;
UINT        i;
ULONG       ref;
ULONG       opt;


    c68k_log("");
    c68k_log("3. One 256-bit scalar multiplication:");

    sp = b_store;
    NX_CRYPTO_EC_POINT_INITIALIZE(&b_point, NX_CRYPTO_EC_POINT_AFFINE, sp, 32);
    NX_CRYPTO_EC_POINT_INITIALIZE(&b_base, NX_CRYPTO_EC_POINT_AFFINE, sp, 32);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&b_d, sp, 32);

    for (i = 0; i < 8u; i++)
    {
        b_k[i] = c68k_rand();
        b_d.nx_crypto_huge_number_data[i] = b_k[i];
    }
    b_d.nx_crypto_huge_number_data[7] &= 0x7FFFFFFFUL;
    b_k[7] &= 0x7FFFFFFFUL;
    b_d.nx_crypto_huge_number_size = 8u;
    b_d.nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;

    /* A generic point that really is on the curve: 3*G. */
    {
        NX_CRYPTO_HUGE_NUMBER   three;
        HN_UBASE               *sp2 = sp;

        NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&three, sp2, 32);
        three.nx_crypto_huge_number_data[0] = 3UL;
        three.nx_crypto_huge_number_size = 1u;
        three.nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
        _nx_crypto_ec_fp_projective_multiple(b_ref_curve,
                                             &b_ref_curve -> nx_crypto_ec_g,
                                             &three, &b_base, b_scratch);
    }

    for (i = 0; i < 8u; i++)
    {
        b_aff_q.x[i] = (i < b_base.nx_crypto_ec_point_x.nx_crypto_huge_number_size) ?
                       b_base.nx_crypto_ec_point_x.nx_crypto_huge_number_data[i] : 0UL;
        b_aff_q.y[i] = (i < b_base.nx_crypto_ec_point_y.nx_crypto_huge_number_size) ?
                       b_base.nx_crypto_ec_point_y.nx_crypto_huge_number_data[i] : 0UL;
    }

    ref = b_time_n(b_mul_ref_fixed, 3UL);
    opt = b_time_n(b_mul_opt_fixed, 3UL);
    b_report("k * G  (fixed base, comb) ", ref, opt);

    ref = b_time_n(b_mul_ref_generic, 3UL);
    opt = b_time_n(b_mul_opt_generic, 3UL);
    b_report("k * Q  (generic, NAF/wNAF)", ref, opt);
}


/* ------------------------------------------ 4. the headline operations --- */

static VOID b_op_keygen(VOID)
{

    b_curve -> nx_crypto_ec_multiple(b_curve, &b_curve -> nx_crypto_ec_g,
                                     &b_d, &b_point, b_scratch);
}

static VOID b_op_ecdh(VOID)
{

UCHAR   secret[32];
ULONG   len;


    (VOID) _nx_crypto_ecdh_compute_secret(&b_ecdh, secret, 32UL, &len,
                                          (UCHAR *)v_ecdh_peer, 65UL,
                                          b_ecdh.nx_crypto_ecdh_scratch_buffer);
}

static VOID b_op_ecdsa(VOID)
{

    if (_nx_crypto_ecdsa_verify(b_curve, (UCHAR *)v_hash_sample, 32u,
                                (UCHAR *)v_pubkey, 65u,
                                (UCHAR *)v_sig_sample,
                                (UINT)sizeof(v_sig_sample),
                                b_ecdsa_scratch) != NX_CRYPTO_SUCCESS)
    {
        b_failures++;
    }
}

static VOID b_bench_headline(VOID)
{

ULONG   ref;
ULONG   opt;
ULONG   ratio;


    c68k_log("");
    c68k_log("4. The three operations docs/RESEARCH.md 9 measured:");

    /* ECDHE key generation is one fixed-base multiplication; the random draw
       around it is noise by comparison. */
    b_curve = b_ref_curve;
    ref = b_time_n(b_op_keygen, 3UL);
    b_curve = &b_opt_curve;
    opt = b_time_n(b_op_keygen, 3UL);
    b_report("ECDHE P-256 keygen (k*G)  ", ref, opt);
    b_sum_ref += ref; b_sum_opt += opt;

    /*
     * _nx_crypto_ecdh_key_pair_import() sets the key size but not the curve
     * pointer, only _nx_crypto_ecdh_setup() does that, and setup generates a
     * random key, which a known-answer test cannot use.  So the curve is
     * assigned here.  Vendored behaviour, not ours; without it compute_secret
     * dereferences a null curve.
     */
    b_ecdh.nx_crypto_ecdh_curve = b_ref_curve;
    (VOID) _nx_crypto_ecdh_key_pair_import(&b_ecdh, b_ref_curve,
                                           (UCHAR *)v_ecdh_priv, 32UL,
                                           (UCHAR *)v_ecdh_pub, 65UL);
    ref = b_time_n(b_op_ecdh, 3UL);
    b_ecdh.nx_crypto_ecdh_curve = &b_opt_curve;
    (VOID) _nx_crypto_ecdh_key_pair_import(&b_ecdh, &b_opt_curve,
                                           (UCHAR *)v_ecdh_priv, 32UL,
                                           (UCHAR *)v_ecdh_pub, 65UL);
    opt = b_time_n(b_op_ecdh, 3UL);
    b_report("ECDHE P-256 shared secret ", ref, opt);
    b_sum_ref += ref; b_sum_opt += opt;

    b_curve = b_ref_curve;
    ref = b_time_n(b_op_ecdsa, 3UL);
    b_curve = &b_opt_curve;
    opt = b_time_n(b_op_ecdsa, 3UL);
    b_report("ECDSA P-256 verify        ", ref, opt);
    b_sum_ref += ref; b_sum_opt += opt;

    c68k_log("");
    c68k_log("  A client's ECDHE_ECDSA handshake is one of each of the above,");
    c68k_log("  plus one ECDSA verify per extra certificate in the chain:");
    ratio = (b_sum_opt != 0) ? ((b_sum_ref * 10UL) / b_sum_opt) : 0UL;
    c68k_log("    reference %lu ms, optimised %lu ms  = %lu.%lux",
             b_sum_ref / 1000UL, b_sum_opt / 1000UL,
             ratio / 10UL, ratio % 10UL);
}


/* ------------------------------------------------------------------ main -- */

int main(VOID)
{

UINT    status;
ULONG   start;
UCHAR   secret[32];
ULONG   len;
UINT    i;


    ami_crash_set_reference((APTR)main, "crypto68k_ec_bench");
    if (!ami_crash_install())
    {
        c68k_log("CRASHED -- see the serial log and DH0:crash.txt");
        c68k_flush();
        ami_crash_remove();
        return(20);
    }

    c68k_log("AmiNetXDuo -- crypto68k P-256 before/after benchmark");
    c68k_log("  limb primitives: %s",
             (LONG)(c68k_using_assembly() ? "68020 assembly" : "portable C"));

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

    b_ref_curve = NX_CRYPTO_NULL;
    status = _nx_crypto_ec_get_named_curve(&b_ref_curve, NX_CRYPTO_EC_SECP256R1);
    if ((status != NX_CRYPTO_SUCCESS) || (b_ref_curve == NX_CRYPTO_NULL))
    {
        c68k_log("FATAL: secp256r1 not available");
        c68k_flush();
        c68k_timer_close();
        ami_crash_remove();
        return(20);
    }

    b_opt_curve = *b_ref_curve;
    b_opt_curve.nx_crypto_ec_multiple = c68k_p256_ec_multiple;

    if (c68k_p256_self_check() != NX_CRYPTO_SUCCESS)
    {
        c68k_log("FATAL: comb table self check failed");
        b_failures++;
    }

    /* Correctness before speed: the timed ECDH and ECDSA must produce the
       right answers, or the timings are measuring the wrong computation. */
    b_ecdh.nx_crypto_ecdh_curve = &b_opt_curve;
    (VOID) _nx_crypto_ecdh_key_pair_import(&b_ecdh, &b_opt_curve,
                                           (UCHAR *)v_ecdh_priv, 32UL,
                                           (UCHAR *)v_ecdh_pub, 65UL);
    (VOID) _nx_crypto_ecdh_compute_secret(&b_ecdh, secret, 32UL, &len,
                                          (UCHAR *)v_ecdh_peer, 65UL,
                                          b_ecdh.nx_crypto_ecdh_scratch_buffer);
    for (i = 0; i < 32u; i++)
    {
        if (secret[i] != v_ecdh_secret[i])
        {
            c68k_log("  FAIL: optimised ECDH secret is wrong");
            b_failures++;
            break;
        }
    }

    if (_nx_crypto_ecdsa_verify(&b_opt_curve, (UCHAR *)v_hash_sample, 32u,
                                (UCHAR *)v_pubkey, 65u, (UCHAR *)v_sig_sample,
                                (UINT)sizeof(v_sig_sample),
                                b_ecdsa_scratch) != NX_CRYPTO_SUCCESS)
    {
        c68k_log("  FAIL: optimised ECDSA verify rejects a good signature");
        b_failures++;
    }

    if (_nx_crypto_ecdsa_verify(&b_opt_curve, (UCHAR *)v_hash_sample, 32u,
                                (UCHAR *)v_pubkey, 65u, (UCHAR *)v_bad_r_plus_one,
                                (UINT)sizeof(v_bad_r_plus_one),
                                b_ecdsa_scratch) == NX_CRYPTO_SUCCESS)
    {
        c68k_log("  FAIL: optimised ECDSA verify ACCEPTS a bad signature");
        b_failures++;
    }

    c68k_rng_seed(0x0EC0DEB1UL);
    start = c68k_eclock();

    b_bench_field();
    b_bench_points();
    b_bench_scalar();
    b_bench_headline();

    c68k_log("");
    c68k_log("Wall time: %lu ms", c68k_eclock_millis(c68k_eclock() - start));

    if (b_failures == 0)
    {
        c68k_log("0 correctness failures -- every timed path was checked");
    }
    else
    {
        c68k_log("%lu CORRECTNESS FAILURES -- timings are meaningless",
                 b_failures);
    }

    c68k_timer_close();
    c68k_flush();
    ami_crash_remove();

    return((b_failures == 0) ? 0 : 20);
}
