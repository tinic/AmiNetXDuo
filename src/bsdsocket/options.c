/*
 * bsdsocket.library, socket options, ioctls and names.
 *
 * setsockopt/getsockopt/IoctlSocket/getsockname/getpeername/getdtablesize
 * plus Dup2Socket, which is nearly free once descriptors are reference
 * counted.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "interfaces.h"
#include "opt_time.h"

#include <sys/sockio.h>

#include <proto/exec.h>
#include <netinet/tcp.h>

/* TCP_USER_TIMEOUT, TCP_STALLINFO, struct TcpStallInfo. */
#include "aminetxduo/tcp.h"

/* NX_TCP_MAXIMUM_RX_QUEUE, SO_RCVBUF's ceiling in a low-watermark build. */
#include "nx_tcp.h"

/* For _nx_ip_route_find(): getsockname() on a socket bound to INADDR_ANY has
   to know which interface the packets leave by. */
#include "nx_ip.h"
#ifdef AMINETXDUO_IPV6
#include "../ipv6/ipv6_srcsel.h"
#endif

/*
 * SO_RCVBUF and SO_SNDBUF arrive in bytes and NetX Duo counts both queues in
 * packets, so one has to be turned into the other. A full-MTU segment is what
 * either queue actually holds, and rounding up means a caller asking for one
 * byte still gets one packet rather than none.
 */
#define BSD_OPT_SEGMENT     1460

static UINT bsd_opt_packets(LONG bytes, UINT ceiling)
{
    ULONG packets = ((ULONG)bytes + (BSD_OPT_SEGMENT - 1UL)) / BSD_OPT_SEGMENT;

    if (packets == 0UL)
        packets = 1UL;
    if (packets > (ULONG)ceiling)
        packets = (ULONG)ceiling;

    return (UINT)packets;
}

static LONG bsd_opt_get_long(struct AmiSocketBase *base, APTR optval,
                             socklen_t *optlen, LONG value)
{
    socklen_t len;

    if (optval == NULL || optlen == NULL)
        return bsd_fail(base, AMI_EFAULT);

    len = *optlen;
    if (len >= (socklen_t)sizeof(LONG))
    {
        bsd_bcopy(&value, optval, sizeof(value));
        *optlen = (socklen_t)sizeof(LONG);
    }
    else if (len >= (socklen_t)sizeof(WORD))
    {
        WORD short_value = (WORD)value;

        bsd_bcopy(&short_value, optval, sizeof(short_value));
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
        bsd_bcopy(optval, value, sizeof(*value));
    else if (optlen >= (socklen_t)sizeof(WORD))
    {
        WORD short_value;

        bsd_bcopy(optval, &short_value, sizeof(short_value));
        *value = short_value;
    }
    else
        return bsd_fail(base, AMI_EINVAL);

    return 0;
}

/*
 * IP_TTL and IP_TOS onto the live NetX socket.
 *
 * Both are arguments _nx_ip_packet_send() takes from the socket on every send,
 * so writing them here is what puts them on the wire. Without it the caller's
 * value was stored, echoed back by getsockopt, and never used. raw.c reads
 * as_Ttl and as_Tos directly and needs nothing from here.
 *
 * NetX Duo carries the TOS octet in bits 16..23 of a ULONG, that is where
 * NX_IP_NORMAL and NX_IP_MIN_DELAY sit, and NX_IP_TOS_MASK is 0x00FF0000.
 *
 * The IPv6 halves are not covered: _nx_ipv6_packet_send() takes the traffic
 * class as a literal 0 from both the TCP and the UDP send paths, and takes the
 * TCP hop limit from nx_ipv6_hop_limit on the NX_IP rather than from the
 * socket. The UDP hop limit is nx_udp_socket_time_to_live, so
 * IPV6_UNICAST_HOPS reaches the wire on a UDP socket through the line below.
 *
 * Must be called inside a bsd_nx_enter() bracket: this is live NX state.
 */
VOID bsd_opt_apply_ip(AmiSocket *sock)
{
    UINT  ttl = (UINT)(sock->as_Ttl & 0xFF);
    ULONG tos = ((ULONG)sock->as_Tos & 0xFFUL) << 16;

    if ((sock->as_Flags & (ASF_RAW | ASF_DELETED)) != 0)
        return;

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        sock->as_Nx.tcp.nx_tcp_socket_time_to_live    = ttl;
        sock->as_Nx.tcp.nx_tcp_socket_type_of_service = tos;
    }
    else if ((sock->as_Flags & ASF_UDP) != 0)
    {
        sock->as_Nx.udp.nx_udp_socket_time_to_live    = ttl;
        sock->as_Nx.udp.nx_udp_socket_type_of_service = tos;
    }
}

/*
 * struct timeval -> ThreadX ticks, rounded up so a tiny timeout still waits.
 * NX_WAIT_FOREVER is a sentinel rather than a finite delay, and zero is the
 * value the socket object uses for "no timeout", so saturation stops one tick
 * below the sentinel.
 */
