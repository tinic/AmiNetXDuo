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

#define BSD_TCP_WINDOW      8192        /* receive window, 68020/4 MB floor */
#define BSD_UDP_QUEUE_MAX   8           /* datagrams queued per socket      */

static char bsd_tcp_name[] = "AmiNetXDuo TCP";
static char bsd_udp_name[] = "AmiNetXDuo UDP";

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

    return sock;
}

/* Tear the NetX Duo socket down; safe to call more than once. */
static VOID bsd_socket_destroy(AmiSocket *sock)
{
    if ((sock->as_Flags & ASF_DELETED) != 0)
        return;
    sock->as_Flags |= ASF_DELETED;

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

        if ((sock->as_Flags & ASF_INCOMING) != 0)
            nx_tcp_server_socket_unaccept(&sock->as_Nx.tcp);
        else if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_tcp_client_socket_unbind(&sock->as_Nx.tcp);

        nx_tcp_socket_delete(&sock->as_Nx.tcp);
    }
    else
    {
        if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_udp_socket_unbind(&sock->as_Nx.udp);

        nx_udp_socket_delete(&sock->as_Nx.udp);
    }
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
     */
    if ((sock->as_Flags & ASF_LISTENING) != 0)
    {
        NX_IP *ip = netstack_ip();

        if (ip != NULL)
            nx_tcp_server_socket_unlisten(ip, sock->as_ListenPort);

        sock->as_Flags &= ~ASF_LISTENING;
    }

    /* A listening socket owns the spare parked on the port. */
    if (sock->as_Incoming != NULL)
    {
        bsd_socket_destroy(sock->as_Incoming);
        ami_free(sock->as_Incoming);
        sock->as_Incoming = NULL;
    }

    bsd_socket_destroy(sock);
    ami_free(sock);
}

VOID bsd_close_all(struct AmiSocketBase *base)
{
    LONG fd;

    if (base->sb_Table == NULL)
        return;

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        AmiSocket *sock = base->sb_Table[fd];

        if (sock == NULL)
            continue;

        base->sb_Table[fd] = NULL;
        bsd_socket_release(base, sock);
    }
}

/* -------------------------------------------------------- sockaddr helpers */

LONG bsd_sockaddr_in(struct AmiSocketBase *base, const struct sockaddr *sa,
                     socklen_t len, ULONG *addr, UINT *port)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    if (sa == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (len < (socklen_t)sizeof(struct sockaddr_in))
        return bsd_fail(base, AMI_EINVAL);

    if (sin->sin_family != AF_INET && sin->sin_family != AF_UNSPEC)
        return bsd_fail(base, AMI_EAFNOSUPPORT);

    /* m68k is the wire order, so the ntoh* below are documentation. */
    *addr = BSD_NTOHL(sin->sin_addr.s_addr);
    *port = (UINT)BSD_NTOHS(sin->sin_port);

    return 0;
}

