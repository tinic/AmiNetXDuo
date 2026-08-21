/*
 * AmiNetXDuo, entropy pool and random number generation.
 *
 * A classic Amiga has no hardware RNG.  This samples many individually weak
 * sources, mixes them through SHA-256 and expands with a hash DRBG.  The
 * conditioning is textbook; the input is the weak part, and hashing does not
 * create entropy that was not there.
 *
 * The collection is unaudited and its yield unmeasured on real hardware.
 * ami_random_entropy_bits() returns this module's own conservative guess, not a
 * measurement.  Several sources are identical run to run on a fixed boot image:
 * every AvailMem() figure, the AllocVec() addresses and the uninitialised memory
 * residue were byte-for-byte equal over three cold boots under FS-UAE.  They are
 * mixed and credited nothing.  That is a property of the machine rather than of
 * this code, so re-run tools/smoke/randtest.c on a new target instead of
 * assuming it carries over.
 *
 * ami_random_is_seeded() reports whether the pool reached AMI_RANDOM_MIN_BITS
 * from sources this module will count.  The internal sources cap at 26 bits,
 * below that bar, because they describe the machine and a fixed boot image
 * repeats most of them.  What clears the bar is ami_random_arrival(), fed from
 * the receive path: when a frame lands is not a property of the boot image.
 * A caller with better entropy still feeds it in through
 * ami_random_add_entropy().
 *
 * For what the stack uses it for, IP ids, TCP initial sequence numbers,
 * ephemeral ports, DHCP and DNS transaction ids, and TLS key agreement, it is
 * an improvement on the 32-bit LCG it replaces, at 21 ms once at init.
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
 * 8 + 4 + 2 + 12 = 26 bits and measure ~21, so nothing that only describes
 * the machine can reach it however many times it is sampled.  Frame arrival
 * timing is what takes the pool over it, within seconds of an interface
 * carrying traffic; before that, and on a machine with no interface up, it
 * stays FALSE and is reported, not enforced.
 */
#define AMI_RANDOM_MIN_BITS     64UL

/* The most the built-in clock, Exec-state, task-list and jitter sources can
   contribute in total, however many times they are sampled. */
#define AMI_RANDOM_INTERNAL_MAX_BITS  26UL

/* And the most frame arrival timing may contribute, after which the sampling
   switches itself off for the life of the machine. */
#define AMI_RANDOM_ARRIVAL_MAX_BITS   64UL

/*
 * Gather from every source we have and mix into the pool.  Safe to call
 * repeatedly; each call only ever adds.  Costs 21-22 ms on an emulated 68020,
 * nearly all of it in the E-Clock jitter sampling.  Called lazily by the
 * generation functions if nothing called it first, so there is no ordering
 * requirement; call it early anyway, from a context where 22 ms does not
 * matter, because the lazy path would otherwise land on whatever sends the
 * first packet.
 */
VOID ami_random_init(VOID);

/*
 * Mix caller-supplied material in.  credit_bits is the caller's own estimate
 * of how much unpredictability the material carries, and it is added to the
 * pool's running total; pass 0 for material that is merely unique (a MAC
 * address, a boot count) rather than unpredictable.  The material is always
 * mixed regardless of the credit claimed.
 *
 * Never replaces the pool, so a caller cannot make the state worse by supplying
 * something bad, only fail to make it better.
 */
VOID ami_random_add_entropy(const void *data, ULONG length, ULONG credit_bits);

/*
 * One frame has arrived.  Called from the SANA-II receive path, once per
 * delivered frame, and from nowhere else.
 *
 * When a frame lands is decided by a remote clock, the wire, the card's
 * interrupt latency and Exec's dispatcher, none of which is in the boot image,
 * so this is the only source on the machine worth real credit.  Only the low
 * bits of the interval are kept -- the part a peer pacing its own traffic
 * cannot set -- and the credit is measured the way gather_jitter() measures
 * its own, one bit per bit position that varied across a batch of sixteen.
 *
 * Costs one load and a branch once AMI_RANDOM_ARRIVAL_MAX_BITS is reached or
 * the pool is over AMI_RANDOM_MIN_BITS, whichever comes first, and never
 * restarts.  Not interrupt-callable, for pool_mix()'s reasons.
 */
VOID ami_random_arrival(VOID);

/* Fill a buffer.  Seeds on first use if ami_random_init() was never called. */
VOID ami_random_bytes(APTR buffer, ULONG length);

/* One 32-bit value.  Same generator, same caveats. */
ULONG ami_random_ulong(VOID);

/*
 * This module's own running estimate of the entropy credited to the pool, in
 * bits, saturating at 256.  It is a bookkeeping figure derived from fixed
 * per-source guesses and a few "did this vary?" checks, not a measurement of
 * the generator's output.  Its job is to drive ami_random_is_seeded().
 */
ULONG ami_random_entropy_bits(VOID);

/* TRUE once ami_random_entropy_bits() >= AMI_RANDOM_MIN_BITS. */
BOOL ami_random_is_seeded(VOID);

/*
 * NX_RAND / NX_SRAND bindings, port/netxduo-amiga/inc/nx_port.h points the
 * NetX Duo and nx_secure macros at these.  ami_random_rand() returns
 * 0..0x7FFFFFFF like C's rand().
 *
 * ami_random_srand() mixes its argument in and leaves the credit alone.  It
 * does not reset the generator: NX_SRAND exists so an application can make a
 * run reproducible, and honouring that literally would turn a caller's
 * convenience into a key-recovery bug.
 */
int  ami_random_rand(void);
void ami_random_srand(unsigned int seed);

/*
 * NX_CRYPTO_RBG binding.  nx_crypto's own is _nx_crypto_huge_number_rbg(),
 * which builds the number one NX_CRYPTO_RAND() per 32 bits, and
 * NX_CRYPTO_RAND is ami_random_rand(), whose contract is rand()'s 0..0x7FFFFFFF.
 * The top bit of every word it packs is therefore always zero, in the ECDHE
 * private key among other things.  This fills the buffer from the generator
 * directly instead, which is where the bytes were coming from anyway.
 *
 * Writes ceil(bits/8) bytes, like the function it replaces, and always
 * succeeds: 0 is NX_CRYPTO_SUCCESS.  Plain C types because nx_port.h has to
 * repeat this declaration by hand, they are UINT and UCHAR on both ports.
 */
unsigned int ami_crypto_rbg(unsigned int bits, unsigned char *result);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_RANDOM_H */
