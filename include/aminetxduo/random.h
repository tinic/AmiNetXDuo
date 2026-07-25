/*
 * AmiNetXDuo -- entropy pool and random number generation.
 *
 * READ THIS BEFORE TRUSTING IT WITH A KEY.
 *
 * A classic Amiga has no hardware RNG, no /dev/urandom, no RDRAND and no
 * jitter source that anyone has ever analysed.  What this module does is the
 * standard fallback: sample a lot of individually weak things, mix them all
 * through SHA-256, and expand the result with a hash-based DRBG.  The
 * expansion is sound.  The *input* is the problem, and no amount of hashing
 * creates entropy that was not there.
 *
 * The honest summary, stated so nobody has to guess:
 *
 *   - The conditioning (SHA-256 mixing, counter-mode expansion, forward
 *     ratchet) is textbook and is not where the risk lives.
 *   - The collection is UNAUDITED and its yield is UNMEASURED on real
 *     hardware.  ami_random_entropy_bits() returns this module's own
 *     conservative *guess*, not a measurement.  Treat it as a lower bound on
 *     nothing; it is an accounting convenience.
 *   - Several of the sources ARE identical run to run on a fixed boot image,
 *     measured: every AvailMem() figure, the AllocVec() addresses, and the
 *     "uninitialised" memory residue were byte-for-byte the same over three
 *     cold boots under FS-UAE.  They are mixed and credited nothing.  Which
 *     ones those are is a property of the machine, not of this code, so
 *     re-run tools/smoke/randtest.c on any new target rather than assuming
 *     these findings carry over.
 *   - Nothing here has been reviewed by anyone who does this for a living.
 *
 * Consequence, and it is deliberate: ami_random_is_seeded() reports FALSE
 * until the pool has been credited AMI_RANDOM_MIN_BITS from sources this
 * module is willing to count.  Anything generating a long-lived or
 * adversarially exposed secret -- TLS key agreement above all -- is expected
 * to check it and REFUSE rather than proceed with a guess.  Callers that
 * genuinely have entropy (an operator seed, a persisted seed file, mouse or
 * keystroke timing from a Process) feed it in with ami_random_add_entropy()
 * and say how much they think it is worth.
 *
 * For the stack's own non-secret uses -- IP identification fields, TCP
 * initial sequence numbers, ephemeral port selection, DHCP transaction ids,
 * DNS query ids -- the generator is used unconditionally and is a strict
 * improvement on the 32-bit LCG it replaces.  Those uses want
 * unpredictability, not secrecy, and an off-path attacker guessing them is a
 * much weaker adversary than one attacking a key.
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
 * The bar ami_random_is_seeded() has to clear.  64 bits is not a security
 * level -- it is the point below which this module refuses to pretend.
 *
 * THE INTERNAL COLLECTION CANNOT REACH IT, BY CONSTRUCTION.  The four sources
 * that are credited anything at all cap out at 8 + 4 + 2 + 12 = 26 bits, and
 * measured 31 bits before the accounting was corrected and ~21 after.  So
 * ami_random_is_seeded() is FALSE on a machine left to itself, always, and
 * the only way to make it TRUE is ami_random_add_entropy() with a credit the
 * caller is prepared to stand behind.
 *
 * That is the design, not an oversight.  A vintage machine with no hardware
 * RNG cannot honestly self-seed to a level worth putting a key on, and a
 * module that returned TRUE anyway would just be moving the lie somewhere
 * harder to find.
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
