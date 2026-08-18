/*
 * bsdsocket.library, send/sendto/sendmsg and recv/recvfrom/recvmsg.
 *
 * NetX Duo is packet-oriented: a send allocates an NX_PACKET from the stack
 * pool, appends the caller's bytes to it and hands it over. A receive returns
 * a packet the caller has to drain. A BSD stream read need not consume a whole
 * packet, so a partially drained one is parked on the socket
 * (as_RxPending/as_RxOffset) until it runs out.
 *
 * Every path here works on an iovec list, and the flat-buffer calls hand it a
 * one-element list on the stack, so sendmsg()/recvmsg() are the same code as
 * send()/recv(). No bounce buffer is needed: nx_packet_data_append() called
 * once per iovec builds one packet, and nx_packet_data_extract_offset()
 * scatters straight into each destination.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "netmonitor.h"

#include "nx_ip.h"
#include "nx_ipv4.h"
#ifdef AMINETXDUO_IPV6
#include "nx_ipv6.h"
#endif

#include <proto/exec.h>

/* Fallback segment size, used before the socket has negotiated an MSS. */
#define BSD_DEFAULT_MSS     536

/*
 * struct msghdr / struct iovec are ABI. The definitions come from the Roadshow
 * netinclude headers in the toolchain's ndk-include (sys/socket.h, sys/uio.h),
 * the only ones on the include path. This toolchain's newlib tree has neither,
 * so the wrong lineage cannot be picked up the way ndk-include/pwd.h shadows
 * the usergroup struct passwd.
 *
 * That header carries the POSIX / 4.4BSD-Lite msghdr with ancillary data
 * (msg_control / msg_controllen / msg_flags), not the older 4.3BSD form with
 * msg_accrights. Pinned below so a header swap is a build error rather than a
 * silent 8-byte shift from msg_control onwards.
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

/* ------------------------------------------------------------- iovec ------ */

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

/* ---------------------------------------------------------------- packet, */

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
            /* The caller decides what an append failure means, so it needs
               the status. */
            if (why != NULL)
                *why = status;

            return (done > 0) ? (LONG)done : -1;
        }

        bsd_iov_advance(cur, chunk);
        done += chunk;
    }

    return (LONG)done;
}

/* ------------------------------------------------------------------- send, */

/* The two blocking points of a TCP send: waiting for a free packet (pool
   empty -> NX_NO_PACKET) and waiting for send-window room (peer not reading ->
   NX_WINDOW_OVERFLOW / NX_TX_QUEUE_DEPTH).  Both are statuses bsd_wait_sliced()
   retries on, so Ctrl-C breaks a send stalled on either.  Global rather than
   static, for the reason bsd_recv_once (below) gives. */
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
 *
 * It is not all-or-nothing. _nx_tcp_socket_send_internal() segments the packet
 * itself, to min(peer window, congestion window, MSS), in a loop. Each pass
 * copies that many bytes into a segment, queues it on the socket's transmit
 * list and hands it to _nx_ip_packet_send(), which puts it on the wire. It
 * then trims those bytes off the caller's packet before looking at the window
 * again. When a later pass finds no room and cannot wait, it returns
 * NX_WINDOW_OVERFLOW or NX_TX_QUEUE_DEPTH with the earlier segments long gone.
 *
 * So the packet that comes back holds only what did not go, and `filled` less
 * that is what the peer has already seen. Releasing the packet and reporting
 * only the chunks that completed tells the caller to send those bytes a second
 * time, which duplicates data in the middle of the stream.
 *
 * Safe on every error return in that function: none of them releases the
 * caller's packet.
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
    /*
     * Why the loop stopped. Every first-iteration failure below breaks out and
     * lands on the "sent == 0" test at the bottom, which used to answer
     * EWOULDBLOCK whatever had happened, so a signalled thread
     * (NX_WAIT_ABORTED, EINTR) and a stack under teardown (NX_POOL_DELETED,
     * ENOBUFS) both came back as EAGAIN on a blocking socket.
     * docs/RESEARCH.md 37.2.
     */
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
                /* A slice before the break can already have sent part. */
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

        /* A non-blocking socket takes what fits and reports the rest short. */
        if ((sock->as_Flags & ASF_NONBLOCK) != 0)
            wait = NX_NO_WAIT;
    }

    if (sent == 0 && len > 0)
        return bsd_fail(base, bsd_wait_errno(wait, why));

    return sent;
}

