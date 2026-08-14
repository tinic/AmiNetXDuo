/*
 * AmiNetXDuo, which CPU an inner loop in this directory is written for.
 *
 * n68k_checksum.S and n68k_copy.S each hold three or four forms of the same
 * routine, one per CPU class, and until now the preprocessor picked one from
 * __mc68020__ and friends: the answer was frozen at compile time, so the
 * archive shipped a library per CPU.  N68K_MV replaces that switch with a
 * number the build passes in, so the same source can be assembled once per
 * class into one binary and the choice made at run time from
 * SysBase->AttnFlags (n68k_cpu.c).
 *
 * N68K_MV is 0, 20, 40 or 60 and names the class, not the part: a 68010 takes
 * 0 and a 68030 takes 20, exactly as the per-CPU builds did.
 *
 * With N68K_MV_MULTI the routines are named _n68k_copy_bytes_mv20 and so on,
 * and n68k_dispatch.S supplies the unsuffixed names as trampolines.  Without
 * it there is one variant and it keeps the plain name, which is what a
 * -DAMINETXDUO_CPU=68020 build and the standalone benches in bench/ get.
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

/*
 * The C half of the same idea, and it needs only two forms rather than four.
 *
 * The assembly variants differ in which 68000 instruction to use where; the C
 * ones differ in what the compiler may emit at all, and that is one question
 * with one boundary: a 68000 has no 32-bit multiply, no scaled index and no
 * bitfield, and everything above it has all three.  So the fast copy is built
 * once, `-mcpu=68060`, and serves every machine with AFF_68020 set -- 68060
 * codegen is a SUBSET of the 68020's, because the 68060 only ever dropped
 * instructions (the 64-bit MULU.L and DIVU.L results), so what assembles for
 * an 060 runs on an 020.  The reverse is not true, which is why this is not
 * built `-m68020`.
 *
 * N68K_MV_FAST renames what that copy exports.  It has to be in effect before
 * net68k.h declares them, so the two files concerned include this first.
 */
#ifdef N68K_MV_FAST

#define n68k_ip_checksum_compute        n68k_ip_checksum_compute_fast
#define n68k_rx_verify                  n68k_rx_verify_fast
#define n68k_rx_verify_sum              n68k_rx_verify_sum_fast

#endif

#endif /* AMINETXDUO_N68K_VARIANT_H */