static BOOL bsd_timeval_ticks(const struct timeval *tv, ULONG *out)
{
    ULONG seconds;
    ULONG micros;
    ULONG fraction;
    ULONG ticks;
    const ULONG maximum = NX_WAIT_FOREVER - 1UL;

    if (tv == NULL || out == NULL)
        return FALSE;

    /* devices/timer.h spells these fields ULONG even though the socket ABI
       treats a value with the sign bit set as a negative timeout.  Test the
       ABI value explicitly; comparing the fields themselves with zero is
       dead code and a -Wtype-limits error in the shipping cross build. */
    if ((LONG)tv->tv_secs < 0 || (LONG)tv->tv_micro < 0)
        return FALSE;

    seconds = (ULONG)tv->tv_secs;
    micros  = (ULONG)tv->tv_micro;

    if (micros >= 1000000UL)
        return FALSE;

    fraction = (micros * (ULONG)NX_IP_PERIODIC_RATE + 999999UL) /
               1000000UL;

    if (seconds > maximum / (ULONG)NX_IP_PERIODIC_RATE)
    {
        *out = maximum;
        return TRUE;
    }

    ticks = seconds * (ULONG)NX_IP_PERIODIC_RATE;
    if (fraction > maximum - ticks)
        ticks = maximum;
    else
        ticks += fraction;

    if (ticks == 0 && (seconds != 0 || micros != 0))
        ticks = 1;

    *out = ticks;
    return TRUE;
}

/*
 * Milliseconds -> ThreadX ticks, rounded up. Rounding up matters here: at 50
 * ticks a second anything under 20 ms truncates to zero, and zero is the
 * value that means "no deadline", so a caller asking for the shortest
 * deadline it can name gets no deadline at all.
 *
 * Splitting whole seconds from the remainder keeps the intermediate multiply
 * inside a ULONG.  The accepted one-day maximum is beyond the point where
 * ms * NX_IP_PERIODIC_RATE wraps at the Amiga's 50 Hz tick.
 */
#define BSD_OPT_MAX_MS      86400000UL      /* a day */

static ULONG bsd_ticks_ms(ULONG ticks)
{
    const ULONG rate = (ULONG)NX_IP_PERIODIC_RATE;
    const ULONG max  = (ULONG)-1;
    ULONG       whole;
    ULONG       fraction;

    if (ticks / rate > max / 1000UL)
        return max;

    whole    = (ticks / rate) * 1000UL;
    fraction = ((ticks % rate) * 1000UL) / rate;

    return (fraction > max - whole) ? max : whole + fraction;
}

static VOID bsd_ticks_timeval(ULONG ticks, struct timeval *tv)
{
    tv->tv_secs  = ticks / (ULONG)NX_IP_PERIODIC_RATE;
    tv->tv_micro = (ticks % (ULONG)NX_IP_PERIODIC_RATE) *
                   (1000000UL / (ULONG)NX_IP_PERIODIC_RATE);
}

/* ------------------------------------------------------------ setsockopt, */

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
            /*
             * SO_REUSEADDR lets a bind take a port whose only holders are in
             * TIME-WAIT, which is what a server restarting after its last
             * client disconnected runs into: the port is unavailable for up
             * to 2MSL and the program looks broken.
             *
             * It cannot displace a live listener or an established
             * connection, nx_tcp_socket_reuse_address_set() says so and the
             * bind enforces it, which is the BSD rule.
             *
             * SO_REUSEPORT is the same flag here.  BSD's REUSEPORT also
             * allows several live sockets on one port and shares arrivals
             * between them.  NetX Duo demultiplexes to one socket, so that
             * half has nowhere to go and pretending otherwise delivers every
             * connection to whichever socket bound first.
             *
             * Set on the NX socket as well as recorded, and only while the
             * socket is unbound: after bind() the flag has already been read
             * and the call does nothing.
             */
            case SO_REUSEADDR:
            case SO_REUSEPORT:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value != 0)
                    sock->as_Flags |= ASF_REUSEADDR;
                else
                    sock->as_Flags &= ~ASF_REUSEADDR;

                if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) == ASF_TCP)
                {
                    if (bsd_nx_enter(SocketBase) != 0)
                        return bsd_fail(SocketBase, AMI_ENETDOWN);
                    (VOID)nx_tcp_socket_reuse_address_set(&sock->as_Nx.tcp,
                                                          (value != 0) ? NX_TRUE
                                                                       : NX_FALSE);
                    bsd_nx_leave(SocketBase);
                }
                return 0;

            /*
             * Likewise. BSD uses this as permission, sendto() to a broadcast
             * address is EACCES without it, and this stack has never asked,
             * so enforcing it now starts failing sends that work today.
             * The flag is kept so getsockopt answers what was set.
             */
            case SO_BROADCAST:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value != 0)
                    sock->as_Flags |= ASF_BROADCAST;
                else
                    sock->as_Flags &= ~ASF_BROADCAST;
                return 0;

            /*
             * SO_KEEPALIVE reaches the NX socket only when
             * NX_ENABLE_TCP_KEEPALIVE is defined.  Without it
             * nx_tcp_periodic_processing.c's keepalive block is compiled out
             * and an idle connection is never probed.  The flag is kept
             * alongside because getsockopt() must answer for a socket that is
             * not a live NX one yet, and the two must agree.
             */
            case SO_KEEPALIVE:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value != 0)
                    sock->as_Flags |= ASF_KEEPALIVE;
                else
                    sock->as_Flags &= ~ASF_KEEPALIVE;
