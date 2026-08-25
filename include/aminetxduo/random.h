/*
 * AmiNetXDuo, entropy pool and random number generation.  The collection is
 * unaudited: entropy_bits() is this module's own guess, not a measurement.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_RANDOM_H
#define AMINETXDUO_RANDOM_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bar ami_random_is_seeded() has to clear.  Only frame-arrival timing can
   take the pool over it, and falling short is reported, not enforced. */
#define AMI_RANDOM_MIN_BITS     64UL

/* The most the built-in clock, Exec-state, task-list and jitter sources can
   contribute in total, however many times they are sampled. */
#define AMI_RANDOM_INTERNAL_MAX_BITS  26UL

/* And the most frame arrival timing may contribute, after which the sampling
   switches itself off for the life of the machine. */
#define AMI_RANDOM_ARRIVAL_MAX_BITS   64UL

/*
 * Safe to call repeatedly; each call only ever adds.  Called lazily by the
 * generation functions, so there is no ordering requirement, but it blocks for
 * tens of milliseconds -- call it early, not on the first packet.
 */
VOID ami_random_init(VOID);

/*
 * credit_bits is the caller's own estimate and is added to the pool's running
 * total; pass 0 for material that is merely unique rather than unpredictable.
 * The material is always mixed, and never replaces the pool.
 */
VOID ami_random_add_entropy(const void *data, ULONG length, ULONG credit_bits);

/*
 * One frame has arrived.  Called from the SANA-II receive path, once per
 * delivered frame, and from nowhere else.  NOT interrupt-callable.
 */
VOID ami_random_arrival(VOID);

/* Fill a buffer.  Seeds on first use if ami_random_init() was never called. */
VOID ami_random_bytes(APTR buffer, ULONG length);

/* One 32-bit value.  Same generator, same caveats. */
ULONG ami_random_ulong(VOID);

/* A bookkeeping estimate of the credit given to the pool, in bits, saturating
   at 256.  Not a measurement of the generator's output. */
ULONG ami_random_entropy_bits(VOID);

/* TRUE once ami_random_entropy_bits() >= AMI_RANDOM_MIN_BITS. */
BOOL ami_random_is_seeded(VOID);

/*
 * NX_RAND / NX_SRAND bindings.  ami_random_rand() returns 0..0x7FFFFFFF like
 * C's rand().  ami_random_srand() only mixes its argument in; it must never
 * reset the generator, whatever NX_SRAND's contract says.
 */
int  ami_random_rand(void);
void ami_random_srand(unsigned int seed);

/*
 * NX_CRYPTO_RBG binding.  Writes ceil(bits/8) bytes and always succeeds; 0 is
 * NX_CRYPTO_SUCCESS.  Plain C types because nx_port.h repeats this declaration
 * by hand.
 */
unsigned int ami_crypto_rbg(unsigned int bits, unsigned char *result);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_RANDOM_H */
