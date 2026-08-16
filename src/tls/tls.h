/*
 * AmiNetXDuo, nx_secure glue.
 *
 * The platform glue nx_secure needs, and nothing else.  The published API is
 * include/aminetxduo/tlslib.h, which src/tlslib/ builds into tls.library.
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
 * Seed the generator nx_crypto uses, and report the pool's entropy estimate.
 *
 * NX_RAND points at src/common/ami_random.c (see
 * port/netxduo-amiga/inc/nx_port.h), a SHA-256 hash DRBG over an entropy
 * pool, instead of newlib's 32-bit LCG.  That fixes the expansion.  It does
 * not create entropy on a machine that has none.
 *
 * Returns the pool's own credited entropy estimate in bits.  A caller about
 * to perform a real TLS handshake must treat a value below
 * AMI_RANDOM_MIN_BITS as a refusal to proceed.  ami_random_is_seeded() is the
 * same test.  Such a caller must then seed the pool through
 * ami_random_add_entropy(), from the operator, a persisted seed file or user
 * input timing.  If it cannot, it must decline to generate the key.  An ECDHE
 * private key from a pool that credits itself 6 bits is worse than no TLS at
 * all, because the caller believes it worked.
 */
ULONG ami_tls_seed_rng(VOID);

/*
 * Microsecond timer backed by timer.device's E-Clock (~709 kHz PAL,
 * ~715 kHz NTSC, that is ~1.4 us resolution).  ami_millis() in compat.c is
 * millisecond-granular, which is too coarse for a single AES block or SHA
 * round.  Every figure in the TLS benchmark comes from these.
 *
 * ami_tls_timer_open() is idempotent and returns FALSE if timer.device will
 * not open.  ami_tls_eclock() returns the raw 32-bit E-Clock low word, which
 * wraps every ~100 minutes.  A difference stays correct across a single wrap.
 * ami_tls_eclock_hz() is the tick rate the machine reported.
 */
BOOL  ami_tls_timer_open(VOID);
VOID  ami_tls_timer_close(VOID);

/*
 * TRUE after ami_tls_timer_open() succeeds.  ami_tls_crypto.c's
 * instrumentation asks this rather than calling ami_tls_timer_open() itself,
 * because a crypto method must not do an OpenDevice() in the middle of a
 * handshake.  An application that never opened the timer gets counts with no
 * microseconds.
 */
BOOL  ami_tls_timer_is_open(VOID);
ULONG ami_tls_eclock(VOID);
ULONG ami_tls_eclock_hz(VOID);

/* Convert an E-Clock tick delta to microseconds (64-bit intermediate). */
ULONG ami_tls_eclock_micros(ULONG ticks);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TLS_H */
