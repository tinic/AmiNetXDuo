/*
 * bsdsocket.library, send/sendto/sendmsg and recv/recvfrom/recvmsg.
 *
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/budget.h"
#include "netmonitor.h"
#include "udp_queue.h"

#include "nx_ip.h"
#include "nx_ipv4.h"
#ifdef AMINETXDUO_IPV6
#include "nx_ipv6.h"
#include "../ipv6/ipv6_srcsel.h"
#endif

#include <proto/exec.h>

/* Fallback segment size, used before the socket has negotiated an MSS. */
#define BSD_DEFAULT_MSS     536

/*
 * struct msghdr / struct iovec are ABI. The definitions come from the Roadshow
 * netinclude headers in the toolchain's ndk-include (sys/socket.h, sys/uio.h),
 */
_Static_assert(sizeof(struct iovec) == 8, "iovec is not 8 bytes");
_Static_assert(offsetof(struct iovec, iov_base) == 0, "iov_base moved");
_Static_assert(offsetof(struct iovec, iov_len)  == 4, "iov_len moved");

_Static_assert(sizeof(struct msghdr) == 28, "msghdr is not the 4.4BSD shape");
_Static_assert(offsetof(struct msghdr, msg_name)       ==  0, "msg_name moved");
_Static_assert(offsetof(struct msghdr, msg_namelen)    ==  4, "msg_namelen moved");
_Static_assert(offsetof(struct msghdr, msg_iov)        ==  8, "msg_iov moved");
_Static_assert(offsetof(struct msghdr, msg_iovlen)     == 12, "msg_iovlen moved");
_Static_assert(offsetof(struct msghdr, msg_control)    == 16, "msg_control moved");
_Static_assert(offsetof(struct msghdr, msg_controllen) == 20, "msg_controllen moved");
_Static_assert(offsetof(struct msghdr, msg_flags)      == 24, "msg_flags moved");

/*
 * A position in a scatter/gather list: which entry, and how far into it.
 * Empty entries are skipped, so bsd_iov_chunk() never reports a zero-length
 * run except at the end of the list.
 */
typedef struct BsdIovCursor
{
    const struct iovec *ic_Vec;
    LONG                ic_Count;
    LONG                ic_Index;
    ULONG               ic_Offset;
} BsdIovCursor;

static VOID bsd_iov_init(BsdIovCursor *cur, const struct iovec *iov, LONG count)
{
    cur->ic_Vec    = iov;
    cur->ic_Count  = count;
    cur->ic_Index  = 0;
    cur->ic_Offset = 0;
}

/*
 * Total byte count, or -1 if the list is malformed. LONG is the ABI's return
 * type for send()/recv(), so a list whose total does not fit in a positive
 * LONG cannot be reported and is rejected rather than truncated.
 */
static LONG bsd_iov_total(const struct iovec *iov, LONG count)
{
    ULONG total = 0;
    LONG  i;

    if (count < 0)
        return -1;

    if (count > 0 && iov == NULL)
        return -1;

    for (i = 0; i < count; i++)
    {
        ULONG len = (ULONG)iov[i].iov_len;

        if (len == 0)
            continue;

        if (iov[i].iov_base == NULL)
            return -1;

        if (len > 0x7FFFFFFFUL || total > 0x7FFFFFFFUL - len)
            return -1;

        total += len;
    }

    return (LONG)total;
}

/* The contiguous run at the cursor. Returns 0 when the list is exhausted. */
static ULONG bsd_iov_chunk(BsdIovCursor *cur, UBYTE **ptr)
{
    while (cur->ic_Index < cur->ic_Count)
    {
        const struct iovec *v   = &cur->ic_Vec[cur->ic_Index];
        ULONG               len = (ULONG)v->iov_len;

        if (cur->ic_Offset < len)
        {
            *ptr = (UBYTE *)v->iov_base + cur->ic_Offset;
            return len - cur->ic_Offset;
        }

        cur->ic_Index++;
        cur->ic_Offset = 0;
    }

    *ptr = NULL;

    return 0;
}

static VOID bsd_iov_advance(BsdIovCursor *cur, ULONG bytes)
{
    while (bytes > 0 && cur->ic_Index < cur->ic_Count)
    {
        ULONG len   = (ULONG)cur->ic_Vec[cur->ic_Index].iov_len;
        ULONG avail = (cur->ic_Offset < len) ? len - cur->ic_Offset : 0;

        if (avail == 0)
        {
            cur->ic_Index++;
            cur->ic_Offset = 0;
            continue;
        }

        if (bytes < avail)
        {
            cur->ic_Offset += bytes;
            return;
        }

        bytes -= avail;
        cur->ic_Index++;
        cur->ic_Offset = 0;
    }
}

static ULONG bsd_packet_len(NX_PACKET *packet)
{
    ULONG length = 0;

    if (packet != NULL)
        nx_packet_length_get(packet, &length);

    return length;
}

static VOID bsd_drop_pending(AmiSocket *sock)
{
    if (sock->as_RxPending != NULL)
    {
        nx_packet_release(sock->as_RxPending);
        sock->as_RxPending = NULL;
    }
    sock->as_RxOffset = 0;
}

/*
 * Append up to `want` bytes from the cursor onto `packet`, coalescing across
 * iovec boundaries. Returns the number appended, or -1 on failure.
 */
static LONG bsd_packet_append_iov(NX_PACKET *packet, BsdIovCursor *cur,
                                  ULONG want, NX_PACKET_POOL *pool, ULONG wait,
                                  UINT *why)
{
    ULONG done = 0;

    while (done < want)
    {
        UBYTE *src   = NULL;
        ULONG  chunk = bsd_iov_chunk(cur, &src);
        UINT   status;

        if (chunk == 0)
            break;

        if (chunk > want - done)
            chunk = want - done;

        status = nx_packet_data_append(packet, src, chunk, pool, wait);
        if (status != NX_SUCCESS)
        {
            if (why != NULL)
                *why = status;

            return (done > 0) ? (LONG)done : -1;
        }

        bsd_iov_advance(cur, chunk);
        done += chunk;
    }

    return (LONG)done;
}

typedef struct
{
    NX_PACKET_POOL *pool;
    NX_PACKET      **packet;
} BsdAllocArgs;

UINT bsd_alloc_once(VOID *arg, ULONG wait)
{
    BsdAllocArgs *a = (BsdAllocArgs *)arg;

    return nx_packet_allocate(a->pool, a->packet, NX_TCP_PACKET, wait);
}

typedef struct
{
    NX_TCP_SOCKET *tcp;
    NX_PACKET     *packet;
} BsdSendArgs;

UINT bsd_send_once(VOID *arg, ULONG wait)
{
    BsdSendArgs *a = (BsdSendArgs *)arg;

    return nx_tcp_socket_send(a->tcp, a->packet, wait);
}

/*
 * How much of the packet nx_tcp_socket_send() took before it failed.
 */
static LONG bsd_send_consumed(NX_PACKET *packet, LONG filled)
{
    LONG left = (LONG)bsd_packet_len(packet);

    if (left < 0 || left >= filled)
        return 0;

    return filled - left;
}

