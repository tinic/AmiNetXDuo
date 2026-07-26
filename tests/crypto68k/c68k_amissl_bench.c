/*
 * AmiNetXDuo -- crypto68k against AmiSSL, one process, identical inputs.
 *
 * WHAT THIS ANSWERS
 *
 *   Everything src/crypto68k/ has ever been measured against is the vendored
 *   nx_crypto it replaces.  That is the right baseline for "did the change
 *   work" and the wrong one for "is this stack worth having", because the
 *   Amiga already has an OpenSSL: AmiSSL, which is what the Aminet curl links
 *   against and what every other TLS client on the machine uses.  This program
 *   runs both on the same numbers, back to back, in one process, and checks
 *   that they agree before it believes any timing.
 *
 * WHY ONE PROCESS
 *
 *   Two runs of one binary under FS-UAE agree to about one part in ten
 *   thousand on register kernels (tests/perf/cpucal), but a handshake-scale
 *   measurement drifts with disk state, library load order and whatever else
 *   the emulator is doing.  Timing both sides inside a single run removes all
 *   of it: the only thing that differs between an "ours" number and a "theirs"
 *   number is the code that ran.
 *
 * THE EMULATOR CAVEAT, AND WHY IT IS NOT SYMMETRIC
 *
 *   FS-UAE's A1200 model charges MULU.L 32.14 cycles where a real 68020
 *   charges 45 for the 64-bit form -- measured, tests/perf/cpucal, and
 *   reproduced by a_calibrate() below in this very run.  That is a 29%
 *   discount on the one instruction bignum arithmetic is made of, and it does
 *   NOT cancel between two implementations that issue different numbers of it:
 *   it flatters whichever side multiplies more.  So every row carries a
 *   statically derived MULU.L count and a corrected time beside the measured
 *   one.  A ratio quoted without that correction is a ratio of two differently
 *   flattered numbers.
 *
 * WHAT IS DELIBERATELY NOT COMPARED
 *
 *   The whole handshake.  AmiSSL is a TLS stack and so are we, but a
 *   handshake includes a network round trip, certificate parsing and record
 *   framing, and the question here is arithmetic.  The primitives below are
 *   what a handshake spends its time in, and they are what a change to either
 *   side would move.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_support.h"
#include "c68k_timer.h"
#include "c68k_p256.h"
#include "c68k_amissl.h"

#include "aminetxduo/crashguard.h"

#include "nx_crypto_ec.h"
#include "nx_crypto_ecdh.h"
#include "nx_crypto_ecdsa.h"
#include "nx_crypto_aes.h"
#include "nx_crypto_cbc.h"
#include "nx_crypto_sha2.h"
#include "nx_crypto_hmac.h"
#include "nx_crypto_hmac_sha2.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "c68k_vectors.h"
#include "c68k_ec_vectors.h"

/* nx_crypto's shipped method table entry.  Declared here for the same reason
   src/tls/ami_tls_crypto.c declares it: it is defined in
   nx_crypto_generic_ciphersuites.c and no header exports it. */
extern NX_CRYPTO_METHOD crypto_method_hmac_sha256;


/* --------------------------------------------------------------- state --- */

#define A_MAX_LIMBS         64u
#define A_POWM_SCRATCH      3000u
#define A_HN_SCRATCH        4096u

/* 16 KiB is one TLS record's worth of payload and then some -- RFC 5246 caps
   a plaintext fragment at 2^14 -- so this is the unit the bulk path actually
   moves. */
#define A_BULK_BYTES        16384UL

static ULONG        a_failures;
static ULONG        a_mismatches;
static UINT         a_full;             /* run the slow extras too */

static c68k_limb    a_scratch[A_POWM_SCRATCH];
static c68k_limb    a_hn_scratch[A_HN_SCRATCH];
static c68k_limb    a_result[(A_MAX_LIMBS * 2) + 8];
static c68k_limb    a_m[A_MAX_LIMBS];
static c68k_limb    a_p[A_MAX_LIMBS / 2];
static c68k_limb    a_q[A_MAX_LIMBS / 2];
static c68k_limb    a_x_copy[A_MAX_LIMBS];
static c68k_limb    a_e_copy[A_MAX_LIMBS];

static NX_CRYPTO_HUGE_NUMBER a_x_hn, a_e_hn, a_m_hn, a_p_hn, a_q_hn, a_r_hn;

/* Big-endian byte forms of the same vectors, which is what OpenSSL wants. */
static UCHAR        a_be_n[256];
static UCHAR        a_be_d[256];
static UCHAR        a_be_p[128];
static UCHAR        a_be_q[128];
static UCHAR        a_be_msg[256];
static UCHAR        a_be_pub_expected[256];
static UCHAR        a_be_priv_expected[256];
static const UCHAR  a_be_e[3] = { 0x01, 0x00, 0x01 };

static UCHAR        a_ossl_out[256];
static UCHAR        a_ours_out[256];

/* P-256 */
static NX_CRYPTO_EC        *a_ref_curve;
static NX_CRYPTO_EC         a_our_curve;
static NX_CRYPTO_ECDH       a_ecdh;
static HN_UBASE             a_ec_scratch[2048];
static HN_UBASE             a_ecdsa_scratch[NX_CRYPTO_ECDSA_SCRATCH_BUFFER_SIZE >> HN_SIZE_SHIFT];
static HN_UBASE             a_ec_store[512];
static NX_CRYPTO_EC_POINT   a_point;
static NX_CRYPTO_HUGE_NUMBER a_d;
static UCHAR                a_ours_secret[32];
static UCHAR                a_theirs_secret[32];
static UCHAR                a_ours_g[65];
static UCHAR                a_theirs_g[65];
static UCHAR                a_kg_scalar[32];

/* bulk */
static UCHAR                a_bulk_in[A_BULK_BYTES];
static UCHAR                a_bulk_ours[A_BULK_BYTES];
static UCHAR                a_bulk_theirs[A_BULK_BYTES];
static UCHAR                a_aes_key[16];
static UCHAR                a_aes_iv[16];
static UCHAR                a_mac_key[32];
static UCHAR                a_mac_ours[32];
static UCHAR                a_mac_theirs[32];
static NX_CRYPTO_AES        a_aes;
static NX_CRYPTO_CBC        a_cbc;
static NX_CRYPTO_SHA256_HMAC a_hmac_meta;

/* The running comparison, so the summary can be assembled from measurements
   rather than retyped. */
