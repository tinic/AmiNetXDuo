/*
 * bsdsocket.library, SOCK_RAW.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#ifdef AMINETXDUO_IPV6
#include "nx_ip.h"
#include "nx_ipv6.h"
#include "../ipv6/ipv6_srcsel.h"
#endif

#include <proto/exec.h>

/*
 * How many datagrams one raw socket can hold un-read.
 */
#define BSD_RAW_QUEUE_MIN        4
#define BSD_RAW_QUEUE_CEILING   16
#define BSD_RAW_POOL_SHARE      16      /* 1/N of the pool per socket        */

/* Bytes of IPv4 header we are prepared to wind back over (5..15 words). */
#define BSD_RAW_MAX_IPHDR       60

/* The fixed IPv6 header. Extension headers sit behind it and are not read. */
#define BSD_RAW_IPV6_HDR        40

/*
 * Every open raw descriptor on the machine, in one list.
 */
static AmiSocket *bsd_raw_list;
static ULONG      bsd_raw_installed;

static ULONG bsd_raw_queue_max(VOID)
{
    NX_PACKET_POOL *pool = netstack_pool();
    ULONG           queue;

    if (pool == NULL)
        return BSD_RAW_QUEUE_MIN;

    queue = pool->nx_packet_pool_total / BSD_RAW_POOL_SHARE;

    if (queue < BSD_RAW_QUEUE_MIN)
        queue = BSD_RAW_QUEUE_MIN;
    if (queue > BSD_RAW_QUEUE_CEILING)
        queue = BSD_RAW_QUEUE_CEILING;

    return queue;
}

/* Caller holds nx_ip_protection. */
static VOID bsd_raw_flush(AmiSocket *sock)
{
    while (sock->as_RawHead != NX_NULL)
    {
        NX_PACKET *packet = sock->as_RawHead;

        sock->as_RawHead = packet->nx_packet_queue_next;
        nx_packet_release(packet);
    }

    sock->as_RawTail  = NX_NULL;
    sock->as_RawCount = 0;
}

/*
 * Does an inbound datagram belong to a connected raw socket?
 */
static BOOL bsd_raw_from_peer(const AmiSocket *sock, const NX_PACKET *packet,
                              BOOL is_v6)
{
    const UBYTE *header = packet->nx_packet_ip_header;

    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return TRUE;

#ifdef AMINETXDUO_IPV6
    if (is_v6)
    {
        NXD_ADDRESS source;

        if (sock->as_PeerAddr.nxd_ip_version != NX_IP_VERSION_V6)
            return FALSE;

        source.nxd_ip_version = NX_IP_VERSION_V6;
        bsd_in6_to_words(&header[8], source.nxd_ip_address.v6);

        if (source.nxd_ip_address.v6[0] !=
                sock->as_PeerAddr.nxd_ip_address.v6[0] ||
            source.nxd_ip_address.v6[1] !=
                sock->as_PeerAddr.nxd_ip_address.v6[1] ||
            source.nxd_ip_address.v6[2] !=
                sock->as_PeerAddr.nxd_ip_address.v6[2] ||
            source.nxd_ip_address.v6[3] !=
                sock->as_PeerAddr.nxd_ip_address.v6[3])
            return FALSE;

        if (sock->as_PeerScopeId != 0UL &&
            anx6_scope(source.nxd_ip_address.v6) < 0xEU &&
            !(source.nxd_ip_address.v6[0] == 0UL &&
              source.nxd_ip_address.v6[1] == 0UL &&
              source.nxd_ip_address.v6[2] == 0UL &&
              source.nxd_ip_address.v6[3] == 1UL))
        {
            const NXD_IPV6_ADDRESS *matched =
                packet->nx_packet_address.nx_packet_ipv6_address_ptr;
            const NX_INTERFACE *nxif =
                (matched != NX_NULL)
                    ? matched->nxd_ipv6_address_attached : NX_NULL;

            if (nxif == NX_NULL ||
                (ULONG)nxif->nx_interface_index + 1UL !=
                    sock->as_PeerScopeId)
                return FALSE;
        }

        return TRUE;
    }
#else
    (VOID)is_v6;
#endif

    if (sock->as_PeerAddr.nxd_ip_version != NX_IP_VERSION_V4)
        return FALSE;

    return (((ULONG)header[12] << 24) |
            ((ULONG)header[13] << 16) |
            ((ULONG)header[14] <<  8) |
             (ULONG)header[15]) == sock->as_PeerAddr.nxd_ip_address.v4
               ? TRUE : FALSE;
}