#ifdef NX_ENABLE_TCP_KEEPALIVE
                if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) == ASF_TCP)
                {
                    /* Live NX socket state, so it needs the bracket every
                       neighbouring option takes. The IP thread reads this
                       field in nx_tcp_periodic_processing(). */
                    if (bsd_nx_enter(SocketBase) != 0)
                        return bsd_fail(SocketBase, AMI_ENETDOWN);
                    sock->as_Nx.tcp.nx_tcp_socket_keepalive_enabled =
                        (value != 0) ? NX_TRUE : NX_FALSE;
                    bsd_nx_leave(SocketBase);
                }
#endif
                return 0;

            /*
             * Accepted, stored nowhere, and getsockopt always answers 1: the
             * urgent byte is delivered in the stream whatever this says, which
             * is oob.c's first documented divergence. Echoing back a 0 the
             * caller set hides the one fact it cannot discover any other way.
             */
            case SO_OOBINLINE:
                return (bsd_opt_set_long(SocketBase, optval, optlen,
                                         &value) != 0) ? -1 : 0;

            case SO_LINGER:
            {
                struct linger lin;

                if (optval == NULL ||
                    optlen < (socklen_t)sizeof(struct linger))
                    return bsd_fail(SocketBase, AMI_EINVAL);

                /* Copied out: the caller's buffer need not be aligned for the
                   loads below, the same reason mcast.c copies its mreq. */
                bsd_bcopy(optval, &lin, sizeof lin);

                /*
                 * bsd_socket_close() turns l_linger into a tick count, so a
                 * negative one becomes about 497 days and CloseSocket() never
                 * comes back. 4.4BSD's sosetopt() bounds it the same way, at
                 * SHRT_MAX/hz seconds.
                 */
                if (lin.l_linger < 0 ||
                    lin.l_linger > (LONG)(32767L / NX_IP_PERIODIC_RATE))
                    return bsd_fail(SocketBase, AMI_EINVAL);

                sock->as_LingerOn   = lin.l_onoff;
                sock->as_LingerTime = lin.l_linger;
                return 0;
            }

            /*
             * SO_RCVBUF and SO_SNDBUF are answered in bytes because that is
             * what a caller sets, and applied in packets because that is the
             * only unit NetX Duo counts either queue in.
             *
             * On TCP nothing is applied. The receive queue depth is the only
             * knob NetX Duo offers, and the whole body of
             * nx_tcp_socket_receive_queue_max_set() is inside
             * NX_ENABLE_LOW_WATERMARK, which this port does not define. The
             * note at the end of nx_user.h says why. Defining it is a piece of
             * work with its own measurement. The advertised
             * window is sized from the packet pool at socket-create time
             * (ami_bsd_tcp_window()) and is not settable afterwards. The call
             * used to be made unconditionally with its NX_NOT_SUPPORTED
             * discarded, which read as an option that worked.
             */
            case SO_RCVBUF:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value < 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_RcvBuf = value;
#ifdef NX_ENABLE_LOW_WATERMARK
                if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) == ASF_TCP &&
                    value > 0)
                {
                    if (bsd_nx_enter(SocketBase) != 0)
                        return bsd_fail(SocketBase, AMI_ENETDOWN);
                    (VOID)nx_tcp_socket_receive_queue_max_set(
                        &sock->as_Nx.tcp,
                        bsd_opt_packets(value, NX_TCP_MAXIMUM_RX_QUEUE));
                    bsd_nx_leave(SocketBase);
                }