static LONG bsd_send_tcp(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags)
{
    NX_PACKET_POOL *pool = netstack_pool();
    ULONG           mss  = 0;
    LONG            sent = 0;
    ULONG           wait;
    UINT            why  = NX_NO_PACKET;

    if (pool == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(base, AMI_ENOTCONN);

    if ((sock->as_Flags & ASF_WRSHUT) != 0)
        return bsd_fail(base, AMI_EPIPE);

    nx_tcp_socket_mss_get(&sock->as_Nx.tcp, &mss);
    if (mss == 0)
        mss = BSD_DEFAULT_MSS;

    wait = bsd_wait_option(sock, sock->as_SndTimeout, flags);

    while (sent < len)
    {
        NX_PACKET *packet = NX_NULL;
        ULONG      chunk  = (ULONG)(len - sent);
        LONG       filled;
        UINT       status;

        if (chunk > mss)
            chunk = mss;

        {
            BsdAllocArgs aargs;
            BOOL         aborted;

            aargs.pool   = pool;
            aargs.packet = &packet;

            status = bsd_wait_sliced(base, wait, bsd_alloc_once, &aargs,
                                     &aborted);
            if (aborted)
            {
                if (sent > 0)
                    return sent;        /* short write, as BSD allows */
                return bsd_fail(base, AMI_EINTR);
            }
        }
        if (status != NX_SUCCESS)
        {
            why = status;
            break;
        }

        filled = bsd_packet_append_iov(packet, cur, chunk, pool, wait, &why);
        if (filled <= 0)
        {
            nx_packet_release(packet);
            break;
        }

        {
            BsdSendArgs sargs;
            BOOL        aborted;

            sargs.tcp    = &sock->as_Nx.tcp;
            sargs.packet = packet;

            status = bsd_wait_sliced(base, wait, bsd_send_once, &sargs,
                                     &aborted);
            if (aborted)
            {
                sent += bsd_send_consumed(packet, filled);
                nx_packet_release(packet);
                if (sent > 0)
                    return sent;        /* short write, as BSD allows */
                return bsd_fail(base, AMI_EINTR);
            }
        }
        if (status != NX_SUCCESS)
        {
            sent += bsd_send_consumed(packet, filled);
            nx_packet_release(packet);

            if (sent > 0)
                return sent;            /* short write, as BSD allows */

            if (status == NX_NOT_CONNECTED)
            {
                sock->as_Flags &= ~ASF_CONNECTED;
                return bsd_fail(base, AMI_EPIPE);
            }

            return bsd_fail(base, bsd_wait_errno(wait, status));
        }

        sent += filled;

        if ((sock->as_Flags & ASF_NONBLOCK) != 0)
            wait = NX_NO_WAIT;
    }

    if (sent == 0 && len > 0)
        return bsd_fail(base, bsd_wait_errno(wait, why));

    return sent;
}

LONG bsd_route_mtu(NX_IP *ip, const NXD_ADDRESS *addr,
                   const NX_INTERFACE *source_interface)
{
    NX_INTERFACE *iface = NX_NULL;

    if (ip == NULL || addr == NULL)
        return -1;

    if (source_interface != NX_NULL)
        return (LONG)source_interface->nx_interface_ip_mtu_size;

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
    {
        NXD_IPV6_ADDRESS *source = NX_NULL;

        tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
        if (_nxd_ipv6_interface_find(ip, (ULONG *)addr->nxd_ip_address.v6,
                                     &source, NX_NULL) == NX_SUCCESS &&
            source != NX_NULL)
        {
            iface = source->nxd_ipv6_address_attached;
        }
        tx_mutex_put(&ip->nx_ip_protection);
    }
    else
#endif
    {
        ULONG next_hop = 0;

        tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
        (VOID)_nx_ip_route_find(ip, addr->nxd_ip_address.v4, &iface, &next_hop);
        tx_mutex_put(&ip->nx_ip_protection);
    }

    if (iface == NX_NULL)
        return -1;

    return (LONG)iface->nx_interface_ip_mtu_size;
}

static LONG bsd_udp_maxdgram(NX_IP *ip, const NXD_ADDRESS *addr,
                            const NX_INTERFACE *source_interface)
{
    LONG  mtu = bsd_route_mtu(ip, addr, source_interface);
    ULONG overhead;

#ifdef AMINETXDUO_IPV6
    if (addr != NULL && addr->nxd_ip_version == NX_IP_VERSION_V6)
        overhead = (ULONG)NX_IPv6_UDP_PACKET - (ULONG)NX_PHYSICAL_HEADER;
    else
#endif
        overhead = (ULONG)NX_IPv4_UDP_PACKET - (ULONG)NX_PHYSICAL_HEADER;

    if (mtu < 0 || (ULONG)mtu <= overhead)
        return -1;

    return (LONG)((ULONG)mtu - overhead);
}

/*
 * The interface a received datagram arrived on.
 */
static const NX_INTERFACE *bsd_packet_interface(const NX_PACKET *packet)
{
#ifdef AMINETXDUO_IPV6
    if (packet->nx_packet_ip_version == NX_IP_VERSION_V6)
    {
        const NXD_IPV6_ADDRESS *a =
            packet->nx_packet_address.nx_packet_ipv6_address_ptr;

        return (a != NX_NULL) ? a->nxd_ipv6_address_attached : NX_NULL;
    }
#endif

    return packet->nx_packet_ip_interface;
}

/*
 * Does this datagram belong to the local address the socket named?
 */
static BOOL bsd_udp_to_local(const AmiSocket *sock, const NX_PACKET *packet)
{
    const UBYTE *header;
    ULONG        available;

    if (packet == NX_NULL ||
        !bsd_bind_wants_interface(sock, bsd_packet_interface(packet)))
        return FALSE;

    if (sock->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V4 &&
        sock->as_LocalAddr.nxd_ip_address.v4 == 0UL)
        return TRUE;

#ifdef AMINETXDUO_IPV6
    if (sock->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V6 &&
        (sock->as_LocalAddr.nxd_ip_address.v6[0] |
         sock->as_LocalAddr.nxd_ip_address.v6[1] |
         sock->as_LocalAddr.nxd_ip_address.v6[2] |
         sock->as_LocalAddr.nxd_ip_address.v6[3]) == 0UL)
        return TRUE;
#endif

    if (packet->nx_packet_ip_header == NX_NULL)
        return FALSE;

    header = (const UBYTE *)packet->nx_packet_ip_header;
    if (packet->nx_packet_append_ptr < header)
        return FALSE;

    available = (ULONG)(packet->nx_packet_append_ptr - header);

#ifdef AMINETXDUO_IPV6
    if (packet->nx_packet_ip_version == NX_IP_VERSION_V6)
    {
        ULONG destination[4];

        if (sock->as_LocalAddr.nxd_ip_version != NX_IP_VERSION_V6 ||
            available < 40UL)
            return FALSE;

        bsd_in6_to_words(&header[24], destination);

        return (destination[0] == sock->as_LocalAddr.nxd_ip_address.v6[0] &&
                destination[1] == sock->as_LocalAddr.nxd_ip_address.v6[1] &&
                destination[2] == sock->as_LocalAddr.nxd_ip_address.v6[2] &&
                destination[3] == sock->as_LocalAddr.nxd_ip_address.v6[3])
                   ? TRUE : FALSE;
    }
#endif

    if (packet->nx_packet_ip_version == NX_IP_VERSION_V4 && available >= 20UL)
    {
        ULONG destination = ((ULONG)header[16] << 24) |
                            ((ULONG)header[17] << 16) |
                            ((ULONG)header[18] <<  8) |
                             (ULONG)header[19];

        return (sock->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V4 &&
                destination == sock->as_LocalAddr.nxd_ip_address.v4)
                   ? TRUE : FALSE;
    }

    return FALSE;
}

static ULONG bsd_packet_scope_id(const NX_PACKET *packet,
                                 const NXD_ADDRESS *source)
{
#ifdef AMINETXDUO_IPV6
    const NX_INTERFACE *nxif;

    if (packet == NX_NULL || source == NX_NULL ||
        source->nxd_ip_version != NX_IP_VERSION_V6 ||
        anx6_scope(source->nxd_ip_address.v6) >= 0xEU ||
        (source->nxd_ip_address.v6[0] == 0UL &&
         source->nxd_ip_address.v6[1] == 0UL &&
         source->nxd_ip_address.v6[2] == 0UL &&
         source->nxd_ip_address.v6[3] == 1UL))
        return 0UL;

    nxif = bsd_packet_interface(packet);

    if (nxif != NX_NULL &&
        (UINT)nxif->nx_interface_index < (UINT)NX_MAX_IP_INTERFACES)
        return (ULONG)nxif->nx_interface_index + 1UL;
#else
    (VOID)packet;
    (VOID)source;
#endif

    return 0UL;
}

/*
 * Is this datagram from the peer a connected socket named?
 */
BOOL bsd_udp_from_peer(const AmiSocket *sock, const NXD_ADDRESS *src,
                       UINT src_port, ULONG src_scope)
{
    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return TRUE;

    if (src_port != sock->as_PeerPort)
        return FALSE;

    if (src->nxd_ip_version != sock->as_PeerAddr.nxd_ip_version)
        return FALSE;

#ifdef AMINETXDUO_IPV6
    if (src->nxd_ip_version == NX_IP_VERSION_V6)
    {
        if (src->nxd_ip_address.v6[0] !=
                sock->as_PeerAddr.nxd_ip_address.v6[0] ||
            src->nxd_ip_address.v6[1] !=
                sock->as_PeerAddr.nxd_ip_address.v6[1] ||
            src->nxd_ip_address.v6[2] !=
                sock->as_PeerAddr.nxd_ip_address.v6[2] ||
            src->nxd_ip_address.v6[3] !=
                sock->as_PeerAddr.nxd_ip_address.v6[3])
            return FALSE;

        /* The 128 address bits do not identify a non-global peer.  A connect
           that supplied a zone accepts replies only from that same zone. */
        if (sock->as_PeerScopeId != 0UL &&
            anx6_scope(src->nxd_ip_address.v6) < 0xEU &&
            !(src->nxd_ip_address.v6[0] == 0UL &&
              src->nxd_ip_address.v6[1] == 0UL &&
              src->nxd_ip_address.v6[2] == 0UL &&
              src->nxd_ip_address.v6[3] == 1UL))
            return (src_scope == sock->as_PeerScopeId) ? TRUE : FALSE;

        return TRUE;
    }
#endif

    (VOID)src_scope;

    return (BOOL)(src->nxd_ip_address.v4 ==
                  sock->as_PeerAddr.nxd_ip_address.v4);
}

/* Shared by recv() and WaitSelect(): readiness must promise that recv() can
   return this packet, not merely that NetX queued something on the port. */
BOOL bsd_udp_accepts_packet(const AmiSocket *sock, const NX_PACKET *packet)
{
    NXD_ADDRESS source;
    ULONG       scope;
    UINT        port = 0;

    if (!bsd_udp_to_local(sock, packet))
        return FALSE;

    if (nxd_udp_source_extract((NX_PACKET *)packet, &source, &port) !=
        NX_SUCCESS)
        return FALSE;

    if (bsd_udp_queue_info(packet, &port, NX_NULL) != NX_SUCCESS)
        return FALSE;

    scope = bsd_packet_scope_id(packet, &source);

    return bsd_udp_from_peer(sock, &source, port, scope);
}

BOOL bsd_udp_accepts_received_packet(const AmiSocket *sock,
                                     const NX_PACKET *packet)
{
    NXD_ADDRESS source;
    ULONG       scope;
    UINT        port = 0;

    if (!bsd_udp_to_local(sock, packet))
        return FALSE;

    if (nxd_udp_source_extract((NX_PACKET *)packet, &source, &port) !=
        NX_SUCCESS)
        return FALSE;

    scope = bsd_packet_scope_id(packet, &source);

    return bsd_udp_from_peer(sock, &source, port, scope);
}

/* Size of the next datagram recv() can actually return.  NetX's own query
   reports the queue head, which may be an endpoint mismatch this layer will
   discard. */
ULONG bsd_udp_available(const AmiSocket *sock)
{
    NX_PACKET *packet = sock->as_Nx.udp.nx_udp_socket_receive_head;

    while (packet != NX_NULL)
    {
        if (bsd_udp_accepts_packet(sock, packet))
        {
            ULONG length = 0;

            if (bsd_udp_queue_info(packet, NX_NULL, &length) == NX_SUCCESS)
                return length;

            return 0;
        }

        packet = packet->nx_packet_queue_next;
    }

    return 0;
}

static LONG bsd_send_udp(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags,
                         const NXD_ADDRESS *addr, UINT port, ULONG scope,
                         const BsdCmsgSource *src)
{
    NX_PACKET_POOL *pool   = netstack_pool();
    NX_IP          *ip     = netstack_ip();
    NX_PACKET      *packet = NX_NULL;
    BsdSourceKind   source;
    UINT            source_index = 0;
    NX_INTERFACE   *source_interface = NX_NULL;
    LONG            maxdgram;
    ULONG           wait;
    LONG            filled;
    UINT            status;
#ifdef AMINETXDUO_MULTICAST
    LONG            mcast_if;
#ifdef AMINETXDUO_IPV6
    LONG            mcast6_src;
    ULONG           mcast6_hops = 0UL;
#endif
#endif

    if (pool == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    if (port == 0)
        return bsd_fail(base, AMI_EDESTADDRREQ);

    /*
     * Which address this leaves from, one an RFC 3542 PKTINFO named, the
     * bound one, the one the zone names, or NetX's pick. Settled before the
     * packet is allocated so a send that cannot be honoured costs nothing and
     * sends nothing.
     */
    if (src != NULL && src->cs_Have)
    {
        LONG index = bsd_cmsg_source_index(
                         ip, src,
                         (BOOL)(addr->nxd_ip_version == NX_IP_VERSION_V6));

        if (index < 0)
            return bsd_fail(base, AMI_EADDRNOTAVAIL);

        source       = BSD_SOURCE_INDEX;
        source_index = (UINT)index;
    }
    else
    {
        source = bsd_source_select(sock, addr, scope, &source_index);
        if (source == BSD_SOURCE_REFUSE)
            return bsd_fail(base, AMI_EADDRNOTAVAIL);
        if (source == BSD_SOURCE_UNREACH)
            return bsd_fail(base, AMI_ENETUNREACH);
    }

    if (source == BSD_SOURCE_INDEX)
    {
#ifdef AMINETXDUO_IPV6
        if (addr->nxd_ip_version == NX_IP_VERSION_V6)
            source_interface = ip->nx_ipv6_address[source_index]
                                     .nxd_ipv6_address_attached;
        else
#endif
            source_interface = &ip->nx_ip_interface[source_index];
    }

#ifdef AMINETXDUO_MULTICAST
    if ((src == NULL || !src->cs_Have) &&
        addr->nxd_ip_version == NX_IP_VERSION_V4 &&
        (addr->nxd_ip_address.v4 & 0xF0000000UL) == 0xE0000000UL &&
        sock->as_McastIf >= 0)
    {
        source_interface = &ip->nx_ip_interface[sock->as_McastIf];
    }
#ifdef AMINETXDUO_IPV6
    else if ((src == NULL || !src->cs_Have) &&
             addr->nxd_ip_version == NX_IP_VERSION_V6 &&
             (addr->nxd_ip_address.v6[0] & 0xFF000000UL) == 0xFF000000UL &&
             sock->as_Mcast6If >= 0)
    {
        source_interface = &ip->nx_ip_interface[sock->as_Mcast6If];
    }
#endif
#endif

    maxdgram = bsd_udp_maxdgram(ip, addr, source_interface);
    if (maxdgram >= 0 && len > maxdgram)
        return bsd_fail(base, AMI_EMSGSIZE);

    if ((sock->as_Flags & ASF_NXBOUND) == 0)
    {
        status = nx_udp_socket_bind(&sock->as_Nx.udp, NX_ANY_PORT, NX_NO_WAIT);
        if (status != NX_SUCCESS)
            return bsd_fail(base, bsd_errno_from_nx(status));

        nx_udp_socket_port_get(&sock->as_Nx.udp, &sock->as_LocalPort);
        sock->as_Flags |= ASF_NXBOUND | ASF_BOUND;
    }

#ifdef AMINETXDUO_MULTICAST
    mcast_if = bsd_mcast_prepare_send(sock, addr);
#ifdef AMINETXDUO_IPV6
    mcast6_src = bsd_mcast6_prepare_send(sock, addr, &mcast6_hops);

    /*
     * IPV6_MULTICAST_HOPS is 0 for this group: RFC 3493 5.2 makes that "this
     * host only", and there is nothing here to deliver it to. So it is
     */
    if (mcast6_src == BSD_MCAST6_NO_LINK)
    {
        bsd_mcast6_finish_send(mcast6_hops);
        return len;
    }
#endif
#else
    sock->as_Nx.udp.nx_udp_socket_time_to_live = (UINT)(sock->as_Ttl & 0xFF);
#endif

    /* RFC 3542 6.3: this datagram's hop limit, over the socket's own. */
    if (src != NULL && src->cs_HaveHops)
        sock->as_Nx.udp.nx_udp_socket_time_to_live = (UINT)src->cs_Hops;

    wait = bsd_wait_option(sock, sock->as_SndTimeout, flags);

    status = nx_packet_allocate(pool, &packet, NX_UDP_PACKET, wait);
    if (status != NX_SUCCESS)
    {
#if defined(AMINETXDUO_MULTICAST) && defined(AMINETXDUO_IPV6)
        bsd_mcast6_finish_send(mcast6_hops);
#endif
        return bsd_fail(base, bsd_wait_errno(wait, status));
    }

    /* A zero-length datagram is legal and is sent as an empty packet. */
    filled = (len > 0)
                 ? bsd_packet_append_iov(packet, cur, (ULONG)len, pool, wait,
                                         NULL)
                 : 0;
    if (filled < len)
    {
        nx_packet_release(packet);
#if defined(AMINETXDUO_MULTICAST) && defined(AMINETXDUO_IPV6)
        bsd_mcast6_finish_send(mcast6_hops);
#endif
        return bsd_fail(base, AMI_ENOBUFS);
    }

    if (src != NULL && src->cs_Have && source == BSD_SOURCE_INDEX)
    {
        status = nxd_udp_socket_source_send(&sock->as_Nx.udp, packet,
                                            (NXD_ADDRESS *)addr, port,
                                            source_index);
    }
    else
#ifdef AMINETXDUO_MULTICAST
    if (mcast_if >= 0)
    {
        status = nx_udp_socket_source_send(&sock->as_Nx.udp, packet,
                                           addr->nxd_ip_address.v4, port,
                                           (UINT)mcast_if);
    }
    else
#ifdef AMINETXDUO_IPV6
    if (mcast6_src >= 0)
    {
        status = nxd_udp_socket_source_send(&sock->as_Nx.udp, packet,
                                            (NXD_ADDRESS *)addr, port,
                                            (UINT)mcast6_src);
    }
    else
#endif
#endif
    if (source == BSD_SOURCE_INDEX)
    {
        status = nxd_udp_socket_source_send(&sock->as_Nx.udp, packet,
                                            (NXD_ADDRESS *)addr, port,
                                            source_index);
    }
    else
    {
        status = nxd_udp_socket_send(&sock->as_Nx.udp, packet,
                                     (NXD_ADDRESS *)addr, port);
    }

#if defined(AMINETXDUO_MULTICAST) && defined(AMINETXDUO_IPV6)
    bsd_mcast6_finish_send(mcast6_hops);
#endif

    if (status != NX_SUCCESS)
    {
        nx_packet_release(packet);
        return bsd_fail(base, bsd_errno_from_nx(status));
    }

    return len;
}

/*
 * One raw datagram out. The caller writes the protocol payload, for example
 * an ICMP message, and nxd_ip_raw_packet_send() prepends the IP header from
 * the socket's protocol, TTL and TOS. No partial send: a datagram goes whole
 * or not at all.
 */
static LONG bsd_send_raw(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags,
                         const NXD_ADDRESS *addr, ULONG scope,
                         const BsdCmsgSource *src)
{
    NX_PACKET_POOL *pool   = netstack_pool();
    NX_PACKET      *packet = NX_NULL;
    ULONG           wait;
    LONG            filled;
    UINT            status;

    if (pool == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
    {
        if ((sock->as_Flags & ASF_INET6) == 0)
            return bsd_fail(base, AMI_EAFNOSUPPORT);
    }
    else
#endif
    if (addr->nxd_ip_version != NX_IP_VERSION_V4 ||
        addr->nxd_ip_address.v4 == 0)
        return bsd_fail(base, AMI_EDESTADDRREQ);

    wait = bsd_wait_option(sock, sock->as_SndTimeout, flags);

    status = nx_packet_allocate(pool, &packet, NX_IP_PACKET, wait);
    if (status != NX_SUCCESS)
        return bsd_fail(base, bsd_wait_errno(wait, status));

    filled = (len > 0)
                 ? bsd_packet_append_iov(packet, cur, (ULONG)len, pool, wait,
                                         NULL)
                 : 0;
    if (filled < len)
    {
        nx_packet_release(packet);
        return bsd_fail(base, AMI_EMSGSIZE);
    }

    /* Consumes the packet either way. */
    if (bsd_raw_send_packet(base, sock, packet, addr, scope, src) != 0)
        return -1;

    return len;
}

typedef struct
{
    NX_TCP_SOCKET *tcp;
    NX_PACKET     **packet;
} BsdRecvArgs;

/* Not static, deliberately.  A tail call to a local symbol in another
   -ffunction-sections section of the same object is the relocation this
   toolchain mis-resolves (cmake/check-pcrel-branches.cmake, RESEARCH 25).
   As a global it relocates with a zero addend and comes out right. */
UINT bsd_recv_once(VOID *arg, ULONG wait)
{
    BsdRecvArgs *a = (BsdRecvArgs *)arg;

    return nx_tcp_socket_receive(a->tcp, a->packet, wait);
}

typedef struct
{
    NX_UDP_SOCKET *udp;
    NX_PACKET     **packet;
} BsdRecvUdpArgs;

UINT bsd_recv_udp_once(VOID *arg, ULONG wait)
{
    BsdRecvUdpArgs *a = (BsdRecvUdpArgs *)arg;

    return nx_udp_socket_receive(a->udp, a->packet, wait);
}

typedef struct
{
    AmiSocket  *sock;
    NX_PACKET **packet;
    UINT        why;
} BsdRecvRawArgs;

UINT bsd_recv_raw_once(VOID *arg, ULONG wait)
{
    BsdRecvRawArgs *a = (BsdRecvRawArgs *)arg;
    NX_PACKET      *packet;
    UINT            why = NX_NO_PACKET;

    packet      = bsd_raw_receive(a->sock, wait, &why);
    *a->packet  = packet;
    a->why      = why;

    return (packet != NX_NULL) ? NX_SUCCESS : why;
}


static BOOL bsd_nx_need(struct AmiSocketBase *base, BOOL *held)
{
    if (*held)
        return TRUE;

    if (bsd_nx_enter(base) != 0)
        return FALSE;

    *held = TRUE;

    return TRUE;
}

/*
 * Can this stream read be answered out of the parked packet alone?
 */
static BOOL bsd_recv_parked(AmiSocket *sock, LONG len)
{
    ULONG length;

    if ((sock->as_Flags & (ASF_TCP | ASF_RAW)) != ASF_TCP)
        return FALSE;

    if ((sock->as_Flags & ASF_RDSHUT) != 0)
        return FALSE;

    if (sock->as_RxPending == NULL)
        return FALSE;

    length = bsd_packet_len(sock->as_RxPending);
    if (length <= sock->as_RxOffset)
        return FALSE;               /* drained, the next call releases it */

    return (BOOL)((length - sock->as_RxOffset) > (ULONG)len);
}

#ifdef AMINETXDUO_RX_DIRECT_COMPLETE

/*
 * The caller half: publish the buffer, park in a plain Wait() on the base's
 * event signal, and return what the IP thread copied -- without re-entering
 * NetX Duo on the way out.  Everything that is not the plain blocking stream
 * Entered with the bracket held.  On the handled returns the bracket may or
 * may not still be held; *held says which, and bsd_recv_iov() leaves only
 * what is held.
 */
static LONG bsd_recv_direct(struct AmiSocketBase *base, AmiSocket *sock,
                            BsdIovCursor *cur, LONG len, BOOL *held,
                            BOOL *handled)
{
    UBYTE *dst   = NULL;
    ULONG  chunk = bsd_iov_chunk(cur, &dst);
    ULONG  received;

    *handled = FALSE;

    if (dst == NULL || chunk < (ULONG)len)
        return 0;

    /* The completer signals the opening task; only that task can wait for
       it.  And the Wait() below runs outside the bracket, so this caller
       must be the one that holds it, not a vector nested inside another. */
    if (FindTask(NULL) != base->sb_Task || base->sb_NxNest != 1)
        return 0;

    SetSignal(0UL, base->sb_EventSigMask);

    sock->as_RxDDst    = dst;
    sock->as_RxDWant   = (ULONG)len;
    sock->as_RxDFilled = 0;
    sock->as_RxDStatus = NX_NO_PACKET;
    sock->as_RxDState  = BSD_RXD_ARMED;

    bsd_rxdirect_pump(sock, TRUE);

    if (sock->as_RxDState == BSD_RXD_ARMED &&
        (sock->as_RxDStatus != NX_NO_PACKET || sock->as_RxPending != NULL))
    {
        sock->as_RxDState = BSD_RXD_IDLE;
        return 0;
    }

    if (sock->as_RxDState == BSD_RXD_ARMED)
    {
        /* Park.  The completer runs on the IP thread, which cannot run
           while this task holds the bracket -- so drop it first. */
        bsd_nx_leave(base);
        *held = FALSE;

        received = Wait(base->sb_EventSigMask | base->sb_BreakMask);

        if ((received & base->sb_BreakMask) != 0)
        {
            Signal(base->sb_Task, received & base->sb_BreakMask);

            if (!bsd_nx_need(base, held))
            {
                Forbid();
                sock->as_RxDState = BSD_RXD_IDLE;
                Permit();

                *handled = TRUE;
                return bsd_fail(base, AMI_EINTR);
            }

            if (sock->as_RxDState == BSD_RXD_ARMED)
            {
                sock->as_RxDState = BSD_RXD_IDLE;

                *handled = TRUE;
                return bsd_fail(base, AMI_EINTR);
            }

        }
        else if (sock->as_RxDState != BSD_RXD_DONE)
        {
            if (!bsd_nx_need(base, held))
            {
                Forbid();
                sock->as_RxDState = BSD_RXD_IDLE;
                Permit();

                *handled = TRUE;
                return bsd_fail(base, AMI_ENETDOWN);
            }

            if (sock->as_RxDState == BSD_RXD_ARMED)
            {
                sock->as_RxDState = BSD_RXD_IDLE;
                return 0;
            }
        }
    }

    sock->as_RxDState = BSD_RXD_IDLE;

    bsd_iov_advance(cur, sock->as_RxDFilled);

    *handled = TRUE;

    return (LONG)sock->as_RxDFilled;
}

#endif /* AMINETXDUO_RX_DIRECT_COMPLETE */

static LONG bsd_recv_tcp(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags, BOOL *held)
{
    LONG  copied = 0;
    ULONG wait   = bsd_wait_option(sock, sock->as_RcvTimeout, flags);
    BOOL  peek   = ((flags & MSG_PEEK) != 0);
    BOOL  first  = TRUE;

    if ((sock->as_Flags & ASF_RDSHUT) != 0)
        return 0;

    while (copied < len)
    {
        ULONG  length, avail, want, moved, chunk;
        UBYTE *dst = NULL;
        UINT   status;

        if (sock->as_RxPending == NULL)
        {
            NX_PACKET *packet = NX_NULL;

            ULONG now = (first || (flags & MSG_WAITALL) != 0) ? wait
                                                              : NX_NO_WAIT;

#ifdef AMINETXDUO_RX_DIRECT_COMPLETE
            if (now == NX_WAIT_FOREVER && copied == 0 && len > 0 &&
                (flags & (MSG_PEEK | MSG_WAITALL | MSG_OOB |
                          MSG_DONTWAIT)) == 0 &&
                (sock->as_Flags & (ASF_CONNECTED | ASF_EOF |
                                   ASF_LISTENING)) == ASF_CONNECTED)
            {
                BOOL handled = FALSE;
                LONG direct;

                if (!bsd_nx_need(base, held))
                    return bsd_fail(base, AMI_ENETDOWN);

                direct = bsd_recv_direct(base, sock, cur, len, held,
                                         &handled);
                if (handled)
                    return direct;
            }
#endif

            {
                BsdRecvArgs args;
                BOOL        aborted;

                /* nx_tcp_socket_receive() is THREADS_ONLY. */
                if (!bsd_nx_need(base, held))
                {
                    if (copied > 0)
                        break;
                    return bsd_fail(base, AMI_ENETDOWN);
                }

                args.tcp    = &sock->as_Nx.tcp;
                args.packet = &packet;

                status = bsd_wait_sliced(base, now, bsd_recv_once, &args,
                                         &aborted);
                if (aborted)
                {
                    if (copied > 0)
                        break;
                    return bsd_fail(base, AMI_EINTR);
                }
            }

            if (status == NX_SUCCESS)
            {
#ifdef AMINETXDUO_RXPROBE
                ami_budget_fetch(ami_budget_clock());
                ami_budget_rx_fallback();
#endif
                sock->as_RxPending = packet;
                sock->as_RxOffset  = 0;
            }
            else
            {
                if (copied > 0)
                    break;                      /* return what we have */

                if (status == NX_NOT_CONNECTED)
                {
                    /* The peer closed. A socket that was connected reports
                       end-of-file. One that never was reports ENOTCONN. */
                    if ((sock->as_Flags & (ASF_CONNECTED | ASF_EOF)) != 0)
                    {
                        sock->as_Flags |= ASF_EOF;
                        return 0;
                    }

                    return bsd_fail(base, AMI_ENOTCONN);
                }

                return bsd_fail(base, bsd_wait_errno(now, status));
            }
        }

        length = bsd_packet_len(sock->as_RxPending);
        if (length <= sock->as_RxOffset)
        {
            /* nx_packet_release() hands the packet back to the shared pool and
               can resume a thread suspended on it, so it stays inside. */
            if (!bsd_nx_need(base, held))
                break;
            bsd_drop_pending(sock);
            continue;
        }

        chunk = bsd_iov_chunk(cur, &dst);
        if (chunk == 0)
            break;                              /* caller's buffers are full */

        avail = length - sock->as_RxOffset;
        want  = (ULONG)(len - copied);
        if (want > avail)
            want = avail;
        if (want > chunk)
            want = chunk;

        moved  = 0;
        status = nx_packet_data_extract_offset(sock->as_RxPending,
                                               sock->as_RxOffset,
                                               dst, want, &moved);
        if (status != NX_SUCCESS || moved == 0)
        {
            if (copied > 0)
                break;
            return bsd_fail(base, bsd_errno_from_nx(status));
        }

        copied += (LONG)moved;
        bsd_iov_advance(cur, moved);

        if (peek)
            break;                      /* leave the packet where it is */

        sock->as_RxOffset += moved;
        if (sock->as_RxOffset >= length && bsd_nx_need(base, held))
            bsd_drop_pending(sock);     /* else leave it parked and drained,
                                           the next call releases it */

        first = FALSE;
    }

    return copied;
}

/*
 * One datagram, scattered across the caller's buffers. `truncated` reports
 * whether the datagram was longer than the buffers, which recvmsg() turns into
 * MSG_TRUNC. The excess is discarded, as BSD does.
 */
static LONG bsd_recv_udp(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags,
                         struct sockaddr *from, socklen_t *fromlen,
                         BOOL *truncated, struct msghdr *msg)
{
    NX_PACKET  *packet = NX_NULL;
    NXD_ADDRESS src_ip;
    ULONG       length, taken = 0;
    UINT        src_port = 0;
    UINT        status;
    BOOL        peek = ((flags & MSG_PEEK) != 0);

    if (truncated != NULL)
        *truncated = FALSE;

    if (sock->as_RxPending != NULL)
    {
        packet = sock->as_RxPending;
    }
    else
    {
        ULONG          wait = (len == 0)
                                  ? NX_NO_WAIT
                                  : bsd_wait_option(sock, sock->as_RcvTimeout,
                                                    flags);
        BsdRecvUdpArgs args;
        BOOL           aborted;

        args.udp    = &sock->as_Nx.udp;
        args.packet = &packet;

        for (;;)
        {
            status = bsd_wait_sliced(base, wait, bsd_recv_udp_once, &args,
                                     &aborted);
            if (aborted)
                return bsd_fail(base, AMI_EINTR);
            if (status != NX_SUCCESS)
            {
                if (len == 0 && (status == NX_NO_PACKET ||
                                 status == NX_NOT_SUCCESSFUL))
                    return 0;

                /* An ICMP error the stack held for this socket comes back as
                   the status. Reporting it here consumes it, so SO_ERROR must
                   not answer with it a second time. */
                if (status == NX_NET_UNREACHABLE ||
                    status == NX_HOST_UNREACHABLE ||
                    status == NX_PROTOCOL_UNREACHABLE ||
                    status == NX_PORT_UNREACHABLE)
                    sock->as_SoError = 0;

                return bsd_fail(base, bsd_wait_errno(wait, status));
            }

            if (bsd_udp_accepts_received_packet(sock, packet))
                break;

            nx_packet_release(packet);
            packet = NX_NULL;
        }
    }

    /* nxd_, not nx_: nx_udp_source_extract() reports 0.0.0.0 for a datagram
       that arrived over IPv6. */
    nxd_udp_source_extract(packet, &src_ip, &src_port);

    length = bsd_packet_len(packet);

    while (taken < length && (LONG)taken < len)
    {
        UBYTE *dst   = NULL;
        ULONG  chunk = bsd_iov_chunk(cur, &dst);
        ULONG  want, moved = 0;

        if (chunk == 0)
            break;

        want = length - taken;
        if (want > chunk)
            want = chunk;
        if (want > (ULONG)len - taken)
            want = (ULONG)len - taken;

        if (nx_packet_data_extract_offset(packet, taken, dst, want, &moved)
                != NX_SUCCESS || moved == 0)
            break;

        bsd_iov_advance(cur, moved);
        taken += moved;
    }

    if (truncated != NULL && taken < length)
        *truncated = TRUE;

    if (from != NULL && fromlen != NULL)
        bsd_sockaddr_put(sock, from, fromlen, &src_ip, src_port,
                         bsd_packet_scope_id(packet, &src_ip));

    bsd_cmsg_build(sock, packet, msg);

    if (peek)
    {
        sock->as_RxPending = packet;
        sock->as_RxOffset  = 0;
    }
    else
    {
        sock->as_RxPending = NULL;
        sock->as_RxOffset  = 0;
        nx_packet_release(packet);
    }

    /* Datagram sockets discard whatever did not fit, as BSD does. */
    return (LONG)taken;
}

/*
 * One raw datagram in. IPv4 includes the IP header, which is what 4.4BSD
 * delivers and what ping and traceroute parse. IPv6 does not, per RFC 3542.
 */
static LONG bsd_recv_raw(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags,
                         struct sockaddr *from, socklen_t *fromlen,
                         BOOL *truncated, struct msghdr *msg)
{
    NX_PACKET  *packet;
    NXD_ADDRESS src;
    ULONG       length, taken = 0;
    BOOL        peek = ((flags & MSG_PEEK) != 0);

    if (truncated != NULL)
        *truncated = FALSE;

    if (sock->as_RxPending != NULL)
    {
        packet = sock->as_RxPending;
    }
    else
    {
        ULONG          wait = (len == 0)
                                  ? NX_NO_WAIT
                                  : bsd_wait_option(sock, sock->as_RcvTimeout,
                                                    flags);
        BsdRecvRawArgs args;
        BOOL           aborted;

        args.sock   = sock;
        args.packet = &packet;
        args.why    = NX_NO_PACKET;

        (VOID)bsd_wait_sliced(base, wait, bsd_recv_raw_once, &args, &aborted);
        if (aborted)
            return bsd_fail(base, AMI_EINTR);
        if (packet == NX_NULL && len == 0 && args.why == NX_NO_PACKET)
            return 0;
        if (packet == NX_NULL)
            return bsd_fail(base, bsd_wait_errno(wait, args.why));
    }

    bsd_raw_source(packet, &src);

    length = bsd_packet_len(packet);

    while (taken < length && (LONG)taken < len)
    {
        UBYTE *dst   = NULL;
        ULONG  chunk = bsd_iov_chunk(cur, &dst);
        ULONG  want, moved = 0;

        if (chunk == 0)
            break;

        want = length - taken;
        if (want > chunk)
            want = chunk;
        if (want > (ULONG)len - taken)
            want = (ULONG)len - taken;

        if (nx_packet_data_extract_offset(packet, taken, dst, want, &moved)
                != NX_SUCCESS || moved == 0)
            break;

        bsd_iov_advance(cur, moved);
        taken += moved;
    }

    if (truncated != NULL && taken < length)
        *truncated = TRUE;

    if (from != NULL && fromlen != NULL)
        bsd_sockaddr_put(sock, from, fromlen, &src, 0,
                         bsd_packet_scope_id(packet, &src));

    bsd_cmsg_build(sock, packet, msg);

    if (peek)
    {
        sock->as_RxPending = packet;
        sock->as_RxOffset  = 0;
    }
    else
    {
        sock->as_RxPending = NULL;
        sock->as_RxOffset  = 0;
        nx_packet_release(packet);
    }

    return (LONG)taken;
}

static LONG bsd_send_iov(struct AmiSocketBase *base, AmiSocket *sock,
                         const struct iovec *iov, LONG iovcnt, LONG len,
                         LONG flags, const NXD_ADDRESS *addr, UINT port,
                         ULONG scope, const BsdCmsgSource *src)
{
    BsdIovCursor cur;
    LONG         result;

    if ((sock->as_Flags & (ASF_TCP | ASF_WRSHUT)) == ASF_WRSHUT)
        return bsd_fail(base, AMI_EPIPE);

    bsd_iov_init(&cur, iov, iovcnt);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_RAW) != 0)
        result = bsd_send_raw(base, sock, &cur, len, flags, addr, scope, src);
    else if ((sock->as_Flags & ASF_TCP) != 0)
        result = bsd_send_tcp(base, sock, &cur, len, flags);
    else
        result = bsd_send_udp(base, sock, &cur, len, flags, addr, port,
                              scope, src);

    bsd_nx_leave(base);

    return result;
}

static LONG bsd_recv_iov(struct AmiSocketBase *base, AmiSocket *sock,
                         const struct iovec *iov, LONG iovcnt, LONG len,
                         LONG flags, struct sockaddr *from,
                         socklen_t *fromlen, BOOL *truncated,
                         struct msghdr *msg)
{
    BsdIovCursor cur;
    LONG         result;
    BOOL         held = FALSE;

    if ((sock->as_Flags & (ASF_TCP | ASF_RDSHUT)) == ASF_RDSHUT)
    {
        if (truncated != NULL)
            *truncated = FALSE;
        if (msg != NULL)
            msg->msg_controllen = 0;

        return 0;
    }

    bsd_iov_init(&cur, iov, iovcnt);

    if (!bsd_recv_parked(sock, len))
    {
        if (bsd_nx_enter(base) != 0)
            return bsd_fail(base, AMI_ENETDOWN);
        held = TRUE;
    }

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        result = bsd_recv_raw(base, sock, &cur, len, flags, from, fromlen,
                              truncated, msg);
    }
    else if ((sock->as_Flags & ASF_TCP) != 0)
    {
        result = bsd_recv_tcp(base, sock, &cur, len, flags, &held);
        if (result >= 0 && from != NULL && fromlen != NULL)
            bsd_sockaddr_put(sock, from, fromlen, &sock->as_PeerAddr,
                             sock->as_PeerPort, sock->as_PeerScopeId);
        if (truncated != NULL)
            *truncated = FALSE;         /* a stream never truncates */

        if (msg != NULL)
            msg->msg_controllen = 0;
    }
    else
    {
        result = bsd_recv_udp(base, sock, &cur, len, flags, from, fromlen,
                              truncated, msg);
    }

    if (held)
        bsd_nx_leave(base);

    return result;
}

