/*
 * AmiNetXDuo, nx_secure feasibility gate (docs/RESEARCH.md 9, decision 4).
 *
 * Section 9 records that TLS ships as a separate library behind its own build
 * option and that it is benchmarked on 68020 before any API is promised.  This
 * program is that benchmark: how long the arithmetic in a TLS 1.2 handshake
 * takes on the floor target.
 *
 *   Public key, per handshake, once:
 *     RSA-2048 public op, every certificate signature the client checks,
 *                             plus the ServerKeyExchange signature under
 *                             ECDHE_RSA, plus the premaster encryption under
 *                             plain RSA key exchange.  e = 65537, so this is
 *                             17 squarings and a multiply: the cheap one.
 *     RSA-2048 private op , only if the Amiga is the TLS server, or presents
 *                             a client certificate.  Full 2048-bit exponent;
 *                             measured both plain and CRT, because CRT is ~3x
 *                             and nx_crypto only uses it when the primes are
 *                             supplied.
 *     ECDHE P-256 keygen, one fixed-point scalar multiply.
 *     ECDHE P-256 shared, one variable-point scalar multiply.  These two
 *                             are what an ECDHE_* suite costs the client.
 *     ECDSA P-256 verify, for hosts that serve ECDSA certificates, which
 *                             is most of the modern web.
 *
 *   Bulk, per byte, forever after:
 *     SHA-256, HMAC-SHA256, AES-128/256-CBC, AES-128/256-GCM.  These decide
 *     whether a connection is usable once it is up, a 14 MHz 68020 pulling
 *     150 KB/s off the wire cannot afford a cipher that runs at 40 KB/s.
 *
 *   No ThreadX, NetX Duo, packet pool or network is needed.
 *   crypto_libraries/src makes no tx_* or nx_* call at runtime (verified by
 *   grep over all 56 files), so the primitives are timed in a plain AmigaDOS
 *   process with nothing else running, no tick task, no driver interrupts.
 *   The handshake-composition figures at the end are arithmetic on these
 *   measurements, and are labelled as such.
 *
 *   nx_port.h does not define NX_RAND, so nx_api.h falls back to newlib rand()
 *, a 32-bit LCG.  ECDHE private keys come out of it.  That is fine for a
 *   benchmark (the arithmetic is identical whatever the bits are) and is a
 *   hard blocker for shipping.  Flagged in src/tls/tls.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_crypto.h"
#include "nx_crypto_aes.h"
#include "nx_crypto_ec.h"
#include "nx_crypto_ecdh.h"
#include "nx_crypto_ecdsa.h"
#include "nx_crypto_huge_number.h"
#include "nx_crypto_rsa.h"
#include "nx_crypto_sha2.h"
#include "nx_crypto_hmac_sha2.h"

/* For the session/certificate sizes the memory estimate prints. */
#include "nx_secure_tls_api.h"

#include "tls.h"
#include "aminetxduo/crashguard.h"
#include "aminetxduo/random.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>

#include "rsa2048_key.h"


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define B_LOG_SIZE      8192

static char     b_log_buffer[B_LOG_SIZE];
static ULONG    b_log_used;

static VOID b_put(UBYTE c)
{

    RawPutChar(c);

    if (b_log_used < (ULONG)(B_LOG_SIZE - 1))
    {
        b_log_buffer[b_log_used++] = (char)c;
    }
}

static VOID b_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID) unused;
    if (c != '\0')
    {
        b_put(c);
    }
}

/*
 * RawDoFmt, not printf: %ld/%lu/%lx/%s only, every argument longword sized.
 * See include/aminetxduo/compat.h for why %d and %c are traps here.
 */
static VOID b_log(const char *fmt, ...)
{

va_list args;


    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) b_put_char, NULL);
    va_end(args);

    b_put('\n');
}

static VOID b_flush(VOID)
{

BPTR    out;


    out =  Output();
    if (out != (BPTR)0)
    {
        (VOID) Write(out, (APTR)b_log_buffer, (LONG)b_log_used);
    }
}


/* -------------------------------------------------------------- results -- */

static ULONG    b_failures;
static ULONG    b_started;      /* E-Clock at the first measurement */

/*
 * Results are held in microseconds per operation.  The handshake composition
 * at the end sums them, so they have to survive being added up: nanoseconds in
 * a ULONG top out at 4.29 seconds, and a measured RSA-2048 private operation
 * on the floor target is 158 of them.  Microseconds give 4295 seconds of
 * headroom and are still 1000x finer than the E-Clock's 1.4 us tick.
 */
#define B_MAX_RESULTS   24

typedef struct
{
    const char *name;
    ULONG       per_op_us;          /* microseconds for one operation      */
    ULONG       kb_per_sec;         /* 0 when the operation has no payload */
    ULONG       iterations;
} B_RESULT;

static B_RESULT b_results[B_MAX_RESULTS];
static ULONG    b_result_count;

/*
 * Named slots the composition step reads back, in the order the benchmarks run
 * in: cheapest first.  The RSA private operation without CRT takes minutes on
 * the floor target, so it goes last, a run that hits the harness timeout
 * still delivers every other figure.
 */
#define B_SHA256            0
#define B_HMAC_SHA256       1
#define B_AES128_CBC        2
#define B_AES256_CBC        3
#define B_AES128_GCM        4
#define B_AES256_GCM        5
#define B_ECDHE_KEYGEN      6
#define B_ECDHE_SHARED      7
#define B_ECDSA_VERIFY      8
#define B_RSA_PUBLIC        9
#define B_RSA_PRIVATE_CRT   10
#define B_RSA_PRIVATE       11

