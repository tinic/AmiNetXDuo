/*
 * bsdsocket.library -- send/sendto/sendmsg and recv/recvfrom/recvmsg.
 *
 * NetX Duo is packet-oriented: a send allocates an NX_PACKET from the stack
 * pool, appends the caller's bytes to it and hands it over; a receive returns
 * a packet the caller has to drain. A BSD stream read need not consume a
 * whole packet, so a partially drained one is parked on the socket
 * (as_RxPending/as_RxOffset) until it runs out.
 *
 * SCATTER/GATHER
 *
 * Every path here works on an iovec list, and the flat-buffer calls hand it a
 * one-element list on the stack. That is not gold plating: it is what makes
 * sendmsg()/recvmsg() the same code as send()/recv() rather than a second
 * implementation, and NetX Duo takes to it without a bounce buffer --
 * nx_packet_data_append() called once per iovec builds one packet, and
 * nx_packet_data_extract_offset() scatters straight into each destination.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>

/* Fallback segment size if the socket has not negotiated an MSS yet. */
#define BSD_DEFAULT_MSS     536

/*
 * struct msghdr / struct iovec are ABI, not ours to guess.
 *
 * The definitions we compile against come from the Roadshow netinclude
 * headers in the toolchain's ndk-include (sys/socket.h, sys/uio.h) -- the
 * only <sys/socket.h> and <sys/uio.h> on the include path; this toolchain's
 * newlib tree has neither, so there is no chance of picking up the wrong
 * lineage the way ndk-include/pwd.h shadows the usergroup struct passwd.
 *
 * That header carries the POSIX / 4.4BSD-Lite msghdr with ancillary data
 * (msg_control / msg_controllen / msg_flags), NOT the older 4.3BSD form with
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

/* ---------------------------------------------------------------- packet -- */

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
                                  ULONG want, NX_PACKET_POOL *pool, ULONG wait)
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
            return (done > 0) ? (LONG)done : -1;

        bsd_iov_advance(cur, chunk);
        done += chunk;
    }

    return (LONG)done;
}

/* ------------------------------------------------------------------- send -- */

static LONG bsd_send_tcp(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags)
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
        LONG       filled;
        UINT       status;

        if (chunk > mss)
            chunk = mss;

        status = nx_packet_allocate(pool, &packet, NX_TCP_PACKET, wait);
        if (status != NX_SUCCESS)
            break;

        filled = bsd_packet_append_iov(packet, cur, chunk, pool, wait);
        if (filled <= 0)
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

        sent += filled;

        /* A non-blocking socket takes what fits and reports the rest short. */
        if ((sock->as_Flags & ASF_NONBLOCK) != 0)
            wait = NX_NO_WAIT;
    }

    if (sent == 0 && len > 0)
        return bsd_fail(base, AMI_EWOULDBLOCK);

    return sent;
}

static LONG bsd_send_udp(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags,
                         const NXD_ADDRESS *addr, UINT port)
{
    NX_PACKET_POOL *pool   = netstack_pool();
    NX_PACKET      *packet = NX_NULL;
    ULONG           wait;
    LONG            filled;
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

    /* A zero-length datagram is legal and is sent as an empty packet. */
    filled = (len > 0)
                 ? bsd_packet_append_iov(packet, cur, (ULONG)len, pool, wait)
                 : 0;
    if (filled < len)
    {
        nx_packet_release(packet);
        return bsd_fail(base, AMI_ENOBUFS);
    }

    /* nxd_, not nx_: the v4 wrapper wraps the address and calls this. */
    status = nxd_udp_socket_send(&sock->as_Nx.udp, packet,
                                 (NXD_ADDRESS *)addr, port);
    if (status != NX_SUCCESS)
    {
        nx_packet_release(packet);
        return bsd_fail(base, bsd_errno_from_nx(status));
    }

    return len;
}

/*
 * One raw datagram out.
 *
 * The caller writes the protocol payload -- an ICMP message, say -- and
 * nxd_ip_raw_packet_send() prepends the IP header from the socket's protocol,
 * TTL and TOS. There is no partial send: a datagram either goes whole or not
 * at all.
 */
