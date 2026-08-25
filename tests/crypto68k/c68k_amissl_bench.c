/*
 * AmiNetXDuo, crypto68k against AmiSSL, one process, identical inputs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "c68k_support.h"
#include "c68k_timer.h"
#include "c68k_p256.h"
#include "c68k_aes.h"
#include "c68k_sha256.h"
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
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "c68k_vectors.h"
#include "c68k_ec_vectors.h"

/* nx_crypto's shipped method table entry.  Declared here for the same reason
   src/tls/ami_tls_crypto.c declares it: it is defined in
   nx_crypto_generic_ciphersuites.c and no header exports it. */
extern NX_CRYPTO_METHOD crypto_method_hmac_sha256;

#define A_MAX_LIMBS         64u
#define A_POWM_SCRATCH      4096u
#define A_HN_SCRATCH        4096u

/* 16 KiB is one TLS record's worth of payload and then some, RFC 5246 caps
   a plaintext fragment at 2^14, so this is the unit the bulk path actually
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

static c68k_limb    a_kx[64];
static c68k_limb    a_km[64];
static c68k_limb    a_kr[(64 * 2) + 8];
static UINT         a_klen;
static c68k_limb    a_kn0;

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

static UCHAR                a_bulk_in[A_BULK_BYTES];
static UCHAR                a_bulk_ours[A_BULK_BYTES];
static UCHAR                a_bulk_theirs[A_BULK_BYTES];
static UCHAR                a_aes_key[16];
static UCHAR                a_aes_iv[16];
static UCHAR                a_mac_key[32];
static UCHAR                a_mac_ours[32];
static UCHAR                a_mac_theirs[32];
static C68K_AES             a_aes;
static UCHAR                a_iv_work[16];
static NX_CRYPTO_HMAC       a_hmac_meta;
static C68K_SHA256          a_hmac_ctx;
static UCHAR                a_bulk_plain[A_BULK_BYTES];

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

#define A_M_MUL32       2080UL      /* our Montgomery multiply, 32 limbs   */
#define A_M_SQR32       1584UL
#define A_M_MUL64       8256UL      /* ... 64 limbs                        */
#define A_M_SQR64       6240UL
#define A_O_MUL32       1632UL      /* AmiSSL's, 32 limbs                  */
#define A_O_SQR32       1380UL
#define A_O_MUL64       5888UL      /* ... 64 limbs                        */
#define A_O_SQR64       5132UL

#define A_M_SQR64_KAR   ((3UL * 528UL) + 4160UL)        /* 5744 */
#define A_M_MUL64_KAR   ((3UL * 1024UL) + 4160UL)       /* 7232 */

#define A_MULU_RSAPUB_OURS      ((16UL * A_M_SQR64_KAR) + (3UL * A_M_MUL64_KAR) + 2080UL + 4288UL + 136UL)
#define A_MULU_RSAPUB_THEIRS    ((16UL * A_O_SQR64) + (3UL * A_O_MUL64) + 4160UL)

#define A_MULU_RSACRT_OURS \
    ((2UL * ((1019UL * A_M_SQR32) + (179UL * A_M_MUL32))) + 6000UL)
#define A_MULU_RSACRT_THEIRS \
    ((2UL * ((1021UL * A_O_SQR32) + (233UL * A_O_MUL32))) + 6000UL \
     + A_MULU_RSAPUB_THEIRS)     /* rsa_ossl.c's unconditional verify tail */

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

static ULONG    a_divu_ps;