VOID bsd_sockaddr_out(struct sockaddr *sa, socklen_t *len,
                      ULONG addr, UINT port)
{
    struct sockaddr_in sin;
    socklen_t          copy;

    if (sa == NULL || len == NULL)
        return;

    bsd_bzero(&sin, sizeof(sin));
    sin.sin_len         = (UBYTE)sizeof(struct sockaddr_in);
    sin.sin_family      = AF_INET;
    sin.sin_port        = (in_port_t)BSD_HTONS((UWORD)port);
    sin.sin_addr.s_addr = BSD_HTONL(addr);

    copy = *len;
    if (copy > (socklen_t)sizeof(sin))
        copy = (socklen_t)sizeof(sin);

    bsd_bcopy(&sin, sa, (ULONG)copy);
    *len = (socklen_t)sizeof(struct sockaddr_in);
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

    if (domain != AF_INET)
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return bsd_fail(SocketBase, AMI_ESOCKTNOSUPPORT);

    if (protocol != 0 &&
        protocol != ((type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP))
        return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);

    sock = bsd_socket_alloc(SocketBase, (UWORD)domain, (UWORD)type, protocol);
    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (type == SOCK_STREAM)
    {
        status = nx_tcp_socket_create(ip, &sock->as_Nx.tcp, bsd_tcp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE, BSD_TCP_WINDOW,
                                      NX_NULL, NX_NULL);
    }
    else
    {
        status = nx_udp_socket_create(ip, &sock->as_Nx.udp, bsd_udp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE, BSD_UDP_QUEUE_MAX);
    }

    if (status != NX_SUCCESS)
    {
        ami_free(sock);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    bsd_events_attach(sock);

    fd = bsd_fd_alloc(SocketBase, sock);
    if (fd < 0)
    {
        bsd_socket_destroy(sock);
        ami_free(sock);
        return bsd_fail(SocketBase, AMI_EMFILE);
    }

    return fd;
}

LONG bsd_bind(register LONG sock_fd            __asm("d0"),
              register struct sockaddr *name   __asm("a0"),
              register socklen_t namelen       __asm("d1"),
              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    ULONG      addr;
    UINT       port;
    UINT       status;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if ((sock->as_Flags & ASF_BOUND) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_sockaddr_in(SocketBase, name, namelen, &addr, &port) != 0)
        return -1;

    sock->as_LocalAddr = addr;
    sock->as_LocalPort = port;

    if ((sock->as_Flags & ASF_UDP) != 0)
    {
        status = nx_udp_socket_bind(&sock->as_Nx.udp,
                                    (port != 0) ? port : NX_ANY_PORT,
                                    NX_NO_WAIT);
        if (status != NX_SUCCESS)
            return bsd_fail(SocketBase, bsd_errno_from_nx(status));

        nx_udp_socket_port_get(&sock->as_Nx.udp, &port);
        sock->as_LocalPort = port;
        sock->as_Flags |= ASF_NXBOUND;
    }
    /*
     * TCP binds lazily: NetX Duo takes the port at nx_tcp_client_socket_bind
     * (outbound) or nx_tcp_server_socket_listen (inbound) time, and doing it
     * here would make the two mutually exclusive.
     */

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

    status = nx_tcp_socket_create(ip, &incoming->as_Nx.tcp, bsd_tcp_name,
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE, BSD_TCP_WINDOW,
                                  NX_NULL, NX_NULL);
    if (status != NX_SUCCESS)
    {
        ami_free(incoming);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    incoming->as_Flags     |= ASF_INCOMING;
    incoming->as_Parent     = sock;
    incoming->as_LocalPort  = sock->as_LocalPort;
    bsd_events_attach(incoming);

    status = nx_tcp_server_socket_listen(ip, sock->as_LocalPort,
                                         &incoming->as_Nx.tcp,
                                         (UINT)backlog, bsd_listen_callback);
    if (status != NX_SUCCESS)
    {
        nx_tcp_socket_delete(&incoming->as_Nx.tcp);
        ami_free(incoming);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

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
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    AmiSocket *incoming, *spare;
    NX_IP     *ip = netstack_ip();
    ULONG      peer_ip = 0, peer_port = 0;
    UINT       status;
    LONG       fd;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_LISTENING) == 0 || sock->as_Incoming == NULL)
        return bsd_fail(SocketBase, AMI_EINVAL);

    incoming = sock->as_Incoming;

    status = nx_tcp_server_socket_accept(
        &incoming->as_Nx.tcp,
        bsd_wait_option(sock, sock->as_RcvTimeout));

    if (status == NX_NOT_CONNECTED || status == NX_IN_PROGRESS ||
        status == NX_NO_PACKET)
        return bsd_fail(SocketBase, AMI_EWOULDBLOCK);

    if (status == NX_WAIT_ABORTED)
        return bsd_fail(SocketBase, AMI_EINTR);

    if (status != NX_SUCCESS)
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));

    /* Promote the parked socket to a descriptor of its own. */
    incoming->as_Flags &= ~(ASF_INCOMING | ASF_ACCEPTPEND);
    incoming->as_Flags |= ASF_CONNECTED | ASF_BOUND;
    incoming->as_Parent = NULL;
    incoming->as_Owner  = SocketBase;

    nx_tcp_socket_peer_info_get(&incoming->as_Nx.tcp, &peer_ip, &peer_port);
    incoming->as_PeerAddr = peer_ip;
    incoming->as_PeerPort = (UINT)peer_port;
    incoming->as_LocalPort = sock->as_ListenPort;

    fd = bsd_fd_alloc(SocketBase, incoming);
    if (fd < 0)
    {
        /* Put the socket back on the port rather than losing the listener. */
        nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);
        nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                      &incoming->as_Nx.tcp);

        incoming->as_Flags &= ~ASF_CONNECTED;
        incoming->as_Flags |= ASF_INCOMING;
        incoming->as_Parent = sock;

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
                                      NX_NULL, NX_NULL);
        if (status == NX_SUCCESS)
        {
            spare->as_Flags    |= ASF_INCOMING;
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
                sock->as_Incoming = spare;
                if (status == NX_CONNECTION_PENDING)
                    sock->as_Flags |= ASF_ACCEPTPEND;
            }
            else
            {
                nx_tcp_socket_delete(&spare->as_Nx.tcp);
                ami_free(spare);
                AMI_WARN("bsdsocket: relisten failed, status %ld", (LONG)status);
            }
        }
        else
        {
            ami_free(spare);
        }
    }

    if (addr != NULL && addrlen != NULL)
        bsd_sockaddr_out(addr, addrlen, incoming->as_PeerAddr,
                         incoming->as_PeerPort);

    return fd;
}

LONG bsd_connect(register LONG sock_fd          __asm("d0"),
                 register struct sockaddr *name __asm("a0"),
                 register socklen_t namelen     __asm("d1"),
                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    ULONG      addr;
    UINT       port;
    UINT       status;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (bsd_sockaddr_in(SocketBase, name, namelen, &addr, &port) != 0)
        return -1;

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

        sock->as_PeerAddr = addr;
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

    status = nx_tcp_client_socket_connect(
        &sock->as_Nx.tcp, addr, port,
        bsd_wait_option(sock, sock->as_SndTimeout));

    if (status == NX_SUCCESS)
    {
        sock->as_PeerAddr = addr;
        sock->as_PeerPort = port;
        sock->as_Flags   |= ASF_CONNECTED;
        sock->as_Flags   &= ~ASF_CONNECTING;
        return 0;
    }

    if (status == NX_IN_PROGRESS)
    {
        sock->as_PeerAddr = addr;
        sock->as_PeerPort = port;
        sock->as_Flags   |= ASF_CONNECTING;
        return bsd_fail(SocketBase, AMI_EINPROGRESS);
    }

    if (status == NX_WAIT_ABORTED)
        return bsd_fail(SocketBase, AMI_EINTR);

    if (status == NX_NOT_CONNECTED)
        return bsd_fail(SocketBase, AMI_ECONNREFUSED);

    return bsd_fail(SocketBase, bsd_errno_from_nx(status));
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
            nx_tcp_socket_disconnect(&sock->as_Nx.tcp, NX_NO_WAIT);
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
    bsd_socket_release(SocketBase, sock);

    return 0;
}