static ULONG b_us(ULONG slot)
{

    return (slot < b_result_count) ? b_results[slot].per_op_us : 0;
}

/*
 * Record one measurement.  `ticks` is the raw E-Clock delta over `iterations`
 * runs; `payload` is the bytes processed per run, or 0 for a keyed operation
 * where throughput is meaningless.
 */
static VOID b_record(const char *name, ULONG ticks, ULONG iterations,
                     ULONG payload)
{

ULONG               total_us;
ULONG               per_op_us;
ULONG               kb_per_sec;
unsigned long long  bytes;


    if (b_result_count >= B_MAX_RESULTS)
    {
        return;
    }

    if (iterations == 0)
    {
        iterations = 1;
    }

    total_us  =  ami_tls_eclock_micros(ticks);
    per_op_us =  total_us / iterations;

    kb_per_sec = 0;
    if ((payload != 0) && (total_us != 0))
    {
        bytes = (unsigned long long)payload * (unsigned long long)iterations;
        kb_per_sec = (ULONG)((bytes * 1000000ULL) /
                             ((unsigned long long)total_us * 1024ULL));
    }

    b_results[b_result_count].name       = name;
    b_results[b_result_count].per_op_us  = per_op_us;
    b_results[b_result_count].kb_per_sec = kb_per_sec;
    b_results[b_result_count].iterations = iterations;
    b_result_count++;

    if (payload != 0)
    {
        b_log("  %s: %lu us/op over %lu runs (%lu KB/s)",
              (LONG)name, per_op_us, iterations, kb_per_sec);
    }
    else
    {
        b_log("  %s: %lu us/op over %lu runs", (LONG)name, per_op_us,
              iterations);
    }
}

static VOID b_fail(const char *what, UINT status)
{

    b_failures++;
    b_log("  FAIL %s (status 0x%lx)", (LONG)what, (ULONG)status);

    /*
     * Still record a slot so the composition indices stay aligned with the
     * B_* constants, a missing measurement must read as zero, not shift
     * every later result up by one.
     */
    if (b_result_count < B_MAX_RESULTS)
    {
        b_results[b_result_count].name       = what;
        b_results[b_result_count].per_op_us  = 0;
        b_results[b_result_count].kb_per_sec = 0;
        b_results[b_result_count].iterations = 0;
        b_result_count++;
    }
}

/*
 * Time one operation.
 *
 * A fixed iteration count cannot work here.  The operations span five orders
 * of magnitude, an AES block against a 2048-bit modular exponentiation with
 * a 2048-bit exponent, and the program has to finish inside the harness
 * timeout on a CPU whose speed is the thing being measured.
 *
 * So run once, timed.  If that single run already took longer than
 * B_ENOUGH_TICKS it is the measurement; repeating a four-minute RSA private
 * operation to refine a figure already good to four significant digits would
 * only burn emulator time.  Otherwise pick a repeat count landing near half a
 * second and loop.
 *
 * The operation is a function pointer plus an opaque context rather than five
 * copies of a calibrate/loop/record sequence, where an off-by-one in the
 * iteration count would silently scale a result.
 */
typedef UINT (*B_OP)(VOID *context);

#define B_ENOUGH_TICKS(hz)      ((hz) / 4)      /* 250 ms */
#define B_TARGET_TICKS(hz)      ((hz) / 2)      /* 500 ms */
#define B_MAX_ITERATIONS        4000UL

static VOID b_measure(const char *name, B_OP op, VOID *context, ULONG payload)
{

UINT    status;
ULONG   hz;
ULONG   start;
ULONG   ticks;
ULONG   n;
ULONG   i;


    hz = ami_tls_eclock_hz();

    start =  ami_tls_eclock();
    status = op(context);
    ticks =  ami_tls_eclock() - start;

    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail(name, status);
        return;
    }

    if (ticks >= B_ENOUGH_TICKS(hz))
    {
        b_record(name, ticks, 1, payload);
        return;
    }

    n = (ticks != 0) ? (B_TARGET_TICKS(hz) / ticks) : B_MAX_ITERATIONS;
    if (n == 0)
    {
        n = 1;
    }
    if (n > B_MAX_ITERATIONS)
    {
        n = B_MAX_ITERATIONS;
    }

    start = ami_tls_eclock();
    for (i = 0; i < n; i++)
    {
        (VOID) op(context);
    }
    ticks = ami_tls_eclock() - start;

    b_record(name, ticks, n, payload);
}


/* ------------------------------------------------------- crypto methods --- */

extern NX_CRYPTO_METHOD crypto_method_rsa;
extern NX_CRYPTO_METHOD crypto_method_ecdh;
extern NX_CRYPTO_METHOD crypto_method_ecdsa;
extern NX_CRYPTO_METHOD crypto_method_ec_secp256;
extern NX_CRYPTO_METHOD crypto_method_sha256;
extern NX_CRYPTO_METHOD crypto_method_hmac_sha256;
extern NX_CRYPTO_METHOD crypto_method_aes_cbc_128;
extern NX_CRYPTO_METHOD crypto_method_aes_cbc_256;
extern NX_CRYPTO_METHOD crypto_method_aes_128_gcm_16;
extern NX_CRYPTO_METHOD crypto_method_aes_256_gcm_16;


/* ------------------------------------------------------------- payloads --- */

/*
 * 1024 bytes: comfortably more than one TLS record's worth of framing, small
 * enough that a whole buffer plus the AES round keys stays in a 68030's 256
 * byte caches' working set the same way a real record would.  Both CPUs see
 * the same figure, so the comparison holds even though neither is a
 * cache-accurate model of real silicon.
 */
