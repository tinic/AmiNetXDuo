/*
 * bsdsocket.library, multicast group membership, both families.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/nxstatus.h"

/*
 * How many (socket, group, interface) memberships the library tracks at once.
 */
#define BSD_MCAST_MEMBERSHIPS   16

typedef struct BsdMcastEntry
{
    AmiSocket  *bm_Sock;        /* NULL: free row                           */
    ULONG       bm_Group;
    UINT        bm_Iface;       /* NetX interface index                     */
    ULONG       bm_Epoch;       /* detach generation of that slot           */
} BsdMcastEntry;

/*
 * One table for the machine, not one per opener: the NX_IP is the singleton
 * the memberships belong to. Every entry point below runs inside a
 * bsd_nx_enter() bracket, and that bracket holds the ThreadX scheduler lock
 * (netx_call.c), so the table needs no lock of its own, the same reasoning
 * raw.c's registry rests on.
 */
static BsdMcastEntry bsd_mcast_table[BSD_MCAST_MEMBERSHIPS];

/* A saved IP_MULTICAST_IF is an interface identity, not a permanent numeric
   slot preference.  If that slot was detached, use routing until the caller
   explicitly selects an interface again.  Call within a NetX bracket. */
static LONG bsd_mcast_preference(LONG *iface, ULONG epoch)
{
    if (*iface >= 0 &&
        netstack_interface_epoch((UWORD)*iface) != epoch)
        *iface = -1;

    return *iface;
}

/* NetX drops the actual join on interface detach.  Do not let a BSD row
   outlive that join: after slot reuse, Close() could otherwise leave another
   socket's group, and re-join on this socket would appear duplicated.  All
   callers are in a bsd_nx_enter() bracket. */
static BOOL bsd_mcast_row_live(BsdMcastEntry *e)
{
    if (e->bm_Sock != NULL &&
        e->bm_Epoch != netstack_interface_epoch((UWORD)e->bm_Iface))
        e->bm_Sock = NULL;

    return e->bm_Sock != NULL;
}

static BOOL bsd_mcast_is_group(ULONG addr)
{
    return ((addr & 0xF0000000UL) == 0xE0000000UL) ? TRUE : FALSE;
}

/*
 * imr_interface -> NetX interface index.  INADDR_ANY means "the one the route
 * would pick", which for a multicast destination is the first interface whose
 * link is up (nx_ip_route_find.c).  The choice is made here rather than
 * deferred, so the membership and the sends stay on the same interface.
 */
static LONG bsd_mcast_iface_of(NX_IP *ip, ULONG addr)
{
    UINT i;

    if (ip == NULL)
        return -1;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        const NX_INTERFACE *nxif = &ip->nx_ip_interface[i];

        if (nxif->nx_interface_valid == 0)
            continue;

        if (addr == 0UL)
        {
            if (nxif->nx_interface_link_up)
                return (LONG)i;
            continue;
        }

        if (nxif->nx_interface_ip_address == addr)
            return (LONG)i;
    }

    return -1;
}

static BsdMcastEntry *bsd_mcast_find(const AmiSocket *sock, ULONG group,
                                     UINT iface)
{
    UINT i;

    for (i = 0; i < BSD_MCAST_MEMBERSHIPS; i++)
    {
        BsdMcastEntry *e = &bsd_mcast_table[i];

        if (bsd_mcast_row_live(e) && e->bm_Sock == sock &&
            e->bm_Group == group && e->bm_Iface == iface)
            return e;
    }

    return NULL;
}

static BsdMcastEntry *bsd_mcast_free_row(VOID)
{
    UINT i;

    for (i = 0; i < BSD_MCAST_MEMBERSHIPS; i++)
    {
        if (!bsd_mcast_row_live(&bsd_mcast_table[i]))
            return &bsd_mcast_table[i];
    }

    return NULL;
}

