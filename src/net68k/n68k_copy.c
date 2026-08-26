/*
 * AmiNetXDuo, portable fallback for n68k_copy_bytes().
 *
 * On a 68000 a word or longword access to an odd address is an address error,
 * so when the two pointers disagree in bit 0 there is no alignment of the
 * destination that leaves the source even, and only the byte loop is legal.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#ifndef AMINETXDUO_NET68K_ASM

#if !defined(__mc68020__) && !defined(__mc68030__) && \
    !defined(__mc68040__) && !defined(__mc68060__)
#define N68K_ODD_IS_FATAL 1
#endif

VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{

#ifdef N68K_ODD_IS_FATAL
    if ((((ULONG)(void *)to ^ (ULONG)(const void *)from) & 1UL) != 0UL)
    {
        while (len != 0UL)
        {
            *to++ = *from++;
            len--;
        }
        return;
    }
#endif

    if (len >= 8UL)
    {
        while ((((ULONG)to) & 3UL) != 0UL)
        {
            *to++ = *from++;
            len--;
        }

        while (len >= 16UL)
        {
            *(ULONG *)(void *)to        = *(const ULONG *)(const void *)from;
            *(ULONG *)(void *)(to + 4)  = *(const ULONG *)(const void *)(from + 4);
            *(ULONG *)(void *)(to + 8)  = *(const ULONG *)(const void *)(from + 8);
            *(ULONG *)(void *)(to + 12) = *(const ULONG *)(const void *)(from + 12);
            to   += 16;
            from += 16;
            len  -= 16;
        }

        while (len >= 4UL)
        {
            *(ULONG *)(void *)to = *(const ULONG *)(const void *)from;
            to   += 4;
            from += 4;
            len  -= 4;
        }
    }

    while (len != 0UL)
    {
        *to++ = *from++;
        len--;
    }
}

#endif /* AMINETXDUO_NET68K_ASM */