static ULONG a_cal_divu(ULONG reps, UINT with_divu)
{

ULONG   start;
ULONG   ticks;


    start = c68k_eclock();

#ifdef __mc68020__
    if (with_divu)
    {
        register ULONG r_n __asm("d0") = reps;

        __asm__ __volatile__(
            "       movem.l %%d2-%%d4,-(%%sp)\n"
            "       moveq   #1,%%d1\n"
            "       move.l  #0x55555555,%%d2\n"
            "       move.l  #0x7FFFFFFF,%%d3\n"
            "1:     move.l  %%d1,%%d4\n"
            "       divu.l  %%d3,%%d4:%%d2\n"
            "       move.l  %%d1,%%d4\n"
            "       divu.l  %%d3,%%d4:%%d2\n"
            "       move.l  %%d1,%%d4\n"
            "       divu.l  %%d3,%%d4:%%d2\n"
            "       move.l  %%d1,%%d4\n"
            "       divu.l  %%d3,%%d4:%%d2\n"
            "       move.l  %%d1,%%d4\n"
            "       divu.l  %%d3,%%d4:%%d2\n"
            "       move.l  %%d1,%%d4\n"
            "       divu.l  %%d3,%%d4:%%d2\n"
            "       move.l  %%d1,%%d4\n"
            "       divu.l  %%d3,%%d4:%%d2\n"
            "       move.l  %%d1,%%d4\n"
            "       divu.l  %%d3,%%d4:%%d2\n"
            "       subq.l  #1,%0\n"
            "       bne.s   1b\n"
            "       movem.l (%%sp)+,%%d2-%%d4\n"
            : "+d" (r_n)
            :
            : "d1", "cc", "memory");
    }
    else
    {
        register ULONG r_n __asm("d0") = reps;

        __asm__ __volatile__(
            "       movem.l %%d2-%%d4,-(%%sp)\n"
            "       moveq   #1,%%d1\n"
            "1:     move.l  %%d1,%%d4\n"
            "       move.l  %%d1,%%d4\n"
            "       move.l  %%d1,%%d4\n"
            "       move.l  %%d1,%%d4\n"
            "       move.l  %%d1,%%d4\n"
            "       move.l  %%d1,%%d4\n"
            "       move.l  %%d1,%%d4\n"
            "       move.l  %%d1,%%d4\n"
            "       subq.l  #1,%0\n"
            "       bne.s   1b\n"
            "       movem.l (%%sp)+,%%d2-%%d4\n"
            : "+d" (r_n)
            :
            : "d1", "cc", "memory");
    }
#else
    (VOID)reps;
    (VOID)with_divu;
#endif

    ticks = c68k_eclock() - start;

    return(ticks);
}