static LONG bsd_mcast_join(struct AmiSocketBase *base, AmiSocket *sock,
                           const struct ip_mreq *mreq)
{
    NX_IP         *ip = bsd_stack_ip(base);
    BsdMcastEntry *row;
    ULONG          group;
    LONG           iface;
    UINT           status;

    if (ip == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    group = BSD_NTOHL(mreq->imr_multiaddr.s_addr);
    if (!bsd_mcast_is_group(group))
        return bsd_fail(base, AMI_EINVAL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    iface = bsd_mcast_iface_of(ip, BSD_NTOHL(mreq->imr_interface.s_addr));
    if (iface < 0)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    if (bsd_mcast_find(sock, group, (UINT)iface) != NULL)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRINUSE);
    }

    row = bsd_mcast_free_row();
    if (row == NULL)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_ENOBUFS);
    }

    /* Claim the row inside the bracket. A free row is only free because
       bm_Sock is NULL, so a row left that way until after bsd_nx_leave() lets
       a second base's join pick the same one. */
    row->bm_Sock  = sock;
    row->bm_Group = group;
    row->bm_Iface = (UINT)iface;
    row->bm_Epoch = netstack_interface_epoch((UWORD)iface);

    status = nx_igmp_multicast_interface_join(ip, group, (UINT)iface);

    if (status != NX_SUCCESS)
        row->bm_Sock = NULL;            /* give the row back */

    bsd_nx_leave(base);

    if (status != NX_SUCCESS)
    {
        /* NX_NO_MORE_ENTRIES is the NX_MAX_MULTICAST_GROUPS cap, which is a
           resource shortage and not a bad argument. */
        return bsd_fail(base, (status == NX_NO_MORE_ENTRIES)
                                  ? AMI_ENOBUFS
                                  : bsd_errno_from_nx(status));
    }

    return 0;
}

static LONG bsd_mcast_leave(struct AmiSocketBase *base, AmiSocket *sock,
                            const struct ip_mreq *mreq)
{
    NX_IP         *ip = bsd_stack_ip(base);
    BsdMcastEntry *row;
    ULONG          group;
    LONG           iface;

    if (ip == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    group = BSD_NTOHL(mreq->imr_multiaddr.s_addr);
    if (!bsd_mcast_is_group(group))
        return bsd_fail(base, AMI_EINVAL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    iface = bsd_mcast_iface_of(ip, BSD_NTOHL(mreq->imr_interface.s_addr));
    if (iface < 0)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    row = bsd_mcast_find(sock, group, (UINT)iface);
    if (row == NULL)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    AMI_NX_CLEANUP(nx_igmp_multicast_interface_leave(ip, group, (UINT)iface));
    row->bm_Sock = NULL;

    bsd_nx_leave(base);

    return 0;
}

#ifdef AMINETXDUO_IPV6
static VOID bsd_mcast6_close(NX_IP *ip, AmiSocket *sock);
#endif

VOID bsd_mcast_close(AmiSocket *sock)
{
    NX_IP *ip = bsd_stack_ip(sock->as_Owner);
    UINT   i;

    for (i = 0; i < BSD_MCAST_MEMBERSHIPS; i++)
    {
        BsdMcastEntry *e = &bsd_mcast_table[i];

        if (!bsd_mcast_row_live(e) || e->bm_Sock != sock)
            continue;

        if (ip != NULL)
            AMI_NX_CLEANUP(nx_igmp_multicast_interface_leave(ip, e->bm_Group,
                                                    e->bm_Iface));
        e->bm_Sock = NULL;
    }

#ifdef AMINETXDUO_IPV6
    bsd_mcast6_close(ip, sock);
#endif
}

/* IPv4 group destination: *ttl becomes IP_MULTICAST_TTL and the result is
   the IP_MULTICAST_IF index, -1 for the route.  Otherwise -1, *ttl kept. */
LONG bsd_mcast_send_choice(AmiSocket *sock, const NXD_ADDRESS *addr, UINT *ttl)
{
    if (addr->nxd_ip_version != NX_IP_VERSION_V4 ||
        !bsd_mcast_is_group(addr->nxd_ip_address.v4))
        return -1;

    *ttl = (UINT)sock->as_McastTtl;

    return bsd_mcast_preference(&sock->as_McastIf,
                                sock->as_McastIfEpoch);
}

LONG bsd_mcast_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr)
{
    UINT ttl   = (UINT)(sock->as_Ttl & 0xFF);
    LONG iface = bsd_mcast_send_choice(sock, addr, &ttl);

    sock->as_Nx.udp.nx_udp_socket_time_to_live = ttl;

    return iface;
}

/* NetX snapshots a *global* loopback setting when a group is first joined,
 * then its send path consults that group's entry. IP_MULTICAST_LOOP is instead
 * a property of the sender, which need not have joined the group at all.
 * nxd_udp_socket_send() reaches nx_ip_driver_packet_send() synchronously.
 * Hold the IP mutex across the override and that call: a NetX bracket can
 * yield while taking a mutex, and a second sender must not observe our
 * temporary flag. ThreadX mutexes are recursive for the owning thread, so
 * the send's own tx_mutex_get() is safe. NetX chooses the first entry matching
 * the group, independent of interface; mirror that lookup. No entry means no
 * local receiver to loop to. */
VOID bsd_mcast_loop_begin(NX_IP *ip, const AmiSocket *sock,
                          const NXD_ADDRESS *addr, BsdMcastLoopGuard *guard)
{
    UINT i;

    guard->ip = NULL;
    guard->flag = NULL;
    guard->saved = 0;
    if (ip == NULL || addr->nxd_ip_version != NX_IP_VERSION_V4 ||
        !bsd_mcast_is_group(addr->nxd_ip_address.v4))
        return;

    AMI_NX_ONLY_SUCCESS(tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER));
    for (i = 0; i < (UINT)NX_MAX_MULTICAST_GROUPS; i++)
    {
        NX_IPV4_MULTICAST_ENTRY *entry = &ip->nx_ipv4_multicast_entry[i];

        if (entry->nx_ipv4_multicast_join_list != addr->nxd_ip_address.v4)
            continue;

        guard->flag = &entry->nx_ipv4_multicast_loopback_enable;
        guard->ip = ip;
        guard->saved = *guard->flag;
        *guard->flag = (sock->as_McastLoop != 0) ? NX_TRUE : NX_FALSE;
        return;
    }
    AMI_NX_ONLY_SUCCESS(tx_mutex_put(&ip->nx_ip_protection));
}

