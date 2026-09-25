/*
 * The load requirement of a library file, and whether it fits. See libfit.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "libfit.h"

#define HUNK_HEADER         0x3F3UL
#define LIBFIT_HUNKS_MAX    16UL
#define LOADSEG_OVERHEAD    8UL         /* length long + next-segment BPTR */

static ULONG lf_long(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

BOOL libfit_need(const UBYTE *head, ULONG len, LibFitNeed *need)
{
    ULONG first;
    ULONG last;
    ULONG pos = 20;
    ULONG i;

    need->total   = 0;
    need->largest = 0;

    if (head == NULL || len < 20 || lf_long(head) != HUNK_HEADER ||
        lf_long(head + 4) != 0)
        return FALSE;

    first = lf_long(head + 12);
    last  = lf_long(head + 16);
    if (last < first || last - first >= LIBFIT_HUNKS_MAX)
        return FALSE;

    for (i = 0; i <= last - first; i++)
    {
        ULONG word;
        ULONG bytes;

        if (pos + 4 > len)
        {
            need->total   = 0;
            need->largest = 0;
            return FALSE;
        }
        word = lf_long(head + pos);
        pos += 4;
        if ((word & 0xC0000000UL) == 0xC0000000UL)
            pos += 4;                   /* explicit memory attributes */
        if (pos > len || (word & 0x3FFFFFFFUL) > 0x00FFFFFFUL)
        {
            need->total   = 0;          /* its attribute long cut off, or   */
            need->largest = 0;          /* over 64 MB: sixteen cannot wrap  */
            return FALSE;
        }

        bytes = (word & 0x3FFFFFFFUL) * 4UL + LOADSEG_OVERHEAD;
        need->total += bytes;
        if (bytes > need->largest)
            need->largest = bytes;
    }

    return TRUE;
}

BOOL libfit_short(ULONG free_before, ULONG largest_before,
                  const LibFitNeed *need, ULONG headroom)
{
    ULONG total = (need != NULL) ? need->total : 0;

    if (need != NULL && need->largest > largest_before)
        return TRUE;

    /* free_before < total + headroom, without the sum wrapping. */
    if (total > free_before)
        return TRUE;

    return (BOOL)(free_before - total < headroom);
}
