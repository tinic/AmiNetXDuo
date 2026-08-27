/*
 * bsdsocket.library, socket lifecycle and the per-opener descriptor table.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "netmonitor.h"

#include "nx_tcp.h"

#include "nx_ip.h"
#ifdef AMINETXDUO_IPV6
#include "nx_ipv6.h"
#include "../ipv6/ipv6_srcsel.h"
#endif

#include "aminetxduo/random.h"

#include <proto/exec.h>

static char bsd_tcp_name[] = "AmiNetXDuo TCP";
static char bsd_udp_name[] = "AmiNetXDuo UDP";

/*
 * shutdown(SHUT_WR).  Call with the ThreadX scheduler lock held (bsd_nx_enter);
 * takes the IP protection mutex, as _nx_tcp_packet_send_fin() requires.
 */
static VOID bsd_tcp_send_fin(AmiSocket *sock)
{
    NX_TCP_SOCKET *tcp = &sock->as_Nx.tcp;
    NX_IP         *ip  = tcp->nx_tcp_socket_ip_ptr;

    if (ip == NX_NULL)
        return;

    tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);

    if (tcp->nx_tcp_socket_state == NX_TCP_ESTABLISHED)
    {
        tcp->nx_tcp_socket_state = NX_TCP_FIN_WAIT_1;
    }
    else if (tcp->nx_tcp_socket_state == NX_TCP_CLOSE_WAIT)
    {
        tcp->nx_tcp_socket_state = NX_TCP_LAST_ACK;
    }
    else
    {
        tx_mutex_put(&ip->nx_ip_protection);
        return;
    }

    if (tcp->nx_tcp_socket_transmit_sent_head == NX_NULL)
    {
        tcp->nx_tcp_socket_timeout         = tcp->nx_tcp_socket_timeout_rate;
        tcp->nx_tcp_socket_timeout_retries = 0;
    }

    tcp->nx_tcp_socket_tx_sequence++;
    _nx_tcp_packet_send_fin(tcp, tcp->nx_tcp_socket_tx_sequence - 1);

    tx_mutex_put(&ip->nx_ip_protection);
}

static ULONG bsd_udp_queue_max(VOID)
{
    NX_PACKET_POOL *pool = netstack_pool();
    ULONG           queue;

    if (pool == NULL)
        return BSD_UDP_QUEUE_MIN;

    queue = pool->nx_packet_pool_total / BSD_UDP_POOL_SHARE;

    if (queue < BSD_UDP_QUEUE_MIN)
        queue = BSD_UDP_QUEUE_MIN;
    if (queue > BSD_UDP_QUEUE_CEILING)
        queue = BSD_UDP_QUEUE_CEILING;

    return queue;
}

static ULONG bsd_tcp_consumer_count(NX_IP *ip)
{
    NX_TCP_SOCKET *tcp   = ip->nx_ip_tcp_created_sockets_ptr;
    ULONG          total = ip->nx_ip_tcp_created_sockets_count;
    ULONG          live  = 0;
    ULONG          i;

    for (i = 0; i < total && tcp != NX_NULL; i++)
    {
        if (tcp->nx_tcp_socket_state != NX_TCP_CLOSED &&
            tcp->nx_tcp_socket_state != NX_TCP_LISTEN_STATE)
            live++;

        tcp = tcp->nx_tcp_socket_created_next;
    }

    return live;
}

ULONG ami_bsd_tcp_window(VOID)
{
    static ULONG    last_budget = 0;
    NX_PACKET_POOL *pool = netstack_pool();
    NX_IP          *ip   = netstack_ip();
    ULONG           budget;

    if (pool == NULL || ip == NULL)
        return (ULONG)BSD_TCP_WINDOW;

    budget = ami_bsd_tcp_budget(pool->nx_packet_pool_total,
                                pool->nx_packet_pool_payload_size);

    if (budget != last_budget)
    {
        last_budget = budget;
        AMI_INFO("bsdsocket: TCP window budget %ld bytes (pool %ld packets), "
                 "%ld..%ld per socket",
                 (long)budget, (long)pool->nx_packet_pool_total,
                 (long)BSD_TCP_WINDOW, (long)BSD_TCP_WINDOW_CEILING);
    }

    return ami_bsd_tcp_window_for(pool->nx_packet_pool_total,
                                  pool->nx_packet_pool_payload_size,
                                  bsd_tcp_consumer_count(ip));
}

/*
 * SO_KEEPALIVE must default off, and NetX Duo defaults it on.
 */
static VOID bsd_tcp_keepalive_default(NX_TCP_SOCKET *tcp)
{
#ifdef NX_ENABLE_TCP_KEEPALIVE
    tcp->nx_tcp_socket_keepalive_enabled = NX_FALSE;
#else
    (VOID)tcp;
#endif
}

/*
 * Bound the receive queue in packets, sized from this socket's own window.
 */
static VOID bsd_tcp_rx_queue_cap(NX_TCP_SOCKET *tcp)
{
#ifdef NX_ENABLE_LOW_WATERMARK
    ULONG cap = tcp->nx_tcp_socket_rx_window_default / BSD_TCP_RX_MSS_REF +
                BSD_TCP_RX_QUEUE_SLACK;

    if (cap < NX_TCP_MAXIMUM_RX_QUEUE)
        cap = NX_TCP_MAXIMUM_RX_QUEUE;

    tcp->nx_tcp_socket_receive_queue_maximum = cap;
#else
    (VOID)tcp;
#endif
}

static VOID bsd_tcp_seed_isn(NX_TCP_SOCKET *tcp)
{
    ULONG seed = ami_random_ulong();

    /* Zero means "not seeded" to the code above, so a zero draw would restore
       the biased branch. One draw in 2^32, and it costs a comparison. */
    if (seed == 0)
        seed = 1;

    tcp->nx_tcp_socket_tx_sequence = seed;
}

static LONG bsd_table_ensure(struct AmiSocketBase *base)
{
    if (base->sb_Table != NULL)
        return 0;

    base->sb_Table = (AmiSocket **)ami_alloc(
        (ULONG)BSD_DEFAULT_DTABLESIZE * sizeof(AmiSocket *));
    if (base->sb_Table == NULL)
        return -1;

    base->sb_TableSize = BSD_DEFAULT_DTABLESIZE;

    return 0;
}

LONG bsd_table_size(struct AmiSocketBase *base)
{
    if (base->sb_TableSize == 0)
        return BSD_DEFAULT_DTABLESIZE;

    return base->sb_TableSize;
}

LONG bsd_table_resize(struct AmiSocketBase *base, LONG size)
{
    AmiSocket **table;
    LONG        i, copy;

    if (size < 1 || size > BSD_MAX_DTABLESIZE)
        return -1;

    if (base->sb_Table == NULL)
    {
        base->sb_Table = (AmiSocket **)ami_alloc(
            (ULONG)size * sizeof(AmiSocket *));
        if (base->sb_Table == NULL)
            return -1;

        base->sb_TableSize = size;

        return 0;
    }

    if (size == base->sb_TableSize)
        return 0;

    for (i = base->sb_TableSize - 1; i >= size; i--)
    {
        if (base->sb_Table[i] != NULL)
            return -1;
    }

    table = (AmiSocket **)ami_alloc((ULONG)size * sizeof(AmiSocket *));
    if (table == NULL)
        return -1;

    copy = (size < base->sb_TableSize) ? size : base->sb_TableSize;
    for (i = 0; i < copy; i++)
        table[i] = base->sb_Table[i];

    ami_free(base->sb_Table);
    base->sb_Table     = table;
    base->sb_TableSize = size;

    return 0;
}

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    AmiSocket *sock;

    if (base->sb_Table == NULL || fd < 0 || fd >= base->sb_TableSize)
        return NULL;

    sock = base->sb_Table[fd];

    return (sock == BSD_FD_RESERVED) ? NULL : sock;
}

BOOL bsd_fd_reserved(struct AmiSocketBase *base, LONG fd)
{
    return (BOOL)(base->sb_Table != NULL && fd >= 0 &&
                  fd < base->sb_TableSize &&
                  base->sb_Table[fd] == BSD_FD_RESERVED);
}

/*
 * SBTC_FDCALLBACK: tell the opener that a descriptor came or went.
 */
static LONG bsd_fd_callback(struct AmiSocketBase *base, LONG fd, LONG action)
{
    if (base->sb_FDCallback == NULL)
        return 0;

    return base->sb_FDCallback(fd, action);
}

LONG bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock)
{
    LONG fd;
    LONG error;

    if (bsd_table_ensure(base) != 0)
        return bsd_fail(base, AMI_EMFILE);

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        if (base->sb_Table[fd] == NULL)
        {
            if (bsd_fd_callback(base, fd, FDCB_CHECK) != 0)
            {
                base->sb_Table[fd] = BSD_FD_RESERVED;
                continue;
            }

            error = bsd_fd_callback(base, fd, FDCB_ALLOC);
            if (error != 0)
                return bsd_fail(base, error);

            base->sb_Table[fd] = sock;
            return fd;
        }
    }

    return bsd_fail(base, AMI_EMFILE);
}

LONG bsd_fd_reserve(struct AmiSocketBase *base, LONG fd)
{
    LONG error;

    if (bsd_table_ensure(base) != 0)
        return bsd_fail(base, AMI_EMFILE);

    if (fd < 0)
        return bsd_fd_alloc(base, BSD_FD_RESERVED);

    if (fd >= base->sb_TableSize || base->sb_Table[fd] != NULL)
        return bsd_fail(base, AMI_EMFILE);

    error = bsd_fd_callback(base, fd, FDCB_CHECK);
    if (error != 0)
        return bsd_fail(base, error);

    error = bsd_fd_callback(base, fd, FDCB_ALLOC);
    if (error != 0)
        return bsd_fail(base, error);

    base->sb_Table[fd] = BSD_FD_RESERVED;
    return fd;
}