VOID bsd_mcast_loop_end(BsdMcastLoopGuard *guard)
{
    if (guard->flag != NULL)
    {
        *guard->flag = guard->saved;
        AMI_NX_ONLY_SUCCESS(tx_mutex_put(&guard->ip->nx_ip_protection));
    }
}

/*
 * 4.4BSD types IP_MULTICAST_TTL and IP_MULTICAST_LOOP as u_char and everything
 * written since passes an int, so every width is taken here.  getsockopt
 * answers in whichever width the caller offered room for.  A program that
 */
static LONG bsd_mcast_get_byte_or_long(struct AmiSocketBase *base, APTR optval,
                                       socklen_t optlen, LONG *value)
{
    if (optval == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (optlen >= (socklen_t)sizeof(LONG))
        bsd_bcopy(optval, value, sizeof(*value));
    else if (optlen >= (socklen_t)sizeof(WORD))
    {
        WORD short_value;

        bsd_bcopy(optval, &short_value, sizeof(short_value));
        *value = (LONG)short_value;
    }
    else if (optlen >= (socklen_t)sizeof(UBYTE))
        *value = (LONG)*(UBYTE *)optval;
    else
        return bsd_fail(base, AMI_EINVAL);

    return 0;
}

static LONG bsd_mcast_put_byte_or_long(struct AmiSocketBase *base, APTR optval,
                                       socklen_t *optlen, LONG value)
{
    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (*optlen >= (socklen_t)sizeof(LONG))
    {
        bsd_bcopy(&value, optval, sizeof(value));
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (*optlen >= (socklen_t)sizeof(WORD))
    {
        WORD short_value = (WORD)value;

        bsd_bcopy(&short_value, optval, sizeof(short_value));
        *optlen = (socklen_t)sizeof(WORD);
    }
    else if (*optlen >= (socklen_t)sizeof(UBYTE))
    {
        *(UBYTE *)optval = (UBYTE)value;
        *optlen = (socklen_t)sizeof(UBYTE);
    }
    else
    {
        return bsd_fail(base, AMI_EINVAL);
    }

    return 0;
}

LONG bsd_mcast_setopt(struct AmiSocketBase *base, AmiSocket *sock,
                      LONG optname, APTR optval, socklen_t optlen)
{
    LONG value = 0;

    switch (optname)
    {
        case IP_ADD_MEMBERSHIP:
        case IP_DROP_MEMBERSHIP:
        {
            struct ip_mreq mreq;

            if (optval == NULL)
                return bsd_fail(base, AMI_EFAULT);
            if (optlen < (socklen_t)sizeof(struct ip_mreq))
                return bsd_fail(base, AMI_EINVAL);

            /* Copied out: the caller's buffer need not be aligned for the
               ULONG loads the rest of this file does on it. */
            bsd_bcopy(optval, &mreq, sizeof mreq);

            return (optname == IP_ADD_MEMBERSHIP)
                       ? bsd_mcast_join(base, sock, &mreq)
                       : bsd_mcast_leave(base, sock, &mreq);
        }

        case IP_MULTICAST_IF:
        {
            struct in_addr in;
            NX_IP         *ip = bsd_stack_ip(base);
            LONG           iface;

            if (optval == NULL)
                return bsd_fail(base, AMI_EFAULT);
            if (optlen < (socklen_t)sizeof(struct in_addr))
                return bsd_fail(base, AMI_EINVAL);

            bsd_bcopy(optval, &in, sizeof in);

            if (in.s_addr == 0UL)
            {
                sock->as_McastIf = -1;
                sock->as_McastIfEpoch = 0;
                return 0;
            }

            if (bsd_nx_enter(base) != 0)
                return bsd_fail(base, AMI_ENETDOWN);
            iface = bsd_mcast_iface_of(ip, BSD_NTOHL(in.s_addr));
            if (iface >= 0)
            {
                sock->as_McastIf = iface;
                sock->as_McastIfEpoch = netstack_interface_epoch((UWORD)iface);
            }
            bsd_nx_leave(base);
            if (iface < 0)
                return bsd_fail(base, AMI_EADDRNOTAVAIL);
            return 0;
        }

        case IP_MULTICAST_TTL:
            if (bsd_mcast_get_byte_or_long(base, optval, optlen, &value) != 0)
                return -1;
            if (value < 0 || value > 255)
                return bsd_fail(base, AMI_EINVAL);
            /* 0 is legal and means "this host only". */
            sock->as_McastTtl = value;
            return 0;

        case IP_MULTICAST_LOOP:
            if (bsd_mcast_get_byte_or_long(base, optval, optlen, &value) != 0)
                return -1;
            sock->as_McastLoop = (value != 0) ? 1 : 0;
            return 0;

        default:
            return bsd_fail(base, AMI_ENOPROTOOPT);
    }
}

LONG bsd_mcast_getopt(struct AmiSocketBase *base, AmiSocket *sock,
                      LONG optname, APTR optval, socklen_t *optlen)
{
    switch (optname)
    {
        case IP_MULTICAST_IF:
        {
            struct in_addr in;
            NX_IP         *ip = bsd_stack_ip(base);

            if (optval == NULL || optlen == NULL)
                return bsd_fail(base, AMI_EFAULT);
            if (*optlen < (socklen_t)sizeof(struct in_addr))
                return bsd_fail(base, AMI_EINVAL);

            in.s_addr = 0UL;
            if (bsd_nx_enter(base) != 0)
                return bsd_fail(base, AMI_ENETDOWN);
            if (bsd_mcast_preference(&sock->as_McastIf,
                                      sock->as_McastIfEpoch) >= 0 &&
                ip != NULL &&
                ip->nx_ip_interface[sock->as_McastIf].nx_interface_valid != 0)
            {
                in.s_addr = BSD_HTONL(
                    ip->nx_ip_interface[sock->as_McastIf].nx_interface_ip_address);
            }
            bsd_nx_leave(base);

            bsd_bcopy(&in, optval, sizeof in);
            *optlen = (socklen_t)sizeof(struct in_addr);
            return 0;
        }

        case IP_MULTICAST_TTL:
            return bsd_mcast_put_byte_or_long(base, optval, optlen,
                                              sock->as_McastTtl);

        case IP_MULTICAST_LOOP:
            return bsd_mcast_put_byte_or_long(base, optval, optlen,
                                              sock->as_McastLoop);

        case IP_ADD_MEMBERSHIP:
        case IP_DROP_MEMBERSHIP:
            return bsd_fail(base, AMI_EOPNOTSUPP);

        default:
            return bsd_fail(base, AMI_ENOPROTOOPT);
    }
}

#ifdef AMINETXDUO_IPV6

#define BSD_MCAST6_MEMBERSHIPS  16

typedef struct BsdMcast6Entry
{
    AmiSocket  *bm_Sock;            /* NULL: free row                       */
    ULONG       bm_Group[4];
    UINT        bm_Iface;           /* NetX interface index                 */
    ULONG       bm_Epoch;           /* detach generation of that slot       */
} BsdMcast6Entry;

static BsdMcast6Entry bsd_mcast6_table[BSD_MCAST6_MEMBERSHIPS];

static BOOL bsd_mcast6_row_live(BsdMcast6Entry *e)
{
    if (e->bm_Sock != NULL &&
        e->bm_Epoch != netstack_interface_epoch((UWORD)e->bm_Iface))
        e->bm_Sock = NULL;

    return e->bm_Sock != NULL;
}

static BOOL bsd_mcast6_is_group(const ULONG group[4])
{
    return ((group[0] & 0xFF000000UL) == 0xFF000000UL) ? TRUE : FALSE;
}

/* fe80::/10.  socket.c has its own copy for the zone notation. Two lines are
   cheaper than one export. */
static BOOL bsd_mcast6_is_linklocal(const ULONG v6[4])
{
    return ((v6[0] & 0xFFC00000UL) == 0xFE800000UL) ? TRUE : FALSE;
}

static BOOL bsd_mcast6_same(const ULONG a[4], const ULONG b[4])
{
    return (a[0] == b[0] && a[1] == b[1] &&
            a[2] == b[2] && a[3] == b[3]) ? TRUE : FALSE;
}

/*
 * ipv6mr_interface -> NetX interface index.  The caller's number is the
 * if_nametoindex() kind, one higher than NetX's, the same convention
 * sin6_scope_id follows here.  0 means "the one the route would pick", which
 */
static LONG bsd_mcast6_iface_of(NX_IP *ip, ULONG posix_index)
{
    UINT i;

    if (ip == NULL)
        return -1;

    if (posix_index != 0UL)
    {
        if (posix_index > (ULONG)NX_MAX_PHYSICAL_INTERFACES)
            return -1;

        i = (UINT)(posix_index - 1UL);

        return (ip->nx_ip_interface[i].nx_interface_valid != 0)
                   ? (LONG)i
                   : -1;
    }

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        const NX_INTERFACE *nxif = &ip->nx_ip_interface[i];

        if (nxif->nx_interface_valid != 0 && nxif->nx_interface_link_up)
            return (LONG)i;
    }

    return -1;
}

/*
 * The IPv6 ADDRESS index to send a group datagram from, given the interface it
 * must leave by.  nxd_udp_socket_source_send() indexes nx_ipv6_address, not
 * nx_ip_interface, and a group has no address of its own to match against.
 */
static LONG bsd_mcast6_source_index(NX_IP *ip, UINT iface)
{
    const NX_INTERFACE *nxif;
    LONG                fallback = -1;
    UINT                i;

    if (ip == NULL)
        return -1;

    nxif = &ip->nx_ip_interface[iface];

    for (i = 0; i < (UINT)NX_MAX_IPV6_ADDRESSES; i++)
    {
        const NXD_IPV6_ADDRESS *a = &ip->nx_ipv6_address[i];

        if (a->nxd_ipv6_address_valid == 0 ||
            a->nxd_ipv6_address_state != NX_IPV6_ADDR_STATE_VALID ||
            a->nxd_ipv6_address_attached != nxif)
            continue;

        if (bsd_mcast6_is_linklocal(a->nxd_ipv6_address))
            return (LONG)a->nxd_ipv6_address_index;

        if (fallback < 0)
            fallback = (LONG)a->nxd_ipv6_address_index;
    }

    return fallback;
}

static BsdMcast6Entry *bsd_mcast6_find(const AmiSocket *sock,
                                       const ULONG group[4], UINT iface)
{
    UINT i;

    for (i = 0; i < BSD_MCAST6_MEMBERSHIPS; i++)
    {
        BsdMcast6Entry *e = &bsd_mcast6_table[i];

        if (bsd_mcast6_row_live(e) && e->bm_Sock == sock &&
            e->bm_Iface == iface &&
            bsd_mcast6_same(e->bm_Group, group))
            return e;
    }

    return NULL;
}

static BsdMcast6Entry *bsd_mcast6_free_row(VOID)
{
    UINT i;

    for (i = 0; i < BSD_MCAST6_MEMBERSHIPS; i++)
    {
        if (!bsd_mcast6_row_live(&bsd_mcast6_table[i]))
            return &bsd_mcast6_table[i];
    }

    return NULL;
}

static LONG bsd_mcast6_join(struct AmiSocketBase *base, AmiSocket *sock,
                            const struct ipv6_mreq *mreq)
{
    NX_IP          *ip = bsd_stack_ip(base);
    BsdMcast6Entry *row;
    NXD_ADDRESS     group;
    LONG            iface;
    UINT            status;

    if (ip == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    group.nxd_ip_version = NX_IP_VERSION_V6;
    bsd_in6_to_words(mreq->ipv6mr_multiaddr.s6_addr, group.nxd_ip_address.v6);

    if (!bsd_mcast6_is_group(group.nxd_ip_address.v6))
        return bsd_fail(base, AMI_EINVAL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    iface = bsd_mcast6_iface_of(ip, mreq->ipv6mr_interface);
    if (iface < 0)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    if (bsd_mcast6_find(sock, group.nxd_ip_address.v6, (UINT)iface) != NULL)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRINUSE);
    }

    row = bsd_mcast6_free_row();
    if (row == NULL)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_ENOBUFS);
    }

    /* Claim the row inside the bracket, see bsd_mcast_join(). */
    row->bm_Sock = sock;
    row->bm_Group[0] = group.nxd_ip_address.v6[0];
    row->bm_Group[1] = group.nxd_ip_address.v6[1];
    row->bm_Group[2] = group.nxd_ip_address.v6[2];
    row->bm_Group[3] = group.nxd_ip_address.v6[3];
    row->bm_Iface = (UINT)iface;
    row->bm_Epoch = netstack_interface_epoch((UWORD)iface);

    status = nxd_ipv6_multicast_interface_join(ip, &group, (UINT)iface);

    if (status != NX_SUCCESS)
        row->bm_Sock = NULL;

    bsd_nx_leave(base);

    if (status != NX_SUCCESS)
    {
        /* NX_NO_MORE_ENTRIES is the NX_MAX_MULTICAST_GROUPS cap and
           NX_OVERFLOW is a driver that refused the MAC address. Both are
           resource shortages rather than bad arguments. */
        return bsd_fail(base, (status == NX_NO_MORE_ENTRIES ||
                               status == NX_OVERFLOW)
                                  ? AMI_ENOBUFS
                                  : bsd_errno_from_nx(status));
    }

    return 0;
}