static LONG bsd_send_raw(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, const NXD_ADDRESS *addr)
{
    NX_PACKET_POOL *pool   = netstack_pool();
    NX_PACKET      *packet = NX_NULL;
    ULONG           wait;
    LONG            filled;
    UINT            status;

    if (pool == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    if (addr->nxd_ip_version != NX_IP_VERSION_V4 ||
        addr->nxd_ip_address.v4 == 0)
        return bsd_fail(base, AMI_EDESTADDRREQ);

    wait = bsd_wait_option(sock, sock->as_SndTimeout);

    status = nx_packet_allocate(pool, &packet, NX_IP_PACKET, wait);
    if (status != NX_SUCCESS)
        return bsd_fail(base, (status == NX_NO_PACKET)
                                  ? AMI_EWOULDBLOCK
                                  : bsd_errno_from_nx(status));

    filled = (len > 0)
                 ? bsd_packet_append_iov(packet, cur, (ULONG)len, pool, wait)
                 : 0;
    if (filled < len)
    {
        nx_packet_release(packet);
        return bsd_fail(base, AMI_EMSGSIZE);
    }

    /* Consumes the packet either way. */
    if (bsd_raw_send_packet(base, sock, packet, addr) != 0)
        return -1;

    return len;
}

/* ---------------------------------------------------------------- receive -- */

static LONG bsd_recv_tcp(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags)
{
    LONG  copied = 0;
    ULONG wait   = bsd_wait_option(sock, sock->as_RcvTimeout);
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
             * Only the FIRST read of a call may block: after that, a stream
             * receive returns whatever the socket buffer holds. MSG_WAITALL
             * asks for the opposite -- keep waiting until the caller's
             * buffers are full or the connection ends.
             */
            ULONG now = (first || (flags & MSG_WAITALL) != 0) ? wait
                                                              : NX_NO_WAIT;

            status = nx_tcp_socket_receive(&sock->as_Nx.tcp, &packet, now);

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
        if (sock->as_RxOffset >= length)
            bsd_drop_pending(sock);

        first = FALSE;
    }

    return copied;
}

/*
 * One datagram, scattered across the caller's buffers.
 *
 * `truncated` reports whether the datagram was longer than the buffers, which
 * is what recvmsg() turns into MSG_TRUNC. The excess is discarded, as BSD
 * does.
 */
static LONG bsd_recv_udp(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags,
                         struct sockaddr *from, socklen_t *fromlen,
                         BOOL *truncated)
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
 * One raw datagram in, whole IP header included.
 *
 * That is what 4.4BSD delivers on a raw read and what every ping and
 * traceroute parses; see the header of raw.c. As with UDP, whatever does not
 * fit the caller's buffers is discarded.
 */
static LONG bsd_recv_raw(struct AmiSocketBase *base, AmiSocket *sock,
                         BsdIovCursor *cur, LONG len, LONG flags,
                         struct sockaddr *from, socklen_t *fromlen,
                         BOOL *truncated)
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
        packet = bsd_raw_receive(sock, bsd_wait_option(sock,
                                                       sock->as_RcvTimeout));
        if (packet == NX_NULL)
            return bsd_fail(base, AMI_EWOULDBLOCK);
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

/*
 * Everything below funnels into these two so that send/sendto/sendmsg and
 * recv/recvfrom/recvmsg cannot drift apart.
 */
static LONG bsd_send_iov(struct AmiSocketBase *base, AmiSocket *sock,
                         const struct iovec *iov, LONG iovcnt, LONG len,
                         LONG flags, const NXD_ADDRESS *addr, UINT port)
{
    BsdIovCursor cur;
    LONG         result;

    bsd_iov_init(&cur, iov, iovcnt);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_RAW) != 0)
        result = bsd_send_raw(base, sock, &cur, len, addr);
    else if ((sock->as_Flags & ASF_TCP) != 0)
        result = bsd_send_tcp(base, sock, &cur, len, flags);
    else
        result = bsd_send_udp(base, sock, &cur, len, flags, addr, port);

    bsd_nx_leave(base);

    return result;
}