static VOID a_calibrate_divu(VOID)
{

#define A_DCAL_REPS     8000UL
#define A_DCAL_PER_REP  8UL

ULONG   busy;
ULONG   idle;
ULONG   ns;


    busy = a_cal_divu(A_DCAL_REPS, 1u);
    idle = a_cal_divu(A_DCAL_REPS, 0u);

    if (busy <= idle)
    {
        a_divu_ps = 0UL;
        return;
    }

    ns = c68k_eclock_micros(busy - idle);
    a_divu_ps = (ns * 1000UL) / ((A_DCAL_REPS * A_DCAL_PER_REP) / 1000UL);

    c68k_log("  DIVU.L Dn,Dr:Dq costs %lu.%03lu ns here; the MC68020UM says 78",
             a_divu_ps / 1000UL, a_divu_ps % 1000UL);
    if (a_mulu_ps != 0UL)
    {
        c68k_log("    = %lu.%02lu MULU.L, against 78/45 = 1.73 on the part",
                 (a_divu_ps * 100UL / a_mulu_ps) / 100UL,
                 (a_divu_ps * 100UL / a_mulu_ps) % 100UL);
    }
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
        c68k_log("  MULU.L calibration failed, corrected times unavailable");
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

static VOID a_kar_sqr(VOID)
{

    c68k_mont_sqr(a_kr, a_kx, a_km, a_klen, a_kn0, a_scratch);
}

static VOID a_kar_mul(VOID)
{

    c68k_mont_mul(a_kr, a_kx, a_kx, a_km, a_klen, a_kn0, a_scratch);
}

static c68k_limb    a_rr[64];
static c68k_limb    a_setup_area[(7 * 64) + 8];

static VOID a_setup_kernel(VOID)
{

    c68k_mont_setup_rr(a_rr, a_km, a_klen, a_setup_area);
}

static VOID a_bench_setup(VOID)
{

static const UINT   widths[] = { 32u, 64u };
UINT                w;
UINT                i;
ULONG               slow, fast, ratio;


    c68k_log("");
    c68k_log("0c. R^2 mod m, the vendored 16-bit divider against ours");

    for (w = 0; w < 2u; w++)
    {
        a_klen = widths[w];

        for (i = 0; i < a_klen; i++)
        {
            a_km[i] = t_n[i];
        }
        a_km[0]           |= 1u;
        a_km[a_klen - 1u] |= 0x80000000UL;

        c68k_fast_modulus = 0u;
        slow = a_time_n(a_setup_kernel, (a_klen >= 64u) ? 8UL : 20UL);

        c68k_fast_modulus = 1u;
        fast = a_time_n(a_setup_kernel, (a_klen >= 64u) ? 8UL : 20UL);

        ratio = (fast != 0UL) ? ((slow * 100UL) / fast) : 0UL;
        c68k_log("  %lu limbs: vendored %lu us -> ours %lu us  (%lu.%02lux)",
                 (ULONG)a_klen, slow, fast, ratio / 100UL, ratio % 100UL);
    }

    c68k_fast_modulus = 1u;
}


static VOID a_bench_karatsuba(VOID)
{

static const UINT   widths[] = { 32u, 64u };
UINT                w;
UINT                i;
UINT                thr;
ULONG               base_s, base_m, on_s, on_m;
ULONG               reps;


    c68k_log("");
    c68k_log("0b. Karatsuba crossover, one Montgomery step (product + reduction)");
    c68k_log("   threshold T means: split while the operand is >= T limbs, so");
    c68k_log("   T = n is one level and T = 2 recurses to 2-limb leaves.");

    for (w = 0; w < 2u; w++)
    {
        a_klen = widths[w];

        for (i = 0; i < a_klen; i++)
        {
            a_km[i] = t_n[i];
            a_kx[i] = t_msg[i];
        }
        a_km[0]            |= 1u;               /* Montgomery needs it odd  */
        a_km[a_klen - 1u]  |= 0x80000000UL;     /* and x < m below          */
        a_kx[a_klen - 1u]  &= 0x7FFFFFFFUL;
        a_kn0 = c68k_mont_n0inv(a_km[0]);

        reps = (a_klen >= 64u) ? 20UL : 60UL;

        c68k_karatsuba_limbs = 0xFFFFu;         /* schoolbook, the baseline */
        base_s = a_time_n(a_kar_sqr, reps);
        base_m = a_time_n(a_kar_mul, reps);

        c68k_log("  %lu limbs, schoolbook: square %lu us, multiply %lu us",
                 (ULONG)a_klen, base_s, base_m);

        for (thr = a_klen; thr >= 8u; thr >>= 1)
        {
            c68k_karatsuba_limbs = thr;
            on_s = a_time_n(a_kar_sqr, reps);
            on_m = a_time_n(a_kar_mul, reps);

            c68k_log("    T=%2lu: square %6lu us (%lu.%02lux)   "
                     "multiply %6lu us (%lu.%02lux)",
                     (ULONG)thr,
                     on_s, (on_s != 0UL) ? ((base_s * 100UL) / on_s) / 100UL : 0UL,
                           (on_s != 0UL) ? ((base_s * 100UL) / on_s) % 100UL : 0UL,
                     on_m, (on_m != 0UL) ? ((base_m * 100UL) / on_m) / 100UL : 0UL,
                           (on_m != 0UL) ? ((base_m * 100UL) / on_m) % 100UL : 0UL);
        }
    }

    c68k_karatsuba_limbs = C68K_KARATSUBA_DEFAULT;
    c68k_log("  threshold in force for everything below: %lu limbs",
             (ULONG)c68k_karatsuba_limbs);
}

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
        plain = a_time_n(a_theirs_modexp_priv, 1UL);
        c68k_log("    AmiSSL, no CRT, BN_FLG_CONSTTIME, 2048-bit exponent: "
                 "%lu us", plain);
    }
}

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

    ours   = a_time_n(a_ours_ecdsa,   2UL);
    theirs = a_time_n(a_theirs_ecdsa, 2UL);
    a_report("ECDSA P-256 verify (RFC 6979 A.2.5)", ours, theirs,
             A_MULU_ECDSA_OURS, A_MULU_ECDSA_THEIRS, 1u);

    ours   = a_time_n(a_ours_ecdh,   2UL);
    theirs = a_time_n(a_theirs_ecdh, 2UL);
    a_check("ECDH shared secret", a_ours_secret, a_theirs_secret, 32u);
    a_check("ours ECDH vs known answer", a_ours_secret, v_ecdh_secret, 32u);
    a_report("ECDH P-256 shared secret           ", ours, theirs,
             A_MULU_ECDH_OURS, A_MULU_ECDH_THEIRS, 1u);

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

