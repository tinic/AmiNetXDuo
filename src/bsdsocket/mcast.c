/*
 * bsdsocket.library, multicast group membership, both families.
 *
 * IP_ADD_MEMBERSHIP, IP_DROP_MEMBERSHIP, IP_MULTICAST_IF, IP_MULTICAST_TTL
 * and IP_MULTICAST_LOOP, over nx_igmp_multicast_interface_join()/_leave().
 * RFC 3678 source filtering (IP_ADD_SOURCE_MEMBERSHIP and the MCAST_* family)
 * is not here and is not planned.
 *
 * IPV6_JOIN_GROUP, IPV6_LEAVE_GROUP, IPV6_MULTICAST_IF, IPV6_MULTICAST_HOPS
 * and IPV6_MULTICAST_LOOP are the second half of the file, over
 * nxd_ipv6_multicast_interface_join()/_leave().  Same table discipline, three
 * different reconciliations.  The note above that half says which.
 *
 * A v6 join registers the group's 33:33 MAC with the interface, makes the
 * stack accept datagrams sent to it, and announces it: _nx_mld_group_join()
 * sends a Multicast Listener Report at once and answers the queries that
 * follow.  That is what makes a group above link scope work at all, and it is
 * what stops a snooping switch pruning a link-scope one.  The protocol is in
 * the fork, nx_mld_*.c; port/netxduo-amiga/inc/nx_user.h has the switch.
 *
 * Three things NetX Duo keeps somewhere other than where BSD keeps them, and
 * this file is where they are reconciled:
 *
 *   Membership is per NX_IP, refcounted, and capped at
 *   NX_MAX_MULTICAST_GROUPS distinct groups.  BSD's is per socket.  A socket
 *   that closes without a drop of its groups must still leave them, so the
 *   socket-to-group mapping NetX Duo does not keep is kept here.
 *
 *   The multicast TTL is nx_udp_socket_time_to_live, one field shared with
 *   unicast.  It is written on the way into each send instead
 *   (bsd_mcast_prepare_send), so IP_TTL and IP_MULTICAST_TTL do not overwrite
 *   each other.
 *
 *   Loopback is one flag on the whole NX_IP.  It is read exactly once, by
 *   nx_igmp_multicast_interface_join_internal(), which copies it into the
 *   group's own entry, and the send path reads that copy.  A write to the
 *   global immediately before a join therefore gives per-group loopback, which
 *   is as close to per-socket as this stack gets.  Two sockets that join the
 *   same group on the same interface share the first one's setting, because
 *   the second join only increments a count and never reaches the copy.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

/*
 * How many (socket, group, interface) memberships the library tracks at once.
 * Not NX_MAX_MULTICAST_GROUPS: that caps the distinct groups the NX_IP holds.
 * Several sockets can hold the same one, and each needs its own row, so that
 * a close of one does not drop the group under the others.
 */
#define BSD_MCAST_MEMBERSHIPS   16

typedef struct BsdMcastEntry
{
    AmiSocket  *bm_Sock;        /* NULL: free row                           */
    ULONG       bm_Group;
    UINT        bm_Iface;       /* NetX interface index                     */
} BsdMcastEntry;

/*
 * One table for the machine, not one per opener: the NX_IP is the singleton
 * the memberships belong to. Every entry point below runs inside a
 * bsd_nx_enter() bracket, and that bracket holds the ThreadX scheduler lock
 * (netx_call.c), so the table needs no lock of its own, the same reasoning
 * raw.c's registry rests on.
 */
static BsdMcastEntry bsd_mcast_table[BSD_MCAST_MEMBERSHIPS];

/* ------------------------------------------------------------- addresses, */

static BOOL bsd_mcast_is_group(ULONG addr)
{
    return ((addr & 0xF0000000UL) == 0xE0000000UL) ? TRUE : FALSE;
}

/*
 * imr_interface -> NetX interface index.  INADDR_ANY means "the one the route
 * would pick", which for a multicast destination is the first interface whose
 * link is up (nx_ip_route_find.c).  The choice is made here rather than
 * deferred, so the membership and the sends stay on the same interface.
 *
 * -1 when no interface carries that address.
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

/* ------------------------------------------------------------- the table, */

static BsdMcastEntry *bsd_mcast_find(const AmiSocket *sock, ULONG group,
                                     UINT iface)
{
    UINT i;

    for (i = 0; i < BSD_MCAST_MEMBERSHIPS; i++)
    {
        BsdMcastEntry *e = &bsd_mcast_table[i];

        if (e->bm_Sock == sock && e->bm_Group == group && e->bm_Iface == iface)
            return e;
    }

    return NULL;
}