static LONG bsd_recv_iov(struct AmiSocketBase *base, AmiSocket *sock,
                         const struct iovec *iov, LONG iovcnt, LONG len,
                         LONG flags, struct sockaddr *from,
                         socklen_t *fromlen, BOOL *truncated)
{
    BsdIovCursor cur;
    LONG         result;

    bsd_iov_init(&cur, iov, iovcnt);

    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        result = bsd_recv_raw(base, sock, &cur, len, flags, from, fromlen,
                              truncated);
    }
    else if ((sock->as_Flags & ASF_TCP) != 0)
    {
        result = bsd_recv_tcp(base, sock, &cur, len, flags);
        if (result >= 0 && from != NULL && fromlen != NULL)
            bsd_sockaddr_put(sock, from, fromlen, &sock->as_PeerAddr,
                             sock->as_PeerPort);
        if (truncated != NULL)
            *truncated = FALSE;         /* a stream never truncates */
    }
    else
    {
        result = bsd_recv_udp(base, sock, &cur, len, flags, from, fromlen,
                              truncated);
    }

    bsd_nx_leave(base);

    return result;
}

/*
 * The destination a sendto()/sendmsg() supplied has to belong to the same
 * family as the socket. 0 = ok, -1 = errno set.
 *
 * The one interesting case is a v4-mapped destination on a dual-stack socket:
 * bsd_addr_normalise() turns ::ffff:a.b.c.d into a plain IPv4 address, because
 * NetX Duo would otherwise put the mapped form in an IPv6 header and send it
 * to a host that has no IPv6 at all.
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

    /*
     * MSG_OOB is TCP's, and only TCP's: there is no urgent data on a datagram
     * or a raw socket to send or to wait for. oob.c has the rest.
     */
    if ((flags & MSG_OOB) != 0 && (sock->as_Flags & ASF_TCP) == 0)
        return bsd_fail(base, AMI_EOPNOTSUPP);

    return 0;
}

/*
 * send(..., MSG_OOB): every byte goes, and the LAST one is the urgent one.
 *
 * That is 4.4BSD's rule, and the one telnet and ftp rely on -- they write a
 * short command whose final byte is the signal. The leading bytes take the
 * ordinary path; only the last needs oob.c.
 */
static LONG bsd_send_oob(struct AmiSocketBase *base, AmiSocket *sock,
                         const UBYTE *buf, LONG len)
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

        sent = bsd_send_tcp(base, sock, &cur, len - 1, 0);
        if (sent < len - 1)
        {
            bsd_nx_leave(base);
            return sent;                /* short write, or -1 with errno set */
        }
    }

    rc = bsd_oob_send(base, sock, buf[len - 1]);

    bsd_nx_leave(base);

    if (rc < 0)
        return (sent > 0) ? sent : -1;

    return sent + 1;
}

