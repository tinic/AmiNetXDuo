/*
 * AmiNetXDuo -- entropy pool and random number generation.
 *
 * A classic Amiga has no hardware RNG.  This samples many individually weak
 * sources, mixes them through SHA-256 and expands with a hash DRBG.  The
 * conditioning is textbook; the input is the weak part, and hashing does not
 * create entropy that was not there.
 *
 * The collection is unaudited and its yield unmeasured on real hardware.
 * ami_random_entropy_bits() returns this module's own conservative guess, not
 * a measurement.  Several sources are identical run to run on a fixed boot
 * image -- every AvailMem() figure, the AllocVec() addresses and the
 * uninitialised memory residue were byte-for-byte equal over three cold boots
 * under FS-UAE.  They are mixed and credited nothing.  That is a property of
 * the machine rather than of this code, so re-run tools/smoke/randtest.c on a
 * new target instead of assuming it carries over.
 *
 * ami_random_is_seeded() reports whether the pool reached
 * AMI_RANDOM_MIN_BITS from sources this module will count.  Left to itself it
 * is FALSE, because the internal sources cap below that bar.  Nothing refuses
 * to run because of it and TLS does not check it; a caller with real entropy
 * feeds it in through ami_random_add_entropy().
 *
 * For what the stack uses it for -- IP ids, TCP initial sequence numbers,
 * ephemeral ports, DHCP and DNS transaction ids, and TLS key agreement on a
 * machine fetching web pages -- it is a strict improvement on the 32-bit LCG
 * it replaces, at 21 ms once at init.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_RANDOM_H
#define AMINETXDUO_RANDOM_H

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The bar ami_random_is_seeded() has to clear.  The internal sources cap at
 * 8 + 4 + 2 + 12 = 26 bits and measure ~21, so it is FALSE unless a caller
 * supplies entropy via ami_random_add_entropy().  That is expected on a
 * vintage machine with no hardware RNG; it is reported, not enforced.
 */
#define AMI_RANDOM_MIN_BITS     64UL

/*
 * Gather from every source we have and mix into the pool.  Safe to call
 * repeatedly -- each call only ever adds.  Costs 21-22 ms on an emulated
 * 68020, nearly all of it in the E-Clock jitter sampling.  Called lazily by
 * the generation functions if nothing called it first, so there is no
 * ordering requirement; call it early anyway, from a context where 22 ms does
 * not matter, because the lazy path would otherwise land on whatever sends
 * the first packet.
 */
VOID ami_random_init(VOID);

/*
 * Mix caller-supplied material in.  credit_bits is the caller's own estimate
 * of how much unpredictability the material carries, and it is added to the
 * pool's running total; pass 0 for material that is merely unique (a MAC
 * address, a boot count) rather than unpredictable.  The material is always
 * mixed regardless of the credit claimed.
 *
 * Never replaces the pool: a caller cannot make the state worse by supplying
 * something bad, only fail to make it better.
 */
VOID ami_random_add_entropy(const void *data, ULONG length, ULONG credit_bits);

/* Fill a buffer.  Seeds on first use if ami_random_init() was never called. */
VOID ami_random_bytes(APTR buffer, ULONG length);

/* One 32-bit value.  Same generator, same caveats. */
ULONG ami_random_ulong(VOID);

/*
 * This module's own running estimate of the entropy credited to the pool, in
 * bits, saturating at 256.  It is a bookkeeping figure derived from fixed
 * per-source guesses and a few "did this actually vary?" checks -- NOT a
 * measurement of the generator's output, and NOT evidence of anything to an
 * attacker.  Its only real job is to drive ami_random_is_seeded().
 */
ULONG ami_random_entropy_bits(VOID);

/* TRUE once ami_random_entropy_bits() >= AMI_RANDOM_MIN_BITS. */
BOOL ami_random_is_seeded(VOID);

/*
 * NX_RAND / NX_SRAND bindings -- port/netxduo-amiga/inc/nx_port.h points the
 * NetX Duo and nx_secure macros at these.  ami_random_rand() returns
 * 0..0x7FFFFFFF like C's rand().
 *
 * ami_random_srand() MIXES its argument in and leaves the credit alone.  It
 * deliberately does not reset the generator: NX_SRAND exists so an
 * application can make a run reproducible, and honouring that literally would
 * turn a caller's convenience into a key-recovery bug.
 */
int  ami_random_rand(void);
void ami_random_srand(unsigned int seed);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_RANDOM_H */