/*
 * Does this datagram belong to the address a raw socket bound?
 */
static BOOL bsd_raw_to_local(const AmiSocket *sock, const NX_PACKET *packet,
                             BOOL is_v6)
{
    const UBYTE        *header = packet->nx_packet_ip_header;
    const NX_INTERFACE *nxif;

#ifdef AMINETXDUO_IPV6
    if (is_v6)
    {
        const NXD_IPV6_ADDRESS *matched =
            packet->nx_packet_address.nx_packet_ipv6_address_ptr;
        ULONG destination[4];

        if (sock->as_LocalAddr.nxd_ip_version != NX_IP_VERSION_V6)
            return FALSE;

        nxif = (matched != NX_NULL) ? matched->nxd_ipv6_address_attached
                                    : NX_NULL;

        if (!bsd_bind_wants_interface(sock, nxif))
            return FALSE;

        if ((sock->as_LocalAddr.nxd_ip_address.v6[0] |
             sock->as_LocalAddr.nxd_ip_address.v6[1] |
             sock->as_LocalAddr.nxd_ip_address.v6[2] |
             sock->as_LocalAddr.nxd_ip_address.v6[3]) == 0UL)
            return TRUE;

        bsd_in6_to_words(&header[24], destination);

        return (destination[0] == sock->as_LocalAddr.nxd_ip_address.v6[0] &&
                destination[1] == sock->as_LocalAddr.nxd_ip_address.v6[1] &&
                destination[2] == sock->as_LocalAddr.nxd_ip_address.v6[2] &&
                destination[3] == sock->as_LocalAddr.nxd_ip_address.v6[3])
                   ? TRUE : FALSE;
    }
#else
    (VOID)is_v6;
#endif

    if (sock->as_LocalAddr.nxd_ip_version != NX_IP_VERSION_V4)
        return FALSE;

    nxif = packet->nx_packet_ip_interface;
    if (!bsd_bind_wants_interface(sock, nxif))
        return FALSE;

    if (sock->as_LocalAddr.nxd_ip_address.v4 == 0UL)
        return TRUE;

    return ((((ULONG)header[16] << 24) |
             ((ULONG)header[17] << 16) |
             ((ULONG)header[18] <<  8) |
              (ULONG)header[19]) == sock->as_LocalAddr.nxd_ip_address.v4)
               ? TRUE : FALSE;
}

static BOOL bsd_raw_accepts_packet(const AmiSocket *sock,
                                   const NX_PACKET *packet)
{
    BOOL is_v6 = FALSE;

#ifdef AMINETXDUO_IPV6
    if (packet->nx_packet_ip_version == NX_IP_VERSION_V6)
        is_v6 = TRUE;
#endif

    return (bsd_raw_to_local(sock, packet, is_v6) &&
            bsd_raw_from_peer(sock, packet, is_v6)) ? TRUE : FALSE;
}

/*
 * The tee.  Called from _nx_ip_raw_packet_processing() for every inbound IP
 * packet, on the IP thread, with `protocol` already reduced to the plain IP
 * protocol number.  Always returns "not mine".
 */