/*
 * The destination a sendto()/sendmsg() supplied has to belong to the same
 * family as the socket. 0 = ok, -1 = errno set.
 */
static LONG bsd_dest_check(struct AmiSocketBase *base, AmiSocket *sock,
                           NXD_ADDRESS *addr)
{
#ifdef AMINETXDUO_IPV6
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr->nxd_ip_version != NX_IP_VERSION_V6)
            return bsd_fail(base, AMI_EAFNOSUPPORT);

        if (!bsd_addr_normalise(sock, addr))
            return bsd_fail(base, AMI_ENETUNREACH);

        return 0;
    }

    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
        return bsd_fail(base, AMI_EAFNOSUPPORT);
#else
    (VOID)base;
    (VOID)sock;
    (VOID)addr;
#endif

    return 0;
}

/* Common argument checks for every send/recv shape. 0 = ok, -1 = errno set. */
static LONG bsd_transfer_check(struct AmiSocketBase *base, AmiSocket *sock,
                               LONG len, LONG flags)
{
    if (sock == NULL)
        return bsd_fail(base, AMI_EBADF);

    if (len < 0)
        return bsd_fail(base, AMI_EINVAL);

    /* MSG_OOB is TCP-only: there is no urgent data on a datagram or raw
       socket. oob.c has the rest. */
    if ((flags & MSG_OOB) != 0 && (sock->as_Flags & ASF_TCP) == 0)
        return bsd_fail(base, AMI_EOPNOTSUPP);

    return 0;
}

