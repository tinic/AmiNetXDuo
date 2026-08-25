/*
 * bsdsocket.library, TCP urgent data: MSG_OOB, SIOCATMARK and SIGURG.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "nx_tcp.h"
#include "nx_packet.h"

#include <netinet/in.h>
#include <proto/exec.h>

/* Byte offsets inside a TCP header, from RFC 793 figure 3. */
#define BSD_TCP_OFF_SPORT        0
#define BSD_TCP_OFF_DPORT        2
#define BSD_TCP_OFF_SEQ          4
#define BSD_TCP_OFF_FLAGS       12      /* data offset + reserved + flags   */
#define BSD_TCP_OFF_CHECKSUM    16
#define BSD_TCP_OFF_URGENT      18

/* The URG flag, seen as part of the 16-bit word at BSD_TCP_OFF_FLAGS. */
#define BSD_TCP_FLAGS_URG   0x0020

/*
 * The urgent pointer we send: one past the urgent byte.  RFC 793 says "the
 * sequence number of the octet following the urgent data", RFC 1122 4.2.2.4
 * requires that reading, and every BSD from 4.3-Reno onward uses it.  A single
 * urgent byte at the start of the segment is pointer 1, not 0.
 */
#define BSD_TCP_URGENT_PTR       1

/*
 * RFC 1624 equation 3: HC' = ~(~HC + ~m + m').  Equation 3 rather than 1 or 2
 * because those two fail when a partial sum reaches negative zero, the case
 * the RFC exists to correct.
 */
static UWORD bsd_oob_csum_update(UWORD hc, UWORD old_word, UWORD new_word)
{
    ULONG sum = (ULONG)(UWORD)~hc + (ULONG)(UWORD)~old_word + (ULONG)new_word;

    while ((sum >> 16) != 0)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    return (UWORD)~(UWORD)sum;
}

/*
 * Which segment the filter looks for.  One record rather than a list: it is
 * armed and disarmed inside a single bsd_nx_enter() bracket around one
 */
static struct
{
    BOOL  om_Active;
    UINT  om_LocalPort;
    UINT  om_PeerPort;
    ULONG om_Sequence;
    /*
     * Whether the checksum field is the driver's to fill.
     */
    BOOL  om_Offload;
} bsd_oob_mark;

static UINT bsd_oob_ip_filter(VOID *ip_header_ptr, UINT direction)
{
    UBYTE *ip = (UBYTE *)ip_header_ptr;
    UBYTE *tcp;
    ULONG  ihl, seq;
    UWORD  sport, dport, flags, checksum;

    /* Always return NX_SUCCESS: any other value makes _nx_ip_packet_send()
       drop the packet. */
    if (direction != NX_IP_PACKET_OUT || !bsd_oob_mark.om_Active || ip == NULL)
        return NX_SUCCESS;

    if ((ip[0] >> 4) != 4)
        return NX_SUCCESS;

    ihl = (ULONG)(ip[0] & 0x0F) * 4;
    if (ihl < 20)
        return NX_SUCCESS;

    if (ip[9] != IPPROTO_TCP)
        return NX_SUCCESS;

    tcp = ip + ihl;

    sport = (UWORD)(((UWORD)tcp[BSD_TCP_OFF_SPORT] << 8) |
                    tcp[BSD_TCP_OFF_SPORT + 1]);
    dport = (UWORD)(((UWORD)tcp[BSD_TCP_OFF_DPORT] << 8) |
                    tcp[BSD_TCP_OFF_DPORT + 1]);
    seq   = ((ULONG)tcp[BSD_TCP_OFF_SEQ]     << 24) |
            ((ULONG)tcp[BSD_TCP_OFF_SEQ + 1] << 16) |
            ((ULONG)tcp[BSD_TCP_OFF_SEQ + 2] <<  8) |
             (ULONG)tcp[BSD_TCP_OFF_SEQ + 3];

    if ((UINT)sport != bsd_oob_mark.om_LocalPort ||
        (UINT)dport != bsd_oob_mark.om_PeerPort  ||
        seq         != bsd_oob_mark.om_Sequence)
        return NX_SUCCESS;

    flags = (UWORD)(((UWORD)tcp[BSD_TCP_OFF_FLAGS] << 8) |
                    tcp[BSD_TCP_OFF_FLAGS + 1]);

    if ((flags & BSD_TCP_FLAGS_URG) != 0)
        return NX_SUCCESS;

    flags = (UWORD)(flags | BSD_TCP_FLAGS_URG);

    tcp[BSD_TCP_OFF_FLAGS]         = (UBYTE)(flags >> 8);
    tcp[BSD_TCP_OFF_FLAGS + 1]     = (UBYTE)flags;
    tcp[BSD_TCP_OFF_URGENT]        = (UBYTE)(BSD_TCP_URGENT_PTR >> 8);
    tcp[BSD_TCP_OFF_URGENT + 1]    = (UBYTE)BSD_TCP_URGENT_PTR;

    if (bsd_oob_mark.om_Offload)
        return NX_SUCCESS;

    checksum = (UWORD)(((UWORD)tcp[BSD_TCP_OFF_CHECKSUM] << 8) |
                       tcp[BSD_TCP_OFF_CHECKSUM + 1]);

    checksum = bsd_oob_csum_update(checksum,
                                   (UWORD)(flags & ~BSD_TCP_FLAGS_URG), flags);
    checksum = bsd_oob_csum_update(checksum, 0, BSD_TCP_URGENT_PTR);

    tcp[BSD_TCP_OFF_CHECKSUM]      = (UBYTE)(checksum >> 8);
    tcp[BSD_TCP_OFF_CHECKSUM + 1]  = (UBYTE)checksum;

    return NX_SUCCESS;
}

