/*
 * bsdsocket.library -- socket options, ioctls and names.
 *
 * setsockopt/getsockopt/IoctlSocket/getsockname/getpeername/getdtablesize
 * plus Dup2Socket, which is nearly free once descriptors are reference
 * counted.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>
#include <netinet/tcp.h>

static LONG bsd_opt_get_long(struct AmiSocketBase *base, APTR optval,
                             socklen_t *optlen, LONG value)
{
    socklen_t len;

    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    len = *optlen;
    if (len >= (socklen_t)sizeof(LONG))
    {
        *(LONG *)optval = value;
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (len >= (socklen_t)sizeof(WORD))
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

static LONG bsd_opt_set_long(struct AmiSocketBase *base, APTR optval,
                             socklen_t optlen, LONG *value)
{
    if (optval == NULL)
        return bsd_fail(base, AMI_EFAULT);

    if (optlen >= (socklen_t)sizeof(LONG))
        *value = *(LONG *)optval;
    else if (optlen >= (socklen_t)sizeof(WORD))
        *value = *(WORD *)optval;
    else
        return bsd_fail(base, AMI_EINVAL);

    return 0;
}

/* struct timeval -> ThreadX ticks, rounded up so a tiny timeout still waits. */
static ULONG bsd_timeval_ticks(const struct timeval *tv)
{
    ULONG ticks;

    if (tv == NULL)
        return 0;

    ticks = (ULONG)tv->tv_secs * (ULONG)NX_IP_PERIODIC_RATE;
    ticks += ((ULONG)tv->tv_micro * (ULONG)NX_IP_PERIODIC_RATE + 999999UL) / 1000000UL;

    if (ticks == 0 && ((ULONG)tv->tv_secs != 0 || (ULONG)tv->tv_micro != 0))
        ticks = 1;

    return ticks;
}

static VOID bsd_ticks_timeval(ULONG ticks, struct timeval *tv)
{
    tv->tv_secs  = ticks / (ULONG)NX_IP_PERIODIC_RATE;
    tv->tv_micro = (ticks % (ULONG)NX_IP_PERIODIC_RATE) *
                   (1000000UL / (ULONG)NX_IP_PERIODIC_RATE);
}

/* ------------------------------------------------------------ setsockopt -- */

