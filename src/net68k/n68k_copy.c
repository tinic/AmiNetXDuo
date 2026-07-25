/*
 * AmiNetXDuo -- portable fallback for n68k_copy_bytes().
 *
 * The point of n68k_copy_bytes() is the movem.l loop in n68k_copy.S, which
 * has no C spelling: the compiler cannot emit a memory-to-memory move, let
 * alone a multi-register one.  This file exists so the symbol is present when
 * AMINETXDUO_NET68K_ASM is off -- a host build, or a bisect of a suspected
 * assembly bug -- and so it is not compiled at all on the target.
 *
 * It follows the same rule the assembly does, and the same one libm020's
 * memcpy follows: align the DESTINATION, then move longwords whatever the
 * source is doing.  Requiring both pointers to agree, and dropping to one
 * byte per iteration when they do not, is the mistake this replaces.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

#ifndef AMINETXDUO_NET68K_ASM

VOID n68k_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len)
{

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