LONG bsd_oob_send(struct AmiSocketBase *base, AmiSocket *sock, UBYTE byte,
                  LONG flags)
{
    NX_TCP_SOCKET  *tcp    = &sock->as_Nx.tcp;
    NX_IP          *ip     = tcp->nx_tcp_socket_ip_ptr;
    NX_PACKET_POOL *pool   = netstack_pool();
    NX_PACKET      *packet = NX_NULL;
    UINT          (*saved_filter)(VOID *, UINT) = NX_NULL;
    BOOL            armed  = FALSE;
    ULONG           wait;
    UINT            status;

    if (ip == NX_NULL || pool == NULL)
        return bsd_fail(base, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(base, AMI_ENOTCONN);

    if ((sock->as_Flags & ASF_WRSHUT) != 0)
        return bsd_fail(base, AMI_EPIPE);

    wait = bsd_wait_option(sock, sock->as_SndTimeout, flags);

    status = nx_packet_allocate(pool, &packet, NX_TCP_PACKET, wait);
    if (status != NX_SUCCESS)
        return bsd_fail(base, (status == NX_NO_PACKET)
                                  ? AMI_EWOULDBLOCK
                                  : bsd_errno_from_nx(status));

    status = nx_packet_data_append(packet, &byte, 1UL, pool, wait);
    if (status != NX_SUCCESS)
    {
        nx_packet_release(packet);
        return bsd_fail(base, bsd_errno_from_nx(status));
    }

    if (!bsd_oob_mark.om_Active)
    {
        bsd_oob_mark.om_LocalPort = tcp->nx_tcp_socket_port;
        bsd_oob_mark.om_PeerPort  = tcp->nx_tcp_socket_connect_port;
        bsd_oob_mark.om_Sequence  = tcp->nx_tcp_socket_tx_sequence;
#ifdef NX_ENABLE_INTERFACE_CAPABILITY
        bsd_oob_mark.om_Offload   =
            (tcp->nx_tcp_socket_connect_interface != NX_NULL &&
             (tcp->nx_tcp_socket_connect_interface->nx_interface_capability_flag
              & NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) != 0) ? TRUE : FALSE;
#else
        bsd_oob_mark.om_Offload   = FALSE;
#endif
        bsd_oob_mark.om_Active    = TRUE;

        saved_filter            = ip->nx_ip_packet_filter;
        ip->nx_ip_packet_filter = bsd_oob_ip_filter;
        armed                   = TRUE;
    }

    status = nx_tcp_socket_send(tcp, packet, wait);

    if (armed)
    {
        ip->nx_ip_packet_filter = saved_filter;
        bsd_oob_mark.om_Active  = FALSE;
    }

    if (status != NX_SUCCESS)
    {
        nx_packet_release(packet);

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

    return 1;
}

/*
 * A segment with URG set arrived.  Runs on the IP thread, from
 * _nx_tcp_socket_packet_process(), with nx_ip_protection held.
 */
VOID bsd_tcp_urgent_notify(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;
    NX_PACKET *packet;
    ULONG      remaining;

    if (sock == NULL)
        return;

    remaining = socket_ptr->nx_tcp_socket_receive_queue_count;
    packet    = socket_ptr->nx_tcp_socket_receive_queue_head;

    while (packet != NX_NULL &&
           packet != (NX_PACKET *)NX_PACKET_ENQUEUED &&
           remaining > 0)
    {
        NX_TCP_HEADER *header = (NX_TCP_HEADER *)packet->nx_packet_prepend_ptr;
        ULONG          header_length;
        ULONG          urgent;
        ULONG          offset;
        ULONG          moved = 0;
        UBYTE          value = 0;

        if ((header->nx_tcp_header_word_3 & NX_TCP_URG_BIT) == 0)
            goto next;

        header_length = (header->nx_tcp_header_word_3 >> NX_TCP_HEADER_SHIFT) *
                        (ULONG)sizeof(ULONG);
        urgent        = header->nx_tcp_header_word_4 & 0xFFFFUL;

        /* RFC 1122: the pointer is one past the last urgent octet, so the
           octet itself is at urgent - 1 from the start of the payload. A
           pointer of zero names no octet, so there is nothing to take. */
        if (urgent == 0 || header_length < sizeof(NX_TCP_HEADER) ||
            packet->nx_packet_length < header_length)
            goto next;

        offset = header_length + (urgent - 1);

        if (offset >= packet->nx_packet_length)
            goto next;

        if (nx_packet_data_extract_offset(packet, offset, &value, 1UL, &moved)
                == NX_SUCCESS && moved == 1)
        {
            sock->as_OobData = value;
            sock->as_Flags  |= ASF_OOBHAVE;
        }

        break;

next:
        packet = packet->nx_packet_union_next.nx_packet_tcp_queue_next;
        remaining--;
    }

    bsd_event_post(sock, FD_OOB);
}

BOOL bsd_oob_take(AmiSocket *sock, UBYTE *out)
{
    /* The urgent-data callback posts from the IP task. Keep a new mark from
       arriving between the test/copy and the flag clear. */
    Forbid();

    if ((sock->as_Flags & ASF_OOBHAVE) == 0)
    {
        Permit();
        return FALSE;
    }

    *out = sock->as_OobData;

    sock->as_Flags  &= ~ASF_OOBHAVE;
    sock->as_Events &= ~FD_OOB;

    Permit();

    return TRUE;
}
