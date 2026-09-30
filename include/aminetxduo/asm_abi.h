/*
 * AmiNetXDuo, the one marker for a call boundary that lives in hand-written
 * assembly.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_ASM_ABI_H
#define AMINETXDUO_ASM_ABI_H

/*
 * AMIGA_ASM_ARGS -- pin the amigaos convention, every argument on the stack, on
 * a function the C compiler does not build.
 *
 * The routines in src/net68k and src/crypto68k read 4(sp), 8(sp), 12(sp).  No
 * compiler option reaches them, so the C side is what is held still: under
 * -mregparm=3 the call would hand them d0, d1, d2 and leave the stack holding
 * whatever was there before -- a wrong number, not a trap.
 *
 *     AMIGA_ASM_ARGS ULONG n68k_sum_longwords(const ULONG *p, ULONG count);
 *     AMIGA_ASM_ARGS ULONG (*n68k_vec_sum)(const ULONG *, ULONG);
 *
 * It goes BEFORE the declarator, as the toolchain's own headers spell it
 * (`__stdargs int memcmp(...)`), because a trailing attribute is rejected on a
 * DEFINITION and some of these names have a C definition in configurations with
 * no assembly.  A pointer to a pinned function needs its own pin: it is a
 * different type.
 *
 * regparm(0) is the natural spelling of "stack, please" and does nothing on this
 * backend -- GCC 16.2.0b m68k ignores it; regparm(1..3) all take effect.  A
 * trailing ellipsis would also work, but it changes what the function IS.
 *
 * No-op off m68k, so a host test supplying its own C body sees the prototype it
 * saw before.
 */

#if defined(__m68k__) && (defined(__GNUC__) || defined(__clang__))
#  if defined(__has_attribute) && __has_attribute(__stkparm__)
#    define AMIGA_ASM_ARGS __attribute__((__stkparm__))
#  else
/*
 * Not "define it away": dropping the pin here would compile, link, and pass
 * arguments in d0/d1/d2 to assembly that reads the stack, which is a wrong
 * number rather than a crash.  This toolchain spells the attribute
 * `__stkparm__` (its own libc headers do), so say so and stop.
 */
#    error "m68k compiler without __attribute__((__stkparm__)): the assembly call boundaries cannot be pinned"
#  endif
#else
#  define AMIGA_ASM_ARGS
#endif

#endif /* AMINETXDUO_ASM_ABI_H */