#define B_PAYLOAD       1024

static UCHAR    b_plain[B_PAYLOAD];
static UCHAR    b_cipher[B_PAYLOAD + 32];       /* + GCM tag */
static UCHAR    b_digest[64];

/* TLS 1.2 AEAD additional data is 13 bytes: seq(8) type(1) version(2) len(2) */
static UCHAR    b_aad[13] =
{
    0, 0, 0, 0, 0, 0, 0, 1, 0x17, 0x03, 0x03, 0x04, 0x00
};

/* GCM nonce, in nx_crypto's length-prefixed form: iv[0] = length. */
static UCHAR    b_gcm_iv[13] =
{
    12, 0xCA, 0xFE, 0xBA, 0xBE, 0xFA, 0xCE, 0xDB, 0xAD, 0xDE, 0xCA, 0xF8, 0x88
};

static UCHAR    b_key[32] =
{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};

static UCHAR    b_cbc_iv[16] =
{
    0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87,
    0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F
};

static VOID b_fill_payload(VOID)
{

ULONG   i;


    for (i = 0; i < B_PAYLOAD; i++)
    {
        b_plain[i] = (UCHAR)(i * 7 + 13);
    }
}


/* ------------------------------------------------------------- metadata --- */

/*
 * One metadata block per algorithm, sized from the method tables.  These are
 * the largest single allocations a TLS session needs, and the memory estimate
 * at the end prints their measured sizes.
 */
static NX_CRYPTO_RSA        b_rsa_metadata;
static NX_CRYPTO_ECDH       b_ecdh_metadata;
static NX_CRYPTO_ECDH       b_ecdh_peer_metadata;
static NX_CRYPTO_ECDSA      b_ecdsa_metadata;
static NX_CRYPTO_SHA256     b_sha_metadata;
static NX_CRYPTO_SHA256_HMAC b_hmac_metadata;
static NX_CRYPTO_AES        b_aes_metadata;

static UCHAR    b_rsa_output[256];
static UCHAR    b_rsa_roundtrip[256];


/* ------------------------------------------------------------------ RSA -- */

static UINT b_rsa_op(const UCHAR *exponent, ULONG exponent_bits,
                     const UCHAR *input, UCHAR *output)
{

UINT    status;
VOID   *handler = NX_CRYPTO_NULL;


    status =  crypto_method_rsa.nx_crypto_init(&crypto_method_rsa,
                                               (UCHAR *)t_rsa2048_n,
                                               2048,
                                               &handler,
                                               &b_rsa_metadata,
                                               sizeof(b_rsa_metadata));
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    status =  crypto_method_rsa.nx_crypto_operation(NX_CRYPTO_ENCRYPT,
                                                    handler,
                                                    &crypto_method_rsa,
                                                    (UCHAR *)exponent,
                                                    (NX_CRYPTO_KEY_SIZE)exponent_bits,
                                                    (UCHAR *)input,
                                                    256,
                                                    NX_CRYPTO_NULL,
                                                    output,
                                                    256,
                                                    &b_rsa_metadata,
                                                    sizeof(b_rsa_metadata),
                                                    NX_CRYPTO_NULL, NX_CRYPTO_NULL);

    (VOID) crypto_method_rsa.nx_crypto_cleanup(&b_rsa_metadata);

    return(status);
}

