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
 * Seed the C library RNG that nx_crypto reaches for.
 *
 * NX_RAND is undefined in port/netxduo-amiga/inc/nx_port.h, so nx_api.h falls
 * back to newlib's rand().  That is fine for a benchmark and NOT fine for a
 * shipping TLS client: rand() is a 32-bit LCG, and ECDHE private keys plus the
 * TLS 1.2 client random come out of it.  Anything that ships must define
 * NX_RAND to a real entropy path -- see the note in tests/tls/tls_bench.c.
 *
 * Returns the seed used, so a test can print it and be reproducible.
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
