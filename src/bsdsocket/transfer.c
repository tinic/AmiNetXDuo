/*
 * bsdsocket.library -- send/sendto/recv/recvfrom.
 *
 * NetX Duo is packet-oriented: a send allocates an NX_PACKET from the stack
 * pool, appends the caller's bytes to it and hands it over; a receive returns
 * a packet the caller has to drain. A BSD stream read need not consume a
 * whole packet, so a partially drained one is parked on the socket
 * (as_RxPending/as_RxOffset) until it runs out.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

/* Fallback segment size if the socket has not negotiated an MSS yet. */
#define BSD_DEFAULT_MSS     536

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

/* ------------------------------------------------------------------- send -- */

static LONG bsd_send_tcp(struct AmiSocketBase *base, AmiSocket *sock,
                         APTR buf, LONG len, LONG flags)
{
    NX_PACKET_POOL *pool = netstack_pool();
    ULONG           mss  = 0;
    LONG            sent = 0;
    ULONG           wait;

    (VOID)flags;

    if (pool == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(base, AMI_ENOTCONN);

    if ((sock->as_Flags & ASF_WRSHUT) != 0)
        return bsd_fail(base, AMI_EPIPE);

    nx_tcp_socket_mss_get(&sock->as_Nx.tcp, &mss);
    if (mss == 0)
        mss = BSD_DEFAULT_MSS;

    wait = bsd_wait_option(sock, sock->as_SndTimeout);

    while (sent < len)
    {
        NX_PACKET *packet = NX_NULL;
        ULONG      chunk  = (ULONG)(len - sent);
        UINT       status;

        if (chunk > mss)
            chunk = mss;

        status = nx_packet_allocate(pool, &packet, NX_TCP_PACKET, wait);
        if (status != NX_SUCCESS)
            break;

        status = nx_packet_data_append(packet, (UBYTE *)buf + sent, chunk,
                                       pool, wait);
        if (status != NX_SUCCESS)
        {
            nx_packet_release(packet);
            break;
        }

        status = nx_tcp_socket_send(&sock->as_Nx.tcp, packet, wait);
        if (status != NX_SUCCESS)
        {
            nx_packet_release(packet);

            if (sent > 0)
                return sent;            /* short write, as BSD allows */

            if (status == NX_WAIT_ABORTED)
                return bsd_fail(base, AMI_EINTR);
            if (status == NX_NO_PACKET || status == NX_TX_QUEUE_DEPTH ||
                status == NX_WINDOW_OVERFLOW)
                return bsd_fail(base, AMI_EWOULDBLOCK);
            if (status == NX_NOT_CONNECTED)
            {
                sock->as_Flags &= ~ASF_CONNECTED;
                return bsd_fail(base, AMI_EPIPE);
            }

            return bsd_fail(base, bsd_errno_from_nx(status));
        }

        sent += (LONG)chunk;

        /* A non-blocking socket takes what fits and reports the rest short. */
        if ((sock->as_Flags & ASF_NONBLOCK) != 0)
            wait = NX_NO_WAIT;
    }

    if (sent == 0 && len > 0)
        return bsd_fail(base, AMI_EWOULDBLOCK);

    return sent;
}

static LONG bsd_send_udp(struct AmiSocketBase *base, AmiSocket *sock,
                         APTR buf, LONG len, LONG flags,
                         ULONG addr, UINT port)
{
    NX_PACKET_POOL *pool   = netstack_pool();
    NX_PACKET      *packet = NX_NULL;
    ULONG           wait;
    UINT            status;

    (VOID)flags;

    if (pool == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    if (port == 0)
        return bsd_fail(base, AMI_EDESTADDRREQ);

    if ((sock->as_Flags & ASF_NXBOUND) == 0)
    {
        status = nx_udp_socket_bind(&sock->as_Nx.udp, NX_ANY_PORT, NX_NO_WAIT);
        if (status != NX_SUCCESS)
            return bsd_fail(base, bsd_errno_from_nx(status));

        nx_udp_socket_port_get(&sock->as_Nx.udp, &sock->as_LocalPort);
        sock->as_Flags |= ASF_NXBOUND | ASF_BOUND;
    }

    wait = bsd_wait_option(sock, sock->as_SndTimeout);

    status = nx_packet_allocate(pool, &packet, NX_UDP_PACKET, wait);
    if (status != NX_SUCCESS)
        return bsd_fail(base, (status == NX_NO_PACKET) ? AMI_EWOULDBLOCK
                                                       : bsd_errno_from_nx(status));

    status = nx_packet_data_append(packet, buf, (ULONG)len, pool, wait);
    if (status != NX_SUCCESS)
    {
        nx_packet_release(packet);
        return bsd_fail(base, (status == NX_NO_PACKET) ? AMI_EWOULDBLOCK
                                                       : bsd_errno_from_nx(status));
    }

    status = nx_udp_socket_send(&sock->as_Nx.udp, packet, addr, port);
    if (status != NX_SUCCESS)
    {
        nx_packet_release(packet);
        return bsd_fail(base, bsd_errno_from_nx(status));
    }

    return len;
}

LONG bsd_send(register LONG sock_fd __asm("d0"),
              register APTR buf     __asm("a0"),
              register LONG len     __asm("d1"),
              register LONG flags   __asm("d2"),
              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if (len < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((flags & MSG_OOB) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if ((sock->as_Flags & ASF_TCP) != 0)
        return bsd_send_tcp(SocketBase, sock, buf, len, flags);

    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

    return bsd_send_udp(SocketBase, sock, buf, len, flags,
                        sock->as_PeerAddr, sock->as_PeerPort);
}

LONG bsd_sendto(register LONG sock_fd        __asm("d0"),
                register APTR buf            __asm("a0"),
                register LONG len            __asm("d1"),
                register LONG flags          __asm("d2"),
                register struct sockaddr *to __asm("a1"),
                register socklen_t tolen     __asm("d3"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    ULONG      addr;
    UINT       port;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if (len < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((flags & MSG_OOB) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        /* A destination on a connected stream socket is ignored, as in BSD. */
        return bsd_send_tcp(SocketBase, sock, buf, len, flags);
    }

    if (to == NULL)
    {
        if ((sock->as_Flags & ASF_CONNECTED) == 0)
            return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

        addr = sock->as_PeerAddr;
        port = sock->as_PeerPort;
    }
    else if (bsd_sockaddr_in(SocketBase, to, tolen, &addr, &port) != 0)
    {
        return -1;
    }

    return bsd_send_udp(SocketBase, sock, buf, len, flags, addr, port);
}

/* ---------------------------------------------------------------- receive -- */

static LONG bsd_recv_tcp(struct AmiSocketBase *base, AmiSocket *sock,
                         APTR buf, LONG len, LONG flags)
{
    LONG  copied = 0;
    ULONG wait   = bsd_wait_option(sock, sock->as_RcvTimeout);
    BOOL  peek   = ((flags & MSG_PEEK) != 0);

    if ((sock->as_Flags & ASF_RDSHUT) != 0)
        return 0;

    while (copied < len)
    {
        ULONG length, avail, want, moved;
        UINT  status;

        if (sock->as_RxPending == NULL)
        {
            NX_PACKET *packet = NX_NULL;

            status = nx_tcp_socket_receive(&sock->as_Nx.tcp, &packet, wait);

            if (status == NX_SUCCESS)
            {
                sock->as_RxPending = packet;
                sock->as_RxOffset  = 0;
            }
            else
            {
                if (copied > 0)
                    break;                      /* return what we have */

                if (status == NX_WAIT_ABORTED)
                    return bsd_fail(base, AMI_EINTR);

                if (status == NX_NO_PACKET)
                    return bsd_fail(base, AMI_EWOULDBLOCK);

                if (status == NX_NOT_CONNECTED)
                {
                    /*
                     * The peer closed. A socket that was connected reports
                     * end-of-file; one that never was reports ENOTCONN.
                     */
                    if ((sock->as_Flags & (ASF_CONNECTED | ASF_EOF)) != 0)
                    {
                        sock->as_Flags |= ASF_EOF;
                        return 0;
                    }

                    return bsd_fail(base, AMI_ENOTCONN);
                }

                return bsd_fail(base, bsd_errno_from_nx(status));
            }
        }

        length = bsd_packet_len(sock->as_RxPending);
        if (length <= sock->as_RxOffset)
        {
            bsd_drop_pending(sock);
            continue;
        }

        avail = length - sock->as_RxOffset;
        want  = (ULONG)(len - copied);
        if (want > avail)
            want = avail;

        moved  = 0;
        status = nx_packet_data_extract_offset(sock->as_RxPending,
                                               sock->as_RxOffset,
                                               (UBYTE *)buf + copied,
                                               want, &moved);
        if (status != NX_SUCCESS || moved == 0)
        {
            if (copied > 0)
                break;
            return bsd_fail(base, bsd_errno_from_nx(status));
        }

        copied += (LONG)moved;

        if (peek)
            break;                      /* leave the packet where it is */

        sock->as_RxOffset += moved;
        if (sock->as_RxOffset >= length)
            bsd_drop_pending(sock);

        /*
         * A stream read returns as soon as it has anything, unless the caller
         * asked for the lot.
         */
        if ((flags & MSG_WAITALL) == 0)
            break;

        wait = NX_NO_WAIT;
    }

    return copied;
}

static LONG bsd_recv_udp(struct AmiSocketBase *base, AmiSocket *sock,
                         APTR buf, LONG len, LONG flags,
                         struct sockaddr *from, socklen_t *fromlen)
{
    NX_PACKET *packet = NX_NULL;
    ULONG      src_ip = 0, length, want, moved = 0;
    UINT       src_port = 0;
    UINT       status;
    BOOL       peek = ((flags & MSG_PEEK) != 0);

    if (sock->as_RxPending != NULL)
    {
        packet = sock->as_RxPending;
    }
    else
    {
        status = nx_udp_socket_receive(&sock->as_Nx.udp, &packet,
                                       bsd_wait_option(sock, sock->as_RcvTimeout));
        if (status != NX_SUCCESS)
        {
            if (status == NX_WAIT_ABORTED)
                return bsd_fail(base, AMI_EINTR);
            if (status == NX_NO_PACKET)
                return bsd_fail(base, AMI_EWOULDBLOCK);

            return bsd_fail(base, bsd_errno_from_nx(status));
        }
    }

    nx_udp_source_extract(packet, &src_ip, &src_port);

    length = bsd_packet_len(packet);
    want   = (ULONG)len;
    if (want > length)
        want = length;

    if (want > 0)
        nx_packet_data_extract_offset(packet, 0, buf, want, &moved);

    if (from != NULL && fromlen != NULL)
        bsd_sockaddr_out(from, fromlen, src_ip, src_port);

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
    return (LONG)moved;
}

LONG bsd_recv(register LONG sock_fd __asm("d0"),
              register APTR buf     __asm("a0"),
              register LONG len     __asm("d1"),
              register LONG flags   __asm("d2"),
              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if (len < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((flags & MSG_OOB) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if (len == 0)
        return 0;

    if ((sock->as_Flags & ASF_TCP) != 0)
        return bsd_recv_tcp(SocketBase, sock, buf, len, flags);

    return bsd_recv_udp(SocketBase, sock, buf, len, flags, NULL, NULL);
}

LONG bsd_recvfrom(register LONG sock_fd          __asm("d0"),
                  register APTR buf              __asm("a0"),
                  register LONG len              __asm("d1"),
                  register LONG flags            __asm("d2"),
                  register struct sockaddr *addr __asm("a1"),
                  register socklen_t *addrlen    __asm("a2"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    LONG       result;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    if (len < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((flags & MSG_OOB) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        result = bsd_recv_tcp(SocketBase, sock, buf, len, flags);
        if (result >= 0 && addr != NULL && addrlen != NULL)
            bsd_sockaddr_out(addr, addrlen, sock->as_PeerAddr, sock->as_PeerPort);

        return result;
    }

    if (len == 0)
        return 0;

    return bsd_recv_udp(SocketBase, sock, buf, len, flags, addr, addrlen);
}
