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
    ULONG count;
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

    /*
     * A delete removes what it finds. A stack that joined by single address
     * and leaves by range, or that has already dropped one member, must not
     * be told no AND left in the rest of the groups.
     */
    set_addr(addr, 0, 0, 0);
    set_addr(missing, 0, 0, 1);
    expect("remove setup", netdev_mcast_del(table, missing), TRUE);
    expect("a partially missing delete removes what is there",
           netdev_mcast_range_apply(table, addr, 2, FALSE), TRUE);
    expect("and the member it did find was released",
           find(table, addr) == NULL, TRUE);

    /* A range naming nothing at all is still a caller error. */
    memcpy(before, table, sizeof(table));
    set_addr(missing, 9, 9, 9);
    expect("a delete matching nothing is refused",
           netdev_mcast_range_apply(table, missing, 2, FALSE), FALSE);
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

    /* An unrepresentable join is refused rather than accepted without owning
       the reference it promises. */
    find(table, addr)->refs = 0xffffu;
    expect("a saturated duplicate is refused",
           netdev_mcast_add(table, addr), FALSE);
    expect("a refused duplicate does not wrap", find(table, addr)->refs,
           0xffffu);

    /* A saturated member later in a range must not leave an earlier member
       incremented before the operation reports failure. */
    set_addr(addr, 0, 0, 0xff);
    find(table, addr)->refs = 7;
    set_addr(missing, 0, 1, 0);
    find(table, missing)->refs = 0xffffu;
    memcpy(before, table, sizeof(table));
    expect("a range containing a saturated member is refused",
           netdev_mcast_range_apply(table, addr, 2, TRUE), FALSE);
    expect("a saturated range changes no reference",
           memcmp(table, before, sizeof(table)) == 0, TRUE);

    /* The inclusive size of 00000000..ffffffff is 2^32, which cannot be
       represented in ULONG.  It is wide, not an empty exact range. */
    set_addr(addr, 0, 0, 0);
    set_addr(missing, 0xff, 0xff, 0xff);
    expect("a full 32-bit suffix range is wide",
           netdev_mcast_range_wide(addr, missing, &count), TRUE);
    expect("a wide range supplies no wrapped count", count, 0);

    set_addr(addr, 0, 0, 0);
    set_addr(missing, 0, 0, NETDEV_MCAST_MAX - 1);
    expect("exactly the table capacity is not wide",
           netdev_mcast_range_wide(addr, missing, &count), FALSE);
    expect("the exact range count", count, NETDEV_MCAST_MAX);

    if (failures != 0)
    {
        printf("netdev_mcast: %d failures\n", failures);
        return 1;
    }

    printf("netdev_mcast: all ok\n");
    return 0;
}