static VOID a_ours_aes(VOID)
{

UINT    i;


    for (i = 0; i < 16u; i++)
    {
        a_iv_work[i] = a_aes_iv[i];
    }

    c68k_aes_cbc_encrypt(&a_aes, a_iv_work, a_bulk_in, a_bulk_ours,
                         A_BULK_BYTES / 16UL);
}

static VOID a_ours_aes_dec(VOID)
{

UINT    i;


    for (i = 0; i < 16u; i++)
    {
        a_iv_work[i] = a_aes_iv[i];
    }

    c68k_aes_cbc_decrypt(&a_aes, a_iv_work, a_bulk_ours, a_bulk_plain,
                         A_BULK_BYTES / 16UL);
}

static VOID a_theirs_aes_dec(VOID)
{

    if (a_ossl_aes128_cbc_dec(a_aes_key, a_aes_iv, a_bulk_theirs,
                              a_bulk_plain, A_BULK_BYTES) != 0)
    {
        a_failures++;
    }
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

    (VOID) _nx_crypto_hmac(&a_hmac_meta, a_bulk_in, (UINT)A_BULK_BYTES,
                           a_mac_key, 32u, a_mac_ours, 32u);
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

    if (c68k_aes_key_set(&a_aes, a_aes_key, 128u) != NX_CRYPTO_SUCCESS)
    {
        c68k_log("  FATAL: our AES key schedule failed");
        a_failures++;
        return;
    }

    c68k_log("  our AES: %s", (LONG)c68k_aes_variant_name(c68k_aes_variant));
    c68k_log("  our SHA-256: %s",
             (LONG)c68k_sha256_variant_name(c68k_sha256_variant));

    ours   = a_time_n(a_ours_aes,   4UL);
    theirs = a_time_n(a_theirs_aes, 4UL);
    a_check("AES-128-CBC ciphertext", a_bulk_ours, a_bulk_theirs,
            (UINT)A_BULK_BYTES);
    a_report("AES-128-CBC encrypt                ", ours, theirs,
             A_MULU_NONE, A_MULU_NONE, 2u);
    c68k_log("    ours %lu KB/s, AmiSSL %lu KB/s",
             a_kbs(ours), a_kbs(theirs));

    /* Decryption, which section 15 did not measure and which is the direction
       a download runs in.  Both sides decrypt the ciphertext they agreed on
       above, and the answer is checked against the plaintext. */
    ours = a_time_n(a_ours_aes_dec, 4UL);
    a_check("AES-128-CBC plaintext, ours", a_bulk_plain, a_bulk_in,
            (UINT)A_BULK_BYTES);
    theirs = a_time_n(a_theirs_aes_dec, 4UL);
    a_check("AES-128-CBC plaintext, AmiSSL", a_bulk_plain, a_bulk_in,
            (UINT)A_BULK_BYTES);
    a_report("AES-128-CBC decrypt                ", ours, theirs,
             A_MULU_NONE, A_MULU_NONE, 2u);
    c68k_log("    ours %lu KB/s, AmiSSL %lu KB/s",
             a_kbs(ours), a_kbs(theirs));

    _nx_crypto_hmac_metadata_set(&a_hmac_meta, &a_hmac_ctx,
                                 NX_CRYPTO_AUTHENTICATION_HMAC_SHA2_256,
                                 64u, 32u,
                                 (UINT (*)(VOID *, UINT))
                                     c68k_sha256_initialize,
                                 (UINT (*)(VOID *, UCHAR *, UINT))
                                     c68k_sha256_update,
                                 (UINT (*)(VOID *, UCHAR *, UINT))
                                     c68k_sha256_digest_calculate);

    ours   = a_time_n(a_ours_hmac,   4UL);
    theirs = a_time_n(a_theirs_hmac, 4UL);
    a_check("HMAC-SHA256 tag", a_mac_ours, a_mac_theirs, 32u);
    a_report("HMAC-SHA256                        ", ours, theirs,
             A_MULU_NONE, A_MULU_NONE, 2u);
    c68k_log("    ours %lu KB/s, AmiSSL %lu KB/s",
             a_kbs(ours), a_kbs(theirs));
}

