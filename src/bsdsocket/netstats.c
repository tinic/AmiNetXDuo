/*
 * bsdsocket.library, GetNetworkStatistics().
 *
 * The one call in the Roadshow API that hands back 4.4BSD's own kernel
 * statistics structures: struct ipstat, tcpstat, udpstat, icmpstat, and the
 * per-connection struct protocol_connection_data. netstat -s and every
 * traffic monitor written against Roadshow reads this.
 *
 * Same primary source as interfaces.c and routing.c: NDK 3.2's
 * SANA+RoadshowTCP-IP/doc/bsdsocket.doc, together with libraries/bsdsocket.h
 * and the netinet/ *_var.h headers from the same NDK, used as an ABI reference
 * only. No Roadshow, AmiTCP, AROSTCP or Miami code was consulted or is
 * present.
 *
 *   The return value is a byte count: "length, Number of bytes copied, or -1
 *   for failure". Not zero-on-success and not an entry count, the two other
 *   readings the prototype allows.
 *
 *   NULL means "how much would I need?". "Pass a NULL pointer to find out how
 *   much memory would be required for a complete copy of the data you
 *   requested." A NULL destination is a success that copies nothing, not an
 *   EFAULT.
 *
 *   `version` is mandatory and is 1. "you must specify which version of the
 *   data structures you need to see. The version described in this
 *   documentation is 1."
 *
 * Unlike interfaces.c, this call cannot leave a value unset. The caller gets a
 * whole C struct copied, every member of it. A member this stack does not
 * count is a zero with no way to mark it as unmeasured. The mapping at each
 * fill function below therefore names every member it fills. Anything unnamed
 * is zero because it is uncounted, not because it was measured as none.
 *
 * A type this stack keeps nothing for at all is refused with EOPNOTSUPP. An
 * all-zero mbstat reports a healthy mbuf allocator that does not exist.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "tcp_queue.h"
#include "udp_queue.h"

#include <netinet/ip_var.h>
#include <netinet/icmp_var.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_fsm.h>
#include <netinet/udp_var.h>

#include <proto/exec.h>

/* One buffer big enough for whichever fixed-size answer was asked for. */
typedef union BsdNetStat
{
    struct ipstat    bns_Ip;
    struct icmpstat  bns_Icmp;
    struct tcpstat   bns_Tcp;
    struct udpstat   bns_Udp;
} BsdNetStat;

/* --------------------------------------------------------------- the fills */

/*
 * IP. From nx_ip_info_get(), whose ten counters map onto six ipstat members:
 *
 *   ips_total       <- ip_total_packets_received
 *   ips_badsum      <- ip_receive_checksum_errors
 *   ips_localout    <- ip_total_packets_sent
 *   ips_odropped    <- ip_send_packets_dropped
 *   ips_fragments   <- ip_total_fragments_received
 *   ips_ofragments  <- ip_total_fragments_sent
 *
 * ip_invalid_packets and ip_receive_packets_dropped have no ipstat member that
 * means the same thing. ipstat splits input failure into seven named causes
 * (tooshort, toosmall, badhlen, badlen, badvers, badoptions, noproto), and
 * NetX Duo counts them as one number. It therefore goes into none of them.
 */
static VOID bsd_stat_ip(NX_IP *ip, struct ipstat *out)
{
    ULONG sent = 0, sent_bytes = 0, received = 0, received_bytes = 0;
    ULONG invalid = 0, dropped = 0, checksum = 0, send_dropped = 0;
    ULONG frags_sent = 0, frags_received = 0;

    if (nx_ip_info_get(ip, &sent, &sent_bytes, &received, &received_bytes,
                       &invalid, &dropped, &checksum, &send_dropped,
                       &frags_sent, &frags_received) != NX_SUCCESS)
        return;

    out->ips_total      = received;
    out->ips_badsum     = checksum;
    out->ips_localout   = sent;
    out->ips_odropped   = send_dropped;
    out->ips_fragments  = frags_received;
    out->ips_ofragments = frags_sent;
}

/*
 * ICMP. From nx_icmp_info_get():
 *
 *   icps_outhist[ICMP_ECHO]       <- pings_sent
 *   icps_inhist[ICMP_ECHOREPLY]   <- ping_responses_received
 *   icps_checksum                 <- icmp_checksum_errors
 *
 * icps_reflect is not icmp_unhandled_messages: icps_reflect is the number of
 * ICMP messages this host answered, icmp_unhandled_messages the number it did
 * not. NetX Duo counts no replies sent, so icps_reflect stays zero.
 *
 * ping_threads_suspended and ping_timeouts describe this host's own outbound
 * pings and have no icmpstat member. icmpstat is a per-message-type
 * histogram, not a client statistic.
 */