static UINT bsd_raw_filter(NX_IP *ip_ptr, ULONG protocol, NX_PACKET *packet_ptr)
{
    NX_PACKET_POOL *pool;
    AmiSocket      *sock;
    ULONG           behind;
    ULONG           hdr_len;
    BOOL            is_v6;

    if (packet_ptr == NX_NULL || bsd_raw_list == NULL)
        return NX_NOT_SUCCESSFUL;

    is_v6 = FALSE;

#ifdef AMINETXDUO_IPV6
    if (packet_ptr->nx_packet_ip_version == NX_IP_VERSION_V6)
        is_v6 = TRUE;
    else
#endif
    if (packet_ptr->nx_packet_ip_version != NX_IP_VERSION_V4)
        return NX_NOT_SUCCESSFUL;

    if (packet_ptr->nx_packet_ip_header == NX_NULL ||
        packet_ptr->nx_packet_prepend_ptr < packet_ptr->nx_packet_ip_header)
        return NX_NOT_SUCCESSFUL;

    behind = (ULONG)(packet_ptr->nx_packet_prepend_ptr -
                     packet_ptr->nx_packet_ip_header);

    if (is_v6)
    {
        if (behind < BSD_RAW_IPV6_HDR)
            return NX_NOT_SUCCESSFUL;

        hdr_len = 0;
    }
    else
    {
        if (behind < 20UL || behind > BSD_RAW_MAX_IPHDR)
            return NX_NOT_SUCCESSFUL;

        hdr_len = behind;
    }

    pool = netstack_pool();
    if (pool == NULL)
        return NX_NOT_SUCCESSFUL;

    tx_mutex_get(&ip_ptr->nx_ip_protection, TX_WAIT_FOREVER);

    for (sock = bsd_raw_list; sock != NULL; sock = sock->as_RawNext)
    {
        NX_PACKET *copy = NX_NULL;
        UINT       status;

        if ((ULONG)sock->as_Protocol != protocol)
            continue;

        if (((sock->as_Flags & ASF_INET6) != 0) != (is_v6 != FALSE))
            continue;

        if (!bsd_raw_to_local(sock, packet_ptr, is_v6))
            continue;

        if (!bsd_raw_from_peer(sock, packet_ptr, is_v6))
            continue;

        if (sock->as_RawCount >= sock->as_RawMax)
            continue;                   /* the reader is behind */

#ifdef AMINETXDUO_IPV6
        if (is_v6 && protocol == (ULONG)NX_PROTOCOL_ICMPV6 &&
            packet_ptr->nx_packet_append_ptr > packet_ptr->nx_packet_prepend_ptr)
        {
            ULONG icmp_type = (ULONG)packet_ptr->nx_packet_prepend_ptr[0];

            if ((sock->as_Icmp6Filter[icmp_type >> 5] &
                 (1UL << (icmp_type & 31))) == 0UL)
                continue;
        }
#endif

        /*
         * Wind back over the IP header for the duration of the copy, then
         * restore it exactly: this packet is declined, so the stack goes on to
         * process it and every pointer must be as it was.
         */
        packet_ptr->nx_packet_prepend_ptr -= hdr_len;
        packet_ptr->nx_packet_length      += hdr_len;

        status = nx_packet_copy(packet_ptr, &copy, pool, NX_NO_WAIT);

        packet_ptr->nx_packet_prepend_ptr += hdr_len;
        packet_ptr->nx_packet_length      -= hdr_len;

        if (status != NX_SUCCESS || copy == NX_NULL)
            continue;                   /* pool pressure, drop as BSD does */

        copy->nx_packet_queue_next = NX_NULL;

        if (sock->as_RawTail != NX_NULL)
            sock->as_RawTail->nx_packet_queue_next = copy;
        else
            sock->as_RawHead = copy;

        sock->as_RawTail = copy;
        sock->as_RawCount++;

        if (sock->as_RawSemOk)
            tx_semaphore_put(&sock->as_RawSem);

        bsd_event_post(sock, FD_READ);
    }

    tx_mutex_put(&ip_ptr->nx_ip_protection);

    return NX_NOT_SUCCESSFUL;
}

LONG bsd_raw_open(struct AmiSocketBase *base, AmiSocket *sock)
{
    NX_IP *ip = netstack_ip();
    UINT   status;

    if (ip == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    if (tx_semaphore_create(&sock->as_RawSem, (CHAR *)"AmiNetXDuo RAW", 0)
            != TX_SUCCESS)
        return bsd_fail(base, AMI_ENOBUFS);

    sock->as_RawSemOk = TRUE;
    sock->as_RawMax   = bsd_raw_queue_max();

    tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);

    if (bsd_raw_installed == 0)
    {
        /*
         * The hook at the top of _nx_ip_dispatch_process() needs both the raw
         * service enabled (nx_ip_raw_ip_processing) and a filter installed.
         */
        status = nx_ip_raw_packet_enable(ip);
        if (status != NX_SUCCESS && status != NX_ALREADY_ENABLED)
        {
            tx_mutex_put(&ip->nx_ip_protection);
            tx_semaphore_delete(&sock->as_RawSem);
            sock->as_RawSemOk = FALSE;
            return bsd_fail(base, bsd_errno_from_nx(status));
        }

        status = nx_ip_raw_packet_filter_set(ip, bsd_raw_filter);
        if (status != NX_SUCCESS)
        {
            nx_ip_raw_packet_disable(ip);
            tx_mutex_put(&ip->nx_ip_protection);
            tx_semaphore_delete(&sock->as_RawSem);
            sock->as_RawSemOk = FALSE;
            return bsd_fail(base, bsd_errno_from_nx(status));
        }
    }

    sock->as_RawNext = bsd_raw_list;
    bsd_raw_list     = sock;
    bsd_raw_installed++;

    tx_mutex_put(&ip->nx_ip_protection);

    return 0;
}