#define A_MAX_ROWS  24
typedef struct
{
    const char *name;
    ULONG       ours_us;
    ULONG       theirs_us;
    ULONG       ours_mulu;      /* static MULU.L count, see A_MULU_* below */
    ULONG       theirs_mulu;
    UINT        counted;        /* in the handshake/bulk summary */
} A_ROW;
static A_ROW    a_rows[A_MAX_ROWS];
static UINT     a_row_count;


/* =========================================== the emulator's MULU.L discount ==
 *
 * FS-UAE's A1200 model charges MULU.L 32.1 cycles where an MC68020 charges 43
 * (Dn,Dm) or 45 (Dn,Dh:Dl) -- measured, tests/perf/cpucal, and reproduced by
 * a_calibrate() below in this very run.  Every other instruction the model
 * charges is faithful to under 2%.
 *
 * That is a 29% discount on the single instruction multi-precision arithmetic
 * is made of, and it does NOT cancel between two implementations that issue
 * different numbers of it.  It flatters whichever side does MORE multiplying.
 * So each row carries a statically derived count of 32x32->64 multiplies and
 * the report prints a corrected time beside the measured one:
 *
 *     corrected = measured + count * t_mulu * (45 - 32.14) / 32.14
 *
 * where t_mulu is measured in this process.  The fraction is 0.400.  Working
 * from a measured t_mulu rather than an assumed clock is what makes this
 * independent of -k: the same expression is right at 14 MHz and at 28.
 *
 * WHERE THE COUNTS COME FROM.  Both sides are deterministic given the
 * operands, so these are derived rather than sampled.
 *
 *   crypto68k, SOS Montgomery over s limbs (src/crypto68k/c68k_mont.c):
 *     multiply  s^2 (product) + s^2 (reduction) + s (the u = t[i]*n0inv)
 *               = 2s^2 + s
 *     square    s(s+1)/2 (triangle + diagonal) + s^2 + s = 1.5s^2 + 1.5s
 *     s = 32 -> 2080 / 1584      s = 64 -> 8256 / 6240
 *
 *   AmiSSL: OPENSSL_BN_ASM_MONT is NOT defined for amiga-os3-68020, so
 *   bn_mul_mont_fixed_top() falls through to bn_mul_fixed_top()/
 *   bn_sqr_fixed_top() plus bn_from_montgomery_word().  That means Karatsuba
 *   above 16 limbs and Comba at 8, and it means OpenSSL gets a real squaring
 *   shortcut here.  M(8/16/32/64) = 64/192/576/1728, S = 36/108/324/972;
 *   the reduction is N calls to bn_mul_add_words(N) plus N single multiplies:
 *     multiply  M(N) + N^2 + N     square  S(N) + N^2 + N
 *     N = 32 -> 1632 / 1380      N = 64 -> 5888 / 5132
 *
 *   P-256 field operations cost the SAME on both sides -- 64 multiplies for a
 *   multiply, 36 for a square -- because AmiSSL reaches bn_mul_comba8 /
 *   bn_sqr_comba8 (hand-written 68020 assembly, one mulu.l per product) and
 *   ours reaches eight c68k_addmul_1 rows and a triangle.  The whole
 *   difference is how many field operations each scalar multiplication needs.
 */

#define A_M_MUL32       2080UL      /* our Montgomery multiply, 32 limbs   */
#define A_M_SQR32       1584UL
#define A_M_MUL64       8256UL      /* ... 64 limbs                        */
#define A_M_SQR64       6240UL
#define A_O_MUL32       1632UL      /* AmiSSL's, 32 limbs                  */
#define A_O_SQR32       1380UL
#define A_O_MUL64       5888UL      /* ... 64 limbs                        */
#define A_O_SQR64       5132UL

/*
 * RSA-2048 public, e = 65537 (17 bits, one leading 1 then fifteen 0s then 1).
 *
 *   ours   c68k_window_for(17) = 1, so: acc = x (loaded, not squared), 15
 *          squarings for the zero bits, then 1 squaring + 1 multiply for the
 *          low bit = 16 S + 1 M.  Plus entering the Montgomery domain and
 *          leaving it: 2 M.  Plus the vendored R^2 setup, one 64-limb square
 *          = 64*65/2 = 2080.
 *   AmiSSL BN_mod_exp_mont, window also 1 (BN_window_bits_for_exponent_size),
 *          the same 16 S, 2 M in loop (Shay Gueron's trick makes the initial
 *          r free), 1 M for bn_to_mont_fixed_top and one bare
 *          BN_from_montgomery = N^2 + N.
 */
#define A_MULU_RSAPUB_OURS      ((16UL * A_M_SQR64) + (3UL * A_M_MUL64) + 2080UL)
#define A_MULU_RSAPUB_THEIRS    ((16UL * A_O_SQR64) + (3UL * A_O_MUL64) + 4160UL)

/*
 * RSA-2048 private, CRT: two 1024-bit exponentiations modulo 32-limb primes.
 *
 *   ours   sliding window w = 6 (OpenSSL's own crossover table, and the
 *          scratch is large enough): 31 table multiplies + 1 table squaring,
 *          about 1018 squarings and 146 window multiplies for 1024 bits, and
 *          2 domain conversions.  Plus the R^2 setup twice (2 * 32*33/2).
 *   AmiSSL BN_mod_exp_mont_consttime, FIXED window 6, no zero skipping:
 *          exactly 1021 squarings and 232 multiplies per half (170 windows of
 *          6 squarings + 1 multiply each, a 61-entry table build, and one
 *          bn_to_mont), plus a from_mont.
 *
 * Both then recombine, which is a handful of 32x32 and 64x32 limb products;
 * counted as one 64-limb multiply's worth, which is the right order and is
 * under 0.2% of the total either way.
 */
#define A_MULU_RSACRT_OURS \
    ((2UL * ((1019UL * A_M_SQR32) + (179UL * A_M_MUL32))) + 6000UL)
#define A_MULU_RSACRT_THEIRS \
    ((2UL * ((1021UL * A_O_SQR32) + (233UL * A_O_MUL32))) + 6000UL \
     + A_MULU_RSAPUB_THEIRS)     /* rsa_ossl.c's unconditional verify tail */

