/*
 * bsdsocket.library -- socket lifecycle and the per-opener descriptor table.
 *
 * socket/bind/listen/accept/connect/shutdown/CloseSocket, built on
 * nx_tcp_socket_* and nx_udp_socket_*.
 *
 * The one genuinely awkward mapping is listen/accept. NetX Duo hands an
 * incoming connection to a *named socket* that was parked on the port, and a
 * server keeps listening by handing it a fresh socket afterwards
 * (nx_tcp_server_socket_relisten). BSD instead spawns a new descriptor per
 * connection. So a listening descriptor here keeps a spare "incoming" socket
 * parked on the port; accept() promotes that socket to a descriptor of its
 * own and parks another one.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

/*
 * How many datagrams a UDP socket may hold un-read.
 *
 * This is a pool-budget decision, not a tuning knob. A queued datagram is a
 * whole NX_PACKET out of the shared pool for as long as the application does
 * not read it, and the pool is the stack's only buffer: 16..256 packets of
 * 1568 bytes, sized from AvailMem() at startup (netstack.h). Everything else
 * that can pin packets is already budgeted the same way -- the SANA-II
 * readers keep up to 8 outstanding CMD_READs, and NX_TCP_MAXIMUM_TX_QUEUE
 * caps a TCP socket at 8 in flight.
 *
 * A flat 8 was the old value, and it is far too tight on anything but the
 * floor: a sender that bursts 200 datagrams into loopback before the receiver
 * runs loses 192 of them. A flat large value is worse -- one socket nobody is
 * reading would drain the pool and the IP thread could not allocate to
 * receive or transmit anything at all, which is a machine-wide failure caused
 * by one careless application.
 *
 * So: a quarter of the pool, never fewer than the 8 we had, never more than
 * 64. On the 4 MB floor (16 packets) that is 8, exactly as before; on a
 * full 256-packet pool it is 64. Three sockets asleep on full queues still
 * leave a quarter of the pool for the stack.
 */
#define BSD_UDP_QUEUE_MIN       8
#define BSD_UDP_QUEUE_CEILING   64
#define BSD_UDP_POOL_SHARE      4       /* 1/N of the pool per socket       */

static char bsd_tcp_name[] = "AmiNetXDuo TCP";
static char bsd_udp_name[] = "AmiNetXDuo UDP";

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

/* --------------------------------------------------------- descriptor table */

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

LONG bsd_table_resize(struct AmiSocketBase *base, LONG size)
{
    AmiSocket **table;
    LONG        i, copy;

    if (size < 1 || size > BSD_MAX_DTABLESIZE)
        return -1;

    if (bsd_table_ensure(base) != 0)
        return -1;

    if (size == base->sb_TableSize)
        return 0;

    /* Never shrink below the highest descriptor in use. */
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
    if (base->sb_Table == NULL || fd < 0 || fd >= base->sb_TableSize)
        return NULL;

    return base->sb_Table[fd];
}

LONG bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock)
{
    LONG fd;

    if (bsd_table_ensure(base) != 0)
        return -1;

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        if (base->sb_Table[fd] == NULL)
        {
            base->sb_Table[fd] = sock;
            return fd;
        }
    }

    return -1;
}

VOID bsd_fd_free(struct AmiSocketBase *base, LONG fd)
{
    if (base->sb_Table != NULL && fd >= 0 && fd < base->sb_TableSize)
        base->sb_Table[fd] = NULL;
}

/* ----------------------------------------------------------- socket objects */

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
    sock->as_Flags    = (type == SOCK_STREAM) ? ASF_TCP : ASF_UDP;
    sock->as_Ttl      = (LONG)NX_IP_TIME_TO_LIVE;

    /*
     * The version tag has to be set even though ami_alloc() cleared the block:
     * a cleared NXD_ADDRESS reads as version 0, not NX_IP_VERSION_V4 (4), and
     * every "is this address unset" test in options.c and transfer.c asks the
     * version first. A zero there is not "IPv4 0.0.0.0", it is "no family",
     * and getsockname() on a connected socket bound to INADDR_ANY would stop
     * reporting the interface address.
     */
    bsd_addr_from_v4(&sock->as_LocalAddr, 0UL);
    bsd_addr_from_v4(&sock->as_PeerAddr, 0UL);