VOID bsd_raw_close(AmiSocket *sock)
{
    NX_IP      *ip = netstack_ip();
    AmiSocket **link;

    if ((sock->as_Flags & ASF_RAW) == 0)
        return;

    if (ip != NULL)
    {
        tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);

        for (link = &bsd_raw_list; *link != NULL; link = &(*link)->as_RawNext)
        {
            if (*link == sock)
            {
                *link = sock->as_RawNext;

                if (bsd_raw_installed > 0)
                    bsd_raw_installed--;

                break;
            }
        }

        sock->as_RawNext = NULL;
        bsd_raw_flush(sock);

        if (bsd_raw_installed == 0)
        {
            nx_ip_raw_packet_filter_set(ip, NX_NULL);
            nx_ip_raw_packet_disable(ip);
        }

        tx_mutex_put(&ip->nx_ip_protection);
    }
    else
    {
        Forbid();

        for (link = &bsd_raw_list; *link != NULL; link = &(*link)->as_RawNext)
        {
            if (*link == sock)
            {
                *link = sock->as_RawNext;

                if (bsd_raw_installed > 0)
                    bsd_raw_installed--;

                break;
            }
        }

        Permit();

        sock->as_RawHead  = NX_NULL;
        sock->as_RawTail  = NX_NULL;
        sock->as_RawCount = 0;
        sock->as_RawNext  = NULL;
    }

    if (sock->as_RawSemOk)
    {
        tx_semaphore_delete(&sock->as_RawSem);
        sock->as_RawSemOk = FALSE;
    }
}

#ifdef AMINETXDUO_IPV6

/*
 * The ICMPv6 checksum, which RFC 4443 makes mandatory and RFC 3542 makes the
 * stack's job on a raw socket. It covers the pseudo-header, so the caller
 * cannot compute it: nothing above this line knows which of the machine's
 * addresses the packet will leave with.
 */
static VOID bsd_raw_icmpv6_checksum(NX_PACKET *packet, ULONG *source,
                                    ULONG *dest)
{
    UBYTE *icmp = packet->nx_packet_prepend_ptr;
    USHORT sum;

    if ((ULONG)(packet->nx_packet_append_ptr - icmp) < 4UL)
        return;

    icmp[2] = 0;
    icmp[3] = 0;

    /* The version decides how much of the pseudo-header goes into the sum:
       _nx_ip_checksum_compute() folds in four bytes of each address unless the
       packet says IPv6. The send path sets this again on its way out. */
    packet->nx_packet_ip_version = NX_IP_VERSION_V6;

    sum = _nx_ip_checksum_compute(packet, NX_PROTOCOL_ICMPV6,
                                  (UINT)packet->nx_packet_length,
                                  source, dest);
    sum = (USHORT)~sum;

    icmp[2] = (UBYTE)(sum >> 8);
    icmp[3] = (UBYTE)(sum & 0xFF);
}

/*
 * The source address is picked first and then named on the send, rather than
 * left to nxd_ip_raw_packet_send() to pick again: the checksum below is
 * computed over it, and a second independent choice can differ.
 */