/*
 * P-256.  A field multiply is 64 and a field square 36 on both sides, so
 * these are (doublings, additions) priced at the coordinate formulas each
 * side uses.
 *
 *   ours   doubling dbl-2001-b   3M + 5S = 372
 *          mixed add madd-2007-bl 7M + 4S = 592
 *          k*G   Lim-Lee comb, 26 doublings and (on average) 50 additions
 *          k*Q   width-5 wNAF, 256 doublings and about 43 additions, plus an
 *                8-entry affine table (1 doubling, 7 additions, 2 inversions)
 *   AmiSSL ossl_ec_GFp_simple_dbl      4M + 4S = 400
 *          ossl_ec_GFp_simple_add, Z2=1 8M + 3S = 620
 *          ladder step (ecp_smpl.c)   13M + 7S = 1084, and there are exactly
 *                256 of them -- one per bit of the group order, no skipping
 *          k*G and ECDH BOTH take the ladder: ec_mult.c forces it whenever
 *                the scalar could be secret, ignoring BN_FLG_CONSTTIME
 *          ECDSA verify takes interleaved wNAF, width 3 for a 256-bit scalar,
 *                so 256 doublings and about 128 mixed additions, and there is
 *                NO generator precomputation unless the application asked for
 *                it, which nothing in OpenSSL's own ECDSA code does
 */
#define A_P256_D_OURS       372UL
#define A_P256_A_OURS       592UL
#define A_P256_D_THEIRS     400UL
#define A_P256_A_THEIRS     620UL
#define A_P256_LADDER       (256UL * 1084UL)

#define A_MULU_KG_OURS      ((26UL * A_P256_D_OURS) + (50UL * A_P256_A_OURS))
#define A_MULU_KG_THEIRS    A_P256_LADDER

#define A_MULU_ECDH_OURS    ((257UL * A_P256_D_OURS) + (50UL * A_P256_A_OURS))
#define A_MULU_ECDH_THEIRS  A_P256_LADDER

#define A_MULU_ECDSA_OURS   (A_MULU_KG_OURS + A_MULU_ECDH_OURS)
#define A_MULU_ECDSA_THEIRS ((258UL * A_P256_D_THEIRS) + (134UL * A_P256_A_THEIRS))

/* AES and SHA-256 contain no multiplies at all on either side. */
#define A_MULU_NONE         0UL


/*
 * Emulated cost of one MULU.L Dn,Dh:Dl, in picoseconds, measured here.
 *
 * The kernel is inline assembly for the same reason tests/perf/cpucal has a
 * .S file: a C loop that "should" compile to a multiply is a measurement of
 * the compiler, not of the emulator.  Sixteen multiplies per iteration keeps
 * the DBRA out of the answer, and an identical loop with the multiplies
 * removed is subtracted.
 */
static ULONG    a_mulu_ps;

static ULONG a_cal_run(ULONG reps, UINT with_mulu)
{

ULONG   start;
ULONG   ticks;


    start = c68k_eclock();

#ifdef __mc68020__
    if (with_mulu)
    {
        register ULONG r_n __asm("d0") = reps;

        __asm__ __volatile__(
            "       movem.l %%d2-%%d3,-(%%sp)\n"
            "       moveq   #0x55,%%d2\n"
            "       moveq   #0x33,%%d3\n"
            "1:     mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       mulu.l  %%d2,%%d1:%%d3\n"
            "       subq.l  #1,%0\n"
            "       bne.s   1b\n"
            "       movem.l (%%sp)+,%%d2-%%d3\n"
            : "+d" (r_n)
            :
            : "d1", "cc", "memory");
    }
    else
    {
        register ULONG r_n __asm("d0") = reps;

        __asm__ __volatile__(
            "1:     subq.l  #1,%0\n"
            "       bne.s   1b\n"
            : "+d" (r_n)
            :
            : "cc", "memory");
    }
#else
    (VOID)reps;
    (VOID)with_mulu;
#endif

    ticks = c68k_eclock() - start;

    return(ticks);
}

static VOID a_calibrate(VOID)
{

#define A_CAL_REPS      20000UL
#define A_CAL_PER_REP   16UL

ULONG   busy;
ULONG   idle;
ULONG   ns;


    busy = a_cal_run(A_CAL_REPS, 1u);
    idle = a_cal_run(A_CAL_REPS, 0u);

    if (busy <= idle)
    {
        a_mulu_ps = 0UL;
        c68k_log("  MULU.L calibration failed -- corrected times unavailable");
        return;
    }

    /* Nanoseconds for the whole kernel, then picoseconds per multiply.  The
       tick count is around 200,000 here, so this stays inside 32 bits. */
    ns = c68k_eclock_micros(busy - idle);
    a_mulu_ps = (ns * 1000UL) / ((A_CAL_REPS * A_CAL_PER_REP) / 1000UL);

    c68k_log("  MULU.L Dn,Dh:Dl costs %lu.%03lu ns here; a real 68020 charges",
             a_mulu_ps / 1000UL, a_mulu_ps % 1000UL);
    c68k_log("  45 cycles against this model's 32.14, so every measured time");
    c68k_log("  below is short by 0.400 * (MULU.L count) * that figure.");
    c68k_log("  Implied clock, if this really is 32.14 cycles: %lu.%02lu MHz",
             (32140000UL / a_mulu_ps),
             ((32140000UL * 100UL) / a_mulu_ps) % 100UL);
}

/* measured microseconds + the correction, in microseconds. */
static ULONG a_corrected(ULONG measured_us, ULONG mulu)
{

    if ((a_mulu_ps == 0UL) || (mulu == 0UL))
    {
        return(measured_us);
    }

    /* mulu * ps * 0.400 / 1e6 = microseconds.  Ordered to stay in 32 bits:
       (mulu/1000) * ps * 2 / 5000. */
    return(measured_us + ((((mulu + 999UL) / 1000UL) * a_mulu_ps * 2UL) / 5000UL));
}


/* -------------------------------------------------------------- timing --- */

typedef VOID (*A_OP)(VOID);

static ULONG a_time_n(A_OP op, ULONG n)
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

/*
 * ours / theirs, to one decimal, printed as a ratio in whichever direction
 * makes it a number above 1 -- because "0.4x" and "2.5x" are the same fact and
 * only one of them can be read at a glance.
 */
static VOID a_ratio_line(const char *what, const char *name,
                         ULONG ours, ULONG theirs)
{

ULONG   ratio;


    if ((ours == 0UL) || (theirs == 0UL))
    {
        c68k_log("  %s %s: ours %lu us, AmiSSL %lu us",
                 (LONG)name, (LONG)what, ours, theirs);
        return;
    }

    if (theirs >= ours)
    {
        ratio = (theirs * 10UL) / ours;
        c68k_log("  %s %s: ours %lu us  AmiSSL %lu us   OURS %lu.%lux",
                 (LONG)name, (LONG)what, ours, theirs,
                 ratio / 10UL, ratio % 10UL);
    }
    else
    {
        ratio = (ours * 10UL) / theirs;
        c68k_log("  %s %s: ours %lu us  AmiSSL %lu us   AMISSL %lu.%lux",
                 (LONG)name, (LONG)what, ours, theirs,
                 ratio / 10UL, ratio % 10UL);
    }
}