#ifdef AMINETXDUO_IPV6
    if (domain == AF_INET6)
    {
        sock->as_Flags |= ASF_INET6;

        sock->as_LocalAddr.nxd_ip_version = NX_IP_VERSION_V6;
        sock->as_PeerAddr.nxd_ip_version  = NX_IP_VERSION_V6;

        /*
         * IPV6_V6ONLY DEFAULTS TO OFF, i.e. an AF_INET6 socket is dual-stack.
         *
         * This is not a copy of anyone's default -- Linux says off, modern
         * BSD says on -- it follows from what NetX Duo is. Its port tables are
         * family-agnostic: nx_tcp_server_socket_listen() registers a listen on
         * a PORT, and the SYN that arrives for it may be v4 or v6. There is no
         * arrangement under which an AF_INET6 socket and an AF_INET socket can
         * both hold port 80 here, so the "V6ONLY on lets you run separate v4
         * and v6 servers" argument for defaulting it on does not apply, while
         * the "one socket serves both" behaviour it would block is the only
         * one available.
         *
         * Setting it to 1 is still honoured -- see options.c -- and then a v4
         * peer is refused rather than reported as ::ffff:a.b.c.d.
         */
    }
#endif

    return sock;
}

/*
 * Tear the NetX Duo socket down; safe to call more than once.
 *
 * Returns TRUE only when NetX Duo has actually let go of the control block --
 * i.e. when the caller may hand the memory back to the allocator. This is not
 * bookkeeping pedantry: nx_tcp_socket_delete() links every live socket into
 * ip->nx_ip_tcp_created_sockets_ptr, and freeing a socket that is still on
 * that list means the next nx_tcp_socket_create() walks a freed-and-reused
 * address. It reports NX_PTR_ERROR (EFAULT) and TCP socket() is dead for the
 * lifetime of the process.
 */
static BOOL bsd_socket_destroy(AmiSocket *sock)
{
    UINT status;

    if ((sock->as_Flags & ASF_ORPHANED) != 0)
        return FALSE;                   /* already known to be un-deletable */

    if ((sock->as_Flags & ASF_DELETED) != 0)
        return TRUE;

    if (sock->as_RxPending != NULL)
    {
        nx_packet_release(sock->as_RxPending);
        sock->as_RxPending = NULL;
    }

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        if ((sock->as_Flags & (ASF_CONNECTED | ASF_CONNECTING)) != 0)
            nx_tcp_socket_disconnect(&sock->as_Nx.tcp,
                                     (sock->as_LingerOn != 0)
                                         ? (ULONG)sock->as_LingerTime *
                                           NX_IP_PERIODIC_RATE
                                         : NX_NO_WAIT);

        /*
         * Give the port back. A socket that came off a listen port is
         * released with unaccept() whether or not it was ever accepted --
         * nx_tcp_client_socket_unbind() does not know about listen state, and
         * leaving it bound makes the delete below return NX_STILL_BOUND.
         */
        if ((sock->as_Flags & ASF_SERVER) != 0)
            nx_tcp_server_socket_unaccept(&sock->as_Nx.tcp);
        else if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_tcp_client_socket_unbind(&sock->as_Nx.tcp);

        status = nx_tcp_socket_delete(&sock->as_Nx.tcp);
    }
    else
    {
        if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_udp_socket_unbind(&sock->as_Nx.udp);

        status = nx_udp_socket_delete(&sock->as_Nx.udp);
    }

    /* NX_NOT_CREATED means it was never on the created list, so the memory is
       ours either way. Anything else means NetX Duo still points at it. */
    if (status != NX_SUCCESS && status != NX_NOT_CREATED)
    {
        AMI_WARN("bsdsocket: %s_socket_delete refused (%ld); leaking %ld bytes "
                 "rather than corrupting the created list",
                 ((sock->as_Flags & ASF_TCP) != 0) ? "nx_tcp" : "nx_udp",
                 (long)status, (long)sizeof(AmiSocket));

        sock->as_Flags |= ASF_ORPHANED;

        /*
         * The block stays alive for NetX Duo's benefit, but it must stop
         * pointing at ours: the owning base is about to be freed, and a late
         * receive or disconnect callback would signal a dead task.
         */
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

VOID bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock)
{
    (VOID)base;

    if (sock == NULL)
        return;

    if (sock->as_RefCount > 0)
        sock->as_RefCount--;

    if (sock->as_RefCount > 0)
        return;

    /*
     * Drop the listen request BEFORE tearing down the socket parked on it,
     * or NetX Duo is left holding a pointer to freed memory.
     *
     * The parked socket sits in SYN_RECEIVED, not LISTEN -- bsd_listen() posts
     * the accept up front so NetX Duo answers a SYN without the application
     * having called accept(). nx_tcp_server_socket_unlisten() refuses a socket
     * that is not in LISTEN state, and disconnect() is what winds a *server*
     * socket back there (nx_tcp_socket_disconnect.c: "Server socket, return to
     * LISTEN state").
     */
    if ((sock->as_Flags & ASF_LISTENING) != 0)
    {
        NX_IP *ip = netstack_ip();

        if (sock->as_Incoming != NULL)
            nx_tcp_socket_disconnect(&sock->as_Incoming->as_Nx.tcp, NX_NO_WAIT);

        if (ip != NULL)
            nx_tcp_server_socket_unlisten(ip, sock->as_ListenPort);

        sock->as_Flags &= ~ASF_LISTENING;
    }

    /* A listening socket owns the spare parked on the port. */
    if (sock->as_Incoming != NULL)
    {
        if (bsd_socket_destroy(sock->as_Incoming))
            ami_free(sock->as_Incoming);
        sock->as_Incoming = NULL;
    }

    if (bsd_socket_destroy(sock))
        ami_free(sock);
}