static UINT b_rsa_op_crt(const UCHAR *input, UCHAR *output)
{

UINT    status;
VOID   *handler = NX_CRYPTO_NULL;


    status =  crypto_method_rsa.nx_crypto_init(&crypto_method_rsa,
                                               (UCHAR *)t_rsa2048_n,
                                               2048,
                                               &handler,
                                               &b_rsa_metadata,
                                               sizeof(b_rsa_metadata));
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    status =  crypto_method_rsa.nx_crypto_operation(NX_CRYPTO_SET_PRIME_P,
                                                    handler, &crypto_method_rsa,
                                                    NX_CRYPTO_NULL, 0,
                                                    (UCHAR *)t_rsa2048_p, 128,
                                                    NX_CRYPTO_NULL,
                                                    NX_CRYPTO_NULL, 0,
                                                    &b_rsa_metadata,
                                                    sizeof(b_rsa_metadata),
                                                    NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    status =  crypto_method_rsa.nx_crypto_operation(NX_CRYPTO_SET_PRIME_Q,
                                                    handler, &crypto_method_rsa,
                                                    NX_CRYPTO_NULL, 0,
                                                    (UCHAR *)t_rsa2048_q, 128,
                                                    NX_CRYPTO_NULL,
                                                    NX_CRYPTO_NULL, 0,
                                                    &b_rsa_metadata,
                                                    sizeof(b_rsa_metadata),
                                                    NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    status =  crypto_method_rsa.nx_crypto_operation(NX_CRYPTO_ENCRYPT,
                                                    handler, &crypto_method_rsa,
                                                    (UCHAR *)t_rsa2048_d, 2048,
                                                    (UCHAR *)input, 256,
                                                    NX_CRYPTO_NULL,
                                                    output, 256,
                                                    &b_rsa_metadata,
                                                    sizeof(b_rsa_metadata),
                                                    NX_CRYPTO_NULL, NX_CRYPTO_NULL);

    (VOID) crypto_method_rsa.nx_crypto_cleanup(&b_rsa_metadata);

    return(status);
}

/*
 * The message: raw modexp input, high byte zeroed so it is safely below the
 * modulus.  Not PKCS#1 padded, padding is a memcpy standing next to a
 * 2048-bit exponentiation and would only blur the figure.
 */
static UCHAR    b_rsa_message[256];

typedef struct
{
    const UCHAR *exponent;
    ULONG        exponent_bits;
} B_RSA_CONTEXT;

static UINT b_rsa_run(VOID *context)
{

B_RSA_CONTEXT  *ctx = (B_RSA_CONTEXT *)context;


    return b_rsa_op(ctx -> exponent, ctx -> exponent_bits, b_rsa_message,
                    b_rsa_output);
}

static UINT b_rsa_crt_run(VOID *context)
{

    (VOID) context;
    return b_rsa_op_crt(b_rsa_message, b_rsa_output);
}

static VOID b_bench_rsa(VOID)
{

UINT            status;
ULONG           i;
B_RSA_CONTEXT   public_ctx;
B_RSA_CONTEXT   private_ctx;


    for (i = 0; i < 256; i++)
    {
        b_rsa_message[i] = (UCHAR)(i ^ 0x5A);
    }
    b_rsa_message[0] = 0x00;

    public_ctx.exponent       = t_rsa2048_e;
    public_ctx.exponent_bits  = 32;
    private_ctx.exponent      = t_rsa2048_d;
    private_ctx.exponent_bits = 2048;

    /*
     * Correctness first: public(private(m)) must be m.  The private half uses
     * CRT, which is both the fast path and the one nx_secure actually takes,
     * the PKCS#1 parser hands it p and q, so this validates the path that
     * matters and costs a quarter of what the plain exponentiation would.
     */
    status = b_rsa_op_crt(b_rsa_message, b_rsa_output);
    if (status == NX_CRYPTO_SUCCESS)
    {
        status = b_rsa_op(t_rsa2048_e, 32, b_rsa_output, b_rsa_roundtrip);
    }
    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail("RSA-2048 public  ", status);
        b_fail("RSA-2048 priv CRT", status);
        b_fail("RSA-2048 private ", status);
        return;
    }
    for (i = 0; i < 256; i++)
    {
        if (b_rsa_roundtrip[i] != b_rsa_message[i])
        {
            b_log("  FAIL RSA-2048 round trip differs at byte %lu", i);
            b_failures++;
            break;
        }
    }

    b_measure("RSA-2048 public  ", b_rsa_run,     &public_ctx,  0);
    b_measure("RSA-2048 priv CRT", b_rsa_crt_run, NX_CRYPTO_NULL, 0);
    b_measure("RSA-2048 private ", b_rsa_run,     &private_ctx, 0);
}


/* ----------------------------------------------------------- ECDHE P-256 -- */

static UCHAR    b_local_public[128];
static UCHAR    b_peer_public[128];
static UCHAR    b_shared[64];
static ULONG    b_peer_public_len;

static UINT b_ecdh_begin(NX_CRYPTO_ECDH *metadata)
{

UINT    status;


    status =  crypto_method_ecdh.nx_crypto_init(&crypto_method_ecdh,
                                                NX_CRYPTO_NULL, 0,
                                                NX_CRYPTO_NULL,
                                                metadata, sizeof(*metadata));
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    return crypto_method_ecdh.nx_crypto_operation(NX_CRYPTO_EC_CURVE_SET,
                                                  NX_CRYPTO_NULL,
                                                  &crypto_method_ecdh,
                                                  NX_CRYPTO_NULL, 0,
                                                  (UCHAR *)&crypto_method_ec_secp256,
                                                  sizeof(NX_CRYPTO_METHOD *),
                                                  NX_CRYPTO_NULL,
                                                  NX_CRYPTO_NULL, 0,
                                                  metadata, sizeof(*metadata),
                                                  NX_CRYPTO_NULL, NX_CRYPTO_NULL);
}

static UINT b_ecdh_keygen(NX_CRYPTO_ECDH *metadata, UCHAR *out, ULONG out_size,
                          ULONG *out_len)
{

UINT                        status;
NX_CRYPTO_EXTENDED_OUTPUT   extended;


    extended.nx_crypto_extended_output_data           = out;
    extended.nx_crypto_extended_output_length_in_byte = out_size;

    status = crypto_method_ecdh.nx_crypto_operation(NX_CRYPTO_DH_SETUP,
                                                    NX_CRYPTO_NULL,
                                                    &crypto_method_ecdh,
                                                    NX_CRYPTO_NULL, 0,
                                                    NX_CRYPTO_NULL, 0,
                                                    NX_CRYPTO_NULL,
                                                    (UCHAR *)&extended,
                                                    sizeof(extended),
                                                    metadata, sizeof(*metadata),
                                                    NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if ((status == NX_CRYPTO_SUCCESS) && (out_len != NX_CRYPTO_NULL))
    {
        *out_len = extended.nx_crypto_extended_output_actual_size;
    }

    return(status);
}

static UINT b_ecdh_shared(NX_CRYPTO_ECDH *metadata)
{

NX_CRYPTO_EXTENDED_OUTPUT   extended;


    extended.nx_crypto_extended_output_data           = b_shared;
    extended.nx_crypto_extended_output_length_in_byte = sizeof(b_shared);

    return crypto_method_ecdh.nx_crypto_operation(NX_CRYPTO_DH_CALCULATE,
                                                  NX_CRYPTO_NULL,
                                                  &crypto_method_ecdh,
                                                  NX_CRYPTO_NULL, 0,
                                                  b_peer_public,
                                                  b_peer_public_len,
                                                  NX_CRYPTO_NULL,
                                                  (UCHAR *)&extended,
                                                  sizeof(extended),
                                                  metadata, sizeof(*metadata),
                                                  NX_CRYPTO_NULL, NX_CRYPTO_NULL);
}

static ULONG    b_local_public_len;

static UINT b_ecdh_keygen_run(VOID *context)
{

    return b_ecdh_keygen((NX_CRYPTO_ECDH *)context, b_local_public,
                         sizeof(b_local_public), &b_local_public_len);
}

static UINT b_ecdh_shared_run(VOID *context)
{

    return b_ecdh_shared((NX_CRYPTO_ECDH *)context);
}

static VOID b_bench_ecdhe(VOID)
{

UINT    status;


    /* A peer key pair, built once; it plays the server half in DH_CALCULATE. */
    status = b_ecdh_begin(&b_ecdh_peer_metadata);
    if (status == NX_CRYPTO_SUCCESS)
    {
        status = b_ecdh_keygen(&b_ecdh_peer_metadata, b_peer_public,
                               sizeof(b_peer_public), &b_peer_public_len);
    }
    if (status == NX_CRYPTO_SUCCESS)
    {
        status = b_ecdh_begin(&b_ecdh_metadata);
    }
    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail("ECDHE P-256 keygen", status);
        b_fail("ECDHE P-256 shared", status);
        return;
    }

    /* Fixed-point scalar multiply, then variable-point. */
    b_measure("ECDHE P-256 keygen", b_ecdh_keygen_run, &b_ecdh_metadata, 0);
    b_measure("ECDHE P-256 shared", b_ecdh_shared_run, &b_ecdh_metadata, 0);

    (VOID) crypto_method_ecdh.nx_crypto_cleanup(&b_ecdh_metadata);
    (VOID) crypto_method_ecdh.nx_crypto_cleanup(&b_ecdh_peer_metadata);
}


/* ----------------------------------------------------------- ECDSA P-256 -- */

/*
 * Scratch for the key-pair generation.  _nx_crypto_ec_key_pair_generation_extra
 * wants room for a private scalar, a point, and huge-number working space; the
 * shape is copied from nx_crypto_method_self_test_ecdsa.c, which is the only
 * documentation this API has.
 */
static HN_UBASE b_ecdsa_scratch[3000 / sizeof(HN_UBASE)];
static UCHAR    b_ecdsa_privkey[64];
static UCHAR    b_ecdsa_pubkey[128];
static UCHAR    b_ecdsa_sig[256];
static ULONG    b_ecdsa_sig_len;
static ULONG    b_ecdsa_pubkey_len;
static ULONG    b_ecdsa_key_bytes;

static VOID    *b_ecdsa_handler;

static UINT b_ecdsa_verify_run(VOID *context)
{

    (VOID) context;

    return crypto_method_ecdsa.nx_crypto_operation(NX_CRYPTO_SIGNATURE_VERIFY,
                                                   b_ecdsa_handler,
                                                   &crypto_method_ecdsa,
                                                   b_ecdsa_pubkey,
                                                   (NX_CRYPTO_KEY_SIZE)(b_ecdsa_pubkey_len << 3),
                                                   b_plain, 32,
                                                   NX_CRYPTO_NULL,
                                                   b_ecdsa_sig, b_ecdsa_sig_len,
                                                   &b_ecdsa_metadata,
                                                   sizeof(b_ecdsa_metadata),
                                                   NX_CRYPTO_NULL, NX_CRYPTO_NULL);
}

static VOID b_bench_ecdsa(VOID)
{

UINT                        status;
VOID                       *handler = NX_CRYPTO_NULL;
NX_CRYPTO_EC               *curve = NX_CRYPTO_NULL;
NX_CRYPTO_EC_POINT          public_point;
NX_CRYPTO_HUGE_NUMBER       private_scalar;
NX_CRYPTO_EXTENDED_OUTPUT   extended;
HN_UBASE                   *scratch;
UINT                        buffer_size;


    status =  crypto_method_ecdsa.nx_crypto_init(&crypto_method_ecdsa,
                                                 NX_CRYPTO_NULL, 0,
                                                 &handler,
                                                 &b_ecdsa_metadata,
                                                 sizeof(b_ecdsa_metadata));
    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail("ECDSA P-256 verify", status);
        return;
    }

    status = crypto_method_ecdsa.nx_crypto_operation(NX_CRYPTO_HASH_METHOD_SET,
                                                     handler,
                                                     &crypto_method_ecdsa,
                                                     NX_CRYPTO_NULL, 0,
                                                     (UCHAR *)&crypto_method_sha256,
                                                     sizeof(NX_CRYPTO_METHOD *),
                                                     NX_CRYPTO_NULL,
                                                     NX_CRYPTO_NULL, 0,
                                                     &b_ecdsa_metadata,
                                                     sizeof(b_ecdsa_metadata),
                                                     NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail("ECDSA P-256 verify", status);
        return;
    }

    status = crypto_method_ecdsa.nx_crypto_operation(NX_CRYPTO_EC_CURVE_SET,
                                                     handler,
                                                     &crypto_method_ecdsa,
                                                     NX_CRYPTO_NULL, 0,
                                                     (UCHAR *)&crypto_method_ec_secp256,
                                                     sizeof(NX_CRYPTO_METHOD *),
                                                     NX_CRYPTO_NULL,
                                                     NX_CRYPTO_NULL, 0,
                                                     &b_ecdsa_metadata,
                                                     sizeof(b_ecdsa_metadata),
                                                     NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail("ECDSA P-256 verify", status);
        return;
    }

    status = crypto_method_ec_secp256.nx_crypto_operation(NX_CRYPTO_EC_CURVE_GET,
                                                          NX_CRYPTO_NULL,
                                                          &crypto_method_ec_secp256,
                                                          NX_CRYPTO_NULL, 0,
                                                          NX_CRYPTO_NULL, 0,
                                                          NX_CRYPTO_NULL,
                                                          (UCHAR *)&curve, 0,
                                                          NX_CRYPTO_NULL, 0,
                                                          NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if ((status != NX_CRYPTO_SUCCESS) || (curve == NX_CRYPTO_NULL))
    {
        b_fail("ECDSA P-256 verify", status);
        return;
    }

    buffer_size = curve -> nx_crypto_ec_n.nx_crypto_huge_buffer_size;
    b_ecdsa_key_bytes = buffer_size;

    scratch = &b_ecdsa_scratch[(3 * buffer_size + 4) / sizeof(HN_UBASE)];
    NX_CRYPTO_EC_POINT_INITIALIZE(&public_point, NX_CRYPTO_EC_POINT_AFFINE,
                                  scratch, buffer_size);
    NX_CRYPTO_HUGE_NUMBER_INITIALIZE(&private_scalar, scratch, buffer_size + 8);

    status = _nx_crypto_ec_key_pair_generation_extra(curve,
                                                     &curve -> nx_crypto_ec_g,
                                                     &private_scalar,
                                                     &public_point, scratch);
    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail("ECDSA P-256 verify", status);
        return;
    }

    status = _nx_crypto_huge_number_extract_fixed_size(&private_scalar,
                                                       b_ecdsa_privkey,
                                                       buffer_size);
    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail("ECDSA P-256 verify", status);
        return;
    }

    b_ecdsa_pubkey_len = 0;
    _nx_crypto_ec_point_extract_uncompressed(curve, &public_point,
                                             b_ecdsa_pubkey,
                                             sizeof(b_ecdsa_pubkey),
                                             (UINT *)&b_ecdsa_pubkey_len);

    /* Sign once so there is something to verify. */
    extended.nx_crypto_extended_output_data           = b_ecdsa_sig;
    extended.nx_crypto_extended_output_length_in_byte = sizeof(b_ecdsa_sig);

    status = crypto_method_ecdsa.nx_crypto_operation(NX_CRYPTO_SIGNATURE_GENERATE,
                                                     handler,
                                                     &crypto_method_ecdsa,
                                                     b_ecdsa_privkey,
                                                     (NX_CRYPTO_KEY_SIZE)(buffer_size << 3),
                                                     b_plain, 32,
                                                     NX_CRYPTO_NULL,
                                                     (UCHAR *)&extended,
                                                     sizeof(extended),
                                                     &b_ecdsa_metadata,
                                                     sizeof(b_ecdsa_metadata),
                                                     NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if (status != NX_CRYPTO_SUCCESS)
    {
        b_fail("ECDSA P-256 verify", status);
        return;
    }
    b_ecdsa_sig_len = extended.nx_crypto_extended_output_actual_size;

    b_ecdsa_handler = handler;
    b_measure("ECDSA P-256 verify", b_ecdsa_verify_run, NX_CRYPTO_NULL, 0);

    (VOID) crypto_method_ecdsa.nx_crypto_cleanup(&b_ecdsa_metadata);
}