static VOID bsd_stat_icmp(NX_IP *ip, struct icmpstat *out)
{
    ULONG sent = 0, timeouts = 0, suspended = 0, responses = 0;
    ULONG checksum = 0, unhandled = 0;

    if (nx_icmp_info_get(ip, &sent, &timeouts, &suspended, &responses,
                         &checksum, &unhandled) != NX_SUCCESS)
        return;

    out->icps_checksum = checksum;

    /* The histograms are indexed by ICMP type, and both indices are inside
       ICMP_MAXTYPE by inspection, 8 and 0 against a bound of 18. */
    out->icps_outhist[ICMP_ECHO]     = sent;
    out->icps_inhist[ICMP_ECHOREPLY] = responses;
}

/*
 * TCP. From nx_tcp_info_get():
 *
 *   tcps_rcvtotal       <- tcp_packets_received
 *   tcps_rcvbyte        <- tcp_bytes_received
 *   tcps_sndtotal       <- tcp_packets_sent
 *   tcps_sndbyte        <- tcp_bytes_sent
 *   tcps_rcvbadsum      <- tcp_checksum_errors
 *   tcps_connects       <- tcp_connections
 *   tcps_closed         <- tcp_disconnections
 *   tcps_drops          <- tcp_connections_dropped
 *   tcps_sndrexmitpack  <- tcp_retransmit_packets
 *
 * tcps_connattempt and tcps_accepts are not filled from tcp_connections. That
 * counter is connections established, incoming and outgoing together, and
 * there is no basis for a split.
 */
static VOID bsd_stat_tcp(NX_IP *ip, struct tcpstat *out)
{
    ULONG sent = 0, sent_bytes = 0, received = 0, received_bytes = 0;
    ULONG invalid = 0, dropped = 0, checksum = 0;
    ULONG connections = 0, disconnections = 0, connections_dropped = 0;
    ULONG retransmits = 0;

    if (nx_tcp_info_get(ip, &sent, &sent_bytes, &received, &received_bytes,
                        &invalid, &dropped, &checksum, &connections,
                        &disconnections, &connections_dropped,
                        &retransmits) != NX_SUCCESS)
        return;

    out->tcps_rcvtotal      = received;
    out->tcps_rcvbyte       = received_bytes;
    out->tcps_sndtotal      = sent;
    out->tcps_sndbyte       = sent_bytes;
    out->tcps_rcvbadsum     = checksum;
    out->tcps_connects      = connections;
    out->tcps_closed        = disconnections;
    out->tcps_drops         = connections_dropped;
    out->tcps_sndrexmitpack = retransmits;
}

/*
 * UDP. From nx_udp_info_get(), plus one counter it does not expose:
 *
 *   udps_ipackets  <- udp_packets_received
 *   udps_opackets  <- udp_packets_sent
 *   udps_badsum    <- udp_checksum_errors
 *   udps_fullsock  <- udp_receive_packets_dropped
 *   udps_hdrops    <- udp_invalid_packets
 *   udps_noport    <- nx_ip_udp_no_port_for_delivery
 *
 * On the last three: NetX Duo increments nx_ip_udp_invalid_packets at exactly
 * two places. Both are a datagram whose length is shorter than its header
 * (nx_udp_packet_receive.c), which is udps_hdrops and not udps_badlen. Its
 * receive_packets_dropped is a socket whose queue is full, exactly
 * udps_fullsock. "No socket on port" has a counter of its own that
 * nx_udp_info_get() does not return, so it is read off the NX_IP, the same way
 * netstatus.c reads the interface table.
 */
static VOID bsd_stat_udp(NX_IP *ip, struct udpstat *out)
{
    ULONG sent = 0, sent_bytes = 0, received = 0, received_bytes = 0;
    ULONG invalid = 0, dropped = 0, checksum = 0;

    if (nx_udp_info_get(ip, &sent, &sent_bytes, &received, &received_bytes,
                        &invalid, &dropped, &checksum) != NX_SUCCESS)
        return;

    out->udps_ipackets = received;
    out->udps_opackets = sent;
    out->udps_badsum   = checksum;
    out->udps_fullsock = dropped;
    out->udps_hdrops   = invalid;

#ifndef NX_DISABLE_UDP_INFO
    out->udps_noport = ip->nx_ip_udp_no_port_for_delivery;
#endif
}

/* ------------------------------------------------------------ the sockets */

