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
 * A SOURCE THAT HAS PRODUCED NOTHING IN THIS MANY BATCHES NEVER WILL, and
 * credit used to be the ONLY way out.  arrival_bits grows only when the low
 * bits of the inter-arrival delta MOVE across a batch of sixteen; a perfectly
 * regular cadence -- which a bridged emulator or a hardware pacer can produce
 * -- makes that zero, credits nothing, and leaves the gate open for the life
 * of the machine: one SHA-256 compression every sixteen frames, on the receive
 * path, forever.
 *
 * A healthy source credits up to eight bits a batch and stops in eight.  One
 * crediting a SINGLE bit a batch stops in sixty-four.  This is twice that
 * again, so it cannot truncate a source producing anything at all, and it
 * stops a barren one after about two thousand frames.
 *
 * It gives up no entropy: a source contributing zero bits contributes zero
 * whether it is consulted or not.  What the bound removes is the cost of
 * asking.
 */
#define AMI_RANDOM_ARRIVAL_MAX_BATCHES  128UL

/*
 * AND A BARREN SOURCE IS RECOGNISED IN SIXTEEN BATCHES, NOT A HUNDRED AND
 * TWENTY-EIGHT.
 *
 * The bound above is an absolute backstop and it is the one this rig actually
 * hits: a bridged emulator paces arrivals regularly, `varying` comes out zero
 * batch after batch, and nothing stops the sampling until 128 x 16 = 2,048
 * FRAMES have gone past.  Every one of them pays ReadEClock -- a timer.device
 * call -- plus a Forbid/Permit pair measured at 314 ns on this rig, and every
 * sixteenth pays a SHA-256 compression.  A ten-second iperf receive is about
 * five thousand frames, so FORTY PER CENT OF THE RUN was sampling a source
 * that had already said it has nothing, and `_sha256_k 0.7%` in a pure-receive
 * profile is 128 compressions, not the eight the healthy case predicts.
 *
 * Consecutive, not cumulative: one batch that credits nothing is ordinary, and
 * any credit at all resets the count.  A source that credits a single bit
 * every sixteenth batch still runs to the 64-bit ceiling.  Sixteen consecutive
 * zero-credit batches is 256 frames in which the low eight bits of the
 * inter-arrival delta did not move once.
 *
 * IT GIVES UP NO ENTROPY.  A source contributing zero bits contributes zero
 * whether it is asked 256 times or 2,048.  What the bound removes is the cost
 * of asking, and the outcome -- ami_random_is_seeded() false, reported once
 * and not enforced -- is the same either way.
 */
#define AMI_RANDOM_ARRIVAL_MAX_BARREN   16UL

/*
 * Has frame-arrival sampling finished?  A header inline rather than a private
 * static so the host tier can assert the bound directly -- the collector
 * itself needs Exec, ReadEClock and a timer base, and none of that is needed
 * to answer this.
 */
static inline int ami_random_arrival_stop(unsigned long bits,
                                          unsigned long batches,
                                          unsigned long pool_bits,
                                          unsigned long barren)
{
    return (bits >= AMI_RANDOM_ARRIVAL_MAX_BITS ||
            pool_bits >= AMI_RANDOM_MIN_BITS ||
            barren >= AMI_RANDOM_ARRIVAL_MAX_BARREN ||
            batches >= AMI_RANDOM_ARRIVAL_MAX_BATCHES) ? 1 : 0;
}

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