#endif
                if ((sock->as_Flags & (ASF_UDP | ASF_DELETED)) == ASF_UDP &&
                    value > 0)
                {
                    /* The UDP receive queue is caller-tunable: it is a
                       datagram count on the socket, which nx_udp_socket_create
                       took from bsd_udp_queue_max(). Lowered or raised, never
                       past what the pool can hold. */
                    if (bsd_nx_enter(SocketBase) != 0)
                        return bsd_fail(SocketBase, AMI_ENETDOWN);
                    sock->as_Nx.udp.nx_udp_socket_queue_maximum =
                        bsd_opt_packets(value, BSD_UDP_QUEUE_CEILING);
                    bsd_nx_leave(SocketBase);
                }
                return 0;

            case SO_SNDBUF:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value < 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_SndBuf = value;
                if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) == ASF_TCP &&
                    value > 0)
                {
                    /* nx_tcp_socket_transmit_configure() does this as well,
                       but it rewrites the retransmit timer and retry count too,
                       and
                       sets nx_tcp_socket_rtt_configured, which stops the RTT
                       estimator for the life of the socket. */
                    if (bsd_nx_enter(SocketBase) != 0)
                        return bsd_fail(SocketBase, AMI_ENETDOWN);
                    sock->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum =
                        bsd_opt_packets(value, NX_TCP_MAXIMUM_TX_QUEUE);
                    sock->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum_default =
                        bsd_opt_packets(value, NX_TCP_MAXIMUM_TX_QUEUE);
                    bsd_nx_leave(SocketBase);
                }
                return 0;

            case SO_RCVTIMEO:
            {
                struct timeval tv;

                if (optval == NULL ||
                    optlen < (socklen_t)sizeof(struct timeval))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                bsd_bcopy(optval, &tv, sizeof(tv));
                if (!bsd_timeval_ticks(&tv, &sock->as_RcvTimeout))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                return 0;
            }

            case SO_SNDTIMEO:
            {
                struct timeval tv;

                if (optval == NULL ||
                    optlen < (socklen_t)sizeof(struct timeval))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                bsd_bcopy(optval, &tv, sizeof(tv));
                if (!bsd_timeval_ticks(&tv, &sock->as_SndTimeout))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                return 0;
            }

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
            /*
             * NetX Duo has no Nagle -- nx_tcp_socket_send_internal.c sends a
             * segment as soon as there is one to send, and the vendored tree
             * has no small-segment hold anywhere in it. So TCP_NODELAY is
             * permanently 1 and the get side answering 1 is the truth, not a
             * placeholder.
             *
             * The RFC 1122 4.2.3.4 rule that IS there,
             * _nx_tcp_socket_sws_send_permitted(), is not one: it withholds
             * a write when the peer's WINDOW is a sliver, never because the
             * write is small, so a one-byte send into an open window leaves
             * immediately whatever else is in flight. That is asserted in
             * tests/netstack/host/test_tcp_sws_host.c, because this answer
             * depends on it.
             *
             * Which makes 0 the one value that cannot be honoured, and taking
             * it and returning 0 was the lie: a caller that asked for Nagle
             * and got success had been told the stack would coalesce its
             * small writes, and nothing does. It fails instead, so a ported
             * client sees a refusal it can act on rather than a silent no-op.
             * A caller asking for 1 is asking for what is already true and is
             * told so.
             *
             * The arguments are still checked, and the socket type still has
             * to be TCP. Accepting this on a UDP socket, which it did by
             * returning before looking at anything, told a caller that a level
             * it does not have was configured.
             */
            case TCP_NODELAY:
                if ((sock->as_Flags & ASF_TCP) == 0)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value == 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                return 0;

            case TCP_MAXSEG:
            {
                UINT status;

                if ((sock->as_Flags & ASF_TCP) == 0)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;

                /*
                 * The floor is RFC 791's 68-byte minimum datagram less the two
                 * 20-byte headers. The ceiling is what is left of a maximum
                 * datagram. A negative went in as a four-billion MSS, which
                 * NetX Duo stored without complaint.
                 */
                if (value < 28 || value > 65495)
                    return bsd_fail(SocketBase, AMI_EINVAL);

                if ((sock->as_Flags & ASF_DELETED) != 0)
                    return bsd_fail(SocketBase, AMI_EINVAL);

                if (bsd_nx_enter(SocketBase) != 0)
                    return bsd_fail(SocketBase, AMI_ENETDOWN);
                status = nx_tcp_socket_mss_set(&sock->as_Nx.tcp, (ULONG)value);
                bsd_nx_leave(SocketBase);

                /* NetX Duo refuses once the socket has left SYN_SENT, which is
                   BSD's rule as well: the MSS is negotiated, not changed. */
                if (status != NX_SUCCESS)
                    return bsd_fail(SocketBase,
                                    (status == NX_NOT_SUCCESSFUL)
                                        ? AMI_EISCONN
                                        : AMI_EINVAL);
                return 0;
            }

            /*
             * The deadline that replaces "wait out the ladder". Written to the
             * NX socket rather than kept here and polled, so it holds whether
             * or not a thread is in a call on this socket -- a connection dead
             * for longer than the caller asked for is dead even if nobody is
             * currently reading it, and select() has to say so.
             */
            case TCP_USER_TIMEOUT:
                if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) != ASF_TCP)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value < 0 || (ULONG)value > BSD_OPT_MAX_MS)
                    return bsd_fail(SocketBase, AMI_EINVAL);

                if (bsd_nx_enter(SocketBase) != 0)
                    return bsd_fail(SocketBase, AMI_ENETDOWN);
                sock->as_UserTimeout = (ULONG)value;
                sock->as_Nx.tcp.nx_tcp_socket_user_timeout =
                    bsd_ms_ticks((ULONG)value,
                                 (ULONG)NX_IP_PERIODIC_RATE);
                bsd_nx_leave(SocketBase);
                return 0;

            /* TCP_STALLINFO is read-only and falls through to the default,
               which is where every optname this level does not set goes. */
            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

    if (level == IPPROTO_IP)
    {
        /* IP_PKTINFO / IP_RECVDSTADDR: RFC 3542's plumbing, IPv4 half.
           `optlen` is a register variable, so the copy is not optional. */
        socklen_t len   = optlen;
        LONG      owned = bsd_cmsg_option(SocketBase, sock, level, optname,
                                          optval, &len, TRUE);

        if (owned <= 0)
            return owned;

        switch (optname)
        {
            /*
             * -1 is "the default", as it is for IPV6_UNICAST_HOPS. The range
             * is checked rather than masked: 256 read back as 256 and went on
             * the wire as 0, so every packet was dropped by the first router,
             * and the IPv6 sibling in in6.c has always refused it.
             */
            case IP_TTL:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value < -1 || value > 255)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_Ttl = (value < 0) ? (LONG)NX_IP_TIME_TO_LIVE : value;
                if (bsd_nx_enter(SocketBase) != 0)
                    return bsd_fail(SocketBase, AMI_ENETDOWN);
                bsd_opt_apply_ip(sock);
                bsd_nx_leave(SocketBase);
                return 0;

            /*
             * IP_HDRINCL is only meaningful on a raw socket. BSD returns
             * ENOPROTOOPT on anything else.
             */
            case IP_HDRINCL:
                if (sock->as_Type != SOCK_RAW)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                sock->as_HdrIncl = (value != 0);
                return 0;

            /* Same range and same -1, matching IPV6_TCLASS in in6.c. */
            case IP_TOS:
                if (bsd_opt_set_long(SocketBase, optval, optlen, &value) != 0)
                    return -1;
                if (value < -1 || value > 255)
                    return bsd_fail(SocketBase, AMI_EINVAL);
                sock->as_Tos = (value < 0) ? 0 : value;
                if (bsd_nx_enter(SocketBase) != 0)
                    return bsd_fail(SocketBase, AMI_ENETDOWN);
                bsd_opt_apply_ip(sock);
                bsd_nx_leave(SocketBase);
                return 0;

#ifdef AMINETXDUO_MULTICAST
            case IP_ADD_MEMBERSHIP:
            case IP_DROP_MEMBERSHIP:
            case IP_MULTICAST_IF:
            case IP_MULTICAST_TTL:
            case IP_MULTICAST_LOOP:
                return bsd_mcast_setopt(SocketBase, sock, optname, optval,
                                        optlen);
#endif

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

#ifdef AMINETXDUO_IPV6
    if (level == IPPROTO_IPV6 || level == IPPROTO_ICMPV6)
        return bsd_setsockopt_ipv6(SocketBase, sock, level, optname, optval,
                                   optlen);
#endif

    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
}