/*
 * TCP state, NetX Duo's -> 4.4BSD's. A table, not arithmetic: the two
 * enumerations agree up to CLOSE_WAIT and then diverge. NetX Duo has
 * FIN_WAIT_2 = 8, CLOSING = 9, TIMED_WAIT = 10, LAST_ACK = 11.
 * <netinet/tcp_fsm.h> has CLOSING = 7, LAST_ACK = 8, FIN_WAIT_2 = 9,
 * TIME_WAIT = 10. A subtraction of one, which the first four states invite
 * because NX_TCP_CLOSED is 1 and TCPS_CLOSED is 0, reports a connection in
 * LAST_ACK as FIN_WAIT_2.
 */
typedef struct BsdTcpStateMap
{
    UWORD   btm_Nx;
    WORD    btm_Bsd;
} BsdTcpStateMap;

static const BsdTcpStateMap bsd_tcp_states[] =
{
    { NX_TCP_CLOSED,        TCPS_CLOSED       },
    { NX_TCP_LISTEN_STATE,  TCPS_LISTEN       },
    { NX_TCP_SYN_SENT,      TCPS_SYN_SENT     },
    { NX_TCP_SYN_RECEIVED,  TCPS_SYN_RECEIVED },
    { NX_TCP_ESTABLISHED,   TCPS_ESTABLISHED  },
    { NX_TCP_CLOSE_WAIT,    TCPS_CLOSE_WAIT   },
    { NX_TCP_FIN_WAIT_1,    TCPS_FIN_WAIT_1   },
    { NX_TCP_FIN_WAIT_2,    TCPS_FIN_WAIT_2   },
    { NX_TCP_CLOSING,       TCPS_CLOSING      },
    { NX_TCP_TIMED_WAIT,    TCPS_TIME_WAIT    },
    { NX_TCP_LAST_ACK,      TCPS_LAST_ACK     }
};

/* "Note that this can be -1 if the case cannot be safely determined." */
static LONG bsd_tcp_state(ULONG nx_state)
{
    ULONG i;

    for (i = 0; i < sizeof(bsd_tcp_states) / sizeof(bsd_tcp_states[0]); i++)
    {
        if (bsd_tcp_states[i].btm_Nx == (UWORD)nx_state)
            return (LONG)bsd_tcp_states[i].btm_Bsd;
    }

    return -1;
}

/*
 * How many entries the caller's buffer holds, and where the next one goes.
 * `size` can be smaller than the whole answer. "size, Number of bytes to
 * copy" is the caller's limit, not the total, so this is bounded both ways.
 */
typedef struct BsdPcdWriter
{
    struct protocol_connection_data *bpw_Entry;
    ULONG                            bpw_Room;      /* entries that fit */
    ULONG                            bpw_Written;
    ULONG                            bpw_Available; /* counted regardless */
} BsdPcdWriter;

static struct protocol_connection_data *bsd_pcd_next(BsdPcdWriter *w)
{
    struct protocol_connection_data *slot;

    w->bpw_Available++;

    if (w->bpw_Entry == NULL || w->bpw_Written >= w->bpw_Room)
        return NULL;

    slot = &w->bpw_Entry[w->bpw_Written++];
    bsd_bzero(slot, sizeof(*slot));

    return slot;
}

/*
 * Listening sockets need a rule of their own. One BSD listen() is two
 * NX_TCP_SOCKETs here, and neither is in NetX Duo's LISTEN state. As socket.c
 * describes, the descriptor keeps its own socket, which is never used for a
 * connection and stays in NX_TCP_CLOSED. It also parks a second spare socket
 * on the port with the accept already posted, so NetX Duo can answer a SYN
 * before the application calls accept(). The spare sits in
 * NX_TCP_SYN_RECEIVED.
 *
 * Reported literally, one listen() on port 80 shows a monitor a socket in
 * CLOSED and one in SYN_RECEIVED on the same port, when no SYN arrived. So:
 *
 *   the parked spare      is skipped. The application does not own it and it
 *                         has no descriptor.
 *   the descriptor's own  is reported as TCPS_LISTEN rather than TCPS_CLOSED,
 *                         which is only where NetX Duo left it.
 *
 * Both are decided from the IP's active listen requests, the only place the
 * association between a port and a parked socket is recorded.
 */
