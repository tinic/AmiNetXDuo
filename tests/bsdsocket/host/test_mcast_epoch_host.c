/* The shipping mcast.c row helpers against interface-slot reuse. */
#include <stdio.h>
#include <string.h>
#include "bsdsocket_vectors.h"

static ULONG h_epoch[4];

ULONG netstack_interface_epoch(UWORD index)
{
    return (index < 4) ? h_epoch[index] : 0;
}

/* Keep the exact static row code under test, not a model of it.  Unused
   entry points are discarded by function-section garbage collection. */
#include "../../../src/bsdsocket/mcast.c"

static unsigned checks;
static unsigned failures;

static void check(int condition, const char *name)
{
    checks++;
    if (!condition)
    {
        failures++;
        printf("FAIL %s\n", name);
    }
}

int main(void)
{
    AmiSocket a;
    AmiSocket b;
    const ULONG group6[4] = { 0xff020000UL, 0, 0, 1 };

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(bsd_mcast_table, 0, sizeof(bsd_mcast_table));
    memset(bsd_mcast6_table, 0, sizeof(bsd_mcast6_table));
    memset(h_epoch, 0, sizeof(h_epoch));

    bsd_mcast_table[0].bm_Sock = &a;
    bsd_mcast_table[0].bm_Group = 0xefff2a63UL;
    bsd_mcast_table[0].bm_Iface = 1;
    bsd_mcast_table[0].bm_Epoch = h_epoch[1];
    check(bsd_mcast_find(&a, 0xefff2a63UL, 1) == &bsd_mcast_table[0],
          "IPv4 live membership found");

    /* NetX detach cleared its join and the next Add reuses index 1. */
    h_epoch[1]++;
    check(bsd_mcast_find(&a, 0xefff2a63UL, 1) == NULL,
          "IPv4 stale membership cannot reject rejoin");
    check(bsd_mcast_table[0].bm_Sock == NULL,
          "IPv4 old owner removed without a NetX leave");
    check(bsd_mcast_free_row() == &bsd_mcast_table[0],
          "IPv4 stale row reusable");

    bsd_mcast_table[0].bm_Sock = &b;
    bsd_mcast_table[0].bm_Epoch = h_epoch[1];
    check(bsd_mcast_find(&b, 0xefff2a63UL, 1) == &bsd_mcast_table[0],
          "IPv4 new occupant join remains live");

    bsd_mcast6_table[0].bm_Sock = &a;
    memcpy(bsd_mcast6_table[0].bm_Group, group6, sizeof(group6));
    bsd_mcast6_table[0].bm_Iface = 2;
    bsd_mcast6_table[0].bm_Epoch = h_epoch[2];
    check(bsd_mcast6_find(&a, group6, 2) == &bsd_mcast6_table[0],
          "IPv6 live membership found");

    h_epoch[2]++;
    check(bsd_mcast6_find(&a, group6, 2) == NULL,
          "IPv6 stale membership cannot reject rejoin");
    check(bsd_mcast6_table[0].bm_Sock == NULL,
          "IPv6 old owner removed without a NetX leave");
    check(bsd_mcast6_free_row() == &bsd_mcast6_table[0],
          "IPv6 stale row reusable");

    a.as_McastIf = 1;
    a.as_McastIfEpoch = h_epoch[1];
    check(bsd_mcast_preference(&a.as_McastIf, a.as_McastIfEpoch) == 1,
          "live IPv4 send preference retained");
    h_epoch[1]++;
    check(bsd_mcast_preference(&a.as_McastIf, a.as_McastIfEpoch) == -1,
          "stale IPv4 send preference reverts to routing");

    a.as_Mcast6If = 2;
    a.as_Mcast6IfEpoch = h_epoch[2];
    check(bsd_mcast_preference(&a.as_Mcast6If, a.as_Mcast6IfEpoch) == 2,
          "live IPv6 send preference retained");
    h_epoch[2]++;
    check(bsd_mcast_preference(&a.as_Mcast6If, a.as_Mcast6IfEpoch) == -1,
          "stale IPv6 send preference reverts to routing");

    printf("mcast epoch: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