static const A_ROW *a_find(const char *prefix)
{

UINT    i;
UINT    j;


    for (i = 0; i < a_row_count; i++)
    {
        for (j = 0; prefix[j] != '\0'; j++)
        {
            if (a_rows[i].name[j] != prefix[j])
            {
                break;
            }
        }
        if (prefix[j] == '\0')
        {
            return(&a_rows[i]);
        }
    }

    return(NX_CRYPTO_NULL);
}

static VOID a_compose(const char *what,
                      const A_ROW *chain, ULONG chain_n,
                      const A_ROW *kg, const A_ROW *dh)
{

ULONG   ours;
ULONG   theirs;
ULONG   ours_c;
ULONG   theirs_c;
ULONG   ratio;


    if ((chain == NX_CRYPTO_NULL) || (kg == NX_CRYPTO_NULL) ||
        (dh == NX_CRYPTO_NULL))
    {
        return;
    }

    ours   = (chain_n * chain -> ours_us)   + kg -> ours_us   + dh -> ours_us;
    theirs = (chain_n * chain -> theirs_us) + kg -> theirs_us + dh -> theirs_us;

    ours_c = (chain_n * a_corrected(chain -> ours_us, chain -> ours_mulu)) +
             a_corrected(kg -> ours_us, kg -> ours_mulu) +
             a_corrected(dh -> ours_us, dh -> ours_mulu);
    theirs_c = (chain_n * a_corrected(chain -> theirs_us, chain -> theirs_mulu)) +
               a_corrected(kg -> theirs_us, kg -> theirs_mulu) +
               a_corrected(dh -> theirs_us, dh -> theirs_mulu);

    if (ours == 0UL || theirs == 0UL)
    {
        return;
    }

    ratio = (theirs >= ours) ? ((theirs * 10UL) / ours)
                             : ((ours * 10UL) / theirs);

    c68k_log("    %s: ours %lu ms  AmiSSL %lu ms   %s %lu.%lux",
             (LONG)what, ours / 1000UL, theirs / 1000UL,
             (LONG)((theirs >= ours) ? "OURS" : "AMISSL"),
             ratio / 10UL, ratio % 10UL);

    ratio = (theirs_c >= ours_c) ? ((theirs_c * 10UL) / ours_c)
                                 : ((ours_c * 10UL) / theirs_c);

    c68k_log("      MULU.L-corrected:  ours %lu ms  AmiSSL %lu ms   %s %lu.%lux",
             ours_c / 1000UL, theirs_c / 1000UL,
             (LONG)((theirs_c >= ours_c) ? "OURS" : "AMISSL"),
             ratio / 10UL, ratio % 10UL);
}

