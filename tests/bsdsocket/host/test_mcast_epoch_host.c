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

/* Exercise public option dispatch on both parent and compacted source.
   NetX hooks check row ownership before they are called, while the scheduler
   bracket is held; failed joins must roll back before it is released. */
static LONG h_error;
static BOOL h_enter_fail;
static unsigned h_attempts, h_enters, h_leaves, h_depth, h_joins, h_drops;
static UINT h_join_status = NX_SUCCESS;
static AmiSocket *h_member;

LONG bsd_fail(struct AmiSocketBase *base, LONG error)
{ (VOID)base; h_error = error; return -1; }
LONG bsd_errno_from_nx(UINT status)
{ (VOID)status; return AMI_EIO; }
LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    (VOID)base;
    h_attempts++;
    if (h_enter_fail)
        return -1;
    h_enters++;
    h_depth++;
    return 0;
}
VOID bsd_nx_leave(struct AmiSocketBase *base)
{
    (VOID)base;
    check(h_depth == 1, "membership releases exactly one held bracket");
    h_depth--;
    h_leaves++;
}
VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size)
{ memmove(dst, src, size); }
VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{
    unsigned i;
    for (i = 0; i < 4; i++)
        words[i] = ((ULONG)bytes[i * 4] << 24) |
                   ((ULONG)bytes[i * 4 + 1] << 16) |
                   ((ULONG)bytes[i * 4 + 2] << 8) | bytes[i * 4 + 3];
}

UINT nx_igmp_multicast_interface_join(NX_IP *ip, ULONG group, UINT iface)
{
    (VOID)ip;
    h_joins++;
    check(h_depth == 1 && iface == 1 && group == 0xefff2a63UL,
          "IPv4 join reaches NetX with exact group/interface inside bracket");
    check(bsd_mcast_find(h_member, group, iface) != NULL,
          "IPv4 row claimed before NetX join");
    return h_join_status;
}
UINT nx_igmp_multicast_interface_leave(NX_IP *ip, ULONG group, UINT iface)
{
    (VOID)ip;
    h_drops++;
    check(h_depth == 1 && bsd_mcast_find(h_member, group, iface) != NULL,
          "IPv4 leave reaches NetX before row is cleared, inside bracket");
    return NX_NOT_ENABLED; /* teardown intentionally ignores this status */
}
UINT nxd_ipv6_multicast_interface_join(NX_IP *ip, NXD_ADDRESS *group, UINT iface)
{
    (VOID)ip;
    h_joins++;
    check(h_depth == 1 && iface == 1 && group->nxd_ip_version == NX_IP_VERSION_V6 &&
          group->nxd_ip_address.v6[0] == 0xff020000UL &&
          group->nxd_ip_address.v6[1] == 0 && group->nxd_ip_address.v6[2] == 0 &&
          group->nxd_ip_address.v6[3] == 1,
          "IPv6 join reaches NetX with exact group/interface inside bracket");
    check(bsd_mcast6_find(h_member, group->nxd_ip_address.v6, iface) != NULL,
          "IPv6 row claimed before NetX join");
    return h_join_status;
}
UINT nxd_ipv6_multicast_interface_leave(NX_IP *ip, NXD_ADDRESS *group, UINT iface)
{
    (VOID)ip;
    h_drops++;
    check(h_depth == 1 && group->nxd_ip_version == NX_IP_VERSION_V6 &&
          bsd_mcast6_find(h_member, group->nxd_ip_address.v6, iface) != NULL,
          "IPv6 leave reaches NetX before row is cleared, inside bracket");
    return NX_NOT_ENABLED;
}

static LONG h_membership(struct AmiSocketBase *base, AmiSocket *sock,
                          BOOL v6, BOOL join, BOOL valid_group)
{
    /* Public setters must continue accepting unaligned application buffers. */
    UBYTE unaligned[sizeof(struct ipv6_mreq) + 1];

    h_member = sock;
    if (v6)
    {
        struct ipv6_mreq req;
        memset(&req, 0, sizeof(req));
        req.ipv6mr_multiaddr.s6_addr[0] = valid_group ? 0xff : 0x20;
        req.ipv6mr_multiaddr.s6_addr[1] = 2;
        req.ipv6mr_multiaddr.s6_addr[15] = 1;
        req.ipv6mr_interface = 2; /* POSIX number -> NetX slot 1 */
        memcpy(unaligned + 1, &req, sizeof(req));
        return bsd_mcast6_setopt(base, sock,
                                join ? AMI_IPV6_JOIN_GROUP_BSD : AMI_IPV6_LEAVE_GROUP_BSD,
                                unaligned + 1, sizeof(req));
    }
    else
    {
        struct ip_mreq req;
        req.imr_multiaddr.s_addr = BSD_HTONL(valid_group ? 0xefff2a63UL : 0x0a000001UL);
        req.imr_interface.s_addr = 0; /* first live interface */
        memcpy(unaligned + 1, &req, sizeof(req));
        return bsd_mcast_setopt(base, sock,
                               join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                               unaligned + 1, sizeof(req));
    }
}

