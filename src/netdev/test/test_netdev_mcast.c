/*
 * Exact multicast membership and all-or-nothing range changes, on the host.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "netdev_mcast.h"

static int failures;

static void expect(const char *what, ULONG got, ULONG want)
{
    if (got == want)
    {
        printf("ok   %s = %lu\n", what, (unsigned long)got);
        return;
    }

    printf("FAIL %s: got %lu, want %lu\n", what,
           (unsigned long)got, (unsigned long)want);
    failures++;
}

static NetdevMcast *find(NetdevMcast *table, const UBYTE *addr)
{
    UWORD i;

    for (i = 0; i < NETDEV_MCAST_MAX; i++)
    {
        if (table[i].refs != 0 &&
            memcmp(table[i].addr, addr, NETDEV_ADDR_LEN) == 0)
            return &table[i];
    }
    return NULL;
}

static void set_addr(UBYTE *addr, UBYTE a, UBYTE b, UBYTE c)
{
    addr[0] = 0x01;
    addr[1] = 0x00;
    addr[2] = 0x5e;
    addr[3] = a;
    addr[4] = b;
    addr[5] = c;
}

int main(void)
{
    NetdevMcast table[NETDEV_MCAST_MAX];
    NetdevMcast before[NETDEV_MCAST_MAX];
    UBYTE addr[NETDEV_ADDR_LEN];
    UBYTE missing[NETDEV_ADDR_LEN];
    UWORD i;

    memset(table, 0, sizeof(table));

    /* Leave one row free, then ask atomically for two new memberships. */
    for (i = 0; i < NETDEV_MCAST_MAX - 1; i++)
    {
        set_addr(addr, 0, 0, (UBYTE)i);
        expect("table setup", netdev_mcast_add(table, addr), TRUE);
    }
    memcpy(before, table, sizeof(table));
    set_addr(addr, 0, 1, 0);
    expect("an over-capacity range is refused",
           netdev_mcast_range_apply(table, addr, 2, TRUE), FALSE);
    expect("a refused add changes no row",
           memcmp(table, before, sizeof(table)) == 0, TRUE);

    /* Existing rows need no capacity and acquire one reference each. */
    set_addr(addr, 0, 0, 29);
    expect("existing range fits a nearly full table",
           netdev_mcast_range_apply(table, addr, 2, TRUE), TRUE);
    expect("first existing reference increments", find(table, addr)->refs, 2);
    addr[5]++;
    expect("second existing reference increments", find(table, addr)->refs, 2);

    /* A missing member makes a delete fail before earlier rows are touched. */
    set_addr(addr, 0, 0, 0);
    set_addr(missing, 0, 0, 1);
    expect("remove setup", netdev_mcast_del(table, missing), TRUE);
    memcpy(before, table, sizeof(table));
    expect("a partially missing delete is refused",
           netdev_mcast_range_apply(table, addr, 2, FALSE), FALSE);
    expect("a refused delete changes no row",
           memcmp(table, before, sizeof(table)) == 0, TRUE);

    /* Range increment carries across the low octets rather than wrapping. */
    memset(table, 0, sizeof(table));
    set_addr(addr, 0, 0, 0xff);
    expect("a range crossing an octet applies",
           netdev_mcast_range_apply(table, addr, 2, TRUE), TRUE);
    set_addr(missing, 0, 1, 0);
    expect("the carried address was installed", find(table, missing) != NULL,
           TRUE);

    /* A held row cannot wrap its reference count back to free. */
    find(table, addr)->refs = 0xffffu;
    expect("a saturated duplicate is accepted",
           netdev_mcast_add(table, addr), TRUE);
    expect("a saturated reference does not wrap", find(table, addr)->refs,
           0xffffu);

    if (failures != 0)
    {
        printf("netdev_mcast: %d failures\n", failures);
        return 1;
    }

    printf("netdev_mcast: all ok\n");
    return 0;
}
