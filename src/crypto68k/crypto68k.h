/*
 * AmiNetXDuo -- crypto68k: 68020 multi-precision arithmetic for nx_crypto.
 *
 * WHY THIS EXISTS
 *
 *   docs/RESEARCH.md 9 makes TLS conditional on a 68020 benchmark, and the
 *   thing being benchmarked is dominated by one loop: the multiply-accumulate
 *   at the heart of Montgomery multiplication.  A TLS 1.2 client doing
 *   TLS_ECDHE_RSA or TLS_RSA spends most of its handshake there.
 *
 *   This module is a drop-in replacement for the exponentiation half of
 *   third_party/netxduo/crypto_libraries/src/nx_crypto_huge_number.c.  The
 *   vendored file is not touched; it stays the reference implementation that
 *   tests/crypto68k/rsa_test cross-checks every result against.
 *
 * WHAT IS FASTER, AND WHY
 *
 *   1. Exponent leading-zero bits are skipped.  The vendored
 *      _nx_crypto_huge_number_mont_power_modulus walks all 32 bits of the top
 *      exponent word, so e = 65537 (0x00010001) costs 32 squarings where 16
 *      are needed.  That alone is ~1.9x on every RSA *public* operation --
 *      certificate signature verification, which is what a TLS client does
 *      three times per handshake.
 *
 *   2. Sliding-window exponentiation replaces bit-at-a-time square-and-
 *      multiply for long exponents (the RSA private operation).
 *
 *   3. Montgomery squaring is a separate routine.  Squaring the multiplicand
 *      half costs s(s+1)/2 limb products instead of s^2, so a Montgomery
 *      square is ~3/4 of a Montgomery multiply; in an exponentiation almost
 *      every operation is a squaring.
 *
 *   4. The limb multiply-accumulate loop is hand-written 68020 assembly.  GCC
 *      does emit MULU.L for the portable C (verified -- see the header comment
 *      in c68k_prim.S), so this is the SMALLEST of the four levers, not the
 *      largest: it is worth 1.4x, and MULU.L's 44 cycles are the floor.
 *
 * MEASURED, emulated 68020, tests/crypto68k/crypto68k_bench, every timed
 * result checked against the vendored code in the same run:
 *
 *     limb multiply-accumulate, 64 limbs   373 us  ->    264 us    1.4x
 *     Montgomery multiply, 2048 bit      52.17 ms  ->  34.36 ms    1.5x
 *     Montgomery square,   2048 bit      52.17 ms  ->  26.90 ms    1.9x
 *     RSA-2048 public, e=65537            2.011 s  ->  0.681 s     2.9x
 *     RSA-2048 private, CRT              44.75 s   -> 20.05 s      2.2x
 *     RSA-2048 private, plain           160.83 s   -> 66.40 s      2.4x
 *
 * Ratios, not absolutes, are the trustworthy figures -- FS-UAE's 68020 is not
 * a 14 MHz A1200 and its 68030 model is not a timing model at all (the same
 * binary on the same input produced 597 ms and 337 ms for the same CRT
 * measurement on consecutive runs).
 *
 * CONSTANT TIME: NO.  See the note above c68k_mont_power_modulus().
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_CRYPTO68K_H
#define AMINETXDUO_CRYPTO68K_H

#include "nx_crypto_huge_number.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Limb type.  Deliberately spelled out rather than inherited, because the
 * assembly is written for exactly this: 32-bit limbs, big-endian host, little-
 * endian limb order (limb 0 least significant), which is what HN_UBASE with
 * NX_CRYPTO_HUGE_NUMBER_BITS == 32 gives us.
 */
typedef HN_UBASE    c68k_limb;

#if (NX_CRYPTO_HUGE_NUMBER_BITS != 32)
#error "crypto68k requires NX_CRYPTO_HUGE_NUMBER_BITS == 32"
#endif


/* ------------------------------------------------------------ primitives -- */

