/*
 * AmiNetXDuo, which CPU a crypto68k primitive is written for.
 *
 * Two hardware questions, which no single class number expresses: AFF_68020
 * (true on every part from the 68020 up, the 68060 included), and the 64-bit
 * MULU.L/DIVU.L, which the 68060 dropped and traps to 68060.library.
 *
 * C68K_MV_SUFFIX names a variant apart where two files define the same symbol.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_VARIANT_H
#define AMINETXDUO_C68K_VARIANT_H

/*
 * One gate per file, where the all-or-nothing switch of the module comes
 * apart.  C68K_ASM used to mean every .S here at once, which is why a 68000 or
 * a 68060 got none of them, including c68k_chacha20.S, which both parts can
 * run, and the four routines in c68k_prim.S that are original 68000 code.  A
 * per-CPU build still says C68K_ASM and gets all six, so it is unchanged.  The
 * `any` build names the ones it carries and the rest take their portable C.
 *
 * src/net68k took the same step for the same reason, and its CMakeLists says
 * so: "THE TWO SWITCHES HAVE COME APART".
 */
#if defined(C68K_ASM) && !defined(C68K_MV)
#define C68K_ASM_PRIM           1
#define C68K_ASM_P256           1
#define C68K_ASM_AES            1
#define C68K_ASM_CHACHA20       1
#define C68K_ASM_POLY1305       1
#define C68K_ASM_25519          1
#endif

#define C68K_PASTE2(a, b)       a ## b
#define C68K_PASTE(a, b)        C68K_PASTE2(a, b)

#if defined(C68K_MV) && defined(C68K_MV_SUFFIX)
#define C68K_MV_SYM(n)          C68K_PASTE(n, C68K_MV_SUFFIX)
#else
#define C68K_MV_SYM(n)          n
#endif

#endif /* AMINETXDUO_C68K_VARIANT_H */