static VOID a_report(const char *name, ULONG ours, ULONG theirs,
                     ULONG ours_mulu, ULONG theirs_mulu, UINT counted)
{

    if (a_row_count < A_MAX_ROWS)
    {
        a_rows[a_row_count].name        = name;
        a_rows[a_row_count].ours_us     = ours;
        a_rows[a_row_count].theirs_us   = theirs;
        a_rows[a_row_count].ours_mulu   = ours_mulu;
        a_rows[a_row_count].theirs_mulu = theirs_mulu;
        a_rows[a_row_count].counted     = counted;
        a_row_count++;
    }

    a_ratio_line("measured ", name, ours, theirs);

    if ((ours_mulu != 0UL) || (theirs_mulu != 0UL))
    {
        a_ratio_line("corrected", name,
                     a_corrected(ours, ours_mulu),
                     a_corrected(theirs, theirs_mulu));
        c68k_log("      MULU.L per operation: ours %lu, AmiSSL %lu",
                 ours_mulu, theirs_mulu);
    }
}

static VOID a_check(const char *what, const UCHAR *a, const UCHAR *b, UINT n)
{

UINT    i;


    for (i = 0; i < n; i++)
    {
        if (a[i] != b[i])
        {
            c68k_log("  MISMATCH %s at byte %lu: ours %02lx AmiSSL %02lx",
                     (LONG)what, (ULONG)i, (ULONG)a[i], (ULONG)b[i]);
            a_mismatches++;
            a_failures++;
            return;
        }
    }
}


/* ------------------------------------------------------- limb <-> bytes --- */

/*
 * The vectors are little-endian 32-bit limbs because that is HN_UBASE order;
 * OpenSSL wants big-endian bytes.  One place for the conversion, because
 * getting it backwards produces a number that is still 2048 bits long and
 * still exponentiates, and the only symptom is that nothing agrees.
 */
static VOID a_limbs_to_be(UCHAR *out, const c68k_limb *v, UINT limbs)
{

UINT        i;
UINT        j;
c68k_limb   w;


    for (i = 0; i < limbs; i++)
    {
        w = v[limbs - 1u - i];
        j = i << 2;
        out[j]     = (UCHAR)(w >> 24);
        out[j + 1] = (UCHAR)(w >> 16);
        out[j + 2] = (UCHAR)(w >> 8);
        out[j + 3] = (UCHAR)(w);
    }
}


/* ============================================================ RSA public == */

static VOID a_ours_rsa_public(VOID)
{

UINT    i;


    for (i = 0; i < 64u; i++)
    {
        a_x_copy[i] = t_msg[i];
        a_e_copy[i] = (i < 1u) ? t_e[i] : 0u;
    }

    c68k_hn_set(&a_x_hn, a_x_copy, 64u, A_MAX_LIMBS);
    c68k_hn_set(&a_e_hn, a_e_copy, 1u,  A_MAX_LIMBS);
    c68k_hn_set(&a_m_hn, a_m,      64u, 64u);
    c68k_hn_set(&a_r_hn, a_result, 0,   (A_MAX_LIMBS * 2) + 8);

    c68k_huge_number_mont_power_modulus(&a_x_hn, &a_e_hn, &a_m_hn, &a_r_hn,
                                        a_scratch, A_POWM_SCRATCH);
}

static VOID a_theirs_modexp_pub_fresh(VOID)
{

    if (a_ossl_modexp_public(0, a_ossl_out) != 0)
    {
        a_failures++;
    }
}

static VOID a_theirs_modexp_pub_cached(VOID)
{

    if (a_ossl_modexp_public(1, a_ossl_out) != 0)
    {
        a_failures++;
    }
}

static VOID a_theirs_rsa_public(VOID)
{

    if (a_ossl_rsa_public(a_be_msg, a_ossl_out) != 0)
    {
        a_failures++;
    }
}

static VOID a_bench_rsa_public(VOID)
{

ULONG   ours;
ULONG   theirs;
ULONG   cached;
ULONG   viarsa;


    c68k_log("");
    c68k_log("1. RSA-2048 public operation, e = 65537");

    ours = a_time_n(a_ours_rsa_public, 3UL);
    a_limbs_to_be(a_ours_out, a_result, 64u);
    a_check("ours RSA public vs known answer", a_ours_out, a_be_pub_expected, 256u);

    theirs = a_time_n(a_theirs_modexp_pub_fresh, 3UL);
    a_check("RSA public", a_ours_out, a_ossl_out, 256u);

    cached = a_time_n(a_theirs_modexp_pub_cached, 3UL);
    viarsa = a_time_n(a_theirs_rsa_public, 3UL);

    a_report("BN_mod_exp_mont, ctx built per call", ours, theirs,
             A_MULU_RSAPUB_OURS, A_MULU_RSAPUB_THEIRS, 1u);
    c68k_log("    the same with a cached BN_MONT_CTX: %lu us "
             "(setup is %lu us of it)",
             cached, (theirs > cached) ? (theirs - cached) : 0UL);
    c68k_log("    through RSA_public_encrypt(NO_PADDING): %lu us", viarsa);
    c68k_log("    ours rebuilds R^2 mod m on every call, so the "
             "per-call column is the like-for-like one");
}


/* =========================================================== RSA private == */

static VOID a_ours_rsa_private(VOID)
{

UINT    i;


    for (i = 0; i < 64u; i++)
    {
        a_x_copy[i] = t_msg[i];
        a_e_copy[i] = t_d[i];
    }

    c68k_hn_set(&a_x_hn, a_x_copy, 64u, A_MAX_LIMBS);
    c68k_hn_set(&a_e_hn, a_e_copy, 64u, A_MAX_LIMBS);
    c68k_hn_set(&a_m_hn, a_m,      64u, 64u);
    c68k_hn_set(&a_p_hn, a_p,      32u, 32u);
    c68k_hn_set(&a_q_hn, a_q,      32u, 32u);
    c68k_hn_set(&a_r_hn, a_result, 0,   (A_MAX_LIMBS * 2) + 8);

    c68k_crt_power_modulus(&a_x_hn, &a_e_hn, &a_p_hn, &a_q_hn, &a_m_hn,
                           &a_r_hn, a_hn_scratch,
                           a_scratch, A_POWM_SCRATCH);
}