LONG bsd_fd_free(struct AmiSocketBase *base, LONG fd)
{
    if (base->sb_Table != NULL && fd >= 0 && fd < base->sb_TableSize)
    {
        LONG error;

        if (base->sb_Table[fd] == NULL)
            return 0;

        error = bsd_fd_callback(base, fd, FDCB_FREE);
        if (error != 0)
            return bsd_fail(base, error);

        base->sb_Table[fd] = NULL;
    }

    return 0;
}

static AmiSocket *bsd_socket_alloc(struct AmiSocketBase *base,
                                   UWORD domain, UWORD type, LONG protocol)
{
    AmiSocket *sock = (AmiSocket *)ami_alloc(sizeof(AmiSocket));

    if (sock == NULL)
        return NULL;

    sock->as_Owner    = base;
    sock->as_RefCount = 1;
    sock->as_Domain   = domain;
    sock->as_Type     = type;
    sock->as_Protocol = protocol;
    sock->as_Ttl      = (LONG)NX_IP_TIME_TO_LIVE;

    /* Not only a zeroing: an ICMPv6 filter starts passing everything. */
    bsd_cmsg_reset(sock);

#ifdef AMINETXDUO_MULTICAST
    sock->as_McastTtl  = 1;
    sock->as_McastLoop = 1;
    sock->as_McastIf   = -1;
#ifdef AMINETXDUO_IPV6
    /* RFC 3493: one hop, same as IPv4. The NX_IP's own hop limit is 255, so
       this is applied per send rather than inherited. */
    sock->as_Mcast6Hops = 1;
    sock->as_Mcast6If   = -1;
#endif
#endif

    switch (type)
    {
        case SOCK_STREAM: sock->as_Flags = ASF_TCP; break;
        case SOCK_RAW:    sock->as_Flags = ASF_RAW; break;
        default:          sock->as_Flags = ASF_UDP; break;
    }

    /* A zeroed NXD_ADDRESS has version 0, not NX_IP_VERSION_V4; every
       "is this address unset" test checks the version tag first. */
    bsd_addr_from_v4(&sock->as_LocalAddr, 0UL);
    bsd_addr_from_v4(&sock->as_PeerAddr, 0UL);

#ifdef AMINETXDUO_IPV6
    if (domain == AF_INET6)
    {
        sock->as_Flags |= ASF_INET6;

        sock->as_LocalAddr.nxd_ip_version = NX_IP_VERSION_V6;
        sock->as_PeerAddr.nxd_ip_version  = NX_IP_VERSION_V6;

        /*
         * IPV6_V6ONLY defaults to off, so an AF_INET6 socket is dual-stack.
         */
    }
#endif

    ami_mem_socket_delta(1);

    return sock;
}

static VOID bsd_socket_dispose(AmiSocket *sock)
{
    if (sock == NULL)
        return;

    ami_mem_socket_delta(-1);
    ami_free(sock);
}

#define BSD_CLOSING_DEADLINE    (60UL * NX_IP_PERIODIC_RATE)

static AmiSocket *bsd_closing_head;

static BOOL bsd_socket_destroy(AmiSocket *sock);

static VOID bsd_closing_data_notify(NX_TCP_SOCKET *tcp)
{
    NX_TCP_HEADER header;

    /* A bare FIN queues nothing and must not be answered this way. Only
       unreadable data gets a reset. */
    if (tcp->nx_tcp_socket_receive_queue_count == 0)
        return;

    header.nx_tcp_header_word_3         = NX_TCP_ACK_BIT;
    header.nx_tcp_acknowledgment_number = tcp->nx_tcp_socket_tx_sequence;
    header.nx_tcp_sequence_number       = tcp->nx_tcp_socket_rx_sequence;
    _nx_tcp_packet_send_rst(tcp, &header);

    tcp->nx_tcp_socket_timeout_retries = tcp->nx_tcp_socket_timeout_max_retries;
    tcp->nx_tcp_socket_timeout         = 1;
}

static VOID bsd_closing_park(AmiSocket *sock)
{
    sock->as_Flags |= ASF_CLOSING;

    nx_tcp_socket_receive_notify(&sock->as_Nx.tcp, bsd_closing_data_notify);

    sock->as_Nx.tcp.nx_tcp_socket_reserved_ptr = NX_NULL;
    sock->as_Owner = NULL;

    sock->as_ClosingAt   = tx_time_get();
    sock->as_ClosingNext = bsd_closing_head;
    bsd_closing_head     = sock;
}

static VOID bsd_tcp_abort(NX_TCP_SOCKET *tcp)
{
    UINT state = tcp->nx_tcp_socket_state;

    if (state == NX_TCP_CLOSED || state == NX_TCP_LISTEN_STATE)
        return;

    if (state != NX_TCP_ESTABLISHED && state != NX_TCP_SYN_SENT &&
        state != NX_TCP_SYN_RECEIVED && state != NX_TCP_CLOSE_WAIT)
        tcp->nx_tcp_socket_state = NX_TCP_ESTABLISHED;

    nx_tcp_socket_disconnect(tcp, NX_NO_WAIT);
}

VOID bsd_closing_sweep(VOID)
{
    AmiSocket  *sock;
    AmiSocket **link = &bsd_closing_head;
    ULONG       now  = tx_time_get();

    while ((sock = *link) != NULL)
    {
        UINT state = sock->as_Nx.tcp.nx_tcp_socket_state;
        BOOL done  = (state == NX_TCP_CLOSED) || (state == NX_TCP_TIMED_WAIT) ||
                     (state == NX_TCP_LISTEN_STATE);
        BOOL late  = ((ULONG)(now - sock->as_ClosingAt) >= BSD_CLOSING_DEADLINE) ||
                     (sock->as_Nx.tcp.nx_tcp_socket_receive_queue_count != 0);

        if (!done && !late)
        {
            link = &sock->as_ClosingNext;
            continue;
        }

        *link = sock->as_ClosingNext;
        sock->as_ClosingNext = NULL;

        if (!done)
        {
            if (sock->as_Nx.tcp.nx_tcp_socket_receive_queue_count == 0)
                AMI_WARN("bsdsocket: close did not complete in %ld s "
                         "(state %ld); resetting",
                         (long)(BSD_CLOSING_DEADLINE / NX_IP_PERIODIC_RATE),
                         (long)state);

            bsd_tcp_abort(&sock->as_Nx.tcp);
        }

        if (bsd_socket_destroy(sock))
            bsd_socket_dispose(sock);
    }
}

VOID bsd_closing_drain(VOID)
{
    AmiSocket *sock;

    while ((sock = bsd_closing_head) != NULL)
    {
        bsd_closing_head     = sock->as_ClosingNext;
        sock->as_ClosingNext = NULL;

        bsd_tcp_abort(&sock->as_Nx.tcp);

        if (bsd_socket_destroy(sock))
            bsd_socket_dispose(sock);
    }
}

/*
 * Start the close. TRUE means the socket is finished with and the caller can
 * delete it now. FALSE means the FIN is in flight and the block has been
 * parked, so it is no longer the caller's to free.
 */
static BOOL bsd_tcp_close_start(AmiSocket *sock)
{
    NX_TCP_SOCKET *tcp   = &sock->as_Nx.tcp;
    UINT           state = tcp->nx_tcp_socket_state;

    if (state == NX_TCP_SYN_SENT || state == NX_TCP_SYN_RECEIVED)
    {
        nx_tcp_socket_disconnect(tcp, NX_NO_WAIT);
        return TRUE;
    }

    /* RFC 1122 4.2.2.13: unread data turns a close into an abort. */
    if (sock->as_RxPending != NULL || tcp->nx_tcp_socket_receive_queue_count != 0)
    {
        bsd_tcp_abort(tcp);
        return TRUE;
    }

    /* SO_LINGER, l_linger == 0: the documented abortive close. */
    if (sock->as_LingerOn != 0 && sock->as_LingerTime == 0)
    {
        bsd_tcp_abort(tcp);
        return TRUE;
    }

    if (sock->as_LingerOn != 0)
    {
        if (nx_tcp_socket_disconnect(tcp, (ULONG)sock->as_LingerTime *
                                              NX_IP_PERIODIC_RATE)
            != NX_NOT_CONNECTED)
            return TRUE;
    }
    else
    {
        bsd_tcp_send_fin(sock);
    }

    state = tcp->nx_tcp_socket_state;

    if (state == NX_TCP_CLOSED || state == NX_TCP_LISTEN_STATE)
        return TRUE;

    bsd_closing_park(sock);

    return FALSE;
}

/*
 * Tear the NetX Duo socket down. Safe to call more than once.
 */