/*
 * The largest UDP payload that can leave here in one datagram, from the three
 * things that fix it:
 *
 *   1. Transmit fragmentation is available, src/netstack/ calls
 *      nx_ip_fragment_enable() for the receive side, and is not used here.
 *      A UDP datagram larger than the path is reassembled by whatever
 *      receives it, out of a pool as small as ours, and BSD returning
 *      EMSGSIZE for it is an answer the caller can act on. The cap is the
 *      egress interface's nx_interface_ip_mtu_size.
 *   2. That field is the IP-layer MTU, link header excluded: sana2_device.c
 *      sets it from the SANA-II S2_GetGlobalStats MTU, defined the same way.
 *   3. The IP and UDP headers in front of the payload are what NetX Duo's own
 *      NX_IPv{4,6}_UDP_PACKET offsets reserve, less the NX_PHYSICAL_HEADER
 *      they also carry: 28 bytes for IPv4, 48 for IPv6. No IPv4 options and no
 *      IPv6 extension headers go on a UDP send, so both are exact.
 *
 * That is 1472 and 1452 on a 1500-byte Ethernet interface, 65507 on NetX Duo's
 * loopback. The interface is found the way _nxd_udp_socket_send() finds it, so
 * the MTU measured is the one the datagram meets. -1 means no interface was
 * found, and the send then reports the routing error itself.
 */