/* ------------------------------------------------------------ getsockopt, */

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
            /*
             * SO_ERROR clears on read, but only on a read that happened.
             * Clearing first meant a bad optval returned EFAULT and destroyed
             * the pending error on the way out, and a non-blocking connect()
             * has no other way to find out why it failed.
             */
            case SO_ERROR:
            {
                LONG socket_error;

                /* Validate before consuming the pending error. */
                if (optval == NULL || optlen == NULL)
                    return bsd_fail(SocketBase, AMI_EFAULT);
                if (*optlen < (socklen_t)sizeof(WORD))
                    return bsd_fail(SocketBase, AMI_EINVAL);

                /* The IP task posts asynchronous errors. Pair the read and
                   clear so a newer error cannot land between them and be
                   erased by this read. */
                Forbid();
                socket_error = sock->as_SoError;
                sock->as_SoError = 0;

                /* The UDP ICMP callback records the BSD errno above, then
                   NetX records the same error for its next receive. SO_ERROR
                   consumes a pending socket error, so clear both copies or
                   recv() reports one ICMP message twice. */
                if ((sock->as_Flags & ASF_UDP) != 0)
                    sock->as_Nx.udp.nx_udp_socket_icmp_error = NX_SUCCESS;

                Permit();

                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        socket_error);
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

            /* Always in force, see the set side. */
            case SO_OOBINLINE:
                return bsd_opt_get_long(SocketBase, optval, optlen, 1);

            case SO_EVENTMASK:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        (LONG)sock->as_EventMask);

            /*
             * With nothing set, this reports the window the socket got.  A
             * TCP socket's is sized from the packet pool and the connected
             * socket count at creation (ami_bsd_tcp_window()), so
             * BSD_TCP_WINDOW is only the floor and is not always any socket's
             * actual window.
             */
            case SO_RCVBUF:
            {
                LONG rcvbuf = sock->as_RcvBuf;

                if (rcvbuf == 0)
                {
                    rcvbuf = ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) ==
                              ASF_TCP)
                        ? (LONG)sock->as_Nx.tcp.nx_tcp_socket_rx_window_default
                        : (LONG)BSD_TCP_WINDOW;
                }

                return bsd_opt_get_long(SocketBase, optval, optlen, rcvbuf);
            }

            case SO_SNDBUF:
                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        (sock->as_SndBuf != 0)
                                            ? sock->as_SndBuf
                                            : BSD_TCP_WINDOW);

            case SO_LINGER:
            {
                struct linger lin;

                if (optval == NULL || optlen == NULL ||
                    *optlen < (socklen_t)sizeof(struct linger))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                lin.l_onoff  = sock->as_LingerOn;
                lin.l_linger = sock->as_LingerTime;
                bsd_bcopy(&lin, optval, sizeof(lin));
                *optlen = (socklen_t)sizeof(struct linger);
                return 0;
            }

            case SO_RCVTIMEO:
            case SO_SNDTIMEO:
            {
                struct timeval tv;

                if (optval == NULL || optlen == NULL ||
                    *optlen < (socklen_t)sizeof(struct timeval))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                bsd_ticks_timeval((optname == SO_RCVTIMEO)
                                      ? sock->as_RcvTimeout
                                      : sock->as_SndTimeout,
                                  &tv);
                bsd_bcopy(&tv, optval, sizeof(tv));
                *optlen = (socklen_t)sizeof(struct timeval);
                return 0;
            }

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

    if (level == IPPROTO_TCP)
    {
        ULONG mss = 0;

        switch (optname)
        {
            /* Both refuse a socket that has no TCP under it, as the set side
               does. Answering 1 and 0 there described a level the socket does
               not have.

               1 is not a stored value: there is no Nagle in the stack, so it
               is the state of every TCP socket for its whole life, and the set
               side refuses the only request that would contradict it. */
            case TCP_NODELAY:
                if ((sock->as_Flags & ASF_TCP) == 0)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                return bsd_opt_get_long(SocketBase, optval, optlen, 1);

            case TCP_MAXSEG:
                if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) != ASF_TCP)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                if (bsd_nx_enter(SocketBase) != 0)
                    return bsd_fail(SocketBase, AMI_ENETDOWN);
                nx_tcp_socket_mss_get(&sock->as_Nx.tcp, &mss);
                bsd_nx_leave(SocketBase);
                return bsd_opt_get_long(SocketBase, optval, optlen, (LONG)mss);

            case TCP_USER_TIMEOUT:
                if ((sock->as_Flags & ASF_TCP) == 0)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        (LONG)sock->as_UserTimeout);

            /*
             * The four numbers that make a stall visible while it is running.
             * Read under the NX lock because the fast periodic timer writes
             * three of them.
             */
            case TCP_STALLINFO:
            {
                struct TcpStallInfo info;

                if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) != ASF_TCP)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                if (optval == NULL || optlen == NULL ||
                    *optlen < (socklen_t)sizeof(struct TcpStallInfo))
                    return bsd_fail(SocketBase, AMI_EINVAL);

                if (bsd_nx_enter(SocketBase) != 0)
                    return bsd_fail(SocketBase, AMI_ENETDOWN);
                info.tsi_Stalled =
                    bsd_ticks_ms(sock->as_Nx.tcp.nx_tcp_socket_stall_ticks);
                info.tsi_Retransmits =
                    sock->as_Nx.tcp.nx_tcp_socket_timeout_retries;
                info.tsi_Rto =
                    bsd_ticks_ms(sock->as_Nx.tcp.nx_tcp_socket_timeout);
                bsd_nx_leave(SocketBase);

                info.tsi_UserTimeout = sock->as_UserTimeout;

                bsd_bcopy(&info, optval, sizeof(info));
                *optlen = (socklen_t)sizeof(struct TcpStallInfo);
                return 0;
            }

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

    if (level == IPPROTO_IP)
    {
        LONG owned = bsd_cmsg_option(SocketBase, sock, level, optname, optval,
                                     optlen, FALSE);

        if (owned <= 0)
            return owned;

        switch (optname)
        {
            case IP_TTL:
                return bsd_opt_get_long(SocketBase, optval, optlen, sock->as_Ttl);

            /* Raw only, as on the set side. */
            case IP_HDRINCL:
                if (sock->as_Type != SOCK_RAW)
                    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
                return bsd_opt_get_long(SocketBase, optval, optlen,
                                        sock->as_HdrIncl);

            case IP_TOS:
                return bsd_opt_get_long(SocketBase, optval, optlen, sock->as_Tos);

#ifdef AMINETXDUO_MULTICAST
            case IP_ADD_MEMBERSHIP:
            case IP_DROP_MEMBERSHIP:
            case IP_MULTICAST_IF:
            case IP_MULTICAST_TTL:
            case IP_MULTICAST_LOOP:
                return bsd_mcast_getopt(SocketBase, sock, optname, optval,
                                        optlen);
#endif

            default:
                return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
        }
    }

