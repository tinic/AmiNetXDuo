/*
 * bsdsocket.library, GetNetworkStatistics().
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "udp_queue.h"

#include "aminetxduo/nx_queue.h"

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

/*
 * TCP state, NetX Duo's -> 4.4BSD's. A table, not arithmetic: the two
 * enumerations agree up to CLOSE_WAIT and then diverge. NetX Duo has
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

            if (sock->nx_tcp_socket_connect_interface != NX_NULL)
                out->pcd_local_address.s_addr = (in_addr_t)BSD_HTONL(
                    sock->nx_tcp_socket_connect_interface->nx_interface_ip_address);

            out->pcd_receive_queue_size =
                ami_nx_tcp_receive_bytes(sock);

            out->pcd_send_queue_size = ami_nx_tcp_send_bytes(sock);
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

            out->pcd_receive_queue_size = ami_nx_udp_receive_bytes(sock);

        }

        sock = sock->nx_udp_socket_created_next;
    }
}

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

        case NETSTATUS_mb:
        case NETSTATUS_igmp:
        case NETSTATUS_mrt:
        case NETSTATUS_rt:
            return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

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