static LONG bsd_mcast6_leave(struct AmiSocketBase *base, AmiSocket *sock,
                             const struct ipv6_mreq *mreq)
{
    NX_IP          *ip = bsd_stack_ip(base);
    BsdMcast6Entry *row;
    NXD_ADDRESS     group;
    LONG            iface;

    if (ip == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    group.nxd_ip_version = NX_IP_VERSION_V6;
    bsd_in6_to_words(mreq->ipv6mr_multiaddr.s6_addr, group.nxd_ip_address.v6);

    if (!bsd_mcast6_is_group(group.nxd_ip_address.v6))
        return bsd_fail(base, AMI_EINVAL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    iface = bsd_mcast6_iface_of(ip, mreq->ipv6mr_interface);
    if (iface < 0)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    row = bsd_mcast6_find(sock, group.nxd_ip_address.v6, (UINT)iface);
    if (row == NULL)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    AMI_NX_CLEANUP(nxd_ipv6_multicast_interface_leave(ip, &group, (UINT)iface));
    row->bm_Sock = NULL;

    bsd_nx_leave(base);

    return 0;
}

static VOID bsd_mcast6_close(NX_IP *ip, AmiSocket *sock)
{
    UINT i;

    for (i = 0; i < BSD_MCAST6_MEMBERSHIPS; i++)
    {
        BsdMcast6Entry *e = &bsd_mcast6_table[i];
        NXD_ADDRESS     group;

        if (!bsd_mcast6_row_live(e) || e->bm_Sock != sock)
            continue;

        if (ip != NULL)
        {
            group.nxd_ip_version = NX_IP_VERSION_V6;
            group.nxd_ip_address.v6[0] = e->bm_Group[0];
            group.nxd_ip_address.v6[1] = e->bm_Group[1];
            group.nxd_ip_address.v6[2] = e->bm_Group[2];
            group.nxd_ip_address.v6[3] = e->bm_Group[3];

            AMI_NX_CLEANUP(nxd_ipv6_multicast_interface_leave(ip, &group, e->bm_Iface));
        }

        e->bm_Sock = NULL;
    }
}

LONG bsd_mcast6_prepare_send(struct AmiSocketBase *base, AmiSocket *sock,
                             const NXD_ADDRESS *addr, ULONG *saved)
{
    NX_IP *ip = bsd_stack_ip(base);
    LONG   iface;

    *saved = 0UL;

    if (addr->nxd_ip_version != NX_IP_VERSION_V6 || ip == NULL ||
        !bsd_mcast6_is_group(addr->nxd_ip_address.v6))
        return -1;

    /*
     * RFC 3493 5.2: a hop limit of 0 is "this host only". Nothing here
     * delivers a multicast datagram back to its sender, because
     * IPV6_MULTICAST_LOOP is accepted and reads back 0. Such a datagram
     */
    if (sock->as_Mcast6Hops == 0)
        return BSD_MCAST6_NO_LINK;

    *saved = ip->nx_ipv6_hop_limit;
    ip->nx_ipv6_hop_limit = (ULONG)sock->as_Mcast6Hops;

    iface = bsd_mcast_preference(&sock->as_Mcast6If,
                                  sock->as_Mcast6IfEpoch);
    if (iface < 0)
        return -1;

    return bsd_mcast6_source_index(ip, (UINT)iface);
}

VOID bsd_mcast6_finish_send(struct AmiSocketBase *base, ULONG saved)
{
    NX_IP *ip = bsd_stack_ip(base);

    if (saved != 0UL && ip != NULL)
        ip->nx_ipv6_hop_limit = saved;
}

BOOL bsd_mcast6_is_option(const AmiSocket *sock, LONG optname)
{
    /* The Linux numbering is withdrawn on a raw socket. See
       bsd_v6_linux_numbering() in in6.c. */
    if (!bsd_v6_linux_numbering(sock))
    {
        switch (optname)
        {
            case AMI_IPV6_JOIN_GROUP_LINUX:
            case AMI_IPV6_LEAVE_GROUP_LINUX:
            case AMI_IPV6_MULTICAST_IF_LINUX:
            case AMI_IPV6_MULTICAST_HOPS_LINUX:
            case AMI_IPV6_MULTICAST_LOOP_LINUX:
                return FALSE;
            default:
                break;
        }
    }

    switch (optname)
    {
        case AMI_IPV6_JOIN_GROUP_BSD:
        case AMI_IPV6_JOIN_GROUP_LINUX:
        case AMI_IPV6_LEAVE_GROUP_BSD:
        case AMI_IPV6_LEAVE_GROUP_LINUX:
        case AMI_IPV6_MULTICAST_IF_BSD:
        case AMI_IPV6_MULTICAST_IF_LINUX:
        case AMI_IPV6_MULTICAST_HOPS_BSD:
        case AMI_IPV6_MULTICAST_HOPS_LINUX:
        case AMI_IPV6_MULTICAST_LOOP_BSD:
        case AMI_IPV6_MULTICAST_LOOP_LINUX:
            return TRUE;
        default:
            return FALSE;
    }
}

/*
 * IPV6_MULTICAST_HOPS and _LOOP are ints in RFC 3493, unlike their u_char IPv4
 * counterparts, so there is no byte-or-long dance here.  A short is taken as
 * well for the same reason in6.c does.
 */
static LONG bsd_mcast6_get_int(struct AmiSocketBase *base, APTR optval,
                               socklen_t optlen, LONG *value)
{
    if (optval == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (optlen >= (socklen_t)sizeof(LONG))
        bsd_bcopy(optval, value, sizeof(*value));
    else if (optlen >= (socklen_t)sizeof(WORD))
    {
        WORD short_value;

        bsd_bcopy(optval, &short_value, sizeof(short_value));
        *value = (LONG)short_value;
    }
    else
        return bsd_fail(base, AMI_EINVAL);

    return 0;
}

static LONG bsd_mcast6_put_int(struct AmiSocketBase *base, APTR optval,
                               socklen_t *optlen, LONG value)
{
    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (*optlen >= (socklen_t)sizeof(LONG))
    {
        bsd_bcopy(&value, optval, sizeof(value));
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (*optlen >= (socklen_t)sizeof(WORD))
    {
        WORD short_value = (WORD)value;

        bsd_bcopy(&short_value, optval, sizeof(short_value));
        *optlen = (socklen_t)sizeof(WORD);
    }
    else
    {
        return bsd_fail(base, AMI_EINVAL);
    }

    return 0;
}

LONG bsd_mcast6_setopt(struct AmiSocketBase *base, AmiSocket *sock,
                       LONG optname, APTR optval, socklen_t optlen)
{
    LONG value = 0;

    switch (optname)
    {
        case AMI_IPV6_JOIN_GROUP_BSD:
        case AMI_IPV6_JOIN_GROUP_LINUX:
        case AMI_IPV6_LEAVE_GROUP_BSD:
        case AMI_IPV6_LEAVE_GROUP_LINUX:
        {
            struct ipv6_mreq mreq;
            BOOL             join = (optname == AMI_IPV6_JOIN_GROUP_BSD ||
                                     optname == AMI_IPV6_JOIN_GROUP_LINUX);

            if (optval == NULL)
                return bsd_fail(base, AMI_EFAULT);
            if (optlen < (socklen_t)sizeof(struct ipv6_mreq))
                return bsd_fail(base, AMI_EINVAL);

            /* Copied out: the caller's buffer need not be aligned for the
               ULONG load of ipv6mr_interface. */
            bsd_bcopy(optval, &mreq, sizeof mreq);

            return join ? bsd_mcast6_join(base, sock, &mreq)
                        : bsd_mcast6_leave(base, sock, &mreq);
        }

        case AMI_IPV6_MULTICAST_IF_BSD:
        case AMI_IPV6_MULTICAST_IF_LINUX:
        {
            NX_IP *ip = bsd_stack_ip(base);
            LONG   iface;

            if (bsd_mcast6_get_int(base, optval, optlen, &value) != 0)
                return -1;

            if (value == 0)
            {
                sock->as_Mcast6If = -1;
                sock->as_Mcast6IfEpoch = 0;
                return 0;
            }
            if (value < 0)
                return bsd_fail(base, AMI_EINVAL);

            if (bsd_nx_enter(base) != 0)
                return bsd_fail(base, AMI_ENETDOWN);
            iface = bsd_mcast6_iface_of(ip, (ULONG)value);
            if (iface >= 0)
            {
                sock->as_Mcast6If = iface;
                sock->as_Mcast6IfEpoch =
                    netstack_interface_epoch((UWORD)iface);
            }
            bsd_nx_leave(base);
            if (iface < 0)
                return bsd_fail(base, AMI_ENXIO);
            return 0;
        }

        case AMI_IPV6_MULTICAST_HOPS_BSD:
        case AMI_IPV6_MULTICAST_HOPS_LINUX:
            if (bsd_mcast6_get_int(base, optval, optlen, &value) != 0)
                return -1;
            if (value < -1 || value > 255)
                return bsd_fail(base, AMI_EINVAL);
            /*
             * RFC 3493 5.2's table, exactly: x < -1 is EINVAL, -1 is "the
             * default", which that section puts at one hop, and 0 <= x <= 255
             * uses x. 0 is "this host only", and bsd_mcast6_prepare_send()
             */
            sock->as_Mcast6Hops = (value < 0) ? 1 : value;
            return 0;

        case AMI_IPV6_MULTICAST_LOOP_BSD:
        case AMI_IPV6_MULTICAST_LOOP_LINUX:
            /* Accepted, stored nowhere, always reads back 0. See the note at
               the top of this half. */
            if (bsd_mcast6_get_int(base, optval, optlen, &value) != 0)
                return -1;
            return 0;

        default:
            return bsd_fail(base, AMI_ENOPROTOOPT);
    }
}

LONG bsd_mcast6_getopt(struct AmiSocketBase *base, AmiSocket *sock,
                       LONG optname, APTR optval, socklen_t *optlen)
{
    switch (optname)
    {
        case AMI_IPV6_MULTICAST_IF_BSD:
        case AMI_IPV6_MULTICAST_IF_LINUX:
        {
            LONG iface;

            /* Back in the caller's numbering, one higher, and 0 for "the
               route decides". */
            if (bsd_nx_enter(base) != 0)
                return bsd_fail(base, AMI_ENETDOWN);
            iface = bsd_mcast_preference(&sock->as_Mcast6If,
                                          sock->as_Mcast6IfEpoch);
            bsd_nx_leave(base);
            return bsd_mcast6_put_int(base, optval, optlen,
                                      (iface < 0)
                                          ? 0
                                          : iface + 1);
        }

        case AMI_IPV6_MULTICAST_HOPS_BSD:
        case AMI_IPV6_MULTICAST_HOPS_LINUX:
            return bsd_mcast6_put_int(base, optval, optlen,
                                      sock->as_Mcast6Hops);

        case AMI_IPV6_MULTICAST_LOOP_BSD:
        case AMI_IPV6_MULTICAST_LOOP_LINUX:
            return bsd_mcast6_put_int(base, optval, optlen, 0);

        /* Set-only, as the IPv4 pair are. */
        case AMI_IPV6_JOIN_GROUP_BSD:
        case AMI_IPV6_JOIN_GROUP_LINUX:
        case AMI_IPV6_LEAVE_GROUP_BSD:
        case AMI_IPV6_LEAVE_GROUP_LINUX:
            return bsd_fail(base, AMI_EOPNOTSUPP);

        default:
            return bsd_fail(base, AMI_ENOPROTOOPT);
    }
}

#endif /* AMINETXDUO_IPV6 */