VOID bsd_close_all(struct AmiSocketBase *base)
{
    LONG fd;

    if (base->sb_Table == NULL)
        return;

    /* One bracket for the lot: this runs from CloseLibrary(), where the whole
       table goes at once and adopting per socket would be pure overhead. */
    if (bsd_nx_enter(base) != 0)
    {
        AMI_WARN("bsdsocket: CloseLibrary with the kernel down; "
                 "sockets left to the stack teardown");
        return;
    }

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        AmiSocket *sock = base->sb_Table[fd];

        if (sock == NULL)
            continue;

        base->sb_Table[fd] = NULL;
        bsd_socket_release(base, sock);
    }

    bsd_nx_leave(base);
}

/* -------------------------------------------------------- sockaddr helpers */

VOID bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4)
{
    addr->nxd_ip_version       = NX_IP_VERSION_V4;
    addr->nxd_ip_address.v4    = v4;
}

/*
 * Which family is this sockaddr?
 *
 * It cannot be answered by reading a struct member, because the two structs
 * this NDK offers do not agree on where the family lives:
 *
 *   sockaddr_in    [0] = sin_len (16, or 0 from a memset), [1] = sin_family
 *   sockaddr_in6   [0] = sin6_family, [1] = padding
 *
 * So the answer comes from the bytes plus the length the caller declared,
 * longest and most specific first. The AF_INET6 test requires BOTH a 23 in
 * byte 0 and a length that could hold a sockaddr_in6, which no sockaddr_in
 * can satisfy (a sin_len of 23 with a namelen of 28 is not something any
 * caller produces).
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

    /*
     * AF_UNSPEC is legal on connect() (BSD's "dissolve the association") and
     * on the destination of a sendto() to a connected socket. A zeroed
     * sockaddr reaches here.
     */
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
        /* Too short to be anything is EINVAL; the right size but the wrong
           family is EAFNOSUPPORT, which is what BSD reports and what the
           conformance suite checks. */
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

        /* m68k is the wire order, so the ntoh* below are documentation. */
        bsd_addr_from_v4(addr, BSD_NTOHL(sin->sin_addr.s_addr));
        *port = (UINT)BSD_NTOHS(sin->sin_port);
    }

    return 0;
}

VOID bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                      socklen_t *len, const NXD_ADDRESS *addr, UINT port)
{
    socklen_t copy;

    if (sa == NULL || len == NULL)
        return;

#ifdef AMINETXDUO_IPV6
    /*
     * The SHAPE follows the socket, not the address. An AF_INET6 socket that
     * accepted an IPv4 connection must report the peer as a sockaddr_in6
     * holding ::ffff:a.b.c.d -- an application that called accept() on an
     * AF_INET6 socket has a sockaddr_in6 on its stack and nothing else.
     */
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
        /* No sin6_len field in this NDK's sockaddr_in6 -- see the note in
           bsdsocket_internal.h. Setting one would corrupt sin6_family. */
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = (in_port_t)BSD_HTONS((UWORD)port);
        bsd_words_to_in6(use->nxd_ip_address.v6, sin6.sin6_addr.s6_addr);
        sin6.sin6_scope_id = sock->as_ScopeId;

        copy = *len;
        if (copy > (socklen_t)sizeof(sin6))
            copy = (socklen_t)sizeof(sin6);

        bsd_bcopy(&sin6, sa, (ULONG)copy);
        *len = (socklen_t)sizeof(struct sockaddr_in6);

        return;
    }