static BOOL bsd_socket_destroy(AmiSocket *sock)
{
    UINT status;

    if ((sock->as_Flags & ASF_ORPHANED) != 0)
        return FALSE;                   /* already known to be un-deletable */

    if ((sock->as_Flags & ASF_DELETED) != 0)
        return TRUE;

#ifdef AMINETXDUO_MULTICAST
    bsd_mcast_close(sock);
#endif

    if ((sock->as_Flags & (ASF_TCP | ASF_RAW | ASF_CLOSING)) == ASF_TCP &&
        (sock->as_Flags & (ASF_CONNECTED | ASF_CONNECTING)) != 0)
    {
        if (!bsd_tcp_close_start(sock))
            return FALSE;
    }

    if (sock->as_RxPending != NULL)
    {
        nx_packet_release(sock->as_RxPending);
        sock->as_RxPending = NULL;
    }

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        bsd_raw_close(sock);
        sock->as_Flags |= ASF_DELETED;

        return TRUE;
    }

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        if ((sock->as_Flags & ASF_SERVER) != 0)
            nx_tcp_server_socket_unaccept(&sock->as_Nx.tcp);
        else if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_tcp_client_socket_unbind(&sock->as_Nx.tcp);

        status = nx_tcp_socket_delete(&sock->as_Nx.tcp);

        if (status == NX_STILL_BOUND)
        {
            bsd_tcp_abort(&sock->as_Nx.tcp);

            if ((sock->as_Flags & ASF_SERVER) != 0)
                nx_tcp_server_socket_unaccept(&sock->as_Nx.tcp);
            else
                nx_tcp_client_socket_unbind(&sock->as_Nx.tcp);

            status = nx_tcp_socket_delete(&sock->as_Nx.tcp);
        }
    }
    else
    {
        if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_udp_socket_unbind(&sock->as_Nx.udp);

        status = nx_udp_socket_delete(&sock->as_Nx.udp);
    }

    if (status != NX_SUCCESS && status != NX_NOT_CREATED)
    {
        AMI_WARN("bsdsocket: %s_socket_delete refused (%ld) state %ld "
                 "flags 0x%lx port %ld. It leaks %ld bytes rather than "
                 "corrupting the created list",
                 ((sock->as_Flags & ASF_TCP) != 0) ? "nx_tcp" : "nx_udp",
                 (long)status,
                 ((sock->as_Flags & ASF_TCP) != 0)
                     ? (long)sock->as_Nx.tcp.nx_tcp_socket_state : 0L,
                 (long)sock->as_Flags, (long)sock->as_LocalPort,
                 (long)sizeof(AmiSocket));

        sock->as_Flags |= ASF_ORPHANED;

        if ((sock->as_Flags & ASF_TCP) != 0)
            sock->as_Nx.tcp.nx_tcp_socket_reserved_ptr = NX_NULL;
        else
            sock->as_Nx.udp.nx_udp_socket_reserved_ptr = NX_NULL;

        sock->as_Owner = NULL;

        return FALSE;
    }

    sock->as_Flags |= ASF_DELETED;

    return TRUE;
}

VOID bsd_socket_retain(AmiSocket *sock)
{
    if (sock == NULL)
        return;

    Forbid();
    sock->as_RefCount++;
    Permit();
}

VOID bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock)
{
    ULONG remaining;

    if (sock == NULL)
        return;

    Forbid();
    if (sock->as_RefCount > 0)
        sock->as_RefCount--;
    remaining = sock->as_RefCount;

    if (remaining > 0 && sock->as_Owner == base)
        sock->as_Owner = NULL;
    Permit();

    if (remaining > 0)
    {
        return;
    }

    if ((sock->as_Flags & ASF_LISTENING) != 0)
    {
        NX_IP     *ip = netstack_ip();
        AmiSocket *p;

        for (p = sock->as_Incoming; p != NULL; p = p->as_IncomingNext)
            nx_tcp_socket_disconnect(&p->as_Nx.tcp, NX_NO_WAIT);

        if (ip != NULL)
            nx_tcp_server_socket_unlisten(ip, sock->as_ListenPort);

        sock->as_Flags &= ~ASF_LISTENING;
    }

    while (sock->as_Incoming != NULL)
    {
        AmiSocket *victim = sock->as_Incoming;

        sock->as_Incoming = victim->as_IncomingNext;
        victim->as_IncomingNext = NULL;
        if (sock->as_IncomingCount != 0)
            sock->as_IncomingCount--;

        if (bsd_socket_destroy(victim))
            bsd_socket_dispose(victim);
    }

    if (bsd_socket_destroy(sock))
        bsd_socket_dispose(sock);
}

VOID bsd_close_all(struct AmiSocketBase *base)
{
    LONG fd;
    BOOL bracketed;

    if (base->sb_Table == NULL)
        return;

    bracketed = (bsd_nx_enter(base) == 0);
    if (!bracketed)
    {
        AMI_WARN("bsdsocket: CloseLibrary with the kernel down. "
                 "Sockets are left to the stack teardown");
    }

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        AmiSocket *sock = base->sb_Table[fd];

        if (sock == NULL)
            continue;

        if (bsd_fd_free(base, fd) != 0)
            base->sb_Table[fd] = NULL;

        if (bracketed && sock != BSD_FD_RESERVED)
            bsd_socket_release(base, sock);
    }

    if (!bracketed)
        return;

    bsd_closing_sweep();

    if (base->sb_Master != NULL && base->sb_Master->sb_StackRefs <= 1)
        bsd_closing_drain();

    bsd_nx_leave(base);
}

VOID bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4)
{
    addr->nxd_ip_version       = NX_IP_VERSION_V4;
    addr->nxd_ip_address.v4    = v4;
}

/*
 * Work out a sockaddr's family. Cannot be done by reading a struct member: the
 * two structs this NDK offers do not agree on where the family lives.
 */
LONG bsd_sa_family(const struct sockaddr *sa, socklen_t len)
{
    const UBYTE *b = (const UBYTE *)sa;

    if (sa == NULL)
        return -1;

#ifdef AMINETXDUO_IPV6
    if (len >= (socklen_t)sizeof(struct sockaddr_in6) && b[0] == AF_INET6)
        return AF_INET6;
#endif

    if (len < (socklen_t)sizeof(struct sockaddr_in))
        return -1;

    if (b[1] == AF_INET)
        return AF_INET;

    if (b[1] == AF_UNSPEC && b[0] <= (UBYTE)sizeof(struct sockaddr_in))
        return AF_UNSPEC;

    return -1;
}

LONG bsd_sockaddr_get(struct AmiSocketBase *base, const struct sockaddr *sa,
                      socklen_t len, NXD_ADDRESS *addr, UINT *port,
                      ULONG *scope_id)
{
    LONG family;

    if (scope_id != NULL)
        *scope_id = 0;

    if (sa == NULL)
        return bsd_fail(base, AMI_EFAULT);

    family = bsd_sa_family(sa, len);

    if (family == -1)
    {
        return bsd_fail(base,
                        (len < (socklen_t)sizeof(struct sockaddr_in))
                            ? AMI_EINVAL : AMI_EAFNOSUPPORT);
    }

#ifdef AMINETXDUO_IPV6
    if (family == AF_INET6)
    {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;

        addr->nxd_ip_version = NX_IP_VERSION_V6;
        bsd_in6_to_words(sin6->sin6_addr.s6_addr, addr->nxd_ip_address.v6);

        *port = (UINT)BSD_NTOHS(sin6->sin6_port);

        if (scope_id != NULL)
            *scope_id = (ULONG)sin6->sin6_scope_id;

        return 0;
    }
#endif

    {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

        bsd_addr_from_v4(addr, BSD_NTOHL(sin->sin_addr.s_addr));
        *port = (UINT)BSD_NTOHS(sin->sin_port);
    }

    return 0;
}

VOID bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                      socklen_t *len, const NXD_ADDRESS *addr, UINT port,
                      ULONG scope_id)
{
    socklen_t copy;

    if (sa == NULL || len == NULL)
        return;

#ifdef AMINETXDUO_IPV6
    if (sock != NULL && (sock->as_Flags & ASF_INET6) != 0)
    {
        struct sockaddr_in6 sin6;
        NXD_ADDRESS         mapped;
        const NXD_ADDRESS  *use = addr;

        if (addr->nxd_ip_version == NX_IP_VERSION_V4)
        {
            bsd_addr_to_v4mapped(&mapped, addr->nxd_ip_address.v4);
            use = &mapped;
        }

        bsd_bzero(&sin6, sizeof(sin6));
        /* No sin6_len field in this NDK's sockaddr_in6, see the note in
           bsdsocket_internal.h. Setting one would corrupt sin6_family. */
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = (in_port_t)BSD_HTONS((UWORD)port);
        bsd_words_to_in6(use->nxd_ip_address.v6, sin6.sin6_addr.s6_addr);
        sin6.sin6_scope_id = scope_id;

        copy = *len;
        if (copy > (socklen_t)sizeof(sin6))
            copy = (socklen_t)sizeof(sin6);

        bsd_bcopy(&sin6, sa, (ULONG)copy);
        *len = (socklen_t)sizeof(struct sockaddr_in6);

        return;
    }
#else
    (VOID)sock;
    (VOID)scope_id;
#endif

    {
        struct sockaddr_in sin;

        bsd_bzero(&sin, sizeof(sin));
        sin.sin_len    = (UBYTE)sizeof(struct sockaddr_in);
        sin.sin_family = AF_INET;
        sin.sin_port   = (in_port_t)BSD_HTONS((UWORD)port);
        sin.sin_addr.s_addr =
            BSD_HTONL((addr->nxd_ip_version == NX_IP_VERSION_V4)
                          ? addr->nxd_ip_address.v4 : 0UL);

        copy = *len;
        if (copy > (socklen_t)sizeof(sin))
            copy = (socklen_t)sizeof(sin);

        bsd_bcopy(&sin, sa, (ULONG)copy);
        *len = (socklen_t)sizeof(struct sockaddr_in);
    }
}