LONG bsd_route_mtu(NX_IP *ip, const NXD_ADDRESS *addr)
{
    NX_INTERFACE *iface = NX_NULL;

    if (ip == NULL || addr == NULL)
        return -1;

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

static LONG bsd_udp_maxdgram(NX_IP *ip, const NXD_ADDRESS *addr)
{
    LONG  mtu = bsd_route_mtu(ip, addr);
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
 *
 * nx_packet_ip_interface is one arm of a union, and the IPv6 receive path
 * overwrites the other with the NXD_IPV6_ADDRESS the datagram matched,
 * nx_ipv6_packet_receive.c, "Set the matching address to the packet address".
 * Reading it as an NX_INTERFACE * after that dereferences an address structure
 * as an interface, so the version has to decide which arm to read. This was
 * wrong once, and a socket bound to a specific IPv6 address dropped
 * everything.
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
 * Is this datagram from the peer a connected socket named?
 *
 * RFC 1122 4.1.3.5: once connect() names a peer, the socket is identified by
 * the whole four-tuple, and a datagram from anywhere else is not for it.
 * NetX Duo cannot enforce that. _nx_udp_packet_receive() demultiplexes on the
 * local port alone, and NX_UDP_SOCKET has neither a local address nor a peer
 * to compare against. "connected" is a notion of this layer, so the test is
 * this layer's to make.
 *
 * The resolver is what pays when this test is absent. netstack_dns sends a
 * query from a connected socket and waits. Without this test, the first host
 * on the wire to answer is believed, whoever it is.
 *
 * TRUE for a socket that never connected: an unbound peer takes datagrams from
 * anybody, which is what recvfrom() is for.
 *
 * Not static: select.c asks the same question of an ICMP error's peer, which
 * is the only unambiguous way to attribute one to a UDP socket.
 */
BOOL bsd_udp_from_peer(const AmiSocket *sock, const NXD_ADDRESS *src,
                       UINT src_port)
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
        return (BOOL)(src->nxd_ip_address.v6[0] ==
                          sock->as_PeerAddr.nxd_ip_address.v6[0] &&
                      src->nxd_ip_address.v6[1] ==
                          sock->as_PeerAddr.nxd_ip_address.v6[1] &&
                      src->nxd_ip_address.v6[2] ==
                          sock->as_PeerAddr.nxd_ip_address.v6[2] &&
                      src->nxd_ip_address.v6[3] ==
                          sock->as_PeerAddr.nxd_ip_address.v6[3]);
    }
#endif

    return (BOOL)(src->nxd_ip_address.v4 ==
                  sock->as_PeerAddr.nxd_ip_address.v4);
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
     *
     * PKTINFO wins over bind() and over the zone because it is the narrower
     * statement: bind() is standing and this one was supplied for this
     * datagram. A source the machine does not have is refused either way,
     * rather than replaced by the stack's own choice.
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

    /*
     * "If the message is too long to pass atomically through the underlying
     * protocol, the error EMSGSIZE is returned, and the message is not
     * transmitted." Checked before the packet is allocated, so a refused
     * datagram sends nothing. It used to be assembled, handed over and dropped
     * in the driver send path with NX_SUCCESS already returned, or to run the
     * pool dry first and come back as ENOBUFS.
     */
    maxdgram = bsd_udp_maxdgram(ip, addr);
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
    /*
     * Puts IP_MULTICAST_TTL on the socket for this send and answers with the
     * IP_MULTICAST_IF interface, or -1 when the route can choose. Called for
     * every destination and not only a group, because it is also what puts
     * the TTL back for a unicast one.
     */
    mcast_if = bsd_mcast_prepare_send(sock, addr);
#ifdef AMINETXDUO_IPV6
    /*
     * The v6 half. It answers with an address index, not an interface one,
     * IPV6_MULTICAST_IF names an interface and the source send wants the
     * link-local address on it, and it puts IPV6_MULTICAST_HOPS on the
     * NX_IP, which bsd_mcast6_finish_send() takes back off below. Every path
     * between the two reaches that call.
     */
    mcast6_src = bsd_mcast6_prepare_send(sock, addr, &mcast6_hops);

    /*
     * IPV6_MULTICAST_HOPS is 0 for this group: RFC 3493 5.2 makes that "this
     * host only", and there is nothing here to deliver it to. So it is
     * consumed rather than put on the link, and the caller is told the whole
     * write was accepted.
     */
    if (mcast6_src == BSD_MCAST6_NO_LINK)
    {
        bsd_mcast6_finish_send(mcast6_hops);
        return len;
    }
#endif
#else
    /* What bsd_mcast_prepare_send() does for a unicast destination when
       multicast is compiled in: IP_TTL and IPV6_UNICAST_HOPS reach the wire. */
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

    /*
     * Four ways to name where this leaves from, most specific first. A
     * per-message (or sticky) PKTINFO source overrides the standing multicast
     * interface option. The two multicast choices only answer for a group of
     * their own family, and the remaining source index is bind()/scope state.
     */
    if (src != NULL && src->cs_Have && source == BSD_SOURCE_INDEX)
    {
        status = nxd_udp_socket_source_send(&sock->as_Nx.udp, packet,
                                            (NXD_ADDRESS *)addr, port,
                                            source_index);
    }
    else
#ifdef AMINETXDUO_MULTICAST
    /* IP_MULTICAST_IF named an interface, so the route does not choose. */
    if (mcast_if >= 0)
    {
        status = nx_udp_socket_source_send(&sock->as_Nx.udp, packet,
                                           addr->nxd_ip_address.v4, port,
                                           (UINT)mcast_if);
    }
    else
#ifdef AMINETXDUO_IPV6
    /* IPV6_MULTICAST_IF, same statement one family over. */
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
        /* nxd_, not nx_: the v4 wrapper wraps the address and calls this. */
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

/* ---------------------------------------------------------------- receive, */

/* bsd_wait_sliced() drives this. See select.c for why the wait is sliced. */
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

/* The UDP and raw receives slice the same way, see bsd_recv_once's note on
   why these are global rather than static. */
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

    /* bsd_raw_receive() leaves `why` at NX_NO_PACKET when nothing is queued,
       the status bsd_wait_sliced() retries on. */
    return (packet != NX_NULL) ? NX_SUCCESS : why;
}


/*
 * Take the bracket unless this call already holds it, so a path that reaches
 * NetX Duo after bsd_recv_parked() said it would not is slow rather than
 * wrong. That is what makes the predicate below safe as an optimisation.
 * Returns FALSE only if the bracket cannot be had at all.
 */
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
 *
 * bsd_recv_tcp() reaches a THREADS_ONLY entry point in exactly one place,
 * bsd_wait_sliced() -> nx_tcp_socket_receive(), and only when as_RxPending
 * is empty. A read strictly shorter than what the parked packet still holds
 * never empties it, so it never receives and never releases: it reads a length
 * out of the packet header and memcpys out of the chain, on a packet
 * nx_tcp_socket_receive() already handed over and that the IP thread no longer
 * references. None of that needs the bracket.
 *
 * Strictly shorter, not "at least as long as": a read that drains the packet
 * exactly calls nx_packet_release(), which puts it back in the shared pool.
 * That is the one nx_packet_* helper here that touches state the IP thread
 * also touches. It is cheap to stay away from, because the case is one read
 * length in a packet's worth, and the call that would have hit it takes the
 * bracket for its receive anyway.
 *
 * Worth a predicate because netx_call.c prices the bracket at ~270 us on a
 * 14 MHz 68020 with the TX_THREAD cached, ~790 us without, either of which is
 * more than copying and checksumming a whole MTU packet.
 * tests/perf/bracket_test.c's lazy arm measures the ceiling over 256 KB:
 * 512-byte reads 598 -> 482 ms, 1460-byte reads 496 -> 460 ms.
 */