/*
 * send(..., MSG_OOB): every byte goes and the last one is the urgent one.
 */
static LONG bsd_send_oob(struct AmiSocketBase *base, AmiSocket *sock,
                         const UBYTE *buf, LONG len, LONG flags)
{
    LONG sent = 0;
    LONG rc;

    if (len < 1)
        return bsd_fail(base, AMI_EINVAL);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    if (len > 1)
    {
        BsdIovCursor cur;
        struct iovec iov;

        iov.iov_base = (APTR)buf;
        iov.iov_len  = (size_t)(len - 1);
        bsd_iov_init(&cur, &iov, 1);

        sent = bsd_send_tcp(base, sock, &cur, len - 1, flags);
        if (sent < len - 1)
        {
            bsd_nx_leave(base);
            return sent;                /* short write, or -1 with errno set */
        }
    }

    rc = bsd_oob_send(base, sock, buf[len - 1], flags);

    bsd_nx_leave(base);

    if (rc < 0)
        return (sent > 0) ? sent : -1;

    return sent + 1;
}

/*
 * recv(..., MSG_OOB): the byte the peer marked urgent, once. EINVAL when there
 * is none, as 4.4BSD answers. There is no waiting variant: a caller that wants
 * to be told when one arrives selects on exceptfds or takes SIGURG.
 */
