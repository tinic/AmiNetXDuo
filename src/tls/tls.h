/*
 * AmiNetXDuo -- nx_secure glue.
 *
 * Deliberately tiny.  docs/RESEARCH.md 9 gates the TLS work on a 68020
 * benchmark (tests/tls/tls_bench), so nothing here promises an API shape.
 * What lives here is the platform glue nx_secure needs no matter what shape
 * the eventual library takes.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLS_H
#define AMINETXDUO_TLS_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Seed the generator nx_crypto reaches for, and report how good it is not.
 *
 * NX_RAND now points at src/common/ami_random.c (see
 * port/netxduo-amiga/inc/nx_port.h), a SHA-256 hash DRBG over an entropy
 * pool, instead of newlib's 32-bit LCG.  That fixes the *expansion*; it does
 * not conjure entropy onto a machine that has none.
 *
 * Returns the pool's own credited entropy estimate in bits.  A caller about
 * to perform a real TLS handshake must treat a value below
 * AMI_RANDOM_MIN_BITS as a refusal to proceed -- ami_random_is_seeded() is
 * the same test -- and either obtain a seed from the operator, a persisted
 * seed file or user input timing and feed it to ami_random_add_entropy(), or
 * decline to generate the key.  Generating an ECDHE private key from a pool
 * that credits itself 6 bits is worse than not offering TLS at all, because
 * the caller believes it worked.
 */
ULONG ami_tls_seed_rng(VOID);

/*
 * Microsecond timer backed by timer.device's E-Clock (~709 kHz PAL /
 * ~715 kHz NTSC, i.e. ~1.4 us resolution).  ami_millis() in compat.c is
 * millisecond-granular, which is too coarse for a single AES block or SHA
 * round; every figure in the TLS benchmark comes from these.
 *
 * ami_tls_timer_open() is idempotent and returns FALSE if timer.device will
 * not open.  ami_tls_eclock() returns the raw 32-bit E-Clock low word, which
 * wraps every ~100 minutes; a difference stays correct across a single wrap.
 * ami_tls_eclock_hz() is the tick rate the machine actually reported.
 */
BOOL  ami_tls_timer_open(VOID);
VOID  ami_tls_timer_close(VOID);
ULONG ami_tls_eclock(VOID);
ULONG ami_tls_eclock_hz(VOID);

/* Convert an E-Clock tick delta to microseconds (64-bit intermediate). */
ULONG ami_tls_eclock_micros(ULONG ticks);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TLS_H */