static LONG bsd_raw_send_v6(struct AmiSocketBase *base, AmiSocket *sock,
                            NX_IP *ip, NX_PACKET *packet, NXD_ADDRESS *dest,
                            ULONG protocol, UINT hops, ULONG tos, ULONG scope,
                            const BsdCmsgSource *src)
{
    NXD_IPV6_ADDRESS *source = NX_NULL;
    UINT              src_index = 0;
    UINT              status;
    BsdSourceKind     kind;

    if (src != NULL && src->cs_Have)
    {
        LONG index = bsd_cmsg_source_index(ip, src, TRUE);

        if (index < 0)
        {
            nx_packet_release(packet);
            return bsd_fail(base, AMI_EADDRNOTAVAIL);
        }

        src_index = (UINT)index;
        kind      = BSD_SOURCE_INDEX;
    }
    else
    {
        kind = bsd_source_select(sock, dest, scope, &src_index);
    }

    switch (kind)
    {
        case BSD_SOURCE_INDEX:
            source = &ip->nx_ipv6_address[src_index];
            break;

        case BSD_SOURCE_ROUTE:
            tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
            status = _nxd_ipv6_interface_find(ip, dest->nxd_ip_address.v6,
                                              &source, NX_NULL);
            tx_mutex_put(&ip->nx_ip_protection);

            if (status != NX_SUCCESS || source == NX_NULL)
            {
                nx_packet_release(packet);
                return bsd_fail(base, AMI_ENETUNREACH);
            }
            break;

        case BSD_SOURCE_UNREACH:
            nx_packet_release(packet);
            return bsd_fail(base, AMI_ENETUNREACH);

        case BSD_SOURCE_REFUSE:
        default:
            nx_packet_release(packet);
            return bsd_fail(base, AMI_EADDRNOTAVAIL);
    }

    if (protocol == (ULONG)NX_PROTOCOL_ICMPV6)
    {
        bsd_raw_icmpv6_checksum(packet, source->nxd_ipv6_address,
                                dest->nxd_ip_address.v6);
    }

    status = nxd_ip_raw_packet_source_send(ip, packet, dest,
                                           (UINT)source->nxd_ipv6_address_index,
                                           protocol, hops, tos);
    if (status != NX_SUCCESS)
    {
        nx_packet_release(packet);

        return bsd_fail(base,
                        (status == NX_IP_ADDRESS_ERROR ||
                         status == NX_NO_INTERFACE_ADDRESS)
                            ? AMI_ENETUNREACH
                            : bsd_errno_from_nx(status));
    }

    return 0;
}

#endif /* AMINETXDUO_IPV6 */