static LONG bsd_recv_oob(struct AmiSocketBase *base, AmiSocket *sock,
                         UBYTE *buf, LONG len)
{
    UBYTE byte = 0;

    if (len < 1)
        return bsd_fail(base, AMI_EINVAL);

    if (!bsd_oob_take(sock, &byte))
        return bsd_fail(base, AMI_EINVAL);

    buf[0] = byte;

    return 1;
}

/*
 * MHT_Send: the hook that can refuse a send before any of it happens.
 */
static LONG bsd_send_monitor(struct AmiSocketBase *base, LONG sock_fd,
                             APTR buf, LONG len, LONG flags,
                             struct sockaddr *to, socklen_t tolen,
                             struct msghdr *msg)
{
    struct SendMonitorMessage smm;

    if (!bsd_netmon_have(MHT_Send))
        return 0;

    bsd_bzero(&smm, sizeof(smm));
    smm.smm_Size   = (LONG)sizeof(smm);
    smm.smm_Caller = bsd_netmon_caller(base);
    smm.smm_Socket = sock_fd;
    smm.smm_Buffer = buf;
    smm.smm_Len    = len;
    smm.smm_Flags  = flags;
    smm.smm_To     = to;
    smm.smm_ToLen  = (LONG)tolen;
    smm.smm_Msg    = msg;

