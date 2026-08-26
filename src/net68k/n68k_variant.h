/*
 * AmiNetXDuo, which CPU an inner loop in this directory is written for.
 *
 * N68K_MV is 0, 20, 40 or 60 and names the class, not the part: a 68010 takes
 * 0 and a 68030 takes 20.
 *
 * With N68K_MV_MULTI the routines are named _n68k_copy_bytes_mv20 and so on,
 * and n68k_dispatch.S supplies the unsuffixed names as trampolines.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_N68K_VARIANT_H
#define AMINETXDUO_N68K_VARIANT_H

#ifndef N68K_MV
#if defined(__mc68060__)
#define N68K_MV 60
#elif defined(__mc68040__)
#define N68K_MV 40
#elif defined(__mc68020__) || defined(__mc68030__)
#define N68K_MV 20
#else
#define N68K_MV 0
#endif
#endif

#define N68K_PASTE2(a, b)       a ## b
#define N68K_PASTE(a, b)        N68K_PASTE2(a, b)

#ifdef N68K_MV_MULTI
#define N68K_MV_SYM(n)          N68K_PASTE(N68K_PASTE(n, _mv), N68K_MV)
#else
#define N68K_MV_SYM(n)          n
#endif

#endif /* AMINETXDUO_N68K_VARIANT_H */