static NX_TCP_SOCKET *bsd_listen_spare(NX_IP *ip, UINT port)
{
    NX_TCP_LISTEN *listen_ptr = ip->nx_ip_tcp_active_listen_requests;
    ULONG          n;

    /* Circular, like every other NetX Duo list here, so the walk is bounded by
       the table size rather than by a NULL that never comes. */
    for (n = 0; n < (ULONG)NX_MAX_LISTEN_REQUESTS && listen_ptr != NX_NULL; n++)
    {
        if (listen_ptr->nx_tcp_listen_port == port)
            return listen_ptr->nx_tcp_listen_socket_ptr;

        listen_ptr = listen_ptr->nx_tcp_listen_next;

        if (listen_ptr == ip->nx_ip_tcp_active_listen_requests)
            break;
    }

    return NX_NULL;
}

/*
 * The created-socket lists are singly linked and circular, so the walk is
 * bounded by the count NetX Duo keeps rather than by a NULL that never comes.
 * netstatus.c notes the same for the same two lists.
 */
static VOID bsd_pcd_tcp(NX_IP *ip, BsdPcdWriter *w)
{
    NX_TCP_SOCKET *sock = ip->nx_ip_tcp_created_sockets_ptr;
    ULONG          n;

    for (n = 0; n < ip->nx_ip_tcp_created_sockets_count && sock != NX_NULL; n++)
    {
        NX_TCP_SOCKET                   *spare;
        struct protocol_connection_data *out;
        BOOL                             listening = FALSE;

        spare = bsd_listen_spare(ip, sock->nx_tcp_socket_port);

        if (spare == sock)
        {
            /* The parked spare: skipped before bsd_pcd_next(), so it is not
               counted either. A caller that sized its buffer from the
               NULL-destination call must get the same number of entries. */
            sock = sock->nx_tcp_socket_created_next;
            continue;
        }

        if (spare != NX_NULL && sock->nx_tcp_socket_state == NX_TCP_CLOSED)
            listening = TRUE;

        out = bsd_pcd_next(w);

        if (out != NULL)
        {
            out->pcd_local_port   = (UWORD)sock->nx_tcp_socket_port;
            out->pcd_foreign_port = (UWORD)sock->nx_tcp_socket_connect_port;
            out->pcd_tcp_state    = listening
                                        ? (LONG)TCPS_LISTEN
                                        : bsd_tcp_state(sock->nx_tcp_socket_state);

            out->pcd_foreign_address.s_addr = (in_addr_t)BSD_HTONL(
                sock->nx_tcp_socket_connect_ip.nxd_ip_address.v4);

            /* The local address is the address of the interface the connection
               went out of. A listening socket has none yet, so it stays
               INADDR_ANY. */
            if (sock->nx_tcp_socket_connect_interface != NX_NULL)
                out->pcd_local_address.s_addr = (in_addr_t)BSD_HTONL(
                    sock->nx_tcp_socket_connect_interface->nx_interface_ip_address);

            out->pcd_receive_queue_size =
                bsd_tcp_receive_queue_bytes(sock);

            /* Send-Q is application data sent and not yet acknowledged.
               NetX keeps that exact byte count: its packet list uses a
               different link from a normal queue and still carries TCP
               headers, so summing it is neither safe nor the right unit. */
            out->pcd_send_queue_size = bsd_tcp_send_queue_bytes(sock);
        }

        sock = sock->nx_tcp_socket_created_next;
    }
}

static VOID bsd_pcd_udp(NX_IP *ip, BsdPcdWriter *w)
{
    NX_UDP_SOCKET *sock = ip->nx_ip_udp_created_sockets_ptr;
    ULONG          n;

    for (n = 0; n < ip->nx_ip_udp_created_sockets_count && sock != NX_NULL; n++)
    {
        struct protocol_connection_data *out = bsd_pcd_next(w);

        if (out != NULL)
        {
            out->pcd_local_port = (UWORD)sock->nx_udp_socket_port;

            /* "the 'pcd_tcp_state' member of each entry will be -1 since that
               information is valid only for TCP." */
            out->pcd_tcp_state = -1;

            /* NetX leaves an eight-byte UDP header on every packet until
               nx_udp_socket_receive() returns it.  The Roadshow field is the
               socket's receive queue, so count what recv() can deliver rather
               than those private headers.  NetX's own
               nx_udp_socket_bytes_available() applies the same subtraction. */
            out->pcd_receive_queue_size = bsd_udp_queue_payload_bytes(
                sock->nx_udp_socket_receive_head,
                sock->nx_udp_socket_receive_count);

            /* A datagram socket has no peer and nothing queued for output.
               sendto() hands the packet to the IP thread and returns. The
               foreign address, foreign port and send queue are therefore
               really zero, not uncounted. */
        }

        sock = sock->nx_udp_socket_created_next;
    }
}

