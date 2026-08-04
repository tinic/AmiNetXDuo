/*
 * AmiNetXDuo, crypto68k test support: logging, RNG, huge-number glue.
 *
 * Shared by tests/crypto68k/c68k_test.c (correctness) and c68k_bench.c
 * (timings), separate executables because of their run times: the correctness
 * pass fits inside the harness's default timeout, and the benchmark contains a
 * reference RSA-2048 private operation that runs for over two minutes on its
 * own on the floor target.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_SUPPORT_H
#define AMINETXDUO_C68K_SUPPORT_H

#include "crypto68k.h"

#include <exec/types.h>

/* RawDoFmt logging: %ld/%lu/%lx/%s only, every argument longword sized.  See
   include/aminetxduo/compat.h for why %d and %c are traps here. */
VOID  c68k_log(const char *fmt, ...);
VOID  c68k_flush(VOID);

/* xorshift32.  Deterministic and seeded from a constant, so a failing run can
   be reproduced exactly. */
VOID      c68k_rng_seed(ULONG seed);
c68k_limb c68k_rand(VOID);

/* Fill a limb array with random data, force the top limb nonzero. */
VOID c68k_rand_limbs(c68k_limb *p, UINT n);

/* Wrap a limb array as an NX_CRYPTO_HUGE_NUMBER for the vendored routines. */
VOID c68k_hn_set(NX_CRYPTO_HUGE_NUMBER *hn, c68k_limb *data,
                 UINT used_limbs, UINT buffer_limbs);

/* Compare an m_len-limb array against a huge number's value. */
UINT c68k_hn_equals(const NX_CRYPTO_HUGE_NUMBER *hn, const c68k_limb *v,
                    UINT n);

#endif /* AMINETXDUO_C68K_SUPPORT_H */
