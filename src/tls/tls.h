/* AmiNetXDuo, the platform glue nx_secure needs.  The published API is
 * include/aminetxduo/tlslib.h, which src/tlslib/ builds into tls.library.
 * SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_TLS_H
#define AMINETXDUO_TLS_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Seed the generator nx_crypto uses; returns the pool's credited entropy in
 * bits.  A caller about to perform a real handshake MUST treat a value below
 * AMI_RANDOM_MIN_BITS as a refusal, and seed via ami_random_add_entropy().
 */
ULONG ami_tls_seed_rng(VOID);

/*
 * E-Clock microsecond timer (~709 kHz PAL).  ami_tls_timer_open() is idempotent
 * and FALSE if timer.device will not open; ami_tls_eclock() is the raw 32-bit
 * low word, wrapping every ~100 minutes, and a difference survives one wrap.
 */
BOOL  ami_tls_timer_open(VOID);
VOID  ami_tls_timer_close(VOID);

/*
 * TRUE once ami_tls_timer_open() has succeeded.  A crypto method must not
 * OpenDevice() in the middle of a handshake, so ami_tls_crypto.c asks this
 * rather than opening the timer itself.
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
