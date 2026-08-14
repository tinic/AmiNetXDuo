/*
 * AmiNetXDuo, portable fallback for the n68k_iocopy.S routines.
 *
 * Same reason n68k_copy.c exists: the symbols have to be present when
 * AMINETXDUO_NET68K_ASM is off -- a host build, or a bisect of a suspected
 * assembly bug -- and the movem.l loop has no C spelling.  The accesses are
 * longword-wide here as they are there, so the rule the far side depends on
 * (never a byte access to card memory or to a data port) holds on both paths.
 *
 * SPDX-License-Identifier: MIT
 */

#include "n68k_iocopy.h"

#ifndef AMINETXDUO_NET68K_ASM

VOID n68k_copy_longs(volatile void *to, const volatile void *from,
                     ULONG longs)
{
    volatile ULONG       *d = (volatile ULONG *)to;
    const volatile ULONG *s = (const volatile ULONG *)from;

    while (longs-- != 0UL)
        *d++ = *s++;
}

VOID n68k_port_in(void *to, const volatile void *port, ULONG blocks)
{
    volatile ULONG       *d = (volatile ULONG *)to;
    const volatile ULONG *p = (const volatile ULONG *)port;
    ULONG                 i;

    while (blocks-- != 0UL)
    {
        for (i = 0; i < 8UL; i++)
            *d++ = p[i];
    }
}

VOID n68k_port_out(volatile void *port, const void *from, ULONG blocks)
{
    volatile ULONG       *p = (volatile ULONG *)port;
    const ULONG          *s = (const ULONG *)from;
    ULONG                 i;

    while (blocks-- != 0UL)
    {
        for (i = 0; i < 8UL; i++)
            p[i] = *s++;
    }
}

VOID n68k_port_in_w(void *to, const volatile void *port, ULONG blocks)
{
    UWORD                *d = (UWORD *)to;
    const volatile UWORD *p = (const volatile UWORD *)port;
    ULONG                 i;

    while (blocks-- != 0UL)
    {
        for (i = 0; i < 16UL; i++)
            *d++ = *p;
    }
}

VOID n68k_port_out_w(volatile void *port, const void *from, ULONG blocks)
{
    volatile UWORD *p = (volatile UWORD *)port;
    const UWORD    *s = (const UWORD *)from;
    ULONG           i;

    while (blocks-- != 0UL)
    {
        for (i = 0; i < 16UL; i++)
            *p = *s++;
    }
}

#endif /* AMINETXDUO_NET68K_ASM */