#ifdef AMINETXDUO_IPV6
    if (level == IPPROTO_IPV6 || level == IPPROTO_ICMPV6)
        return bsd_getsockopt_ipv6(SocketBase, sock, level, optname, optval,
                                   optlen);
#endif

    return bsd_fail(SocketBase, AMI_ENOPROTOOPT);
}

/* ----------------------------------------------------------- IoctlSocket, */

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

            /* shutdown(SHUT_RD) makes every later receive an EOF even if
               packets were queued beforehand. Report what recv() can return,
               not bytes that are now intentionally hidden. */
            if ((sock->as_Flags & ASF_RDSHUT) != 0)
            {
                *(LONG *)argp = 0;
                return 0;
            }

            if (sock->as_RxPending != NULL)
            {
                ULONG length = 0;

                nx_packet_length_get(sock->as_RxPending, &length);
                if (length > sock->as_RxOffset)
                    available = length - sock->as_RxOffset;

                /* MSG_PEEK parks one complete UDP/raw record outside its
                   NetX queue. FIONREAD on a message socket describes that
                   next record only; adding the queue head would cross a
                   datagram boundary. Streams intentionally continue below. */
                if ((sock->as_Flags & ASF_TCP) == 0)
                {
                    *(LONG *)argp = (LONG)available;
                    return 0;
                }
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

                if ((sock->as_Flags & ASF_TCP) != 0)
                {
                    status = nx_tcp_socket_bytes_available(&sock->as_Nx.tcp,
                                                           &queued);
                    if (status == NX_SUCCESS)
                        available += queued;
                }
                else
                {
                    available += bsd_udp_available(sock);
                }
            }

            bsd_nx_leave(SocketBase);

            *(LONG *)argp = (LONG)available;
            return 0;

        case SIOCATMARK:
            if (argp == NULL)
                return bsd_fail(SocketBase, AMI_EFAULT);

            /*
             * "Is the next byte the urgent one?"  This implementation is
             * always OOBINLINE (see oob.c), so the urgent byte is in the
             * stream and the mark means "one has arrived and recv(MSG_OOB) has
             * not taken it yet".  A caller that uses SIOCATMARK to decide when
             * to stop discarding, as telnet does, gets the right answer.  A
             * caller that expects the byte to be absent from the stream does
             * not, which is the divergence oob.c documents.
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

        /*
         * The interface queries, answered in interfaces.c where the naming
         * rule and the gather live.  They ignore the socket: BSD requires one
         * to be passed and says nothing about which, and libpcap opens a
         * throwaway AF_INET/SOCK_DGRAM for it.
         */
        case SIOCGIFCONF:
        case SIOCGIFFLAGS:
        case SIOCGIFADDR:
        case SIOCGIFNETMASK:
        case SIOCGIFBRDADDR:
            return bsd_if_ioctl(req, argp, SocketBase);

        /* The autodoc's errno for a request this object does not answer:
           "[ENOTTY] The specified request does not apply to the kind of
           object that the descriptor s references." */
        default:
            return bsd_fail(SocketBase, AMI_ENOTTY);
    }
}