    return bsd_netmon_dispatch(MHT_Send, &smm);
}

LONG bsd_send(register LONG sock_fd __asm("d0"),
              register APTR buf     __asm("a0"),
              register LONG len     __asm("d1"),
              register LONG flags   __asm("d2"),
              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket    *sock = bsd_lookup(SocketBase, sock_fd);
    struct iovec  iov;

    if (bsd_transfer_check(SocketBase, sock, len, flags) != 0)
        return -1;

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    {
        LONG denied = bsd_send_monitor(SocketBase, sock_fd, buf, len, flags,
                                       NULL, 0, NULL);

        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    if ((sock->as_Flags & ASF_TCP) == 0 &&
        (sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

    if ((flags & MSG_OOB) != 0)
        return bsd_send_oob(SocketBase, sock, (const UBYTE *)buf, len, flags);

    iov.iov_base = buf;
    iov.iov_len  = (size_t)len;

    return bsd_send_iov(SocketBase, sock, &iov, 1, len, flags,
                        &sock->as_PeerAddr, sock->as_PeerPort,
                        sock->as_PeerScopeId, &sock->as_CmsgSticky);
}

LONG bsd_sendto(register LONG sock_fd        __asm("d0"),
                register APTR buf            __asm("a0"),
                register LONG len            __asm("d1"),
                register LONG flags          __asm("d2"),
                register struct sockaddr *to __asm("a1"),
                register socklen_t tolen     __asm("d3"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket    *sock = bsd_lookup(SocketBase, sock_fd);
    struct iovec  iov;
    NXD_ADDRESS   addr;
    UINT          port  = 0;
    ULONG         scope = 0;

    if (bsd_transfer_check(SocketBase, sock, len, flags) != 0)
        return -1;

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    {
        LONG denied = bsd_send_monitor(SocketBase, sock_fd, buf, len, flags,
                                       to, tolen, NULL);

        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    bsd_addr_from_v4(&addr, 0UL);
    scope = 0UL;

    if ((flags & MSG_OOB) != 0)
        return bsd_send_oob(SocketBase, sock, (const UBYTE *)buf, len, flags);

    /* A destination on a connected stream socket is ignored, as in BSD. */
    if ((sock->as_Flags & ASF_TCP) == 0)
    {
        if (to == NULL)
        {
            if ((sock->as_Flags & ASF_CONNECTED) == 0)
                return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

            addr  = sock->as_PeerAddr;
            port  = sock->as_PeerPort;
            scope = sock->as_PeerScopeId;
        }
        else if (bsd_sockaddr_get(SocketBase, to, tolen, &addr, &port,
                                  &scope) != 0)
        {
            return -1;
        }
        else if (bsd_dest_check(SocketBase, sock, &addr) != 0)
        {
            return -1;
        }
    }

    iov.iov_base = buf;
    iov.iov_len  = (size_t)len;

    return bsd_send_iov(SocketBase, sock, &iov, 1, len, flags, &addr, port,
                        scope, &sock->as_CmsgSticky);
}

LONG bsd_recv(register LONG sock_fd __asm("d0"),
              register APTR buf     __asm("a0"),
              register LONG len     __asm("d1"),
              register LONG flags   __asm("d2"),
              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket    *sock = bsd_lookup(SocketBase, sock_fd);
    struct iovec  iov;

    if (bsd_transfer_check(SocketBase, sock, len, flags) != 0)
        return -1;

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if ((flags & MSG_OOB) != 0)
        return bsd_recv_oob(SocketBase, sock, (UBYTE *)buf, len);

    iov.iov_base = buf;
    iov.iov_len  = (size_t)len;

    return bsd_recv_iov(SocketBase, sock, &iov, 1, len, flags, NULL, NULL,
                        NULL, NULL);
}

LONG bsd_recvfrom(register LONG sock_fd          __asm("d0"),
                  register APTR buf              __asm("a0"),
                  register LONG len              __asm("d1"),
                  register LONG flags            __asm("d2"),
                  register struct sockaddr *addr __asm("a1"),
                  register socklen_t *addrlen    __asm("a2"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket    *sock = bsd_lookup(SocketBase, sock_fd);
    struct iovec  iov;

    if (bsd_transfer_check(SocketBase, sock, len, flags) != 0)
        return -1;

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if (addr != NULL && addrlen == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if ((flags & MSG_OOB) != 0)
        return bsd_recv_oob(SocketBase, sock, (UBYTE *)buf, len);

    iov.iov_base = buf;
    iov.iov_len  = (size_t)len;

    return bsd_recv_iov(SocketBase, sock, &iov, 1, len, flags, addr, addrlen,
                        NULL, NULL);
}

/*
 * msg_control carries the RFC 3542 subset in cmsg.c: PKTINFO and HOPLIMIT in,
 * PKTINFO out. That file has the shapes and the option numbers.
 */
LONG bsd_sendmsg(register LONG sock_fd        __asm("d0"),
                 register struct msghdr *msg  __asm("a0"),
                 register LONG flags          __asm("d1"),
                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket    *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS   addr;
    BsdCmsgSource src;
    UINT          port  = 0;
    ULONG         scope = 0;
    LONG          total;

    if (bsd_transfer_check(SocketBase, sock, 0, flags) != 0)
        return -1;

    if ((flags & MSG_OOB) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if (msg == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    total = bsd_iov_total(msg->msg_iov, (LONG)msg->msg_iovlen);
    if (total < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    {
        LONG denied = bsd_send_monitor(SocketBase, sock_fd, NULL, total, flags,
                                       NULL, 0, msg);

        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    /* Ancillary data before anything is sent: it can refuse the whole call. */
    if (bsd_cmsg_parse(SocketBase, sock, msg, &src) != 0)
        return -1;

    bsd_addr_from_v4(&addr, 0UL);

    if ((sock->as_Flags & ASF_TCP) == 0)
    {
        if (msg->msg_name == NULL)
        {
            if ((sock->as_Flags & ASF_CONNECTED) == 0)
                return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

            addr  = sock->as_PeerAddr;
            port  = sock->as_PeerPort;
            scope = sock->as_PeerScopeId;
        }
        else if (bsd_sockaddr_get(SocketBase,
                                  (const struct sockaddr *)msg->msg_name,
                                  (socklen_t)msg->msg_namelen,
                                  &addr, &port, &scope) != 0)
        {
            return -1;
        }
        else if (bsd_dest_check(SocketBase, sock, &addr) != 0)
        {
            return -1;
        }
    }

    return bsd_send_iov(SocketBase, sock, msg->msg_iov,
                        (LONG)msg->msg_iovlen, total, flags, &addr, port,
                        scope, &src);
}

LONG bsd_recvmsg(register LONG sock_fd        __asm("d0"),
                 register struct msghdr *msg  __asm("a0"),
                 register LONG flags          __asm("d1"),
                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket       *sock = bsd_lookup(SocketBase, sock_fd);
    struct sockaddr *from = NULL;
    socklen_t        fromlen = 0;
    BOOL             truncated = FALSE;
    LONG             total;
    LONG             result;

    if (bsd_transfer_check(SocketBase, sock, 0, flags) != 0)
        return -1;

    if ((flags & MSG_OOB) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if (msg == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    total = bsd_iov_total(msg->msg_iov, (LONG)msg->msg_iovlen);
    if (total < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (msg->msg_name != NULL)
    {
        from    = (struct sockaddr *)msg->msg_name;
        fromlen = (socklen_t)msg->msg_namelen;
    }

    /* msg_flags is an out parameter. Whatever the caller left there is not an
       input and must not survive. */
    msg->msg_flags = 0;

    /* bsd_cmsg_build() rewrites msg_controllen, which is value-result: it
       arrives as the size of the caller's buffer and leaves as what was
       written. A path that attaches nothing leaves it 0. */
    result = bsd_recv_iov(SocketBase, sock, msg->msg_iov,
                          (LONG)msg->msg_iovlen, total, flags,
                          from, (from != NULL) ? &fromlen : NULL,
                          &truncated, msg);

    if (result < 0)
    {
        msg->msg_controllen = 0;
        return result;
    }

    if (from != NULL)
        msg->msg_namelen = (socklen_t)fromlen;

    if (truncated)
        msg->msg_flags |= MSG_TRUNC;

    return result;
}