#else
    (VOID)sock;
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

/* ---------------------------------------------------------------- vectors */

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

    /*
     * An AF_INET6 socket on a stack whose IPv6 half failed to come up would
     * be a socket that can never send anything; EAFNOSUPPORT now is better
     * than ENETUNREACH on every later call.
     */
    if (domain == AF_INET6 && !netstack_ipv6_enabled())
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
#else
    if (domain != AF_INET)
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
#endif

    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return bsd_fail(SocketBase, AMI_ESOCKTNOSUPPORT);

    if (protocol != 0 &&
        protocol != ((type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP))
        return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);

    sock = bsd_socket_alloc(SocketBase, (UWORD)domain, (UWORD)type, protocol);
    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (bsd_nx_enter(SocketBase) != 0)
    {
        ami_free(sock);
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    }

    if (type == SOCK_STREAM)
    {
        status = nx_tcp_socket_create(ip, &sock->as_Nx.tcp, bsd_tcp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE, BSD_TCP_WINDOW,
                                      NX_NULL, bsd_tcp_disconnect_callback);
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
        ami_free(sock);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    bsd_events_attach(sock);

    fd = bsd_fd_alloc(SocketBase, sock);
    if (fd < 0)
    {
        if (bsd_socket_destroy(sock))
            ami_free(sock);
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_EMFILE);
    }

    bsd_nx_leave(SocketBase);

    return fd;
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

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if ((sock->as_Flags & ASF_BOUND) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_sockaddr_get(SocketBase, name, namelen, &addr, &port, &scope) != 0)
        return -1;

#ifdef AMINETXDUO_IPV6
    /*
     * The families have to agree. An AF_INET socket handed a sockaddr_in6
     * (or the reverse) is a programming error, not something to interpret --
     * except for the v4-mapped case on a dual-stack socket, which is a
     * legitimate way of saying "bind the v4 side of this port".
     */
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr.nxd_ip_version == NX_IP_VERSION_V4)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
        if (!bsd_addr_normalise(sock, &addr))
            return bsd_fail(SocketBase, AMI_EINVAL);
        sock->as_ScopeId = scope;
    }
    else if (addr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
    }
#endif

    /*
     * NetX Duo binds a socket to a PORT, not to an address: there is no
     * nx_*_socket_bind that takes one. A bind to a specific local address is
     * therefore recorded (so getsockname reports it) but does not restrict
     * what the socket receives. That is a real gap, and it is the same one
     * the IPv4 path has always had.
     */
    sock->as_LocalAddr = addr;
    sock->as_LocalPort = port;

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
        /*
         * TCP takes the port here too, rather than deferring to connect() or
         * listen(). BSD binds at bind() time and two things depend on it:
         * getsockname() after bind(port 0) must report the ephemeral port the
         * stack picked, and a second bind() to a port already in use must fail
         * with EADDRINUSE. Neither is observable if the port is only claimed
         * later.
         *
         * The listening descriptor's own NX socket is never used for a
         * connection -- listen() parks a separate socket on the port -- so
         * holding the port here does not collide with
         * nx_tcp_server_socket_listen(), which registers a listen request and
         * does not touch the bound-port table.
         */
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

    /*
     * The listening descriptor itself never carries a connection: it parks a
     * spare socket on the port for NetX Duo to hand the next connection to.
     */
    incoming = bsd_socket_alloc(SocketBase, sock->as_Domain, sock->as_Type,
                                sock->as_Protocol);
    if (incoming == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (bsd_nx_enter(SocketBase) != 0)
    {
        ami_free(incoming);
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    }

    status = nx_tcp_socket_create(ip, &incoming->as_Nx.tcp, bsd_tcp_name,
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE, BSD_TCP_WINDOW,
                                  NX_NULL, bsd_tcp_disconnect_callback);
    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        ami_free(incoming);
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
            ami_free(incoming);
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    /*
     * Arm the parked socket NOW, before anyone calls accept().
     *
     * BSD completes the three-way handshake in the stack and queues the
     * finished connection for accept(); NetX Duo does not. Its SYN handler
     * only sends the SYN+ACK "if an accept call with suspension has already
     * been made for this socket" -- i.e. if the socket is already in
     * SYN_RECEIVED (nx_tcp_packet_process.c). A socket left in LISTEN state
     * silently ignores the SYN, so a client that connect()s before the server
     * happens to be inside accept() retransmits until it gives up. That is not
     * a corner case: it is what every listen/connect/accept sequence in one
     * task does, and it is why loopback connect() failed with ECONNREFUSED.
     *
     * nx_tcp_server_socket_accept() with NX_NO_WAIT moves LISTEN ->
     * SYN_RECEIVED and returns NX_IN_PROGRESS without blocking, which is
     * exactly the arming step. The real accept() below then either finds the
     * socket ESTABLISHED already or suspends on it.
     */
    status = nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);
    if (status != NX_IN_PROGRESS && status != NX_SUCCESS)
        AMI_WARN("bsdsocket: arming accept on port %ld failed (%ld)",
                 (long)sock->as_LocalPort, (long)status);

    bsd_nx_leave(SocketBase);

    sock->as_Incoming   = incoming;
    sock->as_ListenPort = sock->as_LocalPort;
    sock->as_Backlog    = (UINT)backlog;
    sock->as_Flags     |= ASF_LISTENING;

    return 0;
}

