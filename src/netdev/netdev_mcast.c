/*
 * Exact multicast membership, apart from programming a chip's hash filter.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_mcast.h"

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
        /* Saturate rather than wrap: a wrap to zero frees a row that callers
           still hold, and the group stops being received with nothing said. */
        if (m->refs != 0xffffu)
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
            if (mcast_find(table, addr) == NULL)
                needed++;
            mcast_next(addr);
        }

        if (needed > free_rows)
            return FALSE;
    }
    else
    {
        for (i = 0; i < count; i++)
        {
            if (mcast_find(table, addr) == NULL)
                return FALSE;
            mcast_next(addr);
        }
    }

    /* The preflight makes every operation below infallible. */
    mcast_copy(addr, lo);
    for (i = 0; i < count; i++)
    {
        if (add)
            (VOID)netdev_mcast_add(table, addr);
        else
            (VOID)netdev_mcast_del(table, addr);
        mcast_next(addr);
    }

    return TRUE;
}