static VOID a_theirs_rsa_private(VOID)
{

    if (a_ossl_rsa_private(a_be_msg, a_ossl_out) != 0)
    {
        a_failures++;
    }
}

static VOID a_theirs_modexp_priv(VOID)
{

    if (a_ossl_modexp_private(1, a_be_d, a_ossl_out) != 0)
    {
        a_failures++;
    }
}

static VOID a_bench_rsa_private(VOID)
{

ULONG   ours;
ULONG   noblind;
ULONG   blind;
ULONG   plain;


    c68k_log("");
    c68k_log("2. RSA-2048 private operation, CRT");

    ours = a_time_n(a_ours_rsa_private, 1UL);
    a_limbs_to_be(a_ours_out, a_result, 64u);
    a_check("ours RSA private vs known answer", a_ours_out,
            a_be_priv_expected, 256u);

    /* Blinding off first, because that is the arithmetic comparison; the
       blinded figure follows and is what OpenSSL does by default. */
    if (a_ossl_rsa_setup(a_be_n, a_be_e, a_be_d, a_be_p, a_be_q, 0) != 0)
    {
        c68k_log("  FATAL: AmiSSL RSA key setup failed");
        a_failures++;
        return;
    }
    noblind = a_time_n(a_theirs_rsa_private, 1UL);
    a_check("RSA private CRT", a_ours_out, a_ossl_out, 256u);

    a_report("RSA-2048 private CRT, no blinding  ", ours, noblind,
             A_MULU_RSACRT_OURS, A_MULU_RSACRT_THEIRS, 1u);

    if (a_ossl_rsa_setup(a_be_n, a_be_e, a_be_d, a_be_p, a_be_q, 1) != 0)
    {
        c68k_log("  FATAL: AmiSSL RSA key setup (blinded) failed");
        a_failures++;
        return;
    }
    blind = a_time_n(a_theirs_rsa_private, 1UL);
    a_check("RSA private CRT, blinded", a_ours_out, a_ossl_out, 256u);
    c68k_log("    OpenSSL's DEFAULT (blinding on): %lu us, "
             "which is %lu us of side-channel defence we do not pay",
             blind, (blind > noblind) ? (blind - noblind) : 0UL);

    if (a_full)
    {
        /*
         * The path nx_secure takes when it hands over a full 2048-bit private
         * exponent with no primes -- what our own tree does on two of its
         * three private-key call sites (see c68k_crt.c).  Measured on the
         * AmiSSL side so the cost of NOT using CRT is a number and not a
         * recollection.
         */
        plain = a_time_n(a_theirs_modexp_priv, 1UL);
        c68k_log("    AmiSSL, no CRT, BN_FLG_CONSTTIME, 2048-bit exponent: "
                 "%lu us", plain);
    }
}


/* ================================================================= P-256 == */

static VOID a_ours_ecdsa(VOID)
{

    if (_nx_crypto_ecdsa_verify(&a_our_curve, (UCHAR *)v_hash_sample, 32u,
                                (UCHAR *)v_pubkey, 65u,
                                (UCHAR *)v_sig_sample,
                                (UINT)sizeof(v_sig_sample),
                                a_ecdsa_scratch) != NX_CRYPTO_SUCCESS)
    {
        a_failures++;
    }
}

static VOID a_theirs_ecdsa(VOID)
{

    if (a_ossl_ecdsa_verify(v_hash_sample, v_sig_sample,
                            (int)sizeof(v_sig_sample)) != 0)
    {
        a_failures++;
    }
}

static VOID a_ours_ecdh(VOID)
{

ULONG   len;


    (VOID) _nx_crypto_ecdh_compute_secret(&a_ecdh, a_ours_secret, 32UL, &len,
                                          (UCHAR *)v_ecdh_peer, 65UL,
                                          a_ecdh.nx_crypto_ecdh_scratch_buffer);
}

static VOID a_theirs_ecdh(VOID)
{

    if (a_ossl_ecdh(v_ecdh_peer, a_theirs_secret) != 0)
    {
        a_failures++;
    }
}

static VOID a_ours_kg(VOID)
{

    a_our_curve.nx_crypto_ec_multiple(&a_our_curve, &a_our_curve.nx_crypto_ec_g,
                                      &a_d, &a_point, a_ec_scratch);
}

static VOID a_theirs_kg_var(VOID)
{

    if (a_ossl_ec_mul_g(a_kg_scalar, 0, a_theirs_g) != 0)
    {
        a_failures++;
    }
}

static VOID a_theirs_kg_ct(VOID)
{

    if (a_ossl_ec_mul_g(a_kg_scalar, 1, a_theirs_g) != 0)
    {
        a_failures++;
    }
}

static VOID a_point_to_oct(UCHAR *out, const NX_CRYPTO_EC_POINT *pt)
{

UINT        i;
c68k_limb   v[8];


    out[0] = 0x04;

    for (i = 0; i < 8u; i++)
    {
        v[i] = (i < pt -> nx_crypto_ec_point_x.nx_crypto_huge_number_size) ?
               pt -> nx_crypto_ec_point_x.nx_crypto_huge_number_data[i] : 0UL;
    }
    a_limbs_to_be(&out[1], v, 8u);

    for (i = 0; i < 8u; i++)
    {
        v[i] = (i < pt -> nx_crypto_ec_point_y.nx_crypto_huge_number_size) ?
               pt -> nx_crypto_ec_point_y.nx_crypto_huge_number_data[i] : 0UL;
    }
    a_limbs_to_be(&out[33], v, 8u);
}