static void t_membership_paths(BOOL v6)
{
    static NX_IP ip;
    struct AmiSocketBase base;
    AmiSocket sock, other;
    unsigned join, i;
    static const UINT refused[] = { NX_NO_MORE_ENTRIES, NX_OVERFLOW, NX_NOT_ENABLED };

    memset(&ip, 0, sizeof(ip));
    memset(&base, 0, sizeof(base));
    memset(&sock, 0, sizeof(sock));
    memset(&other, 0, sizeof(other));
    memset(bsd_mcast_table, 0, sizeof(bsd_mcast_table));
    memset(bsd_mcast6_table, 0, sizeof(bsd_mcast6_table));
    base.sb_StackIp = &ip;
    ip.nx_ip_interface[1].nx_interface_valid = NX_TRUE;
    ip.nx_ip_interface[1].nx_interface_link_up = NX_TRUE;
    h_join_status = NX_SUCCESS;
    h_enter_fail = FALSE;
    h_attempts = h_enters = h_leaves = h_depth = h_joins = h_drops = 0;

    for (join = 0; join < 2; join++)
    {
        unsigned before = h_attempts;

        base.sb_StackRefs = 0;
        check(h_membership(&base, &sock, v6, join != 0, FALSE) == -1 &&
              h_error == AMI_ENETDOWN && h_attempts == before,
              "missing stack wins over invalid group, before bracket");
        base.sb_StackRefs = 1;
        check(h_membership(&base, &sock, v6, join != 0, FALSE) == -1 &&
              h_error == AMI_EINVAL && h_attempts == before,
              "invalid group is refused before bracket");
        h_enter_fail = TRUE;
        check(h_membership(&base, &sock, v6, join != 0, TRUE) == -1 &&
              h_error == AMI_ENETDOWN && h_depth == 0 && h_attempts == before + 1,
              "failed bracket does not reach NetX");
        h_enter_fail = FALSE;
        ip.nx_ip_interface[1].nx_interface_valid = NX_FALSE;
        check(h_membership(&base, &sock, v6, join != 0, TRUE) == -1 &&
              h_error == AMI_EADDRNOTAVAIL && h_enters == h_leaves,
              "missing interface balances bracket for join and leave");
        ip.nx_ip_interface[1].nx_interface_valid = NX_TRUE;
    }
    check(h_joins == 0 && h_drops == 0, "validation never calls NetX membership API");
    check(h_membership(&base, &sock, v6, FALSE, TRUE) == -1 &&
          h_error == AMI_EADDRNOTAVAIL, "missing membership cannot leave");
    check(h_membership(&base, &sock, v6, TRUE, TRUE) == 0 && h_joins == 1,
          "first join succeeds");
    check(h_membership(&base, &sock, v6, TRUE, TRUE) == -1 &&
          h_error == AMI_EADDRINUSE && h_joins == 1, "duplicate join makes no NetX call");
    check(h_membership(&base, &sock, v6, FALSE, TRUE) == 0 && h_drops == 1,
          "leave clears row despite teardown status");
    check(h_membership(&base, &sock, v6, FALSE, TRUE) == -1 &&
          h_error == AMI_EADDRNOTAVAIL && h_drops == 1, "second leave finds no row");

    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    {
        h_join_status = refused[i];
        check(h_membership(&base, &sock, v6, TRUE, TRUE) == -1 &&
              h_error == (refused[i] == NX_NO_MORE_ENTRIES ||
                          (v6 && refused[i] == NX_OVERFLOW) ? AMI_ENOBUFS : AMI_EIO),
              "join refusal retains family-specific resource/error mapping");
        check(h_membership(&base, &sock, v6, FALSE, TRUE) == -1 &&
              h_error == AMI_EADDRNOTAVAIL, "failed join returns row before leaving bracket");
    }
    h_join_status = NX_SUCCESS;
    for (i = 0; i < (v6 ? BSD_MCAST6_MEMBERSHIPS : BSD_MCAST_MEMBERSHIPS); i++)
    {
        if (v6)
        {
            bsd_mcast6_table[i].bm_Sock = &other;
            bsd_mcast6_table[i].bm_Iface = 1;
            bsd_mcast6_table[i].bm_Epoch = h_epoch[1];
        }
        else
        {
            bsd_mcast_table[i].bm_Sock = &other;
            bsd_mcast_table[i].bm_Iface = 1;
            bsd_mcast_table[i].bm_Epoch = h_epoch[1];
        }
    }
    i = h_joins;
    check(h_membership(&base, &sock, v6, TRUE, TRUE) == -1 &&
          h_error == AMI_ENOBUFS && h_joins == i, "full table refuses before NetX join");
    check(h_depth == 0 && h_enters == h_leaves, "all acquired membership brackets balanced");
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

    t_membership_paths(FALSE);
    t_membership_paths(TRUE);

    printf("mcast epoch: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