static BOOL bsd_recv_parked(AmiSocket *sock, LONG len)
{
    ULONG length;

    if ((sock->as_Flags & (ASF_TCP | ASF_RAW)) != ASF_TCP)
        return FALSE;

    /* bsd_recv_tcp() returns 0 before it looks at the packet. Stated here
       rather than left to the caller's ordering. */
    if ((sock->as_Flags & ASF_RDSHUT) != 0)
        return FALSE;

    if (sock->as_RxPending == NULL)
        return FALSE;

    length = bsd_packet_len(sock->as_RxPending);
    if (length <= sock->as_RxOffset)
        return FALSE;               /* drained, the next call releases it */

    return (BOOL)((length - sock->as_RxOffset) > (ULONG)len);
}

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

            /*
             * Only the first read of a call can block. After that, a stream
             * receive returns whatever the socket buffer holds. MSG_WAITALL
             * asks for the opposite, and waits until the caller's buffers are
             * full or the connection ends.
             */
            ULONG now = (first || (flags & MSG_WAITALL) != 0) ? wait
                                                              : NX_NO_WAIT;

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
                    /* Ctrl-C, or whatever SBTC_BREAKMASK was set to. The
                       signal is left set: it is the caller's, and a program
                       that polls SetSignal() after an EINTR must still see
                       it. */
                    if (copied > 0)
                        break;
                    return bsd_fail(base, AMI_EINTR);
                }
            }

            if (status == NX_SUCCESS)
            {
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

                /* `now`, not `wait`: only the first read of a call can block,
                   so a later one that came up empty is a caller that asked not
                   to wait. */
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
        /* A zero-capacity datagram read consumes one queued record but does
           not wait for a new one. This is distinct from a zero-length
           datagram, which is still a queued record and reaches this call. */
        ULONG          wait = (len == 0)
                                  ? NX_NO_WAIT
                                  : bsd_wait_option(sock, sock->as_RcvTimeout,
                                                    flags);
        BsdRecvUdpArgs args;
        BOOL           aborted;

        args.udp    = &sock->as_Nx.udp;
        args.packet = &packet;

        /*
         * A socket bound to one local address must not be handed datagrams
         * that arrived for another, and a connected one must not be handed
         * datagrams from anybody but its peer. NetX binds a UDP socket to a
         * port only, so both filters are here: a datagram that matches neither
         * the bind nor the peer is released and the wait resumed, which is
         * what the caller sees if the datagram never arrives.
         *
         * This loops rather than fails, because a mismatch is not an error for
         * this caller. It is someone else's traffic.
         */
        for (;;)
        {
            NXD_ADDRESS from_ip;
            UINT        from_port = 0;

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

            nxd_udp_source_extract(packet, &from_ip, &from_port);

            if (bsd_bind_wants_interface(sock,
                                         bsd_packet_interface(packet)) &&
                bsd_udp_from_peer(sock, &from_ip, from_port))
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
        bsd_sockaddr_put(sock, from, fromlen, &src_ip, src_port);

    /* Before the release below: everything it answers is in the packet. */
    bsd_cmsg_build(sock, packet, msg);

    if (peek)
    {
        /* Keep the datagram queued for the next call. */
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
 * See the header of raw.c. As with UDP, whatever does not fit the caller's
 * buffers is discarded.
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
        bsd_sockaddr_put(sock, from, fromlen, &src, 0);

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

/* --------------------------------------------------- the shared entry paths */

/* Everything below funnels into these two so send/sendto/sendmsg and
   recv/recvfrom/recvmsg cannot drift apart. */
static LONG bsd_send_iov(struct AmiSocketBase *base, AmiSocket *sock,
                         const struct iovec *iov, LONG iovcnt, LONG len,
                         LONG flags, const NXD_ADDRESS *addr, UINT port,
                         ULONG scope, const BsdCmsgSource *src)
{
    BsdIovCursor cur;
    LONG         result;

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

    bsd_iov_init(&cur, iov, iovcnt);

    /* A stream read the parked packet already covers touches nothing
       THREADS_ONLY, so it skips the bracket. bsd_nx_need() takes it if that
       turns out to be wrong. A nested call inside one that took it stays
       covered either way, sb_NxNest is still up. */
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
                             sock->as_PeerPort);
        if (truncated != NULL)
            *truncated = FALSE;         /* a stream never truncates */

        /* A stream has no per-datagram anything, so nothing to attach. */
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
 *
 * For a v4-mapped destination on a dual-stack socket, bsd_addr_normalise()
 * turns ::ffff:a.b.c.d into a plain IPv4 address. Without that, NetX Duo puts
 * the mapped form in an IPv6 header and sends it to a host that has no IPv6.
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
 * That is 4.4BSD's rule, and what telnet and ftp rely on: they write a short
 * command whose final byte is the signal. The leading bytes take the ordinary
 * path. Only the last needs oob.c.
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

/* ---------------------------------------------------------------- vectors, */

/*
 * MHT_Send: the hook that can refuse a send before any of it happens.
 *
 * "The hook function will be invoked before dropping into the kernel 'send()',
 * 'sendto()' or 'sendmsg()' calls ... any error value > 0 will cause the call
 * to be aborted and the errno variable to be set to this value."
 *
 * It runs after the descriptor and the arguments are checked, so a monitor
 * cannot be used to probe which descriptors exist, and before anything is
 * queued, resolved or transmitted.
 *
 * On smm_To and smm_Msg: "Depending upon the type of function to perform, the
 * contents of the SendMonitorMessage may look different. For example, either
 * the 'smm_To' or the 'smm_Msg' field will be NULL." That is an exclusion, not
 * a requirement that exactly one be set. The three calls give:
 *
 *   send()      smm_To NULL, smm_Msg NULL, no destination and no msghdr,
 *                                              the peer is implied
 *   sendto()    smm_To the caller's, smm_Msg NULL   (smm_To is NULL too when
 *                                              the caller passed none, which
 *                                              is legal on a connected socket)
 *   sendmsg()   smm_To NULL, smm_Msg the caller's
 *
 * So the two are never both set, and that is what the probe asserts. "Exactly
 * one is always set" is stronger than the document states, and a plain send()
 * has nothing to put in either field.
 *
 * smm_Buffer is the caller's buffer for send()/sendto() and NULL for
 * sendmsg(), where the buffers are the msghdr's scatter list. smm_Len is the
 * total either way.
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

    /* Connected: the zone is the one connect() was given. */
    return bsd_send_iov(SocketBase, sock, &iov, 1, len, flags,
                        &sock->as_PeerAddr, sock->as_PeerPort,
                        sock->as_ScopeId, &sock->as_CmsgSticky);
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
            scope = sock->as_ScopeId;
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

    /* A source-address buffer without its value-result length cannot be
       filled.  Reject it before receiving anything so a bad argument does
       not silently consume a datagram. */
    if (addr != NULL && addrlen == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if ((flags & MSG_OOB) != 0)
        return bsd_recv_oob(SocketBase, sock, (UBYTE *)buf, len);

    iov.iov_base = buf;
    iov.iov_len  = (size_t)len;

    return bsd_recv_iov(SocketBase, sock, &iov, 1, len, flags, addr, addrlen,
                        NULL, NULL);
}

/* ------------------------------------------------------ sendmsg / recvmsg, */

/*
 * msg_control carries the RFC 3542 subset in cmsg.c: PKTINFO and HOPLIMIT in,
 * PKTINFO out. That file has the shapes and the option numbers.
 *
 * SCM_RIGHTS has no meaning here: it passes a file descriptor, and a socket
 * goes to another task on AmigaOS through ObtainSocket()/ReleaseSocket()
 * (handoff.c). It is refused rather than ignored, along with every other
 * ancillary type this library does not implement. bsd_cmsg_parse() answers
 * EINVAL, so a caller that asks for something it will not get is told so.
 *
 * MSG_CTRUNC means the caller's msg_control was too small for what was going
 * into it.
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

    /* MSG_OOB is a send()/sendto() shape here: the urgent byte is the last one
       written, and a caller can do that with one more send(). */
    if ((flags & MSG_OOB) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if (msg == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    total = bsd_iov_total(msg->msg_iov, (LONG)msg->msg_iovlen);
    if (total < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    {
        /* smm_Buffer is NULL here: the data is the msghdr's scatter list, and
           a pointer to the first iovec describes only part of the send. */
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
            scope = sock->as_ScopeId;
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