static VOID a_summary(VOID)
{

const A_ROW    *rsapub;
const A_ROW    *ecdsa;
const A_ROW    *ecdh;
const A_ROW    *kg;
const A_ROW    *aes;
const A_ROW    *mac;
ULONG           ours;
ULONG           theirs;


    c68k_log("");
    c68k_log("5. What a client actually pays");

    rsapub = a_find("BN_mod_exp_mont");
    ecdsa  = a_find("ECDSA P-256 verify");
    ecdh   = a_find("ECDH P-256 shared");
    kg     = a_find("k*G");
    aes    = a_find("AES-128-CBC");
    mac    = a_find("HMAC-SHA256");

    c68k_log("  the asymmetric half of a TLS 1.2 handshake, 2-cert chain:");
    a_compose("ECDHE_RSA   (3 x RSA-2048 verify + keygen + ECDH)",
              rsapub, 3UL, kg, ecdh);
    a_compose("ECDHE_ECDSA (3 x P-256 verify  + keygen + ECDH)",
              ecdsa, 3UL, kg, ecdh);
    c68k_log("    (arithmetic only, no network, no parsing, no framing)");

    if ((aes != NX_CRYPTO_NULL) && (mac != NX_CRYPTO_NULL))
    {
        ours   = aes -> ours_us   + mac -> ours_us;
        theirs = aes -> theirs_us + mac -> theirs_us;

        c68k_log("");
        c68k_log("  the record path, one 16 KiB TLS record encrypted and MACed:");
        c68k_log("    ours %lu us, AmiSSL %lu us", ours, theirs);
        if ((ours != 0UL) && (theirs != 0UL))
        {
            c68k_log("    = ours %lu KB/s, AmiSSL %lu KB/s of application data",
                     a_kbs(ours), a_kbs(theirs));
        }
    }
}

static int a_body(VOID)
{

char    err[128];
UINT    status;
UINT    i;
ULONG   len;
ULONG   start;


    c68k_log("AmiNetXDuo, crypto68k against AmiSSL, one process");
    c68k_log("  our limb primitives: %s",
             (LONG)(c68k_using_assembly() ? "68020 assembly" : "portable C"));

    if (!c68k_timer_open())
    {
        c68k_log("FATAL: timer.device UNIT_ECLOCK would not open");
        return(20);
    }
    c68k_log("  E-Clock %lu Hz", c68k_eclock_hz());

    a_calibrate();
    a_calibrate_divu();

    c68k_log("");
    c68k_log("0. Opening AmiSSL (LoadSeg of a 3.5 MB library, then OpenSSL "
             "3.x init)");

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

    start = c68k_eclock();
    a_ossl_touch();
    c68k_log("  first OpenSSL call (its lazy provider/DRBG init): %lu ms",
             c68k_eclock_millis(c68k_eclock() - start));

    c68k_log("  AmiSSL: %s", (LONG)a_ossl_version());
    c68k_log("  library: %s", (LONG)a_ossl_library_id());
    c68k_log("  mode: %s", (LONG)(a_full ? "full" : "quick"));
    c68k_log("");
    c68k_log("  Every operation is checked against the other side's answer");
    c68k_log("  before it is timed.  Timings are the emulator's 68020, whose");
    c68k_log("  MULU.L is 32.14 cycles against a real part's 45, the");
    c68k_log("  corrected line beside each result is what a 68020 would do.");

    a_bench_karatsuba();
    a_bench_setup();

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

    a_bench_bulk();

    a_summary();

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

#define A_CHILD_STACK   262144UL

static struct Task *a_parent;
static ULONG        a_sigmask;
static int          a_child_rc = 20;

static VOID a_child(VOID)
{

struct Process *me = (struct Process *)FindTask(NULL);
BPTR            lock;

    me -> pr_WindowPtr = (APTR)-1;

    lock = Lock((STRPTR)"DH0:AmiSSL", SHARED_LOCK);
    if (lock != (BPTR)0)
    {
        if (!AssignLock((STRPTR)"AmiSSL", lock))
        {
            UnLock(lock);
        }
    }

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
        c68k_log("CRASHED, see the serial log and DH0:crash.txt");
        c68k_flush();
        ami_crash_remove();
        return(20);
    }

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