LONG bsd_raw_send_packet(struct AmiSocketBase *base, AmiSocket *sock,
                         NX_PACKET *packet, const NXD_ADDRESS *addr,
                         ULONG scope, const BsdCmsgSource *src)
{
    NX_IP        *ip     = netstack_ip();
    NX_PACKET    *handed = packet;
    NXD_ADDRESS   dest   = *addr;
    BsdSourceKind source;
    UINT          src_index = 0;
    UINT          status;
    ULONG         protocol = (ULONG)(sock->as_Protocol & 0xFF);
    UINT          ttl      = (UINT)(sock->as_Ttl & 0xFF);
    ULONG         tos      = (ULONG)(sock->as_Tos & 0xFF);

    /* RFC 3542 6.3, over IPV6_UNICAST_HOPS. IPv6 only, which is where
       bsd_cmsg_parse() already refuses it for an AF_INET socket. */
    if (src != NULL && src->cs_HaveHops)
        ttl = (UINT)src->cs_Hops;

#ifndef AMINETXDUO_IPV6
    (VOID)scope;
#endif

    if (ip == NULL)
    {
        nx_packet_release(packet);
        return bsd_fail(base, AMI_ENETDOWN);
    }

#ifdef AMINETXDUO_IPV6
    if (dest.nxd_ip_version == NX_IP_VERSION_V6)
        return bsd_raw_send_v6(base, sock, ip, handed, &dest, protocol, ttl,
                               tos, scope, src);
#endif

    /*
     * IP_HDRINCL is translated rather than passed through.
     */
    if (sock->as_HdrIncl)
    {
        const UBYTE *hdr = (const UBYTE *)handed->nx_packet_prepend_ptr;
        ULONG        have = (ULONG)(handed->nx_packet_append_ptr -
                                    handed->nx_packet_prepend_ptr);
        ULONG        ihl;

        if (have < 20UL || (hdr[0] >> 4) != 4)
        {
            nx_packet_release(handed);
            return bsd_fail(base, AMI_EINVAL);
        }

        ihl = (ULONG)(hdr[0] & 0x0F) * 4UL;
        if (ihl < 20UL || ihl > have)
        {
            nx_packet_release(handed);
            return bsd_fail(base, AMI_EINVAL);
        }

        tos      = (ULONG)hdr[1];
        ttl      = (UINT)hdr[8];
        protocol = (ULONG)hdr[9];

        /*
         * The destination in the header wins over the one passed to sendto(),
         * which is what "the header is included" means. traceroute fills both
         * in agreement anyway.
         */
        dest.nxd_ip_version = NX_IP_VERSION_V4;
        dest.nxd_ip_address.v4 = ((ULONG)hdr[16] << 24) |
                                 ((ULONG)hdr[17] << 16) |
                                 ((ULONG)hdr[18] <<  8) |
                                  (ULONG)hdr[19];

        handed->nx_packet_prepend_ptr += ihl;
        handed->nx_packet_length      -= ihl;
    }

    if (src != NULL && src->cs_Have)
    {
        LONG index = bsd_cmsg_source_index(ip, src, FALSE);

        if (index < 0)
        {
            nx_packet_release(handed);
            return bsd_fail(base, AMI_EADDRNOTAVAIL);
        }

        source    = BSD_SOURCE_INDEX;
        src_index = (UINT)index;
    }
    else
    {
        source = bsd_source_select(sock, &dest, 0UL, &src_index);
    }

    if (source == BSD_SOURCE_REFUSE || source == BSD_SOURCE_UNREACH)
    {
        nx_packet_release(handed);
        return bsd_fail(base, (source == BSD_SOURCE_UNREACH)
                                  ? AMI_ENETUNREACH
                                  : AMI_EADDRNOTAVAIL);
    }

    {
        const NX_INTERFACE *source_interface = NX_NULL;
        LONG                mtu;
        ULONG overhead = (dest.nxd_ip_version == NX_IP_VERSION_V6)
                             ? 40UL : 20UL;

        if (source == BSD_SOURCE_INDEX)
        {
#ifdef AMINETXDUO_IPV6
            if (dest.nxd_ip_version == NX_IP_VERSION_V6)
                source_interface = ip->nx_ipv6_address[src_index]
                                         .nxd_ipv6_address_attached;
            else
#endif
                source_interface = &ip->nx_ip_interface[src_index];
        }

        mtu = bsd_route_mtu(ip, &dest, source_interface);

        if (mtu >= 0 && handed->nx_packet_length + overhead > (ULONG)mtu)
        {
            nx_packet_release(handed);
            return bsd_fail(base, AMI_EMSGSIZE);
        }
    }

    if (source == BSD_SOURCE_INDEX)
        status = nxd_ip_raw_packet_source_send(ip, handed, &dest, src_index,
                                               protocol, ttl, tos);
    else
        status = nxd_ip_raw_packet_send(ip, handed, &dest, protocol, ttl, tos);

    if (status != NX_SUCCESS)
    {
        /* The error-checking wrapper only clears the caller's pointer on
           success, so on failure the packet is still ours. */
        if (handed != NX_NULL)
            nx_packet_release(handed);

        return bsd_fail(base,
                        (status == NX_IP_ADDRESS_ERROR ||
                         status == NX_NO_INTERFACE_ADDRESS)
                            ? AMI_ENETUNREACH
                            : bsd_errno_from_nx(status));
    }

    return 0;
}