static VOID a_bench_ec(VOID)
{

ULONG      ours;
ULONG      theirs;
ULONG      theirs_ct;
UINT       i;
HN_UBASE  *sp;


    c68k_log("");
    c68k_log("3. P-256");

    /* ---- ECDSA verify ---- */
    ours   = a_time_n(a_ours_ecdsa,   2UL);
    theirs = a_time_n(a_theirs_ecdsa, 2UL);
    a_report("ECDSA P-256 verify (RFC 6979 A.2.5)", ours, theirs,
             A_MULU_ECDSA_OURS, A_MULU_ECDSA_THEIRS, 1u);

    /* ---- ECDH shared secret ---- */
    ours   = a_time_n(a_ours_ecdh,   2UL);
    theirs = a_time_n(a_theirs_ecdh, 2UL);
    a_check("ECDH shared secret", a_ours_secret, a_theirs_secret, 32u);
    a_check("ours ECDH vs known answer", a_ours_secret, v_ecdh_secret, 32u);
    a_report("ECDH P-256 shared secret           ", ours, theirs,
             A_MULU_ECDH_OURS, A_MULU_ECDH_THEIRS, 1u);

    /* ---- k*G, which is what a key generation costs ---- */
    sp = a_ec_store;
    NX_CRYPTO_EC_POINT_INITIALIZE(&a_point, NX_CRYPTO_EC_POINT_AFFINE, sp, 32);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&a_d, sp, 32);

    /* A fixed scalar, so the two sides multiply the same number and the
       comparison of their answers means something. */
    c68k_rng_seed(0x5EEDU);
    for (i = 0; i < 8u; i++)
    {
        a_d.nx_crypto_huge_number_data[i] = c68k_rand();
    }
    a_d.nx_crypto_huge_number_data[7] &= 0x7FFFFFFFUL;
    a_d.nx_crypto_huge_number_size = 8u;
    a_d.nx_crypto_huge_number_is_negative = NX_CRYPTO_FALSE;
    a_limbs_to_be(a_kg_scalar, a_d.nx_crypto_huge_number_data, 8u);

    ours = a_time_n(a_ours_kg, 2UL);
    a_point_to_oct(a_ours_g, &a_point);

    theirs = a_time_n(a_theirs_kg_var, 2UL);
    a_check("k*G", a_ours_g, a_theirs_g, 65u);

    theirs_ct = a_time_n(a_theirs_kg_ct, 2UL);
    a_check("k*G constant time", a_ours_g, a_theirs_g, 65u);

    a_report("k*G  (an ECDHE key generation)      ", ours, theirs,
             A_MULU_KG_OURS, A_MULU_KG_THEIRS, 1u);
    c68k_log("    the same with BN_FLG_CONSTTIME on the scalar: %lu us",
             theirs_ct);
    c68k_log("    generator precomputation table present: %s",
             (LONG)(a_ossl_ec_have_precompute() ? "yes" : "no"));
}


/* ================================================================== bulk == */

static VOID a_ours_aes(VOID)
{

    (VOID) _nx_crypto_cbc_encrypt_init(&a_cbc, a_aes_iv, 16u);
    (VOID) _nx_crypto_cbc_encrypt(&a_aes, &a_cbc,
                                  (UINT (*)(VOID *, UCHAR *, UCHAR *, UINT))
                                      _nx_crypto_aes_encrypt,
                                  a_bulk_in, a_bulk_ours, A_BULK_BYTES, 16u);
}

static VOID a_theirs_aes(VOID)
{

    if (a_ossl_aes128_cbc(a_aes_key, a_aes_iv, a_bulk_in, a_bulk_theirs,
                          A_BULK_BYTES) != 0)
    {
        a_failures++;
    }
}

static VOID a_ours_hmac(VOID)
{

    (VOID) crypto_method_hmac_sha256.nx_crypto_operation(
                NX_CRYPTO_AUTHENTICATE, NX_CRYPTO_NULL,
                &crypto_method_hmac_sha256,
                a_mac_key, 256u,
                a_bulk_in, A_BULK_BYTES, NX_CRYPTO_NULL,
                a_mac_ours, 32UL,
                &a_hmac_meta, sizeof(a_hmac_meta),
                NX_CRYPTO_NULL, NX_CRYPTO_NULL);
}

static VOID a_theirs_hmac(VOID)
{

    if (a_ossl_hmac_sha256(a_mac_key, 32, a_bulk_in, A_BULK_BYTES,
                           a_mac_theirs) != 0)
    {
        a_failures++;
    }
}

static ULONG a_kbs(ULONG micros)
{

    /* bytes/second = A_BULK_BYTES * 1e6 / micros, then to KiB/s.  Ordered to
       stay inside 32 bits: A_BULK_BYTES*1000 is 16.4 million. */
    if (micros == 0UL)
    {
        return(0UL);
    }

    return(((A_BULK_BYTES * 1000UL) / micros) * 1000UL / 1024UL);
}

static VOID a_bench_bulk(VOID)
{

ULONG   ours;
ULONG   theirs;
UINT    i;


    c68k_log("");
    c68k_log("4. The bulk path, %lu bytes a call", A_BULK_BYTES);

    for (i = 0; i < (UINT)A_BULK_BYTES; i++)
    {
        a_bulk_in[i] = (UCHAR)(i ^ (i >> 8));
    }
    for (i = 0; i < 16u; i++)
    {
        a_aes_key[i] = (UCHAR)(0x10u + i);
        a_aes_iv[i]  = (UCHAR)(0xA0u + i);
    }
    for (i = 0; i < 32u; i++)
    {
        a_mac_key[i] = (UCHAR)(0x40u + i);
    }

    if (_nx_crypto_aes_key_set(&a_aes, a_aes_key,
                               NX_CRYPTO_AES_KEY_SIZE_128_BITS) !=
        NX_CRYPTO_SUCCESS)
    {
        c68k_log("  FATAL: our AES key schedule failed");
        a_failures++;
        return;
    }

    ours   = a_time_n(a_ours_aes,   4UL);
    theirs = a_time_n(a_theirs_aes, 4UL);
    a_check("AES-128-CBC ciphertext", a_bulk_ours, a_bulk_theirs,
            (UINT)A_BULK_BYTES);
    a_report("AES-128-CBC encrypt                ", ours, theirs,
             A_MULU_NONE, A_MULU_NONE, 2u);
    c68k_log("    ours %lu KB/s, AmiSSL %lu KB/s",
             a_kbs(ours), a_kbs(theirs));

    if (crypto_method_hmac_sha256.nx_crypto_init(&crypto_method_hmac_sha256,
                                                 a_mac_key, 256u,
                                                 NX_CRYPTO_NULL,
                                                 &a_hmac_meta,
                                                 sizeof(a_hmac_meta)) !=
        NX_CRYPTO_SUCCESS)
    {
        c68k_log("  FATAL: our HMAC-SHA256 init failed");
        a_failures++;
        return;
    }

    ours   = a_time_n(a_ours_hmac,   4UL);
    theirs = a_time_n(a_theirs_hmac, 4UL);
    a_check("HMAC-SHA256 tag", a_mac_ours, a_mac_theirs, 32u);
    a_report("HMAC-SHA256                        ", ours, theirs,
             A_MULU_NONE, A_MULU_NONE, 2u);
    c68k_log("    ours %lu KB/s, AmiSSL %lu KB/s",
             a_kbs(ours), a_kbs(theirs));
}