LONG bsd_socket(register LONG domain   __asm("d0"),
                register LONG type     __asm("d1"),
                register LONG protocol __asm("d2"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock;
    NX_IP     *ip = netstack_ip();
    UINT       status;
    LONG       fd;

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

#ifdef AMINETXDUO_IPV6
    if (domain != AF_INET && domain != AF_INET6)
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

    if (domain == AF_INET6 && !netstack_ipv6_enabled())
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
#else
    if (domain != AF_INET)
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
#endif

    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);

    if (type == SOCK_RAW)
    {
        if (protocol <= 0 || protocol > 255)
            return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);
    }
    else if (protocol != 0 &&
             protocol != ((type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP))
    {
        return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);
    }

    sock = bsd_socket_alloc(SocketBase, (UWORD)domain, (UWORD)type, protocol);
    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (bsd_nx_enter(SocketBase) != 0)
    {
        bsd_socket_dispose(sock);
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    }

    bsd_closing_sweep();

    if (type == SOCK_RAW)
    {
        if (bsd_raw_open(SocketBase, sock) != 0)
        {
            bsd_nx_leave(SocketBase);
            bsd_socket_dispose(sock);
            return -1;                  /* bsd_raw_open set errno */
        }

        status = NX_SUCCESS;
    }
    else if (type == SOCK_STREAM)
    {
        status = nx_tcp_socket_create(ip, &sock->as_Nx.tcp, bsd_tcp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE, ami_bsd_tcp_window(),
                                      bsd_tcp_urgent_notify,
                                      bsd_tcp_disconnect_callback);
        if (status == NX_SUCCESS)
        {
            bsd_tcp_seed_isn(&sock->as_Nx.tcp);
            bsd_tcp_keepalive_default(&sock->as_Nx.tcp);
            bsd_tcp_rx_queue_cap(&sock->as_Nx.tcp);
        }
    }
    else
    {
        status = nx_udp_socket_create(ip, &sock->as_Nx.udp, bsd_udp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE,
                                      bsd_udp_queue_max());
    }

    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        bsd_socket_dispose(sock);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    bsd_events_attach(sock);

    fd = bsd_fd_alloc(SocketBase, sock);
    if (fd < 0)
    {
        if (bsd_socket_destroy(sock))
            bsd_socket_dispose(sock);
        bsd_nx_leave(SocketBase);
        return -1;
    }

    bsd_nx_leave(SocketBase);

    return fd;
}

typedef enum
{
    BSD_BIND_FOREIGN = 0,   /* not an address this machine has              */
    BSD_BIND_ANY,           /* wildcard or loopback: nothing to enforce     */
    BSD_BIND_SOLE,          /* ours, and the only addressed interface, so
                               the socket ANY would give is the same one    */
    BSD_BIND_SPECIFIC       /* ours, but one of several: cannot be honoured */
} BsdBindKind;

static BOOL bsd_addr_is_unspecified(const NXD_ADDRESS *addr)
{
#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
        return (addr->nxd_ip_address.v6[0] == 0 &&
                addr->nxd_ip_address.v6[1] == 0 &&
                addr->nxd_ip_address.v6[2] == 0 &&
                addr->nxd_ip_address.v6[3] == 0) ? TRUE : FALSE;
#endif
    return (addr->nxd_ip_address.v4 == 0UL) ? TRUE : FALSE;
}

static BOOL bsd_addr_is_loopback(const NXD_ADDRESS *addr)
{
#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
        return (addr->nxd_ip_address.v6[0] == 0 &&
                addr->nxd_ip_address.v6[1] == 0 &&
                addr->nxd_ip_address.v6[2] == 0 &&
                addr->nxd_ip_address.v6[3] == 1UL) ? TRUE : FALSE;
#endif
    /* 127.0.0.0/8, all of it, as BSD treats it. */
    return ((addr->nxd_ip_address.v4 >> 24) == 127UL) ? TRUE : FALSE;
}

#ifdef AMINETXDUO_MULTICAST
static BOOL bsd_addr_is_multicast(const NXD_ADDRESS *addr)
{
#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
        return ((addr->nxd_ip_address.v6[0] & 0xFF000000UL) == 0xFF000000UL)
                   ? TRUE : FALSE;
#endif
    return ((addr->nxd_ip_address.v4 & 0xF0000000UL) == 0xE0000000UL)
               ? TRUE : FALSE;
}
#endif

static BsdBindKind bsd_bind_kind(const NXD_ADDRESS *addr, ULONG scope)
{
    NX_IP *ip = netstack_ip();
#ifdef AMINETXDUO_IPV6
    const NX_INTERFACE *zoned = NX_NULL;
#endif
    UWORD  matches = 0;
    UWORD  addressed = 0;
    UINT   i;

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6 && scope != 0UL &&
        !bsd_addr_is_loopback(addr) &&
        anx6_scope(addr->nxd_ip_address.v6) < 0xEU)
    {
        if (ip == NULL || scope > (ULONG)NX_MAX_PHYSICAL_INTERFACES)
            return BSD_BIND_FOREIGN;

        zoned = &ip->nx_ip_interface[scope - 1UL];
        if (zoned->nx_interface_valid == 0)
            return BSD_BIND_FOREIGN;
    }
#else
    (VOID)scope;
#endif

    if (bsd_addr_is_unspecified(addr) || bsd_addr_is_loopback(addr))
        return BSD_BIND_ANY;

#ifdef AMINETXDUO_MULTICAST
    if (bsd_addr_is_multicast(addr))
        return BSD_BIND_ANY;
#endif

    if (ip == NULL)
        return BSD_BIND_FOREIGN;

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
    {
        for (i = 0; i < (UINT)NX_MAX_IPV6_ADDRESSES; i++)
        {
            const NXD_IPV6_ADDRESS *a = &ip->nx_ipv6_address[i];

            if (a->nxd_ipv6_address_valid == 0)
                continue;

            addressed++;
            if (a->nxd_ipv6_address[0] == addr->nxd_ip_address.v6[0] &&
                a->nxd_ipv6_address[1] == addr->nxd_ip_address.v6[1] &&
                a->nxd_ipv6_address[2] == addr->nxd_ip_address.v6[2] &&
                a->nxd_ipv6_address[3] == addr->nxd_ip_address.v6[3] &&
                (zoned == NX_NULL || a->nxd_ipv6_address_attached == zoned))
                matches++;
        }
    }
    else
#endif
    {
        for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
        {
            const NX_INTERFACE *nxif = &ip->nx_ip_interface[i];

            if (nxif->nx_interface_valid == 0 ||
                nxif->nx_interface_ip_address == 0UL)
                continue;

            addressed++;
            if (nxif->nx_interface_ip_address == addr->nxd_ip_address.v4)
                matches++;
        }
    }

    if (matches == 0)
        return BSD_BIND_FOREIGN;

    return (addressed == 1) ? BSD_BIND_SOLE : BSD_BIND_SPECIFIC;
}

LONG bsd_bind(register LONG sock_fd            __asm("d0"),
              register struct sockaddr *name   __asm("a0"),
              register socklen_t namelen       __asm("d1"),
              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;
    ULONG       scope = 0;
    UINT        port = 0;
    UINT        status;
    BsdBindKind kind;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (bsd_netmon_have(MHT_Bind))
    {
        struct BindMonitorMsg bmm;
        LONG                  denied;

        bsd_bzero(&bmm, sizeof(bmm));
        bmm.bmm_Size    = (LONG)sizeof(bmm);
        bmm.bmm_Caller  = bsd_netmon_caller(SocketBase);
        bmm.bmm_Socket  = sock_fd;
        bmm.bmm_Name    = name;
        bmm.bmm_NameLen = (LONG)namelen;

        denied = bsd_netmon_dispatch(MHT_Bind, &bmm);
        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    if ((sock->as_Flags & ASF_BOUND) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_sockaddr_get(SocketBase, name, namelen, &addr, &port, &scope) != 0)
        return -1;

#ifdef AMINETXDUO_IPV6
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr.nxd_ip_version == NX_IP_VERSION_V4)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
        if (!bsd_addr_normalise(sock, &addr))
            return bsd_fail(SocketBase, AMI_EINVAL);
    }
    else if (addr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
    }
#endif

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    kind = bsd_bind_kind(&addr, scope);
    bsd_nx_leave(SocketBase);

    switch (kind)
    {
        case BSD_BIND_ANY:
        case BSD_BIND_SOLE:
            break;

        case BSD_BIND_SPECIFIC:
            break;

        case BSD_BIND_FOREIGN:
        default:
            return bsd_fail(SocketBase, AMI_EADDRNOTAVAIL);
    }

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        sock->as_LocalAddr    = addr;
        sock->as_LocalPort    = port;
        sock->as_LocalScopeId = scope;
        sock->as_Flags |= ASF_BOUND;
        bsd_raw_revalidate_endpoint(sock);

        bsd_nx_leave(SocketBase);

        return 0;
    }

    sock->as_LocalAddr    = addr;
    sock->as_LocalPort    = port;
    sock->as_LocalScopeId = scope;

    if ((sock->as_Flags & ASF_UDP) != 0)
    {
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        status = nx_udp_socket_bind(&sock->as_Nx.udp,
                                    (port != 0) ? port : NX_ANY_PORT,
                                    NX_NO_WAIT);
        if (status != NX_SUCCESS)
        {
            bsd_nx_leave(SocketBase);
            return bsd_fail(SocketBase, bsd_errno_from_nx(status));
        }

        nx_udp_socket_port_get(&sock->as_Nx.udp, &port);
        bsd_nx_leave(SocketBase);

        sock->as_LocalPort = port;
        sock->as_Flags |= ASF_NXBOUND;
    }
    else
    {
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        status = nx_tcp_client_socket_bind(&sock->as_Nx.tcp,
                                           (port != 0) ? port : NX_ANY_PORT,
                                           NX_NO_WAIT);
        if (status != NX_SUCCESS)
        {
            bsd_nx_leave(SocketBase);
            return bsd_fail(SocketBase,
                            (status == NX_PORT_UNAVAILABLE ||
                             status == NX_ALREADY_BOUND)
                                ? AMI_EADDRINUSE
                                : bsd_errno_from_nx(status));
        }

        nx_tcp_client_socket_port_get(&sock->as_Nx.tcp, &port);
        bsd_nx_leave(SocketBase);

        sock->as_LocalPort = port;
        sock->as_Flags |= ASF_NXBOUND;
    }

    sock->as_Flags |= ASF_BOUND;

    return 0;
}

