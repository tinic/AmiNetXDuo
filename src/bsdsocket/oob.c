/*
 * bsdsocket.library, TCP urgent data: MSG_OOB, SIOCATMARK and SIGURG.
 *
 * Receive needed only wiring up: _nx_tcp_socket_packet_process() tests
 * NX_TCP_URG_BIT and calls the socket's nx_tcp_urgent_data_callback
 * (nx_tcp_socket_packet_process.c:465-483), the segment is still on
 * nx_tcp_socket_receive_queue_head with its TCP header, the header is only
 * stripped at delivery, in nx_tcp_socket_receive.c:176, and the urgent
 * pointer is in the low half of nx_tcp_header_word_4 in host order.
 * bsdsocket.library was passing NX_NULL for that callback.
 *
 * Transmit has no support at all.  NX_TCP_URG_BIT is never set anywhere in the
 * vendored tree, and it cannot be added from outside by preparing a header,
 * because both senders finish with
 *
 *     header_ptr -> nx_tcp_header_word_4 = (checksum << NX_SHIFT_BY_16);
 *
 * a plain assignment, after the checksum has been computed
 * (nx_tcp_socket_send_internal.c:856, nx_tcp_packet_send_control.c:312).  Any
 * urgent pointer planted beforehand is destroyed, and the checksum was
 * computed over it, so you get both a zero pointer and a wrong sum.
 *
 * Open-coding an urgent segment the way bsd_tcp_send_fin() open-codes the
 * graceful FIN does not work either.  A FIN is a control packet:
 * fire-and-forget, never queued for retransmission.  An urgent byte is data
 * and consumes a sequence number, so a copy of nx_tcp_socket_send_internal()
 * would have to reproduce the window arithmetic, the transmit-queue linking,
 * the outstanding-bytes accounting and the mutex-drop race check around the
 * checksum, some eight hundred lines, or skip the retransmit queue and
 * leave a hole in the sequence space that stalls the connection permanently
 * the first time the segment is lost.
 *
 * So the byte goes out through nx_tcp_socket_send() like any other: queued,
 * retransmitted, accounted by NetX Duo.  The URG bit and the urgent pointer
 * are written into that one segment on its way past nx_ip_packet_filter, which
 * _nx_ip_packet_send() consults after _nx_ip_header_add() and before the
 * driver (nx_ip_packet_send.c:209-222), the last point at which the bytes are
 * still ours.  Two 16-bit words change, so the TCP checksum is repaired
 * incrementally by RFC 1624 equation 3 rather than recomputed.
 *
 * The filter is installed for the duration of that one send and removed
 * immediately, so the steady-state cost on the packet path is zero.  A
 * retransmission of the urgent segment therefore carries the byte but not the
 * URG bit (_nx_tcp_socket_retransmit rebuilds word_3 without it anyway,
 * nx_tcp_socket_retransmit.c:283).  The data is always reliable; only the
 * urgency marking is best effort, which both real callers, ftp's ABOR and
 * telnet's interrupt, already assume, since both also send the command
 * inline.
 *
 * nx_ip_packet_filter is the plain hook and is free; src/netstack/ uses
 * nx_ip_packet_filter_extended for packet capture, a different slot and a
 * different call.  The previous value is saved and restored regardless.
 *
 * Two divergences from BSD, both recorded in docs/RESEARCH.md 17
 *
 *   1. The urgent byte is also delivered in the normal stream, as though
 *      SO_OOBINLINE were set, recv(MSG_OOB) returns a copy.  Taking a byte
 *      back out of the middle of a queued NX_PACKET would mean rewriting a
 *      segment the TCP state machine still owns and still counts in its
 *      sequence space, to hide one byte the caller is about to see again.
 *
 *   2. recv(MSG_OOB) with nothing marked returns EINVAL, which is what 4.4BSD
 *      does and what Roadshow returns unconditionally.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

/* For NX_TCP_HEADER and NX_TCP_HEADER_SHIFT; nx_packet.h for the queue
   sentinel that terminates nx_tcp_socket_receive_queue_head. */
#include "nx_tcp.h"
#include "nx_packet.h"

#include <netinet/in.h>

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
 * requires that reading, and every BSD has used it since 4.3-Reno.  A single
 * urgent byte at the start of the segment is pointer 1, not 0.
 */
#define BSD_TCP_URGENT_PTR       1

/* -------------------------------------------------------------- checksum, */