/*
 * r[0..n-1] += a * b[0..n-1], carry propagating.  Returns the carry out of
 * the top limb (a full limb, not a single bit).
 *
 * This is the multiply-accumulate every multi-precision algorithm is built
 * from -- OpenSSL calls it bn_mul_add_words, GMP calls it addmul_1 -- and on
 * this machine it is where essentially all the time goes.
 *
 * The 64-bit intermediate cannot overflow: the largest possible value is
 * (2^32-1)^2 + 2*(2^32-1) = 2^64-1 exactly.
 */
c68k_limb c68k_addmul_1(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a);

/*
 * The portable C version, always present under its own name whichever build
 * option is in force.  The benchmark times the two against each other in one
 * run, because a ratio measured back to back is the only figure an inexact
 * emulator does not distort.
 */
c68k_limb c68k_addmul_1_c(c68k_limb *r, const c68k_limb *b, UINT n, c68k_limb a);

/*
 * dst[j] = src[j] + carry, for j in 0..n-1.  Returns the final carry (0 or 1
 * after the first limb).  dst may alias src.
 */
c68k_limb c68k_add_carry(c68k_limb *dst, const c68k_limb *src, UINT n,
                         c68k_limb carry);

/*
 * r[0..n-1] -= b[0..n-1].  Returns the borrow out (0 or 1).
 */
c68k_limb c68k_sub(c68k_limb *r, const c68k_limb *b, UINT n);

/*
 * Unsigned compare of two n-limb values.  -1, 0 or 1.
 */
INT c68k_cmp(const c68k_limb *a, const c68k_limb *b, UINT n);

/*
 * TRUE when the assembly primitives are in use, FALSE for the portable C
 * fallback.  The benchmark prints it so a result can never be misattributed.
 */
UINT c68k_using_assembly(VOID);


/* ------------------------------------------------------------ Montgomery -- */

/*
 * -m[0]^-1 mod 2^32, by Newton iteration.  m must be odd.
 */
c68k_limb c68k_mont_n0inv(c68k_limb m0);

/*
 * r = x * y * R^-1 mod m, where R = 2^(32*m_len).
 *
 * y must be < m; x need only be m_len limbs.  r is m_len limbs and MAY alias
 * x or y (both are fully consumed before r is written).  work is scratch of at
 * least (2 * m_len + 2) limbs and must not alias anything.
 *
 * Separated form (SOS in Koc/Acar/Kaliski's taxonomy) rather than the usually
 * recommended CIOS -- see the header comment in c68k_mont.c for why the 68020
 * inverts that advice.
 */
VOID c68k_mont_mul(c68k_limb *r,
                   const c68k_limb *x, const c68k_limb *y,
                   const c68k_limb *m, UINT m_len, c68k_limb n0inv,
                   c68k_limb *work);

/*
 * r = x * x * R^-1 mod m.  Same contract as c68k_mont_mul; ~76% of its cost,
 * because the off-diagonal products of a square are computed once and doubled.
 */
VOID c68k_mont_sqr(c68k_limb *r,
                   const c68k_limb *x,
                   const c68k_limb *m, UINT m_len, c68k_limb n0inv,
                   c68k_limb *work);


/* -------------------------------------------------------- exponentiation -- */

/*
 * Scratch, in limbs, that c68k_mont_power_modulus() needs for a modulus of
 * m_len limbs and a window of w bits.  A window of w keeps 2^(w-1) odd powers.
 *
 *   x_mont, acc, one, rr, xpad          =  5 * m_len
 *   CIOS/SOS work area                  =  2 * m_len + 2
 *   radix^2 mod m setup                 =  3 * m_len + 4
 *   window table, 2^(w-1) odd powers    = (1 << (w-1)) * m_len
 */
#define C68K_POWM_SCRATCH_LIMBS(m_len, w) \
    ((((10u + (1u << ((w) - 1u))) * (UINT)(m_len))) + 8u)