static VOID bsd_listen_unlink(AmiSocket *sock, AmiSocket *victim)
{
    AmiSocket **link = &sock->as_Incoming;

    while (*link != NULL)
    {
        if (*link == victim)
        {
            *link = victim->as_IncomingNext;
            victim->as_IncomingNext = NULL;
            if (sock->as_IncomingCount != 0)
                sock->as_IncomingCount--;
            return;
        }
        link = &(*link)->as_IncomingNext;
    }
}

/*
 * Add one socket to a listening descriptor's list. Call inside a ThreadX
 * bracket. TRUE means one was added.
 */
static BOOL bsd_listen_park_one(struct AmiSocketBase *base, AmiSocket *sock)
{
    NX_IP     *ip = netstack_ip();
    AmiSocket *spare;
    UINT       status;

    if (ip == NULL || (sock->as_Flags & ASF_LISTENING) == 0)
        return FALSE;

    spare = bsd_socket_alloc(base, sock->as_Domain, sock->as_Type,
                             sock->as_Protocol);
    if (spare == NULL)
        return FALSE;

    status = nx_tcp_socket_create(ip, &spare->as_Nx.tcp, bsd_tcp_name,
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE, ami_bsd_tcp_window(),
                                  bsd_tcp_urgent_notify,
                                  bsd_tcp_disconnect_callback);
    if (status != NX_SUCCESS)
    {
        bsd_socket_dispose(spare);
        return FALSE;
    }

    bsd_tcp_seed_isn(&spare->as_Nx.tcp);
    bsd_tcp_keepalive_default(&spare->as_Nx.tcp);
    bsd_tcp_rx_queue_cap(&spare->as_Nx.tcp);

    spare->as_Flags    |= ASF_INCOMING | ASF_SERVER;
    spare->as_Parent    = sock;
    spare->as_LocalPort = sock->as_ListenPort;
    bsd_events_attach(spare);

    status = nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                           &spare->as_Nx.tcp);

    if (status != NX_SUCCESS && status != NX_CONNECTION_PENDING &&
        sock->as_Incoming == NULL)
    {
        AMI_WARN("bsdsocket: relisten on port %ld failed (%ld); rebuilding "
                 "the listen request",
                 (long)sock->as_ListenPort, (long)status);

        nx_tcp_server_socket_unlisten(ip, sock->as_ListenPort);

        status = nx_tcp_server_socket_listen(ip, sock->as_ListenPort,
                                             &spare->as_Nx.tcp,
                                             sock->as_Backlog,
                                             bsd_listen_callback);

        if (status != NX_SUCCESS && status != NX_CONNECTION_PENDING)
        {
            AMI_WARN("bsdsocket: port %ld has no listen request left (%ld). "
                     "The next accept() will try again",
                     (long)sock->as_ListenPort, (long)status);

            if (bsd_socket_destroy(spare))
                bsd_socket_dispose(spare);

            return FALSE;
        }
    }

    if (status == NX_SUCCESS || status == NX_CONNECTION_PENDING)
    {
        (VOID)nx_tcp_server_socket_accept(&spare->as_Nx.tcp, NX_NO_WAIT);

        if (status == NX_CONNECTION_PENDING)
            sock->as_Flags |= ASF_ACCEPTPEND;
    }

    spare->as_IncomingNext = sock->as_Incoming;
    sock->as_Incoming      = spare;
    sock->as_IncomingCount++;

    return TRUE;
}

/*
 * Top the listener's parked list back up to the backlog it asked for.
 * TRUE means there is at least one socket on the port.
 */
static BOOL bsd_listen_rearm(struct AmiSocketBase *base, AmiSocket *sock)
{
    UINT limit = (sock->as_Backlog != 0) ? sock->as_Backlog : 1;

    while (sock->as_IncomingCount < limit)
    {
        if (!bsd_listen_park_one(base, sock))
            break;
    }

    return (sock->as_Incoming != NULL) ? TRUE : FALSE;
}

/*
 * Put a socket accept() has decided not to hand over back on the port. The
 * caller has already disconnected and unaccepted it.
 */
static VOID bsd_listen_return(struct AmiSocketBase *base, AmiSocket *sock,
                              AmiSocket *incoming)
{
    NX_IP *ip = netstack_ip();
    UINT   status = NX_NOT_SUCCESSFUL;

    if (ip != NULL)
        status = nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                               &incoming->as_Nx.tcp);

    if (status == NX_SUCCESS || status == NX_CONNECTION_PENDING)
    {
        (VOID)nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);
        return;
    }

    bsd_listen_unlink(sock, incoming);
    if (bsd_socket_destroy(incoming))
        bsd_socket_dispose(incoming);

    (VOID)bsd_listen_rearm(base, sock);
}

