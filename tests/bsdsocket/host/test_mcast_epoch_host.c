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
    struct AmiSocketBase base;
    NX_IP ip;
    NXD_ADDRESS dest;
    LONG chosen;
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

    /* N-035: NetX's IPv6 UDP header builder reads the socket TTL, not the
       instance hop limit. Exercise the actual producer on route-selected and
       explicitly selected sends; unrelated TCP/ping instance state must stay
       untouched throughout allocation/send, not merely be restored later. */
    memset(&base, 0, sizeof(base));
    memset(&ip, 0, sizeof(ip));
    memset(&dest, 0, sizeof(dest));
    base.sb_StackRefs = 1;
    base.sb_StackIp = &ip;
    ip.nx_ipv6_hop_limit = 64;
    a.as_Ttl = 128;
    a.as_McastTtl = 7;
    a.as_Mcast6Hops = 1;
    a.as_Mcast6If = -1;
    dest.nxd_ip_version = NX_IP_VERSION_V6;
    memcpy(dest.nxd_ip_address.v6, group6, sizeof(group6));
    bsd_mcast_prepare_send(&a, &dest);
    chosen = bsd_mcast6_prepare_send(&base, &a, &dest);
    check(chosen == -1, "IPv6 group without interface uses route");
    check(a.as_Nx.udp.nx_udp_socket_time_to_live == 1,
          "default multicast hop limit reaches socket header producer");
    check(ip.nx_ipv6_hop_limit == 64,
          "multicast prepare cannot alter concurrent TCP/ping hop limit");

    a.as_Mcast6If = 2;
    a.as_Mcast6IfEpoch = h_epoch[2];
    ip.nx_ipv6_address[3].nxd_ipv6_address_valid = 1;
    ip.nx_ipv6_address[3].nxd_ipv6_address_state = NX_IPV6_ADDR_STATE_VALID;
    ip.nx_ipv6_address[3].nxd_ipv6_address_attached = &ip.nx_ip_interface[2];
    ip.nx_ipv6_address[3].nxd_ipv6_address_index = 3;
    ip.nx_ipv6_address[3].nxd_ipv6_address[0] = 0xfe800000UL;
    a.as_Mcast6Hops = 255;
    bsd_mcast_prepare_send(&a, &dest);
    chosen = bsd_mcast6_prepare_send(&base, &a, &dest);
    check(chosen == 3, "IPv6 selected interface retains address-index choice");
    check(a.as_Nx.udp.nx_udp_socket_time_to_live == 255,
          "explicit multicast hop limit reaches socket header producer");
    check(ip.nx_ipv6_hop_limit == 64, "explicit hops leave instance unchanged");

    a.as_Mcast6Hops = 0;
    bsd_mcast_prepare_send(&a, &dest);
    check(bsd_mcast6_prepare_send(&base, &a, &dest) == BSD_MCAST6_NO_LINK,
          "zero hops remains host-only no-send");
    check(ip.nx_ipv6_hop_limit == 64, "zero hops leave instance unchanged");

    dest.nxd_ip_address.v6[0] = 0x20010db8UL;
    bsd_mcast_prepare_send(&a, &dest);
    check(bsd_mcast6_prepare_send(&base, &a, &dest) == -1,
          "IPv6 unicast keeps normal route path");
    check(a.as_Nx.udp.nx_udp_socket_time_to_live == 128,
          "next unicast resets socket hop limit");

    dest.nxd_ip_version = NX_IP_VERSION_V4;
    dest.nxd_ip_address.v4 = 0xe0000001UL;
    bsd_mcast_prepare_send(&a, &dest);
    check(bsd_mcast6_prepare_send(&base, &a, &dest) == -1,
          "IPv4 group is not treated as IPv6");
    check(a.as_Nx.udp.nx_udp_socket_time_to_live == 7,
          "IPv4 multicast TTL preserved");

    printf("mcast epoch: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