LONG bsd_accept(register LONG sock_fd          __asm("d0"),
                register struct sockaddr *addr __asm("a0"),
                register socklen_t *addrlen    __asm("a1"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    AmiSocket  *incoming, *spare;
    NX_IP      *ip = netstack_ip();
    NXD_ADDRESS peer;
    ULONG       peer_port = 0;
    UINT        status;
    LONG        fd;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_LISTENING) == 0 || sock->as_Incoming == NULL)
        return bsd_fail(SocketBase, AMI_EINVAL);

    incoming = sock->as_Incoming;

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    status = nx_tcp_server_socket_accept(
        &incoming->as_Nx.tcp,
        bsd_wait_option(sock, sock->as_RcvTimeout));

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

    /*
     * nxd_ (not nx_) peer info: the v4-only entry point reports 0 for a peer
     * that connected over IPv6, so a dual-stack listener would hand accept()
     * a sockaddr full of zeroes.
     */
    nxd_tcp_socket_peer_info_get(&incoming->as_Nx.tcp, &peer, &peer_port);

#ifdef AMINETXDUO_IPV6
    /*
     * IPV6_V6ONLY enforcement lives here and can live nowhere else.
     *
     * NetX Duo's listen is registered against a port, not an address family,
     * so a V6ONLY socket still has an IPv4 SYN answered for it down in the TCP
     * state machine -- by the time this code runs the handshake is complete.
     * The only honest thing left is to close that connection and go back to
     * waiting, which is what a V6ONLY listener on any other stack looks like
     * from the client's side (a connection that is accepted and immediately
     * reset, rather than a SYN that goes unanswered).
     *
     * Reported as EWOULDBLOCK rather than looping internally: a blocking
     * accept() that silently swallowed connections would hide the fact that
     * something is knocking on the v4 side, and a non-blocking one must return
     * anyway. The caller retries, exactly as it would after any spurious
     * wakeup.
     */
    if ((sock->as_Flags & ASF_V6ONLY) != 0 &&
        peer.nxd_ip_version == NX_IP_VERSION_V4)
    {
        AMI_DEBUG("bsdsocket: V6ONLY listener on port %ld refused an IPv4 peer",
                  (long)sock->as_ListenPort);

        nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);
        nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                      &incoming->as_Nx.tcp);
        (VOID)nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);

        bsd_nx_leave(SocketBase);

        return bsd_fail(SocketBase, AMI_EWOULDBLOCK);
    }