/*
 * The largest window the implementation will use.  OpenSSL's threshold table
 * wants 6 for the 1024-bit exponents an RSA-2048 CRT half actually runs; 6
 * costs 32 * m_len limbs of table.  c68k_mont_power_modulus() picks the
 * largest window that fits the scratch it is given, so this is only a cap.
 */
#define C68K_POWM_MAX_WINDOW    6u

/*
 * result = x^e mod m, with m odd and x < m.
 *
 * Sizes are in limbs; result must have room for m_len.  scratch_limbs says how
 * much room `scratch` has: the window size is chosen to fit, down to w = 1
 * (plain square-and-multiply) which needs C68K_POWM_SCRATCH_LIMBS(m_len, 1).
 * Returns NX_CRYPTO_SUCCESS, or NX_CRYPTO_SIZE_ERROR if even that will not fit.
 *
 * SIDE CHANNELS.  This is not constant time and does not try to be:
 *
 *   - the window value multiplied in depends on exponent bits, and the address
 *     of the table entry read depends on them too;
 *   - the final conditional subtraction of each Montgomery step is a branch;
 *   - leading zero bits of the exponent are skipped, which leaks its bit length.
 *
 * The vendored implementation is not constant time either -- it branches
 * per exponent bit between one Montgomery step and two, which is the textbook
 * square-and-multiply leak and a stronger signal than anything here.  So this
 * is not a regression, but it is not a defence.  For the threat model in
 * docs/RESEARCH.md (a vintage machine on a LAN) that is judged acceptable;
 * it would not be acceptable for a server exposed to remote timing.
 */
UINT c68k_mont_power_modulus(c68k_limb *result,
                             const c68k_limb *x, UINT x_len,
                             const c68k_limb *e, UINT e_len,
                             const c68k_limb *m, UINT m_len,
                             c68k_limb *scratch, UINT scratch_limbs);

/*
 * Signature-compatible drop-in for _nx_crypto_huge_number_mont_power_modulus.
 *
 * Same contract, same scratch pointer, and it computes the same value -- so
 * wiring it in is a one-line change at each call site.  It uses the vendored
 * routine's setup (radix^2 mod m by long division) so that the two are
 * comparable, then runs the fast exponentiation.
 *
 * The scratch requirement is larger than the vendored routine's: see
 * C68K_POWM_SCRATCH_LIMBS.  If the caller's buffer is too small this falls
 * back to a smaller window rather than failing, so it stays safe to substitute.
 */
VOID c68k_huge_number_mont_power_modulus(NX_CRYPTO_HUGE_NUMBER *x,
                                         NX_CRYPTO_HUGE_NUMBER *e,
                                         NX_CRYPTO_HUGE_NUMBER *m,
                                         NX_CRYPTO_HUGE_NUMBER *result,
                                         HN_UBASE *scratch,
                                         UINT scratch_limbs);

/*
 * RSA private operation via the Chinese Remainder Theorem: the vendored
 * _nx_crypto_huge_number_crt_power_modulus() with the two exponentiations
 * replaced.  `scratch` is the huge-number bump area the vendored routine
 * wants; powm_scratch is this module's, kept separate so the two allocators
 * cannot overlap.
 *
 * CRT is worth ~3.6x on its own, measured, and multiplies with everything
 * else here.  See the header comment in c68k_crt.c for where nx_secure is
 * currently leaving that on the table.
 */
VOID c68k_crt_power_modulus(NX_CRYPTO_HUGE_NUMBER *x,
                            NX_CRYPTO_HUGE_NUMBER *e,
                            NX_CRYPTO_HUGE_NUMBER *p,
                            NX_CRYPTO_HUGE_NUMBER *q,
                            NX_CRYPTO_HUGE_NUMBER *m,
                            NX_CRYPTO_HUGE_NUMBER *result,
                            HN_UBASE *scratch,
                            HN_UBASE *powm_scratch, UINT powm_scratch_limbs);

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_CRYPTO68K_H */