/* ------------------------------------------------- GetNetworkStatistics, */

LONG bsd_GetNetworkStatistics(register LONG type __asm("d0"),
                              register LONG version __asm("d1"),
                              register APTR destination __asm("a0"),
                              register LONG size __asm("d2"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP       *ip = netstack_ip();
    BsdNetStat   fixed;
    BsdPcdWriter writer;
    ULONG        need;
    ULONG        copy;

    /* "You must fill in this member or the message will be rejected" is the
       neighbouring call's wording. This one says the version is 1. */
    if (version != NETWORKSTATUS_VERSION)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (size < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    switch (type)
    {
        case NETSTATUS_ip:   need = sizeof(struct ipstat);   break;
        case NETSTATUS_icmp: need = sizeof(struct icmpstat); break;
        case NETSTATUS_tcp:  need = sizeof(struct tcpstat);  break;
        case NETSTATUS_udp:  need = sizeof(struct udpstat);  break;

        case NETSTATUS_tcp_sockets:
        case NETSTATUS_udp_sockets:
            need = 0;                   /* counted below, under the bracket */
            break;

        /*
         * Documented types this stack keeps nothing for. EOPNOTSUPP rather
         * than a struct of zeroes, which a caller cannot tell from a real
         * measurement:
         *
         *   NETSTATUS_mb    no mbuf allocator. NetX Duo has a fixed NX_PACKET
         *                   pool with different semantics, and a mapping of
         *                   one onto the other reports free clusters that do
         *                   not exist. The mbuf_* vectors are stubs for the
         *                   same reason.
         *   NETSTATUS_igmp  IGMP runs (src/bsdsocket/mcast.c), but NetX Duo
         *                   counts five things and 4.4BSD's igmpstat has
         *                   nine. Four map, badsum, queries, snd_reports
         *                   and part of tooshort, and the other five are
         *                   unknown rather than zero.
         *   NETSTATUS_mrt   no multicast routing.
         *   NETSTATUS_rt    three of rtstat's five members are genuinely
         *                   zero, because this stack creates no dynamic
         *                   routes and processes no redirects. The other two,
         *                   rts_unreach and rts_wildcard, are lookups nothing
         *                   counts.
         */
        case NETSTATUS_mb:
        case NETSTATUS_igmp:
        case NETSTATUS_mrt:
        case NETSTATUS_rt:
            return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

        /* "This must be one of ...", anything else is a bad argument. */
        default:
            return bsd_fail(SocketBase, AMI_EINVAL);
    }

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (type == NETSTATUS_tcp_sockets || type == NETSTATUS_udp_sockets)
    {
        ULONG entry = (ULONG)sizeof(struct protocol_connection_data);

        writer.bpw_Entry     = (struct protocol_connection_data *)destination;
        writer.bpw_Room      = (destination != NULL) ? ((ULONG)size / entry) : 0;
        writer.bpw_Written   = 0;
        writer.bpw_Available = 0;

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        if (type == NETSTATUS_tcp_sockets)
            bsd_pcd_tcp(ip, &writer);
        else
            bsd_pcd_udp(ip, &writer);

        bsd_nx_leave(SocketBase);

        /*
         * With no destination the answer is the size of a complete copy.
         * It is a snapshot: a caller that allocates from it and asks again can
         * find one more socket, so the second call's return value is what was
         * actually written.
         */
        if (destination == NULL)
            return (LONG)(writer.bpw_Available * entry);

        return (LONG)(writer.bpw_Written * entry);
    }

    /* "Pass a NULL pointer to find out how much memory would be required." */
    if (destination == NULL)
        return (LONG)need;

    bsd_bzero(&fixed, sizeof(fixed));

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    switch (type)
    {
        case NETSTATUS_ip:   bsd_stat_ip(ip, &fixed.bns_Ip);     break;
        case NETSTATUS_icmp: bsd_stat_icmp(ip, &fixed.bns_Icmp); break;
        case NETSTATUS_tcp:  bsd_stat_tcp(ip, &fixed.bns_Tcp);   break;
        default:             bsd_stat_udp(ip, &fixed.bns_Udp);   break;
    }

    bsd_nx_leave(SocketBase);

    /* "size, Number of bytes to copy" is the caller's limit. A caller built
       against an older struct can ask for less than all of it, and a copy of
       `need` regardless overruns its buffer. */
    copy = ((ULONG)size < need) ? (ULONG)size : need;

    bsd_bcopy(&fixed, destination, copy);

    return (LONG)copy;
}
