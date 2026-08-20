/*
 * Exact multicast membership, apart from programming a chip's hash filter.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_mcast.h"

static ULONG mcast_addr32(const UBYTE *a)
{
    return ((ULONG)a[2] << 24) | ((ULONG)a[3] << 16) |
           ((ULONG)a[4] << 8)  | (ULONG)a[5];
}

static UWORD mcast_addr16(const UBYTE *a)
{
    return (UWORD)(((UWORD)a[0] << 8) | a[1]);
}

BOOL netdev_mcast_range_wide(const UBYTE *lo, const UBYTE *hi, ULONG *count)
{
    ULONG lo32 = mcast_addr32(lo);
    ULONG hi32 = mcast_addr32(hi);
    ULONG span;

    if (mcast_addr16(lo) != mcast_addr16(hi) || hi32 < lo32)
    {
        *count = 0;
        return TRUE;
    }

    /* Inclusive count is span + 1.  Test the span first: the full 32-bit
       range has a span of ULONG_MAX and its count is 2^32, which does not fit
       in ULONG and used to wrap to zero here. */
    span = hi32 - lo32;
    if (span >= NETDEV_MCAST_MAX)
    {
        *count = 0;
        return TRUE;
    }

    *count = span + 1;
    return FALSE;
}

static VOID mcast_copy(UBYTE *to, const UBYTE *from)
{
    UWORD i;

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        to[i] = from[i];
}

static VOID mcast_next(UBYTE *addr)
{
    if (++addr[5] == 0 && ++addr[4] == 0 && ++addr[3] == 0)
        addr[2]++;
}

static NetdevMcast *mcast_find(NetdevMcast *table, const UBYTE *addr)
{
    UWORD i;

    for (i = 0; i < NETDEV_MCAST_MAX; i++)
    {
        NetdevMcast *m = &table[i];
        UWORD        j;
        BOOL         same = TRUE;

        if (m->refs == 0)
            continue;
        for (j = 0; j < NETDEV_ADDR_LEN; j++)
        {
            if (m->addr[j] != addr[j])
            {
                same = FALSE;
                break;
            }
        }
        if (same)
            return m;
    }

    return NULL;
}

BOOL netdev_mcast_add(NetdevMcast *table, const UBYTE *addr)
{
    NetdevMcast *m = mcast_find(table, addr);
    UWORD        i;

    if (m != NULL)
    {
        /* A successful join must own a reference.  Silently saturating here
           loses that ownership: after 65535 deletes the group is removed even
           though the extra caller was told its join succeeded. */
        if (m->refs == 0xffffu)
            return FALSE;
        m->refs++;
        return TRUE;
    }

    for (i = 0; i < NETDEV_MCAST_MAX; i++)
    {
        if (table[i].refs == 0)
        {
            mcast_copy(table[i].addr, addr);
            table[i].refs = 1;
            return TRUE;
        }
    }

    return FALSE;
}

BOOL netdev_mcast_del(NetdevMcast *table, const UBYTE *addr)
{
    NetdevMcast *m = mcast_find(table, addr);

    if (m == NULL)
        return FALSE;

    m->refs--;
    return TRUE;
}

BOOL netdev_mcast_range_apply(NetdevMcast *table, const UBYTE *lo,
                              ULONG count, BOOL add)
{
    UBYTE addr[NETDEV_ADDR_LEN];
    ULONG i;
    ULONG removed = 0;

    mcast_copy(addr, lo);

    if (add)
    {
        ULONG needed = 0;
        ULONG free_rows = 0;

        for (i = 0; i < NETDEV_MCAST_MAX; i++)
        {
            if (table[i].refs == 0)
                free_rows++;
        }

        for (i = 0; i < count; i++)
        {
            NetdevMcast *m = mcast_find(table, addr);

            /* Preserve the transaction guarantee for reference exhaustion as
               well as row exhaustion: no earlier member may be incremented
               before a later saturated member refuses the range. */
            if (m != NULL && m->refs == 0xffffu)
                return FALSE;
            if (m == NULL)
                needed++;
            mcast_next(addr);
        }

        if (needed > free_rows)
            return FALSE;
    }
    /*
     * No preflight on the delete side, deliberately. The spec's own worked
     * example for S2_DELMULTICASTADDRESSES is a stack leaving the groups it
     * joined, and a stack that joined by single address and leaves by range
     * -- or that has already had one address dropped -- would otherwise get
     * an error AND stay in every group in the range. Removing what is there
     * and reporting success is what the command is for; a range naming
     * nothing at all is still refused below, because that is a caller error
     * rather than a partial match.
     */

    /* On the add side the preflight above makes every step infallible. On the
       delete side each step reports whether it found its address. */
    mcast_copy(addr, lo);
    for (i = 0; i < count; i++)
    {
        if (add)
        {
            (VOID)netdev_mcast_add(table, addr);
            removed++;
        }
        else if (netdev_mcast_del(table, addr))
        {
            removed++;
        }

        mcast_next(addr);
    }

    return (BOOL)(removed != 0);
}