/* ------------------------------------------------------------------ names, */

/*
 * The IPv4 address a socket bound to INADDR_ANY should report: the address of
 * the interface its packets leave by, so getsockname() names what the peer
 * sees.
 *
 * RFC 6724 is IPv6 only -- src/ipv6/ipv6_srcsel.c has no IPv4 half -- and
 * nothing in NetX Duo picks a v4 source on its own.  _nx_ip_packet_send() stamps the packet with the address of
 * whichever interface the route matched, so the route is the whole question.
 *
 * A connected TCP socket has had it answered already: the connect, or the
 * accept, wrote the interface it settled on into
 * nx_tcp_socket_connect_interface.  Everything else asks the route table,
 * with a null interface hint so the call chooses rather than checks (socket.c
 * has the other form).
 *
 * 127.0.0.0/8 is answered before either, because _nx_ip_route_find() cannot:
 * it walks NX_MAX_PHYSICAL_INTERFACES and NetX Duo keeps the loopback
 * interface past the end of that, so a loopback destination falls through to
 * the default gateway and would name the Ethernet address.
 */
static BOOL bsd_v4_source_for(AmiSocket *sock, ULONG *addr_out)
{
    NX_IP        *ip       = netstack_ip();
    NX_INTERFACE *nxif     = NX_NULL;
    ULONG         next_hop = 0;
    ULONG         peer;

    if (ip == NULL)
        return FALSE;

    if (sock->as_PeerAddr.nxd_ip_version != NX_IP_VERSION_V4)
        return FALSE;

    peer = sock->as_PeerAddr.nxd_ip_address.v4;

#ifndef NX_DISABLE_LOOPBACK_INTERFACE
    if ((peer >> 24) == 127UL)
        nxif = &ip->nx_ip_interface[NX_LOOPBACK_INTERFACE];
    else
#endif
    if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) == ASF_TCP)
        nxif = sock->as_Nx.tcp.nx_tcp_socket_connect_interface;

    if (nxif == NX_NULL)
    {
        if (peer == 0)
            return FALSE;

        tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
        (VOID)_nx_ip_route_find(ip, peer, &nxif, &next_hop);
        tx_mutex_put(&ip->nx_ip_protection);
    }

    if (nxif == NX_NULL || nxif->nx_interface_valid == 0 ||
        nxif->nx_interface_ip_address == 0)
        return FALSE;

    *addr_out = nxif->nx_interface_ip_address;

    return TRUE;
}