/*
 * RFC 1624 equation 3: HC' = ~(~HC + ~m + m').  Equation 3 rather than 1 or 2
 * because those two misbehave when a partial sum reaches negative zero, which
 * is what the RFC exists to correct.
 */
static UWORD bsd_oob_csum_update(UWORD hc, UWORD old_word, UWORD new_word)
{
    ULONG sum = (ULONG)(UWORD)~hc + (ULONG)(UWORD)~old_word + (ULONG)new_word;

    while ((sum >> 16) != 0)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    return (UWORD)~(UWORD)sum;
}

/* ---------------------------------------------------------- the send mark, */

/*
 * Which segment the filter is looking for.  One record rather than a list: it
 * is armed and disarmed inside a single bsd_nx_enter() bracket around one
 * nx_tcp_socket_send(), and a second task that finds it already armed sends
 * its byte unmarked rather than waiting or racing.  Losing the URG bit on a
 * simultaneous second OOB send costs the urgency, never the data.
 */
static struct
{
    BOOL  om_Active;
    UINT  om_LocalPort;
    UINT  om_PeerPort;
    ULONG om_Sequence;
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

    /* Already marked: a retransmission of the segment patched on its first
       pass. Patching it twice would corrupt the checksum. */
    if ((flags & BSD_TCP_FLAGS_URG) != 0)
        return NX_SUCCESS;

    checksum = (UWORD)(((UWORD)tcp[BSD_TCP_OFF_CHECKSUM] << 8) |
                       tcp[BSD_TCP_OFF_CHECKSUM + 1]);

    checksum = bsd_oob_csum_update(checksum, flags,
                                   (UWORD)(flags | BSD_TCP_FLAGS_URG));
    /* The urgent pointer was zero: NetX Duo writes word_4 as the checksum
       shifted up, so its low half is always clear before we get here. */
    checksum = bsd_oob_csum_update(checksum, 0, BSD_TCP_URGENT_PTR);

    flags = (UWORD)(flags | BSD_TCP_FLAGS_URG);

    tcp[BSD_TCP_OFF_FLAGS]         = (UBYTE)(flags >> 8);
    tcp[BSD_TCP_OFF_FLAGS + 1]     = (UBYTE)flags;
    tcp[BSD_TCP_OFF_URGENT]        = (UBYTE)(BSD_TCP_URGENT_PTR >> 8);
    tcp[BSD_TCP_OFF_URGENT + 1]    = (UBYTE)BSD_TCP_URGENT_PTR;
    tcp[BSD_TCP_OFF_CHECKSUM]      = (UBYTE)(checksum >> 8);
    tcp[BSD_TCP_OFF_CHECKSUM + 1]  = (UBYTE)checksum;

    return NX_SUCCESS;
}

/* ------------------------------------------------------------------- send, */

LONG bsd_oob_send(struct AmiSocketBase *base, AmiSocket *sock, UBYTE byte)
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

    wait = bsd_wait_option(sock, sock->as_SndTimeout);

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

/* ---------------------------------------------------------------- receive, */

/*
 * A segment with URG set has arrived.  Runs on the IP thread, from
 * _nx_tcp_socket_packet_process(), with nx_ip_protection held.
 *
 * The segment is normally still on the receive queue with its header intact,
 * so the urgent pointer can be read rather than assumed.  Two cases where it
 * is not: a zero-payload URG segment is never queued, and a segment that
 * satisfied a thread already suspended inside nx_tcp_socket_receive() has been
 * dequeued and header-stripped before this runs
 * (nx_tcp_socket_state_data_check.c:1066-1122).  In both the exception is
 * still reported, WaitSelect() wakes, SIGURG fires, and the byte is in the
 * stream either way, since this implementation is always OOBINLINE.  Only
 * recv(MSG_OOB) misses it, and it answers EINVAL.
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

    /* Report the exception whether or not the byte was recoverable; that is
       what WaitSelect()'s exceptfds and SIGURG signal. */
    bsd_event_post(sock, FD_OOB);
}

BOOL bsd_oob_take(AmiSocket *sock, UBYTE *out)
{
    if ((sock->as_Flags & ASF_OOBHAVE) == 0)
        return FALSE;

    *out = sock->as_OobData;

    sock->as_Flags  &= ~ASF_OOBHAVE;
    sock->as_Events &= ~FD_OOB;

    return TRUE;
}