#endif

    /* Promote the parked socket to a descriptor of its own. ASF_SERVER stays
     * set: the port still has to go back through unaccept() at close. */
    incoming->as_Flags &= ~(ASF_INCOMING | ASF_ACCEPTPEND);
    incoming->as_Flags |= ASF_CONNECTED | ASF_BOUND;
    incoming->as_Parent = NULL;
    incoming->as_Owner  = SocketBase;

    incoming->as_PeerAddr  = peer;
    incoming->as_PeerPort  = (UINT)peer_port;
    incoming->as_LocalPort = sock->as_ListenPort;

    fd = bsd_fd_alloc(SocketBase, incoming);
    if (fd < 0)
    {
        /* Put the socket back on the port rather than losing the listener. */
        nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);
        nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                      &incoming->as_Nx.tcp);
        (VOID)nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);

        incoming->as_Flags &= ~ASF_CONNECTED;
        incoming->as_Flags |= ASF_INCOMING;
        incoming->as_Parent = sock;

        bsd_nx_leave(SocketBase);

        return bsd_fail(SocketBase, AMI_EMFILE);
    }

    sock->as_Incoming = NULL;
    sock->as_Flags   &= ~ASF_ACCEPTPEND;

    /* Park a fresh socket so the port keeps accepting. */
    spare = bsd_socket_alloc(SocketBase, sock->as_Domain, sock->as_Type,
                             sock->as_Protocol);
    if (spare != NULL)
    {
        status = nx_tcp_socket_create(ip, &spare->as_Nx.tcp, bsd_tcp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE, BSD_TCP_WINDOW,
                                      NX_NULL, bsd_tcp_disconnect_callback);
        if (status == NX_SUCCESS)
        {
            spare->as_Flags    |= ASF_INCOMING | ASF_SERVER;
            spare->as_Parent    = sock;
            spare->as_LocalPort = sock->as_ListenPort;
            bsd_events_attach(spare);

            status = nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                                   &spare->as_Nx.tcp);
            /*
             * NX_CONNECTION_PENDING means a queued connection was handed
             * straight to the spare -- still a success, and the next accept()
             * will return immediately.
             */
            if (status == NX_SUCCESS || status == NX_CONNECTION_PENDING)
            {
                /* Arm it, exactly as bsd_listen() does -- see the comment
                   there. Without this the next client's SYN goes unanswered. */
                (VOID)nx_tcp_server_socket_accept(&spare->as_Nx.tcp,
                                                  NX_NO_WAIT);

                sock->as_Incoming = spare;
                if (status == NX_CONNECTION_PENDING)
                    sock->as_Flags |= ASF_ACCEPTPEND;
            }
            else
            {
                if (bsd_socket_destroy(spare))
                    ami_free(spare);
                AMI_WARN("bsdsocket: relisten failed, status %ld", (LONG)status);
            }
        }
        else
        {
            ami_free(spare);
        }
    }

    bsd_nx_leave(SocketBase);

    if (addr != NULL && addrlen != NULL)
        bsd_sockaddr_put(incoming, addr, addrlen, &incoming->as_PeerAddr,
                         incoming->as_PeerPort);

    return fd;
}