LONG bsd_getsockname(register LONG sock_fd          __asm("d0"),
                     register struct sockaddr *name __asm("a0"),
                     register socklen_t *namelen    __asm("a1"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;
    ULONG       scope;
    BOOL        bracketed = FALSE;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (name == NULL || namelen == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    addr = sock->as_LocalAddr;
    scope = sock->as_LocalScopeId;

    /* A wildcard endpoint is resolved from live route/address tables below.
       Adopt this Exec task before taking nx_ip_protection, and keep the
       ThreadX scheduler still while the TCP connection interface is read. */
    if ((sock->as_Flags & ASF_CONNECTED) != 0 &&
        ((addr.nxd_ip_version == NX_IP_VERSION_V4 &&
          addr.nxd_ip_address.v4 == 0UL)
#ifdef AMINETXDUO_IPV6
         || (addr.nxd_ip_version == NX_IP_VERSION_V6 &&
             (addr.nxd_ip_address.v6[0] | addr.nxd_ip_address.v6[1] |
              addr.nxd_ip_address.v6[2] | addr.nxd_ip_address.v6[3]) == 0UL)
#endif
        ))
    {
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);
        bracketed = TRUE;
    }

#ifdef AMINETXDUO_IPV6
    if (addr.nxd_ip_version == NX_IP_VERSION_V6 &&
        (addr.nxd_ip_address.v6[0] | addr.nxd_ip_address.v6[1] |
         addr.nxd_ip_address.v6[2] | addr.nxd_ip_address.v6[3]) == 0)
    {
        /*
         * Bound to in6addr_any. This reports the source address the stack
         * puts on a packet to this socket's peer, through the RFC 6724
         * selection in src/ipv6/ipv6_srcsel.c, so the answer matches what the
         * peer sees. An application cannot choose between link-local and
         * global for itself.
         */
        ULONG chosen[4];
        LONG  interface_index = -1;

        /* TCP has already selected its route, so report that established
           endpoint rather than whatever a new route lookup would choose now.
           A scoped datagram/raw peer makes the same interface choice through
           its one-based socket zone. */
        if ((sock->as_Flags & (ASF_TCP | ASF_DELETED)) == ASF_TCP &&
            sock->as_Nx.tcp.nx_tcp_socket_connect_interface != NX_NULL)
        {
            interface_index = (LONG)sock->as_Nx.tcp
                                        .nx_tcp_socket_connect_interface
                                        ->nx_interface_index;
        }
        else if (sock->as_PeerScopeId > 0UL &&
                 sock->as_PeerScopeId <=
                     (ULONG)NX_MAX_PHYSICAL_INTERFACES)
        {
            interface_index = (LONG)(sock->as_PeerScopeId - 1UL);
        }

        if ((sock->as_Flags & ASF_CONNECTED) != 0 &&
            sock->as_PeerAddr.nxd_ip_version == NX_IP_VERSION_V6 &&
            netstack_ipv6_source_for(sock->as_PeerAddr.nxd_ip_address.v6,
                                     interface_index, chosen))
        {
            addr.nxd_ip_address.v6[0] = chosen[0];
            addr.nxd_ip_address.v6[1] = chosen[1];
            addr.nxd_ip_address.v6[2] = chosen[2];
            addr.nxd_ip_address.v6[3] = chosen[3];

            /* An unbound socket had no local zone to retain.  Once the
               connected route resolves its wildcard to a non-global address,
               report the interface that address actually uses. */
            if (scope == 0UL && interface_index >= 0 &&
                anx6_scope(addr.nxd_ip_address.v6) < 0xEU)
                scope = (ULONG)interface_index + 1UL;
        }
    }
    else
#endif
    if (addr.nxd_ip_version == NX_IP_VERSION_V4 &&
        addr.nxd_ip_address.v4 == 0)
    {
        ULONG chosen = 0;

        /*
         * Bound to INADDR_ANY.  This reports the source address a packet to
         * this socket's peer would carry, see bsd_v4_source_for() above.  It
         * used to name nx_ip_interface[0] whatever the peer was, which is the
         * wrong address on a machine with a second interface and the wrong
         * address for a loopback connection on every machine.
         *
         * Nothing is written when the route cannot be resolved: 0.0.0.0 is
         * what the socket is bound to, and it is a better answer than an
         * address the packets do not use.
         */
        if ((sock->as_Flags & ASF_CONNECTED) != 0 &&
            bsd_v4_source_for(sock, &chosen))
            bsd_addr_from_v4(&addr, chosen);
    }

    if (bracketed)
        bsd_nx_leave(SocketBase);

    bsd_sockaddr_put(sock, name, namelen, &addr, sock->as_LocalPort,
                     scope);

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
                     sock->as_PeerPort, sock->as_PeerScopeId);

    return 0;
}

int bsd_getdtablesize(register struct AmiSocketBase *SocketBase __asm("a6"))
{
    return (int)bsd_table_size(SocketBase);
}

LONG bsd_Dup2Socket(register LONG old_socket __asm("d0"),
                    register LONG new_socket __asm("d1"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock;
    AmiSocket *victim;

    if (old_socket == -1)
    {
        LONG fd;

        if (new_socket < -1 || new_socket >= bsd_table_size(SocketBase))
            return bsd_fail(SocketBase, AMI_EBADF);

        if (new_socket >= 0)
        {
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
            else if (bsd_fd_reserved(SocketBase, new_socket))
            {
                bsd_fd_free(SocketBase, new_socket);
            }
        }

        fd = bsd_fd_reserve(SocketBase, new_socket);
        if (fd < 0)
            return -1;

        return fd;
    }

    sock = bsd_lookup(SocketBase, old_socket);
    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (new_socket < 0)
    {
        LONG fd = bsd_fd_alloc(SocketBase, sock);

        if (fd < 0)
            return -1;

        bsd_socket_retain(sock);

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
    else if (bsd_fd_reserved(SocketBase, new_socket))
    {
        bsd_fd_free(SocketBase, new_socket);
    }

    if (bsd_fd_reserve(SocketBase, new_socket) < 0)
        return -1;

    SocketBase->sb_Table[new_socket] = sock;
    bsd_socket_retain(sock);

    return new_socket;
}