/* =============================================================== summary == */

static VOID a_summary(VOID)
{

UINT    i;
ULONG   ours   = 0UL;
ULONG   theirs = 0UL;
ULONG   ratio;


    c68k_log("");
    c68k_log("5. What a client actually pays");

    /*
     * An ECDHE_ECDSA handshake against a two-certificate chain: two ECDSA
     * verifies for the chain, one for the ServerKeyExchange signature, one
     * key generation and one shared secret.  Composed from the rows above
     * rather than from memory, so it cannot drift from what was measured.
     */
    for (i = 0; i < a_row_count; i++)
    {
        if (a_rows[i].counted != 1u)
        {
            continue;
        }

        c68k_log("    %-36s ours %8lu us  AmiSSL %8lu us",
                 (LONG)a_rows[i].name, a_rows[i].ours_us,
                 a_rows[i].theirs_us);

        ours   += a_rows[i].ours_us;
        theirs += a_rows[i].theirs_us;
    }

    if ((ours != 0UL) && (theirs != 0UL))
    {
        if (theirs >= ours)
        {
            ratio = (theirs * 10UL) / ours;
            c68k_log("    handshake-shaped total: ours %lu ms, "
                     "AmiSSL %lu ms  = OURS %lu.%lux",
                     ours / 1000UL, theirs / 1000UL,
                     ratio / 10UL, ratio % 10UL);
        }
        else
        {
            ratio = (ours * 10UL) / theirs;
            c68k_log("    handshake-shaped total: ours %lu ms, "
                     "AmiSSL %lu ms  = AMISSL %lu.%lux",
                     ours / 1000UL, theirs / 1000UL,
                     ratio / 10UL, ratio % 10UL);
        }
        c68k_log("    (one of each primitive, NOT a whole handshake -- "
                 "no network, no parsing, no framing)");
    }
}


/* ================================================================== body == */

static int a_body(VOID)
{

char    err[128];
UINT    status;
UINT    i;
ULONG   len;
ULONG   start;


    c68k_log("AmiNetXDuo -- crypto68k against AmiSSL, one process");
    c68k_log("  our limb primitives: %s",
             (LONG)(c68k_using_assembly() ? "68020 assembly" : "portable C"));

    if (!c68k_timer_open())
    {
        c68k_log("FATAL: timer.device UNIT_ECLOCK would not open");
        return(20);
    }
    c68k_log("  E-Clock %lu Hz", c68k_eclock_hz());

    a_calibrate();

    /*
     * Timed and logged in two stages.  The first version of this program sat
     * silent for minutes here and there was no way to tell whether it was
     * LoadSeg on a 3.5 MB library, OpenSSL 3.x's own initialisation, or a
     * hang.  It is also a number worth having: on this machine a program that
     * opens AmiSSL pays this before it does any crypto at all.
     */
    c68k_log("");
    c68k_log("0. Opening AmiSSL (LoadSeg of a 3.5 MB library, then OpenSSL "
             "3.x init)");

    /*
     * Read the library file first, and throw the bytes away.
     *
     * Not idle curiosity: the first run of this program sat inside
     * OpenAmiSSLTagList for minutes, and there are two very different
     * explanations -- AmigaDOS reading and relocating 3.4 MB of hunk file
     * through FS-UAE's directory filesystem, or OpenSSL 3.x initialising
     * itself.  One is an artefact of the harness and one is a fact about the
     * machine.  This measures the first so the difference is the second.
     */
    {
        BPTR    fh;
        LONG    n;
        ULONG   total = 0UL;
        ULONG   ms;

        start = c68k_eclock();
        fh = Open((STRPTR)"LIBS:AmiSSL/amissl_v362.library", MODE_OLDFILE);
        if (fh != (BPTR)0)
        {
            do
            {
                n = Read(fh, a_bulk_in, (LONG)A_BULK_BYTES);
                if (n > 0)
                {
                    total += (ULONG)n;
                }
            } while (n > 0);
            Close(fh);

            ms = c68k_eclock_millis(c68k_eclock() - start);
            c68k_log("  reading %lu bytes of it off DH0: took %lu ms (%lu KB/s)",
                     total, ms,
                     (ms != 0UL) ? ((total / ms) * 1000UL / 1024UL) : 0UL);
        }
        else
        {
            c68k_log("  (could not open the library file to time the read)");
        }
    }

    start = c68k_eclock();
    if (a_ossl_open_master(err, sizeof(err)) != 0)
    {
        c68k_log("FATAL: %s", (LONG)err);
        c68k_timer_close();
        return(20);
    }
    c68k_log("  amisslmaster.library open: %lu ms",
             c68k_eclock_millis(c68k_eclock() - start));

    start = c68k_eclock();
    if (a_ossl_open_ssl(err, sizeof(err)) != 0)
    {
        c68k_log("FATAL: %s", (LONG)err);
        a_ossl_close();
        c68k_timer_close();
        return(20);
    }
    c68k_log("  OpenAmiSSL (LoadSeg of amissl_v362.library): %lu ms",
             c68k_eclock_millis(c68k_eclock() - start));

    start = c68k_eclock();
    if (a_ossl_init_ssl(err, sizeof(err)) != 0)
    {
        c68k_log("FATAL: %s", (LONG)err);
        a_ossl_close();
        c68k_timer_close();
        return(20);
    }
    c68k_log("  InitAmiSSL (per-process OpenSSL 3.x init): %lu ms",
             c68k_eclock_millis(c68k_eclock() - start));

    c68k_log("  AmiSSL: %s", (LONG)a_ossl_version());
    c68k_log("  library: %s", (LONG)a_ossl_library_id());
    c68k_log("  mode: %s", (LONG)(a_full ? "full" : "quick"));
    c68k_log("");
    c68k_log("  Every operation is checked against the other side's answer");
    c68k_log("  before it is timed.  Timings are the emulator's 68020, whose");
    c68k_log("  MULU.L is 32 cycles against a real part's 43 -- see the");
    c68k_log("  write-up for the correction.");

    /* ------------------------------------------------------------ RSA -- */

    for (i = 0; i < 64u; i++)
    {
        a_m[i] = t_n[i];
    }
    for (i = 0; i < 32u; i++)
    {
        a_p[i] = t_p[i];
        a_q[i] = t_q[i];
    }

    a_limbs_to_be(a_be_n, t_n, 64u);
    a_limbs_to_be(a_be_d, t_d, 64u);
    a_limbs_to_be(a_be_p, t_p, 32u);
    a_limbs_to_be(a_be_q, t_q, 32u);
    a_limbs_to_be(a_be_msg, t_msg, 64u);
    a_limbs_to_be(a_be_pub_expected, t_msg_pub, 64u);
    a_limbs_to_be(a_be_priv_expected, t_msg_priv, 64u);

    if (a_ossl_modexp_setup(a_be_n, a_be_msg) != 0)
    {
        c68k_log("FATAL: AmiSSL bignum setup failed");
        a_failures++;
    }
    else if (a_ossl_rsa_setup(a_be_n, a_be_e, a_be_d, a_be_p, a_be_q, 0) != 0)
    {
        c68k_log("FATAL: AmiSSL RSA key setup failed");
        a_failures++;
    }
    else
    {
        a_bench_rsa_public();
        a_bench_rsa_private();
    }

    /* ---------------------------------------------------------- P-256 -- */

    a_ref_curve = NX_CRYPTO_NULL;
    status = _nx_crypto_ec_get_named_curve(&a_ref_curve, NX_CRYPTO_EC_SECP256R1);
    if ((status != NX_CRYPTO_SUCCESS) || (a_ref_curve == NX_CRYPTO_NULL))
    {
        c68k_log("FATAL: secp256r1 not available");
        a_failures++;
    }
    else
    {
        a_our_curve = *a_ref_curve;
        a_our_curve.nx_crypto_ec_multiple = c68k_p256_ec_multiple;

        if (c68k_p256_self_check() != NX_CRYPTO_SUCCESS)
        {
            c68k_log("FATAL: comb table self check failed");
            a_failures++;
        }

        /* Same wiring the EC benchmark uses: key_pair_import sets the key
           size but not the curve pointer. */
        a_ecdh.nx_crypto_ecdh_curve = &a_our_curve;
        (VOID) _nx_crypto_ecdh_key_pair_import(&a_ecdh, &a_our_curve,
                                               (UCHAR *)v_ecdh_priv, 32UL,
                                               (UCHAR *)v_ecdh_pub, 65UL);
        (VOID) _nx_crypto_ecdh_compute_secret(&a_ecdh, a_ours_secret, 32UL,
                                              &len, (UCHAR *)v_ecdh_peer, 65UL,
                                              a_ecdh.nx_crypto_ecdh_scratch_buffer);
        a_check("ours ECDH vs published answer", a_ours_secret,
                v_ecdh_secret, 32u);

        if (a_ossl_ec_setup(v_pubkey, v_ecdh_priv, v_ecdh_pub) != 0)
        {
            c68k_log("FATAL: AmiSSL EC setup failed");
            a_failures++;
        }
        else
        {
            a_bench_ec();
        }
    }

    /* ----------------------------------------------------------- bulk -- */

    a_bench_bulk();

    a_summary();

    /* ------------------------------------------------------------ end -- */

    a_ossl_ec_free();
    a_ossl_rsa_free();
    a_ossl_modexp_free();
    a_ossl_close();
    c68k_timer_close();

    c68k_log("");
    if (a_failures == 0UL)
    {
        c68k_log("==> 0 failures, 0 mismatches");
        return(0);
    }

    c68k_log("==> %lu failures (%lu of them answer mismatches)",
             a_failures, a_mismatches);
    return(20);
}