LONG bsd_listen(register LONG sock_fd __asm("d0"),
                register LONG backlog __asm("d1"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    AmiSocket *incoming;
    NX_IP     *ip = netstack_ip();
    UINT       status;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_TCP) == 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return 0;                               /* idempotent, as in BSD */

    if ((sock->as_Flags & ASF_CONNECTED) != 0)
        return bsd_fail(SocketBase, AMI_EISCONN);

    if (sock->as_LocalPort == 0)
        return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

    if (backlog < 1)
        backlog = 1;
    if (backlog > BSD_MAX_BACKLOG)
        backlog = BSD_MAX_BACKLOG;

    incoming = bsd_socket_alloc(SocketBase, sock->as_Domain, sock->as_Type,
                                sock->as_Protocol);
    if (incoming == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (bsd_nx_enter(SocketBase) != 0)
    {
        bsd_socket_dispose(incoming);
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    }

    status = nx_tcp_socket_create(ip, &incoming->as_Nx.tcp, bsd_tcp_name,
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE, ami_bsd_tcp_window(),
                                  bsd_tcp_urgent_notify,
                                  bsd_tcp_disconnect_callback);
    if (status == NX_SUCCESS)
    {
        bsd_tcp_seed_isn(&incoming->as_Nx.tcp);
        bsd_tcp_keepalive_default(&incoming->as_Nx.tcp);
        bsd_tcp_rx_queue_cap(&incoming->as_Nx.tcp);
    }
    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        bsd_socket_dispose(incoming);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    incoming->as_Flags     |= ASF_INCOMING | ASF_SERVER;
    incoming->as_Parent     = sock;
    incoming->as_LocalPort  = sock->as_LocalPort;
    bsd_events_attach(incoming);

    status = nx_tcp_server_socket_listen(ip, sock->as_LocalPort,
                                         &incoming->as_Nx.tcp,
                                         (UINT)backlog, bsd_listen_callback);
    if (status != NX_SUCCESS)
    {
        if (bsd_socket_destroy(incoming))
            bsd_socket_dispose(incoming);
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    status = nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);
    if (status != NX_IN_PROGRESS && status != NX_SUCCESS)
        AMI_WARN("bsdsocket: arming accept on port %ld failed (%ld)",
                 (long)sock->as_LocalPort, (long)status);

    incoming->as_IncomingNext = NULL;
    sock->as_Incoming         = incoming;
    sock->as_IncomingCount    = 1;
    sock->as_ListenPort       = sock->as_LocalPort;
    sock->as_Backlog          = (UINT)backlog;
    sock->as_Flags           |= ASF_LISTENING;

    (VOID)bsd_listen_rearm(SocketBase, sock);

    bsd_nx_leave(SocketBase);

    return 0;
}

/*
 * Is the socket parked on a listener holding a connection accept() can hand
 * over?
 */
BOOL bsd_incoming_ready(const AmiSocket *incoming)
{
    UINT state;

    if (incoming == NULL)
        return FALSE;

    state = incoming->as_Nx.tcp.nx_tcp_socket_state;

    return (state == NX_TCP_ESTABLISHED || state == NX_TCP_CLOSE_WAIT ||
            state == NX_TCP_CLOSING || state == NX_TCP_TIMED_WAIT ||
            state == NX_TCP_LAST_ACK);
}

/*
 * Which parked socket, if any, has a connection ready to hand over. The list
 * is walked oldest-last, so this returns the one that has been waiting
 * longest, the accept queue's order.
 */
AmiSocket *bsd_incoming_first_ready(const AmiSocket *listener)
{
    AmiSocket *p;
    AmiSocket *ready = NULL;

    for (p = listener->as_Incoming; p != NULL; p = p->as_IncomingNext)
    {
        if (bsd_incoming_ready(p))
            ready = p;
    }

    return ready;
}

/*
 * Which parked socket is the one on the port.
 *
 */
static AmiSocket *bsd_incoming_on_port(const AmiSocket *listener)
{
    AmiSocket *p;

    for (p = listener->as_Incoming; p != NULL; p = p->as_IncomingNext)
    {
        UINT state = p->as_Nx.tcp.nx_tcp_socket_state;

        if (state == NX_TCP_LISTEN_STATE || state == NX_TCP_SYN_RECEIVED)
            return p;
    }

    return NULL;
}

typedef struct
{
    AmiSocket *listener;
    AmiSocket *ready;
} BsdAcceptArgs;

UINT bsd_accept_once(VOID *arg, ULONG wait)
{
    BsdAcceptArgs *a = (BsdAcceptArgs *)arg;
    AmiSocket     *listening;
    UINT           status;

    a->ready = bsd_incoming_first_ready(a->listener);
    if (a->ready != NULL)
        return NX_SUCCESS;

    if (a->listener->as_Incoming == NULL)
        return NX_NOT_CONNECTED;

    listening = bsd_incoming_on_port(a->listener);
    if (listening == NULL)
        return NX_NOT_CONNECTED;

    /*
     * accept() is not a harmless wait primitive. A finite wait that expires
     * runs _nx_tcp_connect_cleanup(), moves SYN_RECEIVED back to LISTEN and
     * makes the next call send a fresh SYN+ACK with changed sequence state.
     * Arm the passive open without suspending, then use state_wait(), whose
     * timeout changes no TCP state, for the interruptible slice.
     */
    if (listening->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_LISTEN_STATE)
    {
        status = nx_tcp_server_socket_accept(&listening->as_Nx.tcp,
                                             NX_NO_WAIT);
        if (status != NX_SUCCESS && status != NX_IN_PROGRESS &&
            status != NX_NOT_CONNECTED)
            return status;
    }

    status = nx_tcp_socket_state_wait(&listening->as_Nx.tcp,
                                      NX_TCP_ESTABLISHED, wait);

    a->ready = bsd_incoming_first_ready(a->listener);
    if (a->ready != NULL)
        return NX_SUCCESS;

    if (status == NX_NOT_SUCCESSFUL || status == NX_NOT_CONNECTED ||
        status == NX_IN_PROGRESS)
        return NX_NO_PACKET;

    return status;
}

typedef struct
{
    AmiSocket *sock;
} BsdConnectArgs;

UINT bsd_connect_once(VOID *arg, ULONG wait)
{
    BsdConnectArgs *a    = (BsdConnectArgs *)arg;
    AmiSocket      *sock = a->sock;

    (VOID)nx_tcp_socket_state_wait(&sock->as_Nx.tcp, NX_TCP_ESTABLISHED, wait);

    if ((sock->as_Flags & ASF_CONNECTED) != 0 ||
        sock->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_ESTABLISHED)
        return NX_SUCCESS;

    if ((sock->as_Flags & ASF_CONNECTING) == 0)
        return NX_NOT_CONNECTED;    /* died: a non-retry status, so we stop */

    return NX_NO_PACKET;            /* still connecting: keep slicing */
}

/*
 * Does a completed connection belong to the address the listener was bound to?
 *
 */
BOOL bsd_bind_wants_interface(const AmiSocket *listener,
                              const NX_INTERFACE *nxif)
{
    NX_IP *ip = netstack_ip();

#ifdef AMINETXDUO_IPV6
    if (listener->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V6 &&
        listener->as_LocalScopeId != 0UL &&
        !bsd_addr_is_loopback(&listener->as_LocalAddr) &&
        anx6_scope(listener->as_LocalAddr.nxd_ip_address.v6) < 0xEU)
    {
        if (ip == NX_NULL ||
            listener->as_LocalScopeId > (ULONG)NX_MAX_PHYSICAL_INTERFACES ||
            nxif != &ip->nx_ip_interface[listener->as_LocalScopeId - 1UL])
            return FALSE;
    }
#endif

    if (bsd_addr_is_unspecified(&listener->as_LocalAddr))
        return TRUE;

#ifdef AMINETXDUO_MULTICAST
    if (bsd_addr_is_multicast(&listener->as_LocalAddr))
        return TRUE;
#endif

    if (nxif == NX_NULL || ip == NULL)
        return FALSE;

    if (bsd_addr_is_loopback(&listener->as_LocalAddr))
        return (nxif == &ip->nx_ip_interface[NX_LOOPBACK_INTERFACE])
                   ? TRUE : FALSE;

#ifdef AMINETXDUO_IPV6
    if (listener->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        UINT i;

        for (i = 0; i < (UINT)NX_MAX_IPV6_ADDRESSES; i++)
        {
            const NXD_IPV6_ADDRESS *a = &ip->nx_ipv6_address[i];

            if (a->nxd_ipv6_address_valid == 0 ||
                a->nxd_ipv6_address_attached != nxif)
                continue;

            if (a->nxd_ipv6_address[0] == listener->as_LocalAddr.nxd_ip_address.v6[0] &&
                a->nxd_ipv6_address[1] == listener->as_LocalAddr.nxd_ip_address.v6[1] &&
                a->nxd_ipv6_address[2] == listener->as_LocalAddr.nxd_ip_address.v6[2] &&
                a->nxd_ipv6_address[3] == listener->as_LocalAddr.nxd_ip_address.v6[3])
                return TRUE;
        }

        return FALSE;
    }
#endif

    return (nxif->nx_interface_ip_address ==
            listener->as_LocalAddr.nxd_ip_address.v4) ? TRUE : FALSE;
}

static BOOL bsd_bind_accepts(const AmiSocket *listener, NX_TCP_SOCKET *conn)
{
    if (!bsd_bind_wants_interface(listener,
                                  conn->nx_tcp_socket_connect_interface))
        return FALSE;

    /* A wildcard IPv6 listener is deliberately dual-stack, but an IPv4
       listener is not. NetX's listen table is indexed by port alone, so this
       family check cannot be left to the stack. */
    if (listener->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V4)
        return (conn->nx_tcp_socket_connect_ip.nxd_ip_version ==
                NX_IP_VERSION_V4) ? TRUE : FALSE;

#ifdef AMINETXDUO_IPV6
    if (listener->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        const NXD_IPV6_ADDRESS *local;

        if (bsd_addr_is_unspecified(&listener->as_LocalAddr))
            return TRUE;

        if (conn->nx_tcp_socket_connect_ip.nxd_ip_version !=
            NX_IP_VERSION_V6)
            return FALSE;

        local = conn->nx_tcp_socket_ipv6_addr;

        return (local != NX_NULL &&
                local->nxd_ipv6_address[0] ==
                    listener->as_LocalAddr.nxd_ip_address.v6[0] &&
                local->nxd_ipv6_address[1] ==
                    listener->as_LocalAddr.nxd_ip_address.v6[1] &&
                local->nxd_ipv6_address[2] ==
                    listener->as_LocalAddr.nxd_ip_address.v6[2] &&
                local->nxd_ipv6_address[3] ==
                    listener->as_LocalAddr.nxd_ip_address.v6[3])
                   ? TRUE : FALSE;
    }
#endif

    return FALSE;
}

/*
 * Which source a send from this socket has to use.
 *
 */
BsdSourceKind bsd_source_select(const AmiSocket *sock, const NXD_ADDRESS *dest,
                                ULONG scope, UINT *index)
{
    NX_IP             *ip    = netstack_ip();
    const NXD_ADDRESS *local = &sock->as_LocalAddr;
    BOOL               bound;
    UINT               i;

    *index = 0;

    if (ip == NULL)
        return BSD_SOURCE_REFUSE;

    bound = (!bsd_addr_is_unspecified(local) &&
             local->nxd_ip_version == dest->nxd_ip_version) ? TRUE : FALSE;

#ifdef AMINETXDUO_MULTICAST
    if (bound && bsd_addr_is_multicast(local))
        bound = FALSE;
#endif

#ifdef AMINETXDUO_IPV6
    if (dest->nxd_ip_version == NX_IP_VERSION_V6)
    {
        NX_INTERFACE *zoned       = NX_NULL;
        NX_INTERFACE *local_zoned = NX_NULL;

        if (bound && sock->as_LocalScopeId != 0UL &&
            anx6_scope(local->nxd_ip_address.v6) < 0xEU)
        {
            if (sock->as_LocalScopeId >
                (ULONG)NX_MAX_PHYSICAL_INTERFACES)
                return BSD_SOURCE_REFUSE;

            local_zoned =
                &ip->nx_ip_interface[sock->as_LocalScopeId - 1UL];

            if (local_zoned->nx_interface_valid == 0)
                return BSD_SOURCE_REFUSE;
        }

        /*
         * RFC 4007 section 7 requires the upper layer to identify the zone
         * for every non-global destination when more than one is present.
         */
        if (scope != 0UL &&
            anx6_scope(dest->nxd_ip_address.v6) < 0xEU)
        {
            if (scope > (ULONG)NX_MAX_PHYSICAL_INTERFACES)
                return BSD_SOURCE_REFUSE;

            zoned = &ip->nx_ip_interface[scope - 1UL];

            if (zoned->nx_interface_valid == 0)
                return BSD_SOURCE_REFUSE;
        }

        if (local_zoned != NX_NULL && zoned != NX_NULL &&
            local_zoned != zoned)
            return BSD_SOURCE_UNREACH;

        if (zoned == NX_NULL)
            zoned = local_zoned;

        if (!bound && zoned == NX_NULL)
            return BSD_SOURCE_ROUTE;

        if (!bound)
        {
            NXD_IPV6_ADDRESS *chosen = NX_NULL;
            UINT              status;

            tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
            status = _nxd_ipv6_interface_find(ip,
                                               (ULONG *)dest->nxd_ip_address.v6,
                                               &chosen, zoned);
            tx_mutex_put(&ip->nx_ip_protection);

            if (status != NX_SUCCESS || chosen == NX_NULL)
                return BSD_SOURCE_REFUSE;

            *index = (UINT)chosen->nxd_ipv6_address_index;
            return BSD_SOURCE_INDEX;
        }

        for (i = 0;
             i < (UINT)(NX_MAX_IPV6_ADDRESSES + NX_LOOPBACK_IPV6_ENABLED);
             i++)
        {
            const NXD_IPV6_ADDRESS *a = &ip->nx_ipv6_address[i];

            if (a->nxd_ipv6_address_valid == 0 ||
                a->nxd_ipv6_address_state != NX_IPV6_ADDR_STATE_VALID)
                continue;

            if (zoned != NX_NULL)
            {
                if (a->nxd_ipv6_address_attached != zoned)
                    continue;
            }

            if (bound &&
                (a->nxd_ipv6_address[0] != local->nxd_ip_address.v6[0] ||
                 a->nxd_ipv6_address[1] != local->nxd_ip_address.v6[1] ||
                 a->nxd_ipv6_address[2] != local->nxd_ip_address.v6[2] ||
                 a->nxd_ipv6_address[3] != local->nxd_ip_address.v6[3]))
                continue;

            *index = (UINT)a->nxd_ipv6_address_index;
            return BSD_SOURCE_INDEX;
        }

        return BSD_SOURCE_REFUSE;
    }
#else
    (VOID)scope;
#endif

    if (!bound)
        return BSD_SOURCE_ROUTE;

    if (bsd_addr_is_loopback(local))
    {
        *index = (UINT)NX_LOOPBACK_INTERFACE;
    }
    else
    {
        for (i = 0; i < (UINT)NX_MAX_IP_INTERFACES; i++)
        {
            if (ip->nx_ip_interface[i].nx_interface_valid != 0 &&
                ip->nx_ip_interface[i].nx_interface_ip_address ==
                    local->nxd_ip_address.v4)
                break;
        }

        if (i == (UINT)NX_MAX_IP_INTERFACES)
            return BSD_SOURCE_REFUSE;

        *index = i;
    }

    {
        NX_INTERFACE *nxif     = &ip->nx_ip_interface[*index];
        ULONG         next_hop = 0;

        tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
        (VOID)_nx_ip_route_find(ip, dest->nxd_ip_address.v4, &nxif, &next_hop);
        tx_mutex_put(&ip->nx_ip_protection);

        if (nxif != &ip->nx_ip_interface[*index] || next_hop == 0)
            return BSD_SOURCE_UNREACH;
    }

    return BSD_SOURCE_INDEX;
}

LONG bsd_accept(register LONG sock_fd          __asm("d0"),
                register struct sockaddr *addr __asm("a0"),
                register socklen_t *addrlen    __asm("a1"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    AmiSocket  *incoming;
    NX_IP      *ip = netstack_ip();
    NXD_ADDRESS peer;
    ULONG       peer_port = 0;
    UINT        status;
    LONG        fd;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (addr != NULL && addrlen == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_TCP) == 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if ((sock->as_Flags & ASF_LISTENING) == 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (!bsd_listen_rearm(SocketBase, sock))
    {
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_ENOBUFS);
    }

    {
        BsdAcceptArgs args;
        BOOL          aborted;

        args.listener = sock;
        args.ready    = NULL;

        status = bsd_wait_sliced(SocketBase,
                                 bsd_wait_option(sock, sock->as_RcvTimeout, 0),
                                 bsd_accept_once, &args, &aborted);
        if (aborted)
        {
            bsd_nx_leave(SocketBase);
            return bsd_fail(SocketBase, AMI_EINTR);
        }

        incoming = args.ready;
    }

    if (status == NX_SUCCESS && incoming == NULL)
        status = NX_NO_PACKET;

    if (status == NX_NOT_CONNECTED || status == NX_IN_PROGRESS ||
        status == NX_NO_PACKET)
    {
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_EWOULDBLOCK);
    }

    if (status == NX_WAIT_ABORTED)
    {
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_EINTR);
    }

    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    nxd_tcp_socket_peer_info_get(&incoming->as_Nx.tcp, &peer, &peer_port);

    if (!bsd_bind_accepts(sock, &incoming->as_Nx.tcp))
    {
        AMI_DEBUG("bsdsocket: listener bound elsewhere refused a peer on port %ld",
                  (long)sock->as_ListenPort);

        nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);
        bsd_listen_return(SocketBase, sock, incoming);

        bsd_nx_leave(SocketBase);

        return bsd_fail(SocketBase, AMI_EWOULDBLOCK);
    }

