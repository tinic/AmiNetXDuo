/*
 * AmiNetXDuo, which CPU a crypto68k primitive is written for.
 *
 * src/net68k/n68k_variant.h does this for the data path; this is the same idea
 * where the stakes are higher.  A net68k variant on the wrong part costs a few
 * percent.  Here the module is all-or-nothing today and the wrong answer costs
 * multiples: a 68060 that takes the portable C spends 64% of a TLS 1.3
 * transfer in c68k_addmul_1_c alone, and a 68020 build on a 68060 traps every
 * MULU.L into 68060.library's emulation.
 *
 * TWO HARDWARE FACTS, NOT FOUR CLASSES.  What the files here need is narrower
 * than a CPU model, so this asks the two questions the assembly actually has:
 *
 *   AFF_68020 set          the 68020 addressing modes and instructions exist:
 *                          the scaled index c68k_aes.S needs, extb.l in
 *                          c68k_p256.S.  True on every part from the 68020 up,
 *                          the 68060 included -- it dropped instructions, not
 *                          addressing modes.
 *
 *   the 64-bit MULU.L      MULU.L/DIVU.L with a 64-bit result, which is 68020,
 *                          68030 and 68040 ONLY.  The 68060 dropped both and
 *                          traps them to vector 61, where 68060.library
 *                          emulates them correctly and slowly.  This is what
 *                          c68k_prim.S, c68k_poly1305.S and c68k_25519.S's
 *                          MULU.L half are built on, and it is why a 68060
 *                          cannot simply take "the fast build".
 *
 * So a 68060 is 020-and-up AND has no 64-bit MULU.L, which no single class
 * number expresses.  c68k_cpu.c reads both from AttnFlags once.
 *
 * A FILE IS ASSEMBLED FOR ITS OWN NEEDS, not for the machine.  An object may
 * hold an instruction this CPU lacks as long as nothing calls the routine that
 * uses it: legality is per instruction executed.  That is what lets c68k_prim.S
 * be assembled -m68020 in a binary that runs on a 68000, with four of its six
 * routines -- add, sub, cmp, add_carry -- called there anyway, because their
 * own code is original 68000 and only addmul_1 and div_2by1 are not.
 *
 * C68K_MV_SUFFIX names a variant apart where two files define the same symbol.
 * The build sets it per source file; without it the names are the plain ones,
 * which is what every per-CPU build gets.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_C68K_VARIANT_H
#define AMINETXDUO_C68K_VARIANT_H

/*
 * ONE GATE PER FILE, and this is where the module's all-or-nothing switch comes
 * apart.  C68K_ASM used to mean every .S here at once, which is why a 68000 or
 * a 68060 got none of them -- including c68k_chacha20.S, which both parts can
 * run perfectly well, and the four routines in c68k_prim.S that are original
 * 68000 code.  A per-CPU build still says C68K_ASM and gets all six, so it is
 * unchanged; the `any` build names the ones it carries and the rest take their
 * portable C.
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
