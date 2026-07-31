/*
 * bsdsocket.library -- RFC 1112 IPv4 multicast membership.
 *
 * IP_ADD_MEMBERSHIP, IP_DROP_MEMBERSHIP, IP_MULTICAST_IF, IP_MULTICAST_TTL
 * and IP_MULTICAST_LOOP, over nx_igmp_multicast_interface_join()/_leave().
 * RFC 3678 source filtering (IP_ADD_SOURCE_MEMBERSHIP and the MCAST_* family)
 * is not here and is not planned.
 *
 * Three things NetX Duo keeps somewhere other than where BSD keeps them, and
 * this file is where they are reconciled:
 *
 *   Membership is per NX_IP, refcounted, and capped at
 *   NX_MAX_MULTICAST_GROUPS distinct groups.  BSD's is per socket, and a
 *   socket that closes without dropping its groups must still leave them, so
 *   the socket-to-group mapping NetX Duo does not keep is kept here.
 *
 *   The multicast TTL is nx_udp_socket_time_to_live, one field shared with
 *   unicast.  It is written on the way into each send instead
 *   (bsd_mcast_prepare_send), so IP_TTL and IP_MULTICAST_TTL do not fight
 *   over it.
 *
 *   Loopback is one flag on the whole NX_IP.  It is read exactly once, by
 *   nx_igmp_multicast_interface_join_internal(), which copies it into the
 *   group's own entry; the send path reads that copy.  So setting the global
 *   immediately before a join gives per-group loopback, which is as close to
 *   per-socket as this stack gets.  Two sockets joining the SAME group on the
 *   same interface share the first one's setting -- the second join only
 *   increments a count and never reaches the copy.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

/*
 * How many (socket, group, interface) memberships the library tracks at once.
 * Not NX_MAX_MULTICAST_GROUPS: that caps the DISTINCT groups the NX_IP will
 * hold, and several sockets may hold the same one, each needing its own row so
 * that closing one does not drop the group under the others.
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
 * (netx_call.c), so the table needs no lock of its own -- the same reasoning
 * raw.c's registry rests on.
 */
static BsdMcastEntry bsd_mcast_table[BSD_MCAST_MEMBERSHIPS];

/* ------------------------------------------------------------- addresses -- */

static BOOL bsd_mcast_is_group(ULONG addr)
{
    return ((addr & 0xF0000000UL) == 0xE0000000UL) ? TRUE : FALSE;
}

/*
 * imr_interface -> NetX interface index.  INADDR_ANY means "the one the route
 * would pick", which for a multicast destination is the first interface whose
 * link is up (nx_ip_route_find.c); saying so here rather than deferring keeps
 * the membership and the sends on the same interface.
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

/* ------------------------------------------------------------- the table -- */

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

/* ---------------------------------------------------------- join / leave -- */

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

    /* Read once, by the join, into the group's own entry. See the top. */
    if (sock->as_McastLoop != 0)
        (VOID)nx_igmp_loopback_enable(ip);
    else
        (VOID)nx_igmp_loopback_disable(ip);

    status = nx_igmp_multicast_interface_join(ip, group, (UINT)iface);

    bsd_nx_leave(base);

    if (status != NX_SUCCESS)
    {
        /* NX_NO_MORE_ENTRIES is the NX_MAX_MULTICAST_GROUPS cap, which is a
           resource shortage and not a bad argument. */
        return bsd_fail(base, (status == NX_NO_MORE_ENTRIES)
                                  ? AMI_ENOBUFS
                                  : bsd_errno_from_nx(status));
    }

    row->bm_Sock  = sock;
    row->bm_Group = group;
    row->bm_Iface = (UINT)iface;

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
}

/* ------------------------------------------------------------ the sender -- */

LONG bsd_mcast_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr)
{
    if (addr->nxd_ip_version != NX_IP_VERSION_V4 ||
        !bsd_mcast_is_group(addr->nxd_ip_address.v4))
    {
        sock->as_Nx.udp.nx_udp_socket_time_to_live = (UINT)NX_IP_TIME_TO_LIVE;
        return -1;
    }

    sock->as_Nx.udp.nx_udp_socket_time_to_live = (UINT)sock->as_McastTtl;

    return sock->as_McastIf;
}

/* ----------------------------------------------------------- the options -- */

/*
 * 4.4BSD types IP_MULTICAST_TTL and IP_MULTICAST_LOOP as u_char and everything
 * written since passes an int, so both widths are taken here and getsockopt
 * answers in whichever width the caller offered room for.  A program that
 * hands one byte and is given four writes over three bytes it does not own.
 */
static LONG bsd_mcast_get_byte_or_long(struct AmiSocketBase *base, APTR optval,
                                       socklen_t optlen, LONG *value)
{
    if (optval == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (optlen >= (socklen_t)sizeof(LONG))
        *value = *(LONG *)optval;
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
         * "IP_ADD_MEMBERSHIP ... may only be set" -- 4.4BSD answers EOPNOTSUPP
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
