/*
 * AmiNetXDuo, portable fallback for the n68k_iocopy.S routines.
 *
 * The same reason as n68k_copy.c: the symbols have to be present when
 * AMINETXDUO_NET68K_ASM is off -- a host build, or a bisect of a suspected
 * assembly bug -- and the movem.l loop has no C spelling.  Each routine keeps
 * the access width it has in the assembly, so the rule the far side depends
 * on (never a byte access to card memory or to a data port) holds on both
 * paths.
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


ULONG n68k_port_in_w_sum(void *to, const volatile void *port, ULONG bytes)
{
    UWORD                *d = (UWORD *)to;
    const volatile UWORD *p = (const volatile UWORD *)port;
    ULONG                 sum = 0;
    ULONG                 longs = bytes >> 2;
    ULONG                 tail  = bytes & 3UL;
    ULONG                 w;

    while (longs-- != 0UL)
    {
        UWORD w0 = *p;
        UWORD w1 = *p;

        *d++ = w0;
        *d++ = w1;
        w = ((ULONG)w0 << 16) | w1;
        sum += w;
        if (sum < w)
            sum++;
    }

    if (tail != 0UL)
    {
        UBYTE *db = (UBYTE *)d;
        UWORD  t0 = *p;

        if (tail == 1UL)
        {
            db[0] = (UBYTE)(t0 >> 8);
            w = ((ULONG)(t0 & 0xff00u)) << 16;
        }
        else if (tail == 2UL)
        {
            *d = t0;
            w = (ULONG)t0 << 16;
        }
        else
        {
            UWORD t1 = *p;

            *d = t0;
            db[2] = (UBYTE)(t1 >> 8);
            w = ((ULONG)t0 << 16) | (t1 & 0xff00u);
        }
        sum += w;
        if (sum < w)
            sum++;
    }

    return sum;
}

ULONG n68k_port_in_l_sum(void *to, const volatile void *port, ULONG longs)
{
    ULONG                *d   = (ULONG *)to;
    const volatile ULONG *p   = (const volatile ULONG *)port;
    ULONG                 sum = 0;

    while (longs-- != 0UL)
    {
        ULONG w = *p;

        *d++ = w;
        sum += w;
        if (sum < w)
            sum++;
    }

    return sum;
}

#endif /* AMINETXDUO_NET68K_ASM */