NX_PACKET *bsd_raw_receive(AmiSocket *sock, ULONG wait, UINT *why)
{
    NX_IP *ip = netstack_ip();

    if (why != NULL)
        *why = NX_NO_PACKET;

    if (ip == NULL)
    {
        if (why != NULL)
            *why = NX_NOT_ENABLED;

        return NX_NULL;
    }

    for (;;)
    {
        NX_PACKET *packet;
        UINT       status;

        tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);

        packet = sock->as_RawHead;
        if (packet != NX_NULL)
        {
            sock->as_RawHead = packet->nx_packet_queue_next;
            if (sock->as_RawHead == NX_NULL)
                sock->as_RawTail = NX_NULL;
            if (sock->as_RawCount > 0)
                sock->as_RawCount--;

            packet->nx_packet_queue_next = NX_NULL;
        }

        tx_mutex_put(&ip->nx_ip_protection);

        if (packet != NX_NULL)
        {
            if (sock->as_RawSemOk)
                (VOID)tx_semaphore_get(&sock->as_RawSem, TX_NO_WAIT);

            return packet;
        }

        if (wait == NX_NO_WAIT || !sock->as_RawSemOk)
            return NX_NULL;

        /* Outside the mutex, or the IP thread can never queue anything. */
        status = tx_semaphore_get(&sock->as_RawSem, wait);
        if (status != TX_SUCCESS)
        {
            if (why != NULL)
                *why = (status == TX_WAIT_ABORTED) ? NX_WAIT_ABORTED
                                                   : NX_NO_PACKET;

            return NX_NULL;
        }
    }
}

VOID bsd_raw_source(NX_PACKET *packet, NXD_ADDRESS *addr)
{
    const UBYTE *ip_hdr;
    ULONG        source = 0;

    bsd_addr_from_v4(addr, 0UL);

    if (packet == NX_NULL)
        return;

#ifdef AMINETXDUO_IPV6
    /*
     * An IPv6 datagram does not start at its header, so the source comes from
     * where nx_packet_copy() recorded it: bytes 8..23 of the fixed header.
     */
    if (packet->nx_packet_ip_version == NX_IP_VERSION_V6)
    {
        const UBYTE *v6_hdr = packet->nx_packet_ip_header;

        if (v6_hdr == NX_NULL ||
            (ULONG)(packet->nx_packet_prepend_ptr - v6_hdr) < BSD_RAW_IPV6_HDR)
            return;

        addr->nxd_ip_version = NX_IP_VERSION_V6;
        bsd_in6_to_words(&v6_hdr[8], addr->nxd_ip_address.v6);

        return;
    }
#endif

    /*
     * The datagram starts at its own IP header, see the file comment. The
     * source address is bytes 12..15, in network order, which on m68k is the
     */
    if ((ULONG)(packet->nx_packet_append_ptr -
                packet->nx_packet_prepend_ptr) < 20UL)
        return;

    ip_hdr = packet->nx_packet_prepend_ptr;

    source = ((ULONG)ip_hdr[12] << 24) | ((ULONG)ip_hdr[13] << 16) |
             ((ULONG)ip_hdr[14] <<  8) | (ULONG)ip_hdr[15];

    bsd_addr_from_v4(addr, source);
}

ULONG bsd_raw_available(AmiSocket *sock)
{
    ULONG length = 0;

    if (sock->as_RawHead != NX_NULL)
        nx_packet_length_get(sock->as_RawHead, &length);

    return length;
}

/* A raw bind or connect changes the receive PCB. The IP hook filtered queued
   copies against the endpoint that was current when they arrived, so discard
   any that the new endpoint no longer admits. Called inside a ThreadX bracket,
   while the IP thread cannot change this queue. */
VOID bsd_raw_revalidate_endpoint(AmiSocket *sock)
{
    NX_PACKET *packet;
    NX_PACKET *next;
    NX_PACKET *head = NX_NULL;
    NX_PACKET *tail = NX_NULL;
    ULONG      count = 0;

    if (sock->as_RxPending != NX_NULL &&
        !bsd_raw_accepts_packet(sock, sock->as_RxPending))
    {
        nx_packet_release(sock->as_RxPending);
        sock->as_RxPending = NX_NULL;
        sock->as_RxOffset  = 0;
    }

    for (packet = sock->as_RawHead; packet != NX_NULL; packet = next)
    {
        next = packet->nx_packet_queue_next;
        packet->nx_packet_queue_next = NX_NULL;

        if (bsd_raw_accepts_packet(sock, packet))
        {
            if (tail != NX_NULL)
                tail->nx_packet_queue_next = packet;
            else
                head = packet;

            tail = packet;
            count++;
        }
        else
        {
            nx_packet_release(packet);

            if (sock->as_RawSemOk)
                (VOID)tx_semaphore_get(&sock->as_RawSem, TX_NO_WAIT);
        }
    }

    sock->as_RawHead  = head;
    sock->as_RawTail  = tail;
    sock->as_RawCount = count;
}