/* ------------------------------------------------------------ hash/bulk --- */

static UINT b_hash_once(NX_CRYPTO_METHOD *method, VOID *metadata,
                        ULONG metadata_size, const UCHAR *key, ULONG key_bits)
{

UINT    status;
VOID   *handler = NX_CRYPTO_NULL;


    status = method -> nx_crypto_init(method, (UCHAR *)key,
                                      (NX_CRYPTO_KEY_SIZE)key_bits,
                                      &handler, metadata, metadata_size);
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    status = method -> nx_crypto_operation(NX_CRYPTO_HASH_INITIALIZE, handler,
                                           method, (UCHAR *)key,
                                           (NX_CRYPTO_KEY_SIZE)key_bits,
                                           NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL,
                                           NX_CRYPTO_NULL, 0,
                                           metadata, metadata_size,
                                           NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    status = method -> nx_crypto_operation(NX_CRYPTO_HASH_UPDATE, handler,
                                           method, NX_CRYPTO_NULL, 0,
                                           b_plain, B_PAYLOAD, NX_CRYPTO_NULL,
                                           NX_CRYPTO_NULL, 0,
                                           metadata, metadata_size,
                                           NX_CRYPTO_NULL, NX_CRYPTO_NULL);
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    return method -> nx_crypto_operation(NX_CRYPTO_HASH_CALCULATE, handler,
                                         method, NX_CRYPTO_NULL, 0,
                                         NX_CRYPTO_NULL, 0, NX_CRYPTO_NULL,
                                         b_digest, sizeof(b_digest),
                                         metadata, metadata_size,
                                         NX_CRYPTO_NULL, NX_CRYPTO_NULL);
}

typedef struct
{
    NX_CRYPTO_METHOD   *method;
    VOID               *metadata;
    ULONG               metadata_size;
    const UCHAR        *key;
    ULONG               key_bits;
} B_HASH_CONTEXT;

static UINT b_hash_run(VOID *context)
{

B_HASH_CONTEXT *ctx = (B_HASH_CONTEXT *)context;


    return b_hash_once(ctx -> method, ctx -> metadata, ctx -> metadata_size,
                       ctx -> key, ctx -> key_bits);
}

static VOID b_bench_hash(const char *name, NX_CRYPTO_METHOD *method,
                         VOID *metadata, ULONG metadata_size,
                         const UCHAR *key, ULONG key_bits)
{

B_HASH_CONTEXT  ctx;


    ctx.method        = method;
    ctx.metadata      = metadata;
    ctx.metadata_size = metadata_size;
    ctx.key           = key;
    ctx.key_bits      = key_bits;

    b_measure(name, b_hash_run, &ctx, B_PAYLOAD);
}

static UINT b_cipher_once(NX_CRYPTO_METHOD *method, ULONG key_bits, BOOL gcm)
{

UINT    status;
VOID   *handler = NX_CRYPTO_NULL;


    status = method -> nx_crypto_init(method, b_key,
                                      (NX_CRYPTO_KEY_SIZE)key_bits,
                                      &handler, &b_aes_metadata,
                                      sizeof(b_aes_metadata));
    if (status != NX_CRYPTO_SUCCESS)
    {
        return(status);
    }

    if (gcm)
    {
        status = method -> nx_crypto_operation(NX_CRYPTO_SET_ADDITIONAL_DATA,
                                               handler, method,
                                               NX_CRYPTO_NULL, 0,
                                               b_aad, sizeof(b_aad),
                                               NX_CRYPTO_NULL,
                                               NX_CRYPTO_NULL, 0,
                                               &b_aes_metadata,
                                               sizeof(b_aes_metadata),
                                               NX_CRYPTO_NULL, NX_CRYPTO_NULL);
        if (status != NX_CRYPTO_SUCCESS)
        {
            return(status);
        }
    }

    return method -> nx_crypto_operation(NX_CRYPTO_ENCRYPT, handler, method,
                                         b_key, (NX_CRYPTO_KEY_SIZE)key_bits,
                                         b_plain, B_PAYLOAD,
                                         gcm ? b_gcm_iv : b_cbc_iv,
                                         b_cipher, sizeof(b_cipher),
                                         &b_aes_metadata,
                                         sizeof(b_aes_metadata),
                                         NX_CRYPTO_NULL, NX_CRYPTO_NULL);
}

typedef struct
{
    NX_CRYPTO_METHOD   *method;
    ULONG               key_bits;
    BOOL                gcm;
} B_CIPHER_CONTEXT;

static UINT b_cipher_run(VOID *context)
{

B_CIPHER_CONTEXT *ctx = (B_CIPHER_CONTEXT *)context;


    return b_cipher_once(ctx -> method, ctx -> key_bits, ctx -> gcm);
}

static VOID b_bench_cipher(const char *name, NX_CRYPTO_METHOD *method,
                           ULONG key_bits, BOOL gcm)
{

B_CIPHER_CONTEXT    ctx;


    ctx.method   = method;
    ctx.key_bits = key_bits;
    ctx.gcm      = gcm;

    b_measure(name, b_cipher_run, &ctx, B_PAYLOAD);
}


/* --------------------------------------------------- handshake arithmetic -- */

/*
 * Sums of measurements, not a measured handshake: what the public-key
 * arithmetic in each handshake shape costs, with the record layer, X.509
 * parsing, TCP round trips and the server's own latency all excluded.  A real
 * handshake cannot be faster than these numbers and will be slower.
 * Certificate-chain depth is taken as two signatures (leaf + one
 * intermediate), which is what the public web overwhelmingly serves.
 */
static VOID b_compose(VOID)
{

ULONG   rsa_pub;
ULONG   ecdhe;
ULONG   ecdsa;
ULONG   total;


    rsa_pub = b_us(B_RSA_PUBLIC);
    ecdhe   = b_us(B_ECDHE_KEYGEN) + b_us(B_ECDHE_SHARED);
    ecdsa   = b_us(B_ECDSA_VERIFY);

    b_log("");
    b_log("Handshake public-key arithmetic (sums of the above, ms):");

    /* TLS_RSA_WITH_*: 2 chain verifies + 1 premaster encrypt, all public ops. */
    total = rsa_pub * 3UL;
    b_log("  client, TLS_RSA_WITH_*        : %lu.%lu ms  (3 x RSA public)",
          total / 1000UL, (total % 1000UL) / 100UL);

    /* TLS_ECDHE_RSA_WITH_*: 2 chain verifies + SKE signature verify + ECDHE. */
    total = (rsa_pub * 3UL) + ecdhe;
    b_log("  client, TLS_ECDHE_RSA_WITH_*  : %lu.%lu ms  (3 x RSA public + ECDHE pair)",
          total / 1000UL, (total % 1000UL) / 100UL);

    /* TLS_ECDHE_ECDSA_WITH_*: 3 ECDSA verifies + ECDHE. */
    total = (ecdsa * 3UL) + ecdhe;
    b_log("  client, TLS_ECDHE_ECDSA_WITH_*: %lu.%lu ms  (3 x ECDSA verify + ECDHE pair)",
          total / 1000UL, (total % 1000UL) / 100UL);

    /* Server side adds one private op; CRT if the primes are available. */
    total = b_us(B_RSA_PRIVATE_CRT);
    b_log("  server, RSA-2048 cert, CRT    : %lu.%lu ms  (1 x RSA private, plus the client work above)",
          total / 1000UL, (total % 1000UL) / 100UL);
    total = b_us(B_RSA_PRIVATE);
    b_log("  server, RSA-2048 cert, no CRT : %lu.%lu ms",
          total / 1000UL, (total % 1000UL) / 100UL);
}

/*
 * Session memory.  The 1 MB floor already spends 17 x 1568 bytes on the
 * packet pool (docs/RESEARCH.md 81), so what matters is what a TLS session adds
 * on top of a socket, per connection.
 */
static VOID b_memory(VOID)
{

ULONG   metadata;
ULONG   session;
ULONG   per_conn;


    metadata = (ULONG)sizeof(NX_CRYPTO_RSA) + (ULONG)sizeof(NX_CRYPTO_ECDH) +
               (ULONG)sizeof(NX_CRYPTO_ECDSA) + (ULONG)sizeof(NX_CRYPTO_AES) +
               (ULONG)(2 * sizeof(NX_CRYPTO_SHA256)) +
               (ULONG)sizeof(NX_CRYPTO_SHA256_HMAC);

    session = (ULONG)sizeof(NX_SECURE_TLS_SESSION);

    /*
     * NX_SECURE_TLS_MINIMUM_MESSAGE_BUFFER_SIZE is 4000 and is the packet
     * reassembly buffer the caller has to supply to
     * nx_secure_tls_session_packet_buffer_set().  A handshake that carries a
     * full certificate chain does not fit in less.
     */
    per_conn = session + metadata + NX_SECURE_TLS_MINIMUM_MESSAGE_BUFFER_SIZE;

    b_log("");
    b_log("Memory, per TLS session:");
    b_log("  NX_SECURE_TLS_SESSION       %lu bytes", session);
    b_log("  crypto metadata (all algos) %lu bytes", metadata);
    b_log("    NX_CRYPTO_RSA             %lu", (ULONG)sizeof(NX_CRYPTO_RSA));
    b_log("    NX_CRYPTO_ECDH            %lu", (ULONG)sizeof(NX_CRYPTO_ECDH));
    b_log("    NX_CRYPTO_ECDSA           %lu", (ULONG)sizeof(NX_CRYPTO_ECDSA));
    b_log("    NX_CRYPTO_AES             %lu", (ULONG)sizeof(NX_CRYPTO_AES));
    b_log("    NX_CRYPTO_SHA256 x2       %lu", (ULONG)(2 * sizeof(NX_CRYPTO_SHA256)));
    b_log("    NX_CRYPTO_SHA256_HMAC     %lu", (ULONG)sizeof(NX_CRYPTO_SHA256_HMAC));
    b_log("  handshake reassembly buffer %lu bytes",
          (ULONG)NX_SECURE_TLS_MINIMUM_MESSAGE_BUFFER_SIZE);
    b_log("  ---------------------------------------");
    b_log("  per connection, minimum     %lu bytes", per_conn);
    b_log("  NX_SECURE_X509_CERT (each)  %lu bytes",
          (ULONG)sizeof(NX_SECURE_X509_CERT));
    b_log("  plus the DER of every trusted root you carry (~1 KB each)");
}


/* ----------------------------------------------------------------- main --- */

int main(VOID)
{

ULONG   seed;
ULONG   hz;


    ami_crash_set_reference((APTR)main, "tls_bench");
    if (!ami_crash_install())
    {
        /*
         * A caught exception resumes here.  crashguard.h is explicit that the
         * unwind is not trustworthy, so get the report out and fail.
         */
        b_log("CRASHED, see the serial log and DH0:crash.txt");
        b_flush();
        ami_crash_remove();
        return(20);
    }

    b_log("AmiNetXDuo, nx_secure crypto benchmark (docs/RESEARCH.md 9 gate)");

    if (!ami_tls_timer_open())
    {
        b_log("FATAL: timer.device UNIT_ECLOCK would not open");
        b_flush();
        ami_crash_remove();
        return(20);
    }

    hz = ami_tls_eclock_hz();
    b_log("  E-Clock %lu Hz (%lu ns resolution)", hz,
          (hz != 0) ? (1000000000UL / hz) : 0UL);

    seed = ami_tls_seed_rng();
    b_log("  entropy pool credits itself %lu bits, see "
          "include/aminetxduo/random.h", seed);
    b_log("  ami_random_is_seeded() = %s; a real handshake must refuse or be "
          "given a seed",
          (LONG)(ami_random_is_seeded() ? "TRUE" : "FALSE"));

    b_fill_payload();

    b_fill_payload();
    b_started = ami_tls_eclock();

    /*
     * Bulk first.  It is the fast half, and the RSA private operation at the
     * end can run for minutes on the floor target, if the harness timeout
     * bites, everything above it is already on the serial port.
     */
    b_log("");
    b_log("Bulk (per %lu-byte record):", (ULONG)B_PAYLOAD);
    b_bench_hash("SHA-256          ", &crypto_method_sha256,
                 &b_sha_metadata, sizeof(b_sha_metadata), NX_CRYPTO_NULL, 0);
    b_bench_hash("HMAC-SHA-256     ", &crypto_method_hmac_sha256,
                 &b_hmac_metadata, sizeof(b_hmac_metadata), b_key, 256);
    b_bench_cipher("AES-128-CBC      ", &crypto_method_aes_cbc_128, 128, FALSE);
    b_bench_cipher("AES-256-CBC      ", &crypto_method_aes_cbc_256, 256, FALSE);
    b_bench_cipher("AES-128-GCM      ", &crypto_method_aes_128_gcm_16, 128, TRUE);
    b_bench_cipher("AES-256-GCM      ", &crypto_method_aes_256_gcm_16, 256, TRUE);

    b_log("");
    b_log("Public key (one per handshake):");
    b_bench_ecdhe();
    b_bench_ecdsa();
    b_bench_rsa();

    b_compose();
    b_memory();

    b_log("");
    b_log("Benchmark wall time: %lu s",
          ami_tls_eclock_micros(ami_tls_eclock() - b_started) / 1000000UL);

    if (b_failures == 0)
    {
        b_log("%lu measurements, 0 failures, PASS", b_result_count);
    }
    else
    {
        b_log("%lu measurements, %lu failures, FAIL", b_result_count,
              b_failures);
    }

    ami_tls_timer_close();
    b_flush();
    ami_crash_remove();

    return((b_failures == 0) ? 0 : 20);
}