LONG bsd_setsockopt(register LONG sock_fd    __asm("d0"),
                    register LONG level      __asm("d1"),
                    register LONG optname    __asm("d2"),
                    register APTR optval     __asm("a0"),
                    register socklen_t optlen __asm("d3"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    LONG       value = 0;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (level == SOL_SOCKET)
    {
        switch (optname)
        {
            case SO_REUSEADDR:
            case SO_REUSEPORT:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value != 0)
                    sock->as_Flags |= ASF_REUSEADDR;
                else
                    sock->as_Flags &= ~ASF_REUSEADDR;
                return 0;

            case SO_BROADCAST:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value != 0)
                    sock->as_Flags |= ASF_BROADCAST;
                else
                    sock->as_Flags &= ~ASF_BROADCAST;
                return 0;

            case SO_KEEPALIVE:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value != 0)
                    sock->as_Flags |= ASF_KEEPALIVE;
                else
                    sock->as_Flags &= ~ASF_KEEPALIVE;
                return 0;

            case SO_OOBINLINE:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value != 0)
                    sock->as_Flags |= ASF_OOBINLINE;
                else
                    sock->as_Flags &= ~ASF_OOBINLINE;
                return 0;

            case SO_LINGER:
                if (optval == NULL ||
                    optlen < (socklen_t)sizeof(struct linger))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_LingerOn   = ((struct linger *)optval)->l_onoff;
                sock->as_LingerTime = ((struct linger *)optval)->l_linger;
                return 0;

            case SO_RCVBUF:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value < 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_RcvBuf = value;
                if ((sock->as_Flags & ASF_TCP) != 0 && value > 0)
                {
                    if (bsd_nx_enter(SocketBase) != 0)
                        return bsd_fail(SocketBase, AMI_ENETDOWN);
                    nx_tcp_socket_receive_queue_max_set(&sock->as_Nx.tcp,
                                                        (UINT)(value / 1024 + 1));
                    bsd_nx_leave(SocketBase);
                }
                return 0;

            case SO_SNDBUF:
                /* Remembered and reported back; the wire-side limit is NetX
                   Duo's own transmit queue, which is not caller-tunable. */
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value < 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_SndBuf = value;
                return 0;

            case SO_RCVTIMEO:
                if (optval == NULL ||
                    optlen < (socklen_t)sizeof(struct timeval))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_RcvTimeout = bsd_timeval_ticks((struct timeval *)optval);
                return 0;

            case SO_SNDTIMEO:
                if (optval == NULL ||
                    optlen < (socklen_t)sizeof(struct timeval))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_SndTimeout = bsd_timeval_ticks((struct timeval *)optval);
                return 0;

            case SO_EVENTMASK:
                /* The AmiTCP V4 async event API: which FD_* bits to report. */
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                sock->as_EventMask = (ULONG)value;
                return 0;

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

    if (level == IPPROTO_TCP)
    {
        switch (optname)
        {
            case TCP_NODELAY:
                /* NetX Duo does not implement Nagle, so this is always on. */
                return 0;

            case TCP_MAXSEG:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if ((sock->as_Flags & ASF_TCP) == 0)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                if (bsd_nx_enter(SocketBase) != 0)
                    return bsd_fail(SocketBase, AMI_ENETDOWN);
                nx_tcp_socket_mss_set(&sock->as_Nx.tcp, (ULONG)value);
                bsd_nx_leave(SocketBase);
                return 0;

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

    if (level == IPPROTO_IP)
    {
        switch (optname)
        {
            case IP_TTL:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                sock->as_Ttl = value;
                return 0;

            case IP_TOS:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                sock->as_Tos = value;
                return 0;

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

#ifdef AMINETXDUO_IPV6
    if (level == AMI_IPPROTO_IPV6)
        return bsd_setsockopt_ipv6(SocketBase, sock, optname, optval, optlen);
#endif

    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
}

/* ------------------------------------------------------------ getsockopt -- */

LONG bsd_getsockopt(register LONG sock_fd     __asm("d0"),
                    register LONG level       __asm("d1"),
                    register LONG optname     __asm("d2"),
                    register APTR optval      __asm("a0"),
                    register socklen_t *optlen __asm("a1"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (level == SOL_SOCKET)
    {
        switch (optname)
        {
            case SO_ERROR:
            {
                LONG err = sock->as_SoError;

                sock->as_SoError = 0;       /* SO_ERROR clears on read */
                return bsd_opt_get_long(SocketBase, optval, optlen, err);
            }

            case SO_TYPE:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        (LONG)sock->as_Type);

            case SO_ACCEPTCONN:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                    ((sock->as_Flags & ASF_LISTENING) != 0) ? 1 : 0);

            case SO_REUSEADDR:
            case SO_REUSEPORT:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                    ((sock->as_Flags & ASF_REUSEADDR) != 0) ? 1 : 0);

            case SO_BROADCAST:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                    ((sock->as_Flags & ASF_BROADCAST) != 0) ? 1 : 0);

            case SO_KEEPALIVE:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                    ((sock->as_Flags & ASF_KEEPALIVE) != 0) ? 1 : 0);

            case SO_OOBINLINE:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                    ((sock->as_Flags & ASF_OOBINLINE) != 0) ? 1 : 0);

            case SO_EVENTMASK:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        (LONG)sock->as_EventMask);

            case SO_RCVBUF:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        (sock->as_RcvBuf != 0)
                                            ? sock->as_RcvBuf
                                            : BSD_TCP_WINDOW);

            case SO_SNDBUF:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        (sock->as_SndBuf != 0)
                                            ? sock->as_SndBuf
                                            : BSD_TCP_WINDOW);

            case SO_LINGER:
                if (optval == NULL || optlen == NULL ||
                    *optlen < (socklen_t)sizeof(struct linger))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                ((struct linger *)optval)->l_onoff  = sock->as_LingerOn;
                ((struct linger *)optval)->l_linger = sock->as_LingerTime;
                *optlen = (socklen_t)sizeof(struct linger);
                return 0;

            case SO_RCVTIMEO:
            case SO_SNDTIMEO:
                if (optval == NULL || optlen == NULL ||
                    *optlen < (socklen_t)sizeof(struct timeval))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                bsd_ticks_timeval((optname == SO_RCVTIMEO)
                                      ? sock->as_RcvTimeout
                                      : sock->as_SndTimeout,
                                  (struct timeval *)optval);
                *optlen = (socklen_t)sizeof(struct timeval);
                return 0;

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

    if (level == IPPROTO_TCP)
    {
        ULONG mss = 0;

        switch (optname)
        {
            case TCP_NODELAY:
                return bsd_opt_get_long(SocketBase, optval, optlen, 1);

            case TCP_MAXSEG:
                if ((sock->as_Flags & ASF_TCP) != 0 &&
                    bsd_nx_enter(SocketBase) == 0)
                {
                    nx_tcp_socket_mss_get(&sock->as_Nx.tcp, &mss);
                    bsd_nx_leave(SocketBase);
                }
                return bsd_opt_get_long(SocketBase, optval, optlen, (LONG)mss);

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

    if (level == IPPROTO_IP)
    {
        switch (optname)
        {
            case IP_TTL:
                return bsd_opt_get_long(SocketBase, optval, optlen, sock->as_Ttl);

            case IP_TOS:
                return bsd_opt_get_long(SocketBase, optval, optlen, sock->as_Tos);

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

#ifdef AMINETXDUO_IPV6
    if (level == AMI_IPPROTO_IPV6)
        return bsd_getsockopt_ipv6(SocketBase, sock, optname, optval, optlen);
#endif

    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
}

/* ----------------------------------------------------------- IoctlSocket -- */

LONG bsd_IoctlSocket(register LONG sock_fd __asm("d0"),
                     register ULONG req    __asm("d1"),
                     register APTR argp    __asm("a0"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    ULONG      available = 0;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    switch (req)
    {
        case FIONBIO:
            if (argp == NULL)
                return bsd_fail(SocketBase, AMI_EFAULT);

            if (*(LONG *)argp != 0)
                sock->as_Flags |= ASF_NONBLOCK;
            else
                sock->as_Flags &= ~ASF_NONBLOCK;
            return 0;

        case FIONREAD:
            if (argp == NULL)
                return bsd_fail(SocketBase, AMI_EFAULT);

            if (sock->as_RxPending != NULL)
            {
                ULONG length = 0;

                nx_packet_length_get(sock->as_RxPending, &length);
                if (length > sock->as_RxOffset)
                    available = length - sock->as_RxOffset;
            }

            if (bsd_nx_enter(SocketBase) != 0)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

            if ((sock->as_Flags & ASF_RAW) != 0)
            {
                /* One datagram, as on any message socket. */
                available += bsd_raw_available(sock);
            }
            else
            {
                ULONG queued = 0;
                UINT  status;

                status = ((sock->as_Flags & ASF_TCP) != 0)
                    ? nx_tcp_socket_bytes_available(&sock->as_Nx.tcp, &queued)
                    : nx_udp_socket_bytes_available(&sock->as_Nx.udp, &queued);

                if (status == NX_SUCCESS)
                    available += queued;
            }

            bsd_nx_leave(SocketBase);

            *(LONG *)argp = (LONG)available;
            return 0;

        case SIOCATMARK:
            if (argp == NULL)
                return bsd_fail(SocketBase, AMI_EFAULT);

            /*
             * "Is the next byte the urgent one?"
             *
             * This implementation is always OOBINLINE (see oob.c), so the
             * urgent byte is in the stream and the mark is simply "one has
             * arrived and recv(MSG_OOB) has not taken it yet". A caller that
             * uses SIOCATMARK to decide when to stop discarding -- which is
             * what telnet does -- gets the right answer at the right moment;
             * a caller that expects the byte to be missing from the stream
             * does not, and that is the divergence oob.c documents.
             */
            *(LONG *)argp = ((sock->as_Flags & ASF_OOBHAVE) != 0) ? 1 : 0;
            return 0;

        case FIOASYNC:
            if (argp == NULL)
                return bsd_fail(SocketBase, AMI_EFAULT);

            /* Asynchronous notification is the SO_EVENTMASK/SIGIO path. */
            sock->as_EventMask = (*(LONG *)argp != 0)
                                     ? (FD_READ | FD_WRITE | FD_ACCEPT |
                                        FD_CONNECT | FD_CLOSE | FD_ERROR)
                                     : 0;
            return 0;

        default:
            return bsd_fail(SocketBase, AMI_ENOSYS);
    }
}

/* ------------------------------------------------------------------ names -- */

LONG bsd_getsockname(register LONG sock_fd          __asm("d0"),
                     register struct sockaddr *name __asm("a0"),
                     register socklen_t *namelen    __asm("a1"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (name == NULL || namelen == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    addr = sock->as_LocalAddr;

#ifdef AMINETXDUO_IPV6
    if (addr.nxd_ip_version == NX_IP_VERSION_V6 &&
        (addr.nxd_ip_address.v6[0] | addr.nxd_ip_address.v6[1] |
         addr.nxd_ip_address.v6[2] | addr.nxd_ip_address.v6[3]) == 0)
    {
        /*
         * Bound to in6addr_any. Report the source address the stack would
         * actually put on a packet to this socket's peer -- asking NetX Duo's
         * own RFC 6724 selection rather than guessing means the answer matches
         * what the peer will see, which for link-local vs global is not a
         * detail an application can work out for itself.
         */
        ULONG chosen[4];

        if ((sock->as_Flags & ASF_CONNECTED) != 0 &&
            sock->as_PeerAddr.nxd_ip_version == NX_IP_VERSION_V6 &&
            netstack_ipv6_source_for(sock->as_PeerAddr.nxd_ip_address.v6,
                                     chosen))
        {
            addr.nxd_ip_address.v6[0] = chosen[0];
            addr.nxd_ip_address.v6[1] = chosen[1];
            addr.nxd_ip_address.v6[2] = chosen[2];
            addr.nxd_ip_address.v6[3] = chosen[3];
        }
    }
    else
#endif
    if (addr.nxd_ip_version == NX_IP_VERSION_V4 &&
        addr.nxd_ip_address.v4 == 0)
    {
        NX_IP *ip = netstack_ip();

        /* An unbound-to-INADDR_ANY socket reports the interface address. */
        if (ip != NULL && (sock->as_Flags & ASF_CONNECTED) != 0)
            bsd_addr_from_v4(&addr,
                             ip->nx_ip_interface[0].nx_interface_ip_address);
    }

    bsd_sockaddr_put(sock, name, namelen, &addr, sock->as_LocalPort);

    return 0;
}

LONG bsd_getpeername(register LONG sock_fd          __asm("d0"),
                     register struct sockaddr *name __asm("a0"),
                     register socklen_t *namelen    __asm("a1"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (name == NULL || namelen == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(SocketBase, AMI_ENOTCONN);

    bsd_sockaddr_put(sock, name, namelen, &sock->as_PeerAddr,
                     sock->as_PeerPort);

    return 0;
}

int bsd_getdtablesize(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    if (SocketBase->sb_TableSize == 0)
        return BSD_DEFAULT_DTABLESIZE;

    return (int)SocketBase->sb_TableSize;
}

LONG bsd_Dup2Socket(register LONG old_socket __asm("d0"),
                    register LONG new_socket __asm("d1"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, old_socket);
    AmiSocket *victim;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (new_socket < 0)
    {
        LONG fd = bsd_fd_alloc(SocketBase, sock);

        if (fd < 0)
            return bsd_fail(SocketBase, AMI_EMFILE);

        sock->as_RefCount++;

        return fd;
    }

    if (new_socket >= SocketBase->sb_TableSize)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (new_socket == old_socket)
        return new_socket;

    victim = bsd_lookup(SocketBase, new_socket);
    if (victim != NULL)
    {
        bsd_fd_free(SocketBase, new_socket);

        if (bsd_nx_enter(SocketBase) == 0)
        {
            bsd_socket_release(SocketBase, victim);
            bsd_nx_leave(SocketBase);
        }
    }

    SocketBase->sb_Table[new_socket] = sock;
    sock->as_RefCount++;

    return new_socket;
}