#ifdef AMINETXDUO_IPV6
    if ((sock->as_Flags & ASF_V6ONLY) != 0 &&
        peer.nxd_ip_version == NX_IP_VERSION_V4)
    {
        AMI_DEBUG("bsdsocket: V6ONLY listener on port %ld refused an IPv4 peer",
                  (long)sock->as_ListenPort);

        nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);
        bsd_listen_return(SocketBase, sock, incoming);

        bsd_nx_leave(SocketBase);

        return bsd_fail(SocketBase, AMI_EWOULDBLOCK);
    }
#endif

    incoming->as_Flags &= ~(ASF_INCOMING | ASF_ACCEPTPEND);
    incoming->as_Flags |= ASF_CONNECTED | ASF_BOUND;
    incoming->as_Parent = NULL;
    incoming->as_Owner  = SocketBase;

    incoming->as_LocalAddr    = sock->as_LocalAddr;
    incoming->as_LocalScopeId = sock->as_LocalScopeId;

    if (bsd_addr_is_unspecified(&sock->as_LocalAddr))
    {
        if (peer.nxd_ip_version == NX_IP_VERSION_V4 &&
            incoming->as_Nx.tcp.nx_tcp_socket_connect_interface != NX_NULL)
        {
            bsd_addr_from_v4(
                &incoming->as_LocalAddr,
                incoming->as_Nx.tcp.nx_tcp_socket_connect_interface
                    ->nx_interface_ip_address);
            incoming->as_LocalScopeId = 0UL;
        }
#ifdef AMINETXDUO_IPV6
        else if (peer.nxd_ip_version == NX_IP_VERSION_V6 &&
                 incoming->as_Nx.tcp.nx_tcp_socket_ipv6_addr != NX_NULL)
        {
            const NXD_IPV6_ADDRESS *local =
                incoming->as_Nx.tcp.nx_tcp_socket_ipv6_addr;

            incoming->as_LocalAddr.nxd_ip_version = NX_IP_VERSION_V6;
            incoming->as_LocalAddr.nxd_ip_address.v6[0] =
                local->nxd_ipv6_address[0];
            incoming->as_LocalAddr.nxd_ip_address.v6[1] =
                local->nxd_ipv6_address[1];
            incoming->as_LocalAddr.nxd_ip_address.v6[2] =
                local->nxd_ipv6_address[2];
            incoming->as_LocalAddr.nxd_ip_address.v6[3] =
                local->nxd_ipv6_address[3];

            if (!bsd_addr_is_loopback(&incoming->as_LocalAddr) &&
                anx6_scope(incoming->as_LocalAddr.nxd_ip_address.v6) < 0xEU &&
                incoming->as_Nx.tcp.nx_tcp_socket_connect_interface != NX_NULL)
                incoming->as_LocalScopeId =
                    (ULONG)incoming->as_Nx.tcp.nx_tcp_socket_connect_interface
                        ->nx_interface_index + 1UL;
            else
                incoming->as_LocalScopeId = 0UL;
        }
#endif
    }

    incoming->as_PeerAddr  = peer;
    incoming->as_PeerPort  = (UINT)peer_port;
    incoming->as_LocalPort = sock->as_ListenPort;

#ifdef AMINETXDUO_IPV6
    incoming->as_PeerScopeId  = 0UL;
    if (peer.nxd_ip_version == NX_IP_VERSION_V6 &&
        (peer.nxd_ip_address.v6[0] & 0xFFC00000UL) == 0xFE800000UL &&
        incoming->as_Nx.tcp.nx_tcp_socket_connect_interface != NX_NULL)
    {
        incoming->as_PeerScopeId =
            (ULONG)incoming->as_Nx.tcp.nx_tcp_socket_connect_interface
                ->nx_interface_index + 1UL;
    }
#endif

    fd = bsd_fd_alloc(SocketBase, incoming);
    if (fd < 0)
    {
        nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);

        incoming->as_Flags &= ~ASF_CONNECTED;
        incoming->as_Flags |= ASF_INCOMING;
        incoming->as_Parent = sock;

        bsd_listen_return(SocketBase, sock, incoming);

        bsd_nx_leave(SocketBase);

        return -1;
    }

    bsd_listen_unlink(sock, incoming);
    if (bsd_incoming_first_ready(sock) == NULL)
        sock->as_Flags &= ~ASF_ACCEPTPEND;

    (VOID)bsd_listen_rearm(SocketBase, sock);

    bsd_nx_leave(SocketBase);

    if (addr != NULL && addrlen != NULL)
        bsd_sockaddr_put(incoming, addr, addrlen, &incoming->as_PeerAddr,
                         incoming->as_PeerPort, incoming->as_PeerScopeId);

    return fd;
}

/*
 * Which source TCP is to connect from.
 * *pinned is TRUE with *index set when the connect has to name the source.
 * Zero to go ahead, -1 with the errno filed.
 */
static LONG bsd_tcp_source_check(struct AmiSocketBase *SocketBase,
                                 AmiSocket *sock, const NXD_ADDRESS *addr,
                                 ULONG scope, UINT *index, BOOL *pinned)
{
    NX_IP *ip = netstack_ip();

    *index  = 0;
    *pinned = FALSE;

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    switch (bsd_source_select(sock, addr, scope, index))
    {
        case BSD_SOURCE_ROUTE:
            return 0;

        case BSD_SOURCE_INDEX:
            *pinned = TRUE;
            break;

        case BSD_SOURCE_UNREACH:
            return bsd_fail(SocketBase, AMI_ENETUNREACH);

        case BSD_SOURCE_REFUSE:
        default:
            return bsd_fail(SocketBase, AMI_EADDRNOTAVAIL);
    }

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
    {
        NXD_ADDRESS       dest   = *addr;   /* the lookup takes it non-const */
        NXD_IPV6_ADDRESS *chosen = NX_NULL;
        NX_INTERFACE     *nxif   =
            ip->nx_ipv6_address[*index].nxd_ipv6_address_attached;
        UINT              status;

        if (nxif == NX_NULL)
            return bsd_fail(SocketBase, AMI_EADDRNOTAVAIL);

        tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
        status = _nxd_ipv6_interface_find(ip, dest.nxd_ip_address.v6,
                                          &chosen, nxif);
        tx_mutex_put(&ip->nx_ip_protection);

        if (status != NX_SUCCESS)
            return bsd_fail(SocketBase, AMI_ENETUNREACH);
    }
#endif

    return 0;
}