/* The body of connect(), run inside a ThreadX context bracket. */
static LONG bsd_connect_locked(struct AmiSocketBase *SocketBase,
                               AmiSocket *sock, const NXD_ADDRESS *addr,
                               UINT port)
{
    UINT status;

    if ((sock->as_Flags & ASF_UDP) != 0)
    {
        /*
         * A connected UDP socket is just a default destination here; NetX Duo
         * has no UDP connect, so record it and let send()/recv() use it.
         */
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
        sock->as_Flags   |= ASF_CONNECTED;

        return 0;
    }

    if ((sock->as_Flags & ASF_CONNECTED) != 0)
        return bsd_fail(SocketBase, AMI_EISCONN);

    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((sock->as_Flags & ASF_CONNECTING) != 0)
    {
        /*
         * A second connect() on a non-blocking socket reports the outcome of
         * the first, as BSD requires.
         */
        if (sock->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_ESTABLISHED)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            sock->as_Flags |= ASF_CONNECTED;
            return 0;
        }

        return bsd_fail(SocketBase, AMI_EALREADY);
    }

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

    /*
     * ASF_CONNECTING goes on BEFORE the call, not after.
     *
     * The establish / disconnect-complete callbacks (select.c) can fire from
     * *inside* nx_tcp_client_socket_connect(): a loopback SYN is queued to the
     * IP thread, which outranks us, and tx_mutex_put() inside the connect is a
     * scheduling point -- so on loopback the entire SYN -> RST -> reset
     * sequence can finish before the connect returns NX_IN_PROGRESS to us.
     * Setting the flag afterwards meant the callback saw a socket that was not
     * yet "connecting", filed the RST as an ordinary EOF, and left SO_ERROR at
     * zero. Setting it first makes the callback authoritative and this code
     * merely read its answer.
     */
    sock->as_PeerAddr = *addr;
    sock->as_PeerPort = port;
    sock->as_Flags   |= ASF_CONNECTING;

    /*
     * nxd_, not nx_: the v4-only wrapper builds an NXD_ADDRESS tagged
     * NX_IP_VERSION_V4 and calls exactly this, so going straight to it costs
     * nothing in the floor build and is the only way to reach an IPv6 peer.
     */
    status = nxd_tcp_client_socket_connect(
        &sock->as_Nx.tcp, (NXD_ADDRESS *)addr, port,
        bsd_wait_option(sock, sock->as_SndTimeout));

    if (status == NX_SUCCESS)
    {
        sock->as_Flags |= ASF_CONNECTED;
        sock->as_Flags &= ~ASF_CONNECTING;
        return 0;
    }

    if (status == NX_IN_PROGRESS)
    {
        /* Already established inside the call? Then the connect is done, and
           BSD says a non-blocking connect that completes at once returns 0. */
        if ((sock->as_Flags & ASF_CONNECTED) != 0)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            return 0;
        }

        /* Already failed inside the call? The callback cleared the flag and
           left the reason in as_SoError. */
        if ((sock->as_Flags & ASF_CONNECTING) == 0)
        {
            if (sock->as_SoError == 0)
                sock->as_SoError = AMI_ECONNREFUSED;

            return bsd_fail(SocketBase, sock->as_SoError);
        }

        return bsd_fail(SocketBase, AMI_EINPROGRESS);
    }

    sock->as_Flags &= ~ASF_CONNECTING;

    if (status == NX_WAIT_ABORTED)
        return bsd_fail(SocketBase, AMI_EINTR);

    /* A failed connect leaves the reason behind for getsockopt(SO_ERROR),
       which is how a non-blocking caller finds out what went wrong. */
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

    if (bsd_sockaddr_get(SocketBase, name, namelen, &addr, &port, &scope) != 0)
        return -1;

#ifdef AMINETXDUO_IPV6
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr.nxd_ip_version != NX_IP_VERSION_V6)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

        /* ::ffff:a.b.c.d on a dual-stack socket becomes a real IPv4 connect;
           on a V6ONLY socket it is refused. */
        if (!bsd_addr_normalise(sock, &addr))
            return bsd_fail(SocketBase, AMI_ENETUNREACH);

        sock->as_ScopeId = scope;
    }
    else if (addr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
    }
#endif

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    result = bsd_connect_locked(SocketBase, sock, &addr, port);

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

    if (how == 0 || how == 2)
        sock->as_Flags |= ASF_RDSHUT;

    if (how == 1 || how == 2)
    {
        sock->as_Flags |= ASF_WRSHUT;

        /*
         * NetX Duo has no half-close, so the closest thing to "send FIN and
         * carry on reading" is a non-blocking disconnect, which queues the
         * FIN and returns NX_IN_PROGRESS.
         */
        if ((sock->as_Flags & (ASF_TCP | ASF_CONNECTED)) ==
            (ASF_TCP | ASF_CONNECTED))
        {
            if (bsd_nx_enter(SocketBase) != 0)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

            nx_tcp_socket_disconnect(&sock->as_Nx.tcp, NX_NO_WAIT);

            bsd_nx_leave(SocketBase);
        }
    }

    return 0;
}

LONG bsd_CloseSocket(register LONG sock_fd __asm("d0"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    bsd_fd_free(SocketBase, sock_fd);

    /*
     * The descriptor is gone whatever happens next, so the caller always sees
     * success; the teardown itself needs ThreadX context, and if the stack is
     * already down there is nothing left to hand the socket back to.
     */
    if (bsd_nx_enter(SocketBase) == 0)
    {
        bsd_socket_release(SocketBase, sock);
        bsd_nx_leave(SocketBase);
    }
    else
    {
        AMI_WARN("bsdsocket: CloseSocket(%ld) with the kernel down; leaking",
                 (long)sock_fd);
    }

    return 0;
}