/* ================================================================== main == */

/*
 * OpenSSL cannot run on a boot Shell's 4 KB stack, and this toolchain's crt0
 * exports no __stack hook to ask for more (docs/RESEARCH.md 11.5).  So the
 * work happens in a process created with NP_StackSize and main() waits for it.
 * Everything AmiSSL touches -- InitAmiSSL is per process -- happens inside
 * that process, which is also what the autodoc requires.
 */

#define A_CHILD_STACK   262144UL

static struct Task *a_parent;
static ULONG        a_sigmask;
static int          a_child_rc = 20;

static VOID a_child(VOID)
{

    a_child_rc = a_body();
    Signal(a_parent, a_sigmask);
}

int main(VOID)
{

struct TagItem  tags[5];
BYTE            sig;
char            var[16];


    ami_crash_set_reference((APTR)main, "crypto68k_amissl");
    if (!ami_crash_install())
    {
        c68k_log("CRASHED -- see the serial log and DH0:crash.txt");
        c68k_flush();
        ami_crash_remove();
        return(20);
    }

    /*
     * The slow extras are opt-in through an environment variable rather than
     * an argument, because the harness's Startup-Sequence runs the executable
     * with no command line and this toolchain hands main() the wrong argv
     * anyway (docs/RESEARCH.md 11.3).  Stage an env/ directory to set it.
     */
    a_full = 0;
    if (GetVar((STRPTR)"C68K_AMISSL", (STRPTR)var, sizeof(var), 0UL) > 0)
    {
        if ((var[0] == 'f') || (var[0] == 'F'))
        {
            a_full = 1;
        }
    }

    sig = AllocSignal(-1);
    if (sig < 0)
    {
        c68k_log("FATAL: no signal available");
        c68k_flush();
        ami_crash_remove();
        return(20);
    }

    a_parent  = FindTask(NULL);
    a_sigmask = 1UL << (ULONG)sig;

    tags[0].ti_Tag  = NP_Entry;
    tags[0].ti_Data = (ULONG)a_child;
    tags[1].ti_Tag  = NP_Name;
    tags[1].ti_Data = (ULONG)"crypto68k_amissl";
    tags[2].ti_Tag  = NP_StackSize;
    tags[2].ti_Data = A_CHILD_STACK;
    tags[3].ti_Tag  = NP_Priority;
    tags[3].ti_Data = 0UL;
    tags[4].ti_Tag  = TAG_DONE;
    tags[4].ti_Data = 0UL;

    if (CreateNewProc(tags) == NULL)
    {
        c68k_log("FATAL: CreateNewProc failed");
        c68k_flush();
        FreeSignal(sig);
        ami_crash_remove();
        return(20);
    }

    (VOID) Wait(a_sigmask);
    FreeSignal(sig);

    /* The child logs into the shared buffer; the parent has the file handle,
       so the flush belongs here. */
    c68k_flush();
    ami_crash_remove();

    return(a_child_rc);
}