/* The body of connect(), run inside a ThreadX context bracket. */
static LONG bsd_connect_locked(struct AmiSocketBase *SocketBase,
                               AmiSocket *sock, const NXD_ADDRESS *addr,
                               UINT port, ULONG scope)
{
    UINT status;
    UINT src_index  = 0;
    BOOL src_pinned = FALSE;

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        sock->as_PeerAddr = *addr;
        sock->as_PeerPort = port;
        sock->as_PeerScopeId = scope;
        sock->as_Flags   |= ASF_CONNECTED;

        bsd_raw_revalidate_endpoint(sock);

        return 0;
    }

    if ((sock->as_Flags & ASF_UDP) != 0)
    {
        if ((sock->as_Flags & ASF_NXBOUND) == 0)
        {
            status = nx_udp_socket_bind(&sock->as_Nx.udp, NX_ANY_PORT,
                                        NX_NO_WAIT);
            if (status != NX_SUCCESS)
                return bsd_fail(SocketBase, bsd_errno_from_nx(status));

            nx_udp_socket_port_get(&sock->as_Nx.udp, &sock->as_LocalPort);
            sock->as_Flags |= ASF_NXBOUND | ASF_BOUND;
        }

        sock->as_PeerAddr = *addr;
        sock->as_PeerPort = port;
        sock->as_PeerScopeId = scope;
        sock->as_Flags   |= ASF_CONNECTED;

        if (sock->as_RxPending != NX_NULL &&
            !bsd_udp_accepts_received_packet(sock, sock->as_RxPending))
        {
            nx_packet_release(sock->as_RxPending);
            sock->as_RxPending = NX_NULL;
            sock->as_RxOffset  = 0;
        }

        return 0;
    }

    if ((sock->as_Flags & ASF_CONNECTED) != 0)
        return bsd_fail(SocketBase, AMI_EISCONN);

    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((sock->as_Flags & ASF_CONNECTING) != 0)
    {
        if (sock->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_ESTABLISHED)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            sock->as_Flags |= ASF_CONNECTED;
            return 0;
        }

        return bsd_fail(SocketBase, AMI_EALREADY);
    }

    if (bsd_tcp_source_check(SocketBase, sock, addr, scope,
                             &src_index, &src_pinned) != 0)
        return -1;

    if ((sock->as_Flags & ASF_NXBOUND) == 0)
    {
        status = nx_tcp_client_socket_bind(
            &sock->as_Nx.tcp,
            (sock->as_LocalPort != 0) ? sock->as_LocalPort : NX_ANY_PORT,
            NX_NO_WAIT);
        if (status != NX_SUCCESS)
            return bsd_fail(SocketBase, bsd_errno_from_nx(status));

        nx_tcp_client_socket_port_get(&sock->as_Nx.tcp, &sock->as_LocalPort);
        sock->as_Flags |= ASF_NXBOUND | ASF_BOUND;
    }

    sock->as_PeerAddr = *addr;
    sock->as_PeerPort = port;
    sock->as_PeerScopeId = scope;
    sock->as_Flags   |= ASF_CONNECTING;

    status = src_pinned
                 ? nxd_tcp_client_socket_source_connect(
                       &sock->as_Nx.tcp, (NXD_ADDRESS *)addr, port, src_index,
                       NX_NO_WAIT)
                 : nxd_tcp_client_socket_connect(
                       &sock->as_Nx.tcp, (NXD_ADDRESS *)addr, port, NX_NO_WAIT);

    if (status == NX_SUCCESS)
    {
        sock->as_Flags |= ASF_CONNECTED;
        sock->as_Flags &= ~ASF_CONNECTING;
        return 0;
    }

    if (status == NX_IN_PROGRESS)
    {
        BsdConnectArgs args;
        BOOL           aborted;
        UINT           ws;

        if ((sock->as_Flags & ASF_CONNECTED) != 0)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            return 0;
        }

        if ((sock->as_Flags & ASF_CONNECTING) == 0)
        {
            if (sock->as_SoError == 0)
                sock->as_SoError = AMI_ECONNREFUSED;

            return bsd_fail(SocketBase, sock->as_SoError);
        }

        if ((sock->as_Flags & ASF_NONBLOCK) != 0)
            return bsd_fail(SocketBase, AMI_EINPROGRESS);

        args.sock = sock;
        ws = bsd_wait_sliced(SocketBase,
                             (sock->as_SndTimeout != 0) ? sock->as_SndTimeout
                                                        : NX_WAIT_FOREVER,
                             bsd_connect_once, &args, &aborted);

        if (aborted)
        {
            bsd_tcp_abort(&sock->as_Nx.tcp);
            sock->as_Flags &= ~(ASF_CONNECTING | ASF_CONNECTED);
            return bsd_fail(SocketBase, AMI_EINTR);
        }

        if (ws == NX_SUCCESS)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            sock->as_Flags |= ASF_CONNECTED;
            return 0;
        }

        if (ws == NX_NO_PACKET)
        {
            bsd_tcp_abort(&sock->as_Nx.tcp);
            sock->as_Flags &= ~(ASF_CONNECTING | ASF_CONNECTED);
            sock->as_SoError = AMI_ETIMEDOUT;
            return bsd_fail(SocketBase, AMI_ETIMEDOUT);
        }

        sock->as_Flags &= ~ASF_CONNECTING;

        if (sock->as_SoError == 0)
            sock->as_SoError = AMI_ECONNREFUSED;

        return bsd_fail(SocketBase, sock->as_SoError);
    }

    sock->as_Flags &= ~ASF_CONNECTING;

    if (status == NX_WAIT_ABORTED)
        return bsd_fail(SocketBase, AMI_EINTR);

    sock->as_SoError = (status == NX_NOT_CONNECTED)
                           ? AMI_ECONNREFUSED
                           : bsd_errno_from_nx(status);

    return bsd_fail(SocketBase, sock->as_SoError);
}

LONG bsd_connect(register LONG sock_fd          __asm("d0"),
                 register struct sockaddr *name __asm("a0"),
                 register socklen_t namelen     __asm("d1"),
                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;
    ULONG       scope = 0;
    UINT        port = 0;
    LONG        result;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (bsd_netmon_have(MHT_Connect))
    {
        struct ConnectMonitorMsg cmm;
        LONG                     denied;

        bsd_bzero(&cmm, sizeof(cmm));
        cmm.cmm_Size    = (LONG)sizeof(cmm);
        cmm.cmm_Caller  = bsd_netmon_caller(SocketBase);
        cmm.cmm_Socket  = sock_fd;
        cmm.cmm_Name    = name;
        cmm.cmm_NameLen = (LONG)namelen;

        denied = bsd_netmon_dispatch(MHT_Connect, &cmm);
        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    if ((sock->as_Flags & ASF_TCP) == 0 &&
        bsd_sa_family(name, namelen) == AF_UNSPEC)
    {
        ULONG version = sock->as_PeerAddr.nxd_ip_version;

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        bsd_bzero(&sock->as_PeerAddr, sizeof(sock->as_PeerAddr));
        sock->as_PeerAddr.nxd_ip_version = version;
        sock->as_PeerPort = 0;
        sock->as_PeerScopeId = 0UL;
        sock->as_Flags   &= ~ASF_CONNECTED;

        bsd_nx_leave(SocketBase);

        return 0;
    }

    if (bsd_sockaddr_get(SocketBase, name, namelen, &addr, &port, &scope) != 0)
        return -1;

#ifdef AMINETXDUO_IPV6
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr.nxd_ip_version != NX_IP_VERSION_V6)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

        if (!bsd_addr_normalise(sock, &addr))
            return bsd_fail(SocketBase, AMI_ENETUNREACH);

    }
    else if (addr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
    }
#endif

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    result = bsd_connect_locked(SocketBase, sock, &addr, port, scope);

    bsd_nx_leave(SocketBase);

    return result;
}

LONG bsd_shutdown(register LONG sock_fd __asm("d0"),
                  register LONG how     __asm("d1"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (how < 0 || how > 2)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(SocketBase, AMI_ENOTCONN);

    if (how == 0 || how == 2)
        sock->as_Flags |= ASF_RDSHUT;

    if (how == 1 || how == 2)
    {
        sock->as_Flags |= ASF_WRSHUT;

        if ((sock->as_Flags & (ASF_TCP | ASF_CONNECTED)) ==
            (ASF_TCP | ASF_CONNECTED))
        {
            if (bsd_nx_enter(SocketBase) != 0)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

            bsd_tcp_send_fin(sock);

            bsd_nx_leave(SocketBase);
        }
    }

    return 0;
}

LONG bsd_CloseSocket(register LONG sock_fd __asm("d0"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (bsd_fd_reserved(SocketBase, sock_fd))
    {
        if (bsd_fd_free(SocketBase, sock_fd) != 0)
            return -1;
        return 0;
    }

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (bsd_fd_free(SocketBase, sock_fd) != 0)
        return -1;

    if (bsd_nx_enter(SocketBase) == 0)
    {
        bsd_socket_release(SocketBase, sock);

        bsd_closing_sweep();

        bsd_nx_leave(SocketBase);
    }
    else
    {
        AMI_WARN("bsdsocket: CloseSocket(%ld) with the kernel down. "
                 "The socket leaks", (long)sock_fd);
    }

    return 0;
}