/*
 * recv(..., MSG_OOB): the byte the peer marked urgent, once.
 *
 * EINVAL when there is none is what 4.4BSD answers, and there is no waiting
 * variant: a caller that wants to be told when one arrives selects on
 * exceptfds or takes SIGURG.
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

/* ---------------------------------------------------------------- vectors -- */

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

    if ((sock->as_Flags & ASF_TCP) == 0 &&
        (sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

    if ((flags & MSG_OOB) != 0)
        return bsd_send_oob(SocketBase, sock, (const UBYTE *)buf, len);

    iov.iov_base = buf;
    iov.iov_len  = (size_t)len;

    return bsd_send_iov(SocketBase, sock, &iov, 1, len, flags,
                        &sock->as_PeerAddr, sock->as_PeerPort);
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
    UINT          port = 0;

    if (bsd_transfer_check(SocketBase, sock, len, flags) != 0)
        return -1;

    if (buf == NULL && len > 0)
        return bsd_fail(SocketBase, AMI_EFAULT);

    bsd_addr_from_v4(&addr, 0UL);

    if ((flags & MSG_OOB) != 0)
        return bsd_send_oob(SocketBase, sock, (const UBYTE *)buf, len);

    /* A destination on a connected stream socket is ignored, as in BSD. */
    if ((sock->as_Flags & ASF_TCP) == 0)
    {
        if (to == NULL)
        {
            if ((sock->as_Flags & ASF_CONNECTED) == 0)
                return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

            addr = sock->as_PeerAddr;
            port = sock->as_PeerPort;
        }
        else if (bsd_sockaddr_get(SocketBase, to, tolen, &addr, &port,
                                  NULL) != 0)
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

    return bsd_send_iov(SocketBase, sock, &iov, 1, len, flags, &addr, port);
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

    if (len == 0)
        return 0;

    if ((flags & MSG_OOB) != 0)
        return bsd_recv_oob(SocketBase, sock, (UBYTE *)buf, len);

    iov.iov_base = buf;
    iov.iov_len  = (size_t)len;

    return bsd_recv_iov(SocketBase, sock, &iov, 1, len, flags, NULL, NULL,
                        NULL);
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

    if ((sock->as_Flags & ASF_TCP) == 0 && len == 0)
        return 0;

    if ((flags & MSG_OOB) != 0)
        return bsd_recv_oob(SocketBase, sock, (UBYTE *)buf, len);

    iov.iov_base = buf;
    iov.iov_len  = (size_t)len;

    return bsd_recv_iov(SocketBase, sock, &iov, 1, len, flags, addr, addrlen,
                        NULL);
}

/* ------------------------------------------------------ sendmsg / recvmsg -- */

/*
 * ANCILLARY DATA
 *
 * msg_control is ignored on send and reported as empty on receive. That is
 * not a shortcut: the only thing a BSD stack passes through SCM_RIGHTS is a
 * file descriptor, and AmigaOS has no descriptor passing over a socket at all
 * -- handing a socket to another task is ObtainSocket()/ReleaseSocket()
 * (handoff.c), a completely different mechanism. There is therefore nothing
 * that could legitimately arrive in msg_control, so MSG_CTRUNC is never set:
 * no ancillary data is ever dropped, because none can ever exist.
 */
LONG bsd_sendmsg(register LONG sock_fd        __asm("d0"),
                 register struct msghdr *msg  __asm("a0"),
                 register LONG flags          __asm("d1"),
                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;
    UINT        port = 0;
    LONG        total;

    if (bsd_transfer_check(SocketBase, sock, 0, flags) != 0)
        return -1;

    /* MSG_OOB is a send()/sendto() shape here: the urgent byte is the last
       one written, and finding it in a scatter list buys nothing that a
       caller cannot do with one more send(). */
    if ((flags & MSG_OOB) != 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if (msg == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    total = bsd_iov_total(msg->msg_iov, (LONG)msg->msg_iovlen);
    if (total < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    bsd_addr_from_v4(&addr, 0UL);

    if ((sock->as_Flags & ASF_TCP) == 0)
    {
        if (msg->msg_name == NULL)
        {
            if ((sock->as_Flags & ASF_CONNECTED) == 0)
                return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

            addr = sock->as_PeerAddr;
            port = sock->as_PeerPort;
        }
        else if (bsd_sockaddr_get(SocketBase,
                                  (const struct sockaddr *)msg->msg_name,
                                  (socklen_t)msg->msg_namelen,
                                  &addr, &port, NULL) != 0)
        {
            return -1;
        }
        else if (bsd_dest_check(SocketBase, sock, &addr) != 0)
        {
            return -1;
        }
    }

    return bsd_send_iov(SocketBase, sock, msg->msg_iov,
                        (LONG)msg->msg_iovlen, total, flags, &addr, port);
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

    if (msg->msg_name != NULL && msg->msg_namelen > 0)
    {
        from    = (struct sockaddr *)msg->msg_name;
        fromlen = (socklen_t)msg->msg_namelen;
    }

    /* msg_flags is an out parameter; whatever the caller left there is not
       an input and must not survive. */
    msg->msg_flags = 0;

    result = bsd_recv_iov(SocketBase, sock, msg->msg_iov,
                          (LONG)msg->msg_iovlen, total, flags,
                          from, (from != NULL) ? &fromlen : NULL,
                          &truncated);

    if (result < 0)
        return result;

    if (from != NULL)
        msg->msg_namelen = (socklen_t)fromlen;

    /* No ancillary data exists on this platform -- see the note above. */
    msg->msg_controllen = 0;

    if (truncated)
        msg->msg_flags |= MSG_TRUNC;

    return result;
}