static BsdMcastEntry *bsd_mcast_free_row(VOID)
{
    UINT i;

    for (i = 0; i < BSD_MCAST_MEMBERSHIPS; i++)
    {
        if (bsd_mcast_table[i].bm_Sock == NULL)
            return &bsd_mcast_table[i];
    }

    return NULL;
}

/* ---------------------------------------------------------- join / leave, */

static LONG bsd_mcast_join(struct AmiSocketBase *base, AmiSocket *sock,
                           const struct ip_mreq *mreq)
{
    NX_IP         *ip = netstack_ip();
    BsdMcastEntry *row;
    ULONG          group;
    LONG           iface;
    UINT           status;

    if (ip == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    group = BSD_NTOHL(mreq->imr_multiaddr.s_addr);
    if (!bsd_mcast_is_group(group))
        return bsd_fail(base, AMI_EINVAL);

    iface = bsd_mcast_iface_of(ip, BSD_NTOHL(mreq->imr_interface.s_addr));
    if (iface < 0)
        return bsd_fail(base, AMI_EADDRNOTAVAIL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

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

    /* Read once, by the join, into the group's own entry. See the top. */
    if (sock->as_McastLoop != 0)
        (VOID)nx_igmp_loopback_enable(ip);
    else
        (VOID)nx_igmp_loopback_disable(ip);

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
    NX_IP         *ip = netstack_ip();
    BsdMcastEntry *row;
    ULONG          group;
    LONG           iface;

    if (ip == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    group = BSD_NTOHL(mreq->imr_multiaddr.s_addr);
    if (!bsd_mcast_is_group(group))
        return bsd_fail(base, AMI_EINVAL);

    iface = bsd_mcast_iface_of(ip, BSD_NTOHL(mreq->imr_interface.s_addr));
    if (iface < 0)
        return bsd_fail(base, AMI_EADDRNOTAVAIL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    row = bsd_mcast_find(sock, group, (UINT)iface);
    if (row == NULL)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    (VOID)nx_igmp_multicast_interface_leave(ip, group, (UINT)iface);
    row->bm_Sock = NULL;

    bsd_nx_leave(base);

    return 0;
}

#ifdef AMINETXDUO_IPV6
static VOID bsd_mcast6_close(NX_IP *ip, AmiSocket *sock);
#endif

VOID bsd_mcast_close(AmiSocket *sock)
{
    NX_IP *ip = netstack_ip();
    UINT   i;

    for (i = 0; i < BSD_MCAST_MEMBERSHIPS; i++)
    {
        BsdMcastEntry *e = &bsd_mcast_table[i];

        if (e->bm_Sock != sock)
            continue;

        if (ip != NULL)
            (VOID)nx_igmp_multicast_interface_leave(ip, e->bm_Group,
                                                    e->bm_Iface);
        e->bm_Sock = NULL;
    }

#ifdef AMINETXDUO_IPV6
    bsd_mcast6_close(ip, sock);
#endif
}

/* ------------------------------------------------------------ the sender, */

LONG bsd_mcast_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr)
{
    if (addr->nxd_ip_version != NX_IP_VERSION_V4 ||
        !bsd_mcast_is_group(addr->nxd_ip_address.v4))
    {
        /* IP_TTL / IPV6_UNICAST_HOPS, not the NetX default: as_Ttl starts at
           NX_IP_TIME_TO_LIVE and options.c and in6.c both write it. */
        sock->as_Nx.udp.nx_udp_socket_time_to_live = (UINT)(sock->as_Ttl & 0xFF);
        return -1;
    }

    sock->as_Nx.udp.nx_udp_socket_time_to_live = (UINT)sock->as_McastTtl;

    return sock->as_McastIf;
}

/* ----------------------------------------------------------- the options, */

/*
 * 4.4BSD types IP_MULTICAST_TTL and IP_MULTICAST_LOOP as u_char and everything
 * written since passes an int, so every width is taken here.  getsockopt
 * answers in whichever width the caller offered room for.  A program that
 * hands one byte and is given four writes over three bytes it does not own.
 *
 * Two bytes matters as much as one and four.  m68k is big-endian, so a caller
 * that passes a `short` of 5 hands over 0x00,0x05.  Read as a UBYTE that is
 * the high byte, 0, and IP_MULTICAST_TTL 0 keeps the datagram off the link
 * entirely.  The reply had the reverse fault: one byte written into a
 * two-byte buffer left the low half untouched, and the caller read 5 as 1280.
 */
static LONG bsd_mcast_get_byte_or_long(struct AmiSocketBase *base, APTR optval,
                                       socklen_t optlen, LONG *value)
{
    if (optval == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (optlen >= (socklen_t)sizeof(LONG))
        *value = *(LONG *)optval;
    else if (optlen >= (socklen_t)sizeof(WORD))
        *value = (LONG)*(WORD *)optval;
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
        *(LONG *)optval = value;
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (*optlen >= (socklen_t)sizeof(WORD))
    {
        *(WORD *)optval = (WORD)value;
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
            NX_IP         *ip = netstack_ip();
            LONG           iface;

            if (optval == NULL)
                return bsd_fail(base, AMI_EFAULT);
            if (optlen < (socklen_t)sizeof(struct in_addr))
                return bsd_fail(base, AMI_EINVAL);

            bsd_bcopy(optval, &in, sizeof in);

            /* INADDR_ANY puts the choice back with the route. */
            if (in.s_addr == 0UL)
            {
                sock->as_McastIf = -1;
                return 0;
            }

            iface = bsd_mcast_iface_of(ip, BSD_NTOHL(in.s_addr));
            if (iface < 0)
                return bsd_fail(base, AMI_EADDRNOTAVAIL);

            sock->as_McastIf = iface;
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
            NX_IP         *ip = netstack_ip();

            if (optval == NULL || optlen == NULL)
                return bsd_fail(base, AMI_EFAULT);
            if (*optlen < (socklen_t)sizeof(struct in_addr))
                return bsd_fail(base, AMI_EINVAL);

            in.s_addr = 0UL;
            if (sock->as_McastIf >= 0 && ip != NULL)
            {
                in.s_addr = BSD_HTONL(
                    ip->nx_ip_interface[sock->as_McastIf].nx_interface_ip_address);
            }

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

        /*
         * "IP_ADD_MEMBERSHIP ... may only be set", 4.4BSD answers EOPNOTSUPP
         * for a read of either, which is what ip_ctloutput() does for a
         * PRCO_GETOPT it has no case for.
         */
        case IP_ADD_MEMBERSHIP:
        case IP_DROP_MEMBERSHIP:
            return bsd_fail(base, AMI_EOPNOTSUPP);

        default:
            return bsd_fail(base, AMI_ENOPROTOOPT);
    }
}

/* ====================================================================== v6 ==
 *
 * RFC 3493 section 5.2.  The header at the top of this file says what a join
 * puts on the wire.  What follows is where NetX Duo keeps things and BSD does
 * not, which is a different list from the IPv4 one:
 *
 *   Membership is per NX_IP again, refcounted, capped at
 *   NX_MAX_MULTICAST_GROUPS.  Same problem, same answer: the table below is
 *   the socket-to-group mapping NetX Duo does not keep.  It is a second table
 *   rather than a widened first one so that a no-IPv6 build carries neither
 *   the bytes nor the branch.
 *
 *   The hop limit is nx_ipv6_hop_limit on the NX_IP, not on the socket, as
 *   the IPv4 TTL is.  nxd_udp_socket_send() reads it at send time, so
 *   IPV6_MULTICAST_HOPS is applied with a write, a send, and a restore.  Safe
 *   because every send runs inside a bsd_nx_enter() bracket, which holds the
 *   ThreadX scheduler lock and stops the IP thread (netx_call.c).
 *
 *   Loopback does not exist.  nx_ipv6_packet_send() loops a datagram back only
 *   when the destination is ::1 or one of this host's own addresses, and a
 *   group is neither.  IPV6_MULTICAST_LOOP is therefore accepted and reads
 *   back 0, and there is nothing to store.
 */

#ifdef AMINETXDUO_IPV6

#define BSD_MCAST6_MEMBERSHIPS  16

typedef struct BsdMcast6Entry
{
    AmiSocket  *bm_Sock;            /* NULL: free row                       */
    ULONG       bm_Group[4];
    UINT        bm_Iface;           /* NetX interface index                 */
} BsdMcast6Entry;

static BsdMcast6Entry bsd_mcast6_table[BSD_MCAST6_MEMBERSHIPS];

/* ------------------------------------------------------------- addresses, */

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
 * for a group is the first interface whose link is up, said here rather than
 * deferred, so the membership and the sends agree.
 *
 * -1 when there is no such interface.
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
 * The link-local one on that interface is the answer.  It is the source a
 * link-local group wants, and the one that always exists once the interface is
 * up.
 *
 * -1 when the interface has no usable address yet, for example while DAD runs.
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

/* ------------------------------------------------------------- the table, */

static BsdMcast6Entry *bsd_mcast6_find(const AmiSocket *sock,
                                       const ULONG group[4], UINT iface)
{
    UINT i;

    for (i = 0; i < BSD_MCAST6_MEMBERSHIPS; i++)
    {
        BsdMcast6Entry *e = &bsd_mcast6_table[i];

        if (e->bm_Sock == sock && e->bm_Iface == iface &&
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
        if (bsd_mcast6_table[i].bm_Sock == NULL)
            return &bsd_mcast6_table[i];
    }

    return NULL;
}

/* ---------------------------------------------------------- join / leave, */

static LONG bsd_mcast6_join(struct AmiSocketBase *base, AmiSocket *sock,
                            const struct ipv6_mreq *mreq)
{
    NX_IP          *ip = netstack_ip();
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

    iface = bsd_mcast6_iface_of(ip, mreq->ipv6mr_interface);
    if (iface < 0)
        return bsd_fail(base, AMI_EADDRNOTAVAIL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

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
    NX_IP          *ip = netstack_ip();
    BsdMcast6Entry *row;
    NXD_ADDRESS     group;
    LONG            iface;

    if (ip == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    group.nxd_ip_version = NX_IP_VERSION_V6;
    bsd_in6_to_words(mreq->ipv6mr_multiaddr.s6_addr, group.nxd_ip_address.v6);

    if (!bsd_mcast6_is_group(group.nxd_ip_address.v6))
        return bsd_fail(base, AMI_EINVAL);

    iface = bsd_mcast6_iface_of(ip, mreq->ipv6mr_interface);
    if (iface < 0)
        return bsd_fail(base, AMI_EADDRNOTAVAIL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    row = bsd_mcast6_find(sock, group.nxd_ip_address.v6, (UINT)iface);
    if (row == NULL)
    {
        bsd_nx_leave(base);
        return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    (VOID)nxd_ipv6_multicast_interface_leave(ip, &group, (UINT)iface);
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

        if (e->bm_Sock != sock)
            continue;

        if (ip != NULL)
        {
            group.nxd_ip_version = NX_IP_VERSION_V6;
            group.nxd_ip_address.v6[0] = e->bm_Group[0];
            group.nxd_ip_address.v6[1] = e->bm_Group[1];
            group.nxd_ip_address.v6[2] = e->bm_Group[2];
            group.nxd_ip_address.v6[3] = e->bm_Group[3];

            (VOID)nxd_ipv6_multicast_interface_leave(ip, &group, e->bm_Iface);
        }

        e->bm_Sock = NULL;
    }
}

/* ------------------------------------------------------------ the sender, */

LONG bsd_mcast6_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr,
                             ULONG *saved)
{
    NX_IP *ip = netstack_ip();
    LONG   iface;

    *saved = 0UL;

    if (addr->nxd_ip_version != NX_IP_VERSION_V6 || ip == NULL ||
        !bsd_mcast6_is_group(addr->nxd_ip_address.v6))
        return -1;

    /*
     * RFC 3493 5.2: a hop limit of 0 is "this host only". Nothing here
     * delivers a multicast datagram back to its sender, because
     * IPV6_MULTICAST_LOOP is accepted and reads back 0. Such a datagram
     * therefore goes nowhere at all, which is still not one hop onto the
     * link. This used to coerce 0 to 1, and it leaked every such datagram.
     */
    if (sock->as_Mcast6Hops == 0)
        return BSD_MCAST6_NO_LINK;

    *saved = ip->nx_ipv6_hop_limit;
    ip->nx_ipv6_hop_limit = (ULONG)sock->as_Mcast6Hops;

    iface = sock->as_Mcast6If;
    if (iface < 0)
        return -1;

    return bsd_mcast6_source_index(ip, (UINT)iface);
}

VOID bsd_mcast6_finish_send(ULONG saved)
{
    NX_IP *ip = netstack_ip();

    if (saved != 0UL && ip != NULL)
        ip->nx_ipv6_hop_limit = saved;
}

/* ----------------------------------------------------------- the options, */

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
        *value = *(LONG *)optval;
    else if (optlen >= (socklen_t)sizeof(WORD))
        *value = (LONG)*(WORD *)optval;
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
        *(LONG *)optval = value;
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (*optlen >= (socklen_t)sizeof(WORD))
    {
        *(WORD *)optval = (WORD)value;
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
            NX_IP *ip = netstack_ip();
            LONG   iface;

            if (bsd_mcast6_get_int(base, optval, optlen, &value) != 0)
                return -1;

            /* 0 puts the choice back with the route. */
            if (value == 0)
            {
                sock->as_Mcast6If = -1;
                return 0;
            }
            if (value < 0)
                return bsd_fail(base, AMI_EINVAL);

            iface = bsd_mcast6_iface_of(ip, (ULONG)value);
            if (iface < 0)
                return bsd_fail(base, AMI_ENXIO);

            sock->as_Mcast6If = iface;
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
             * keeps the datagram off the link for it. It used to be rounded
             * up to one hop, which put it on the link.
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
            /* Back in the caller's numbering, one higher, and 0 for "the
               route decides". */
            return bsd_mcast6_put_int(base, optval, optlen,
                                      (sock->as_Mcast6If < 0)
                                          ? 0
                                          : sock->as_Mcast6If + 1);

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
