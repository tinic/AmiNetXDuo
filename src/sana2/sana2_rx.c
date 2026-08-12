/*
 * AmiNetXDuo, SANA-II receive pipeline.
 *
 * SANA-II devices have no buffers of their own: a frame that arrives with no
 * matching CMD_READ outstanding is dropped, and CMD_READ is per packet type.
 * So the shim runs one reader thread per type, 0x0800, 0x0806 and, when
 * AMINETXDUO_IPV6 is defined, 0x86DD, each keeping several reads in flight
 * so the pipeline never empties.
 *
 * Because S2_CopyToBuff is called at interrupt level, the NX_PACKET a read
 * will land in must already exist when the read is posted. Each outstanding
 * read therefore pins one packet, and the copy hook writes straight into it at
 * the right offset. There is no bounce buffer anywhere in this path.
 *
 * On completion the reader synthesises the 14-byte Ethernet header from
 * ios2_DstAddr / ios2_SrcAddr / ios2_PacketType, presents the frame to NetX
 * Duo in exactly the shape an Ethernet driver would, and immediately reposts
 * the read.
 *
 * One of these threads also notices transmit completions. They are the only
 * threads in the shim that block in exec Wait() rather than on a ThreadX
 * object, so they are the only ones a device's ReplyMsg can wake, and without
 * a thread that wakes on a completion TCP never learns that the driver has
 * finished with a segment and never retransmits it. The reader does not touch
 * the packet: it asks NetX Duo for deferred processing and the IP thread does
 * the work. See sana2_tx.c; the mechanism here is one extra signal bit in a
 * Wait() the reader was making anyway.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

/* tx_amiga_stack_in_use(), for the reader stacks. */
#include "tx_amiga.h"

#ifdef AMINETXDUO_RX_VERIFY
#include "net68k.h"
#endif

#include "nx_ip.h"
#ifndef NX_DISABLE_IPV4
#include "nx_arp.h"
#include "nx_rarp.h"
#endif

#ifdef AMINETXDUO_BPF
#include "aminetxduo/bpf.h"
#endif

#include <proto/exec.h>

VOID ami_sana2_block_enter(VOID);
VOID ami_sana2_block_leave(VOID);

/* --------------------------------------------------------- receive probe */

#ifdef AMINETXDUO_RXPROBE

#include <proto/timer.h>
#include <devices/timer.h>

extern struct Device *TimerBase;

static ULONG ami_rxprobe_clock(VOID)
{
    struct EClockVal ev;

    if (TimerBase == NULL)
        return 0;

    (VOID)ReadEClock(&ev);

    return ev.ev_lo;
}

static UWORD ami_rxprobe_bucket(ULONG ticks)
{
    UWORD b = 0;

    while (ticks != 0 && b < (AMI_RXPROBE_BUCKETS - 1))
    {
        ticks >>= 1;
        b++;
    }

    return b;
}

/* Replies already sitting on the port. depth - this is what the device holds. */
static UWORD ami_rxprobe_backlog(struct MsgPort *port)
{
    struct Node *node;
    UWORD        n = 0;

    Disable();
    for (node = port->mp_MsgList.lh_Head;
         node->ln_Succ != NULL && n <= AMI_SANA2_RX_MAX_DEPTH;
         node = node->ln_Succ)
        n++;
    Enable();

    return n;
}

static ULONG ami_rxprobe_be32(const UCHAR *p)
{
    return (((ULONG)p[0]) << 24) | (((ULONG)p[1]) << 16) |
           (((ULONG)p[2]) <<  8) |  ((ULONG)p[3]);
}

static UWORD ami_rxprobe_be16(const UCHAR *p)
{
    return (UWORD)((((UWORD)p[0]) << 8) | (UWORD)p[1]);
}

/*
 * The sequence continuity of the bulk flow, read where the frame is handed
 * over. A segment starting past `next` means the frame before it is not here
 * and never was: the loss is below this line, not in NetX Duo.
 */
VOID ami_sana2_rxprobe_deliver(AmiSana2If *iface, const UCHAR *frame,
                               ULONG length)
{
    AmiRxSeqProbe *sp = &iface->seq;
    const UCHAR   *ip;
    const UCHAR   *tcp;
    ULONG          seq;
    ULONG          total;
    UWORD          ihl;
    UWORD          doff;
    UWORD          payload;

    if (length < AMI_ETH_HEADER_SIZE + 40)
        return;
    if (ami_rxprobe_be16(&frame[12]) != AMI_ETHERTYPE_IPV4)
        return;

    ip = frame + AMI_ETH_HEADER_SIZE;

    if ((ip[0] >> 4) != 4 || ip[9] != 6)
        return;

    ihl   = (UWORD)((ip[0] & 0x0F) * 4);
    total = (ULONG)ami_rxprobe_be16(&ip[2]);

    if (ihl < 20 || total < ihl + 20 ||
        total > length - AMI_ETH_HEADER_SIZE)
        return;

    tcp     = ip + ihl;
    doff    = (UWORD)((tcp[12] >> 4) * 4);

    if (doff < 20 || (ULONG)ihl + doff > total)
        return;

    payload = (UWORD)(total - ihl - doff);
    seq     = ami_rxprobe_be32(&tcp[4]);

    if (payload == 0)
    {
        sp->pure_ack++;
        return;
    }

    sp->data_frames++;

    if (!sp->armed)
    {
        if (payload < 512)
        {
            sp->other++;
            return;
        }

        sp->peer  = ami_rxprobe_be32(&ip[12]);
        sp->sport = ami_rxprobe_be16(&tcp[0]);
        sp->dport = ami_rxprobe_be16(&tcp[2]);
        sp->next  = seq;
        sp->armed = TRUE;
    }
    else if (sp->peer  != ami_rxprobe_be32(&ip[12]) ||
             sp->sport != ami_rxprobe_be16(&tcp[0]) ||
             sp->dport != ami_rxprobe_be16(&tcp[2]))
    {
        sp->other++;
        return;
    }

    if (seq == sp->next)
    {
        sp->inorder++;
        sp->next = seq + payload;
    }
    else if ((LONG)(seq - sp->next) > 0)
    {
        if (sp->gaps < AMI_RXPROBE_GAPS)
        {
            sp->gap_want[sp->gaps]  = sp->next;
            sp->gap_got[sp->gaps]   = seq;
            sp->gap_avail[sp->gaps] = sp->avail;
            sp->gap_open[sp->gaps]  = ami_rxprobe_clock();
        }
        sp->gaps++;
        sp->ahead++;
        sp->ahead_bytes += seq - sp->next;
        sp->next = seq + payload;
    }
    else
    {
        UWORD g;

        sp->behind++;

        /* A hole closing sub-millisecond is a frame that came late; one
           closing an RTT later is one the peer sent again. */
        for (g = 0; g < sp->gaps && g < AMI_RXPROBE_GAPS; g++)
        {
            if (sp->gap_want[g] == seq && sp->gap_fill[g] == 0)
            {
                ULONG dt = ami_rxprobe_clock() - sp->gap_open[g];

                sp->gap_fill[g] = (dt != 0) ? dt : 1;
                break;
            }
        }

        if ((LONG)(seq + payload - sp->next) > 0)
            sp->next = seq + payload;
    }
}

#ifdef AMINETXDUO_RX_VERIFY

/*
 * One line per frame the receive verifier rejected, with the three facts that
 * attribute the rejection.  `rewalk` is a fresh walk of the same bytes: 0 with
 * a summed lane is a carried-sum fault in the copy hook, 1 means the bytes in
 * the packet fail on their own.  `partial` flags a checksum field holding
 * exactly the transport pseudo-header sum (or its complement): the frame left
 * a CHECKSUM_PARTIAL offload path with the hardware step never applied, so the
 * fault is the sender's or a bridge's, not this stack's or the driver's.  The
 * 2026-08 read-collapse rig was attributed with this line: every rejected
 * segment was a burst first-transmission carrying the pseudo-header sum.
 */
static VOID ami_sana2_rxprobe_drop(NX_PACKET *packet, const AmiRxSlot *slot)
{
    const UCHAR *ip     = packet->nx_packet_prepend_ptr;
    UINT         rewalk = NX_FALSE;
    ULONG        seq    = 0;
    ULONG        stored = 0;
    ULONG        pseudo = 0;
    UWORD        part   = 0;

    (VOID)n68k_rx_verify(packet, &rewalk);

    if (packet->nx_packet_length >= 40 && (ip[0] >> 4) == 4)
    {
        UWORD ihl   = (UWORD)((ip[0] & 0x0F) * 4);
        ULONG total = (ULONG)ami_rxprobe_be16(&ip[2]);

        if (ihl >= 20 && total >= (ULONG)ihl + 20 &&
            total <= packet->nx_packet_length &&
            (ip[9] == 6 || ip[9] == 17))
        {
            const UCHAR *tr    = ip + ihl;
            ULONG        trlen = total - ihl;
            UWORD        coff  = (ip[9] == 6) ? 16 : 6;

            if (ip[9] == 6)
                seq = ami_rxprobe_be32(&tr[4]);
            stored = (ULONG)ami_rxprobe_be16(&tr[coff]);

            pseudo  = (ULONG)ami_rxprobe_be16(&ip[12]);
            pseudo += (ULONG)ami_rxprobe_be16(&ip[14]);
            pseudo += (ULONG)ami_rxprobe_be16(&ip[16]);
            pseudo += (ULONG)ami_rxprobe_be16(&ip[18]);
            pseudo += (ULONG)ip[9] + trlen;
            while ((pseudo >> 16) != 0UL)
                pseudo = (pseudo & 0xFFFFUL) + (pseudo >> 16);

            part = (stored == pseudo ||
                    stored == ((~pseudo) & 0xFFFFUL)) ? 1 : 0;
        }
    }

    AMI_ERROR("rxprobe drop: seq %08lx lane %s rewalk %ld partial %ld "
              "stored %04lx pseudo %04lx len %ld",
              (unsigned long)seq,
              (slot != NULL && slot->summed != FALSE) ? "sum" : "walk",
              (long)rewalk, (long)part,
              (unsigned long)stored, (unsigned long)pseudo,
              (long)packet->nx_packet_length);
}

#endif /* AMINETXDUO_RX_VERIFY */

VOID ami_sana2_rxprobe_report(const AmiSana2If *iface)
{
    const AmiRxSeqProbe *sp = &iface->seq;
    UWORD          i;
    UWORD          j;

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        const AmiSana2Rx *rx = &iface->rx[i];
        const AmiRxProbe *pr = &rx->probe;

        if (pr->drains == 0 && pr->posts == 0)
            continue;

        AMI_ERROR("rxprobe %ld: type %04lx depth %ld posts %ld drains %ld "
                  "dry %ld postzero %ld postpartial %ld",
                  (long)i, (long)rx->packet_type, (long)rx->depth,
                  (long)pr->posts, (long)pr->drains, (long)pr->dry,
                  (long)pr->post_zero, (long)pr->post_partial);

        AMI_ERROR("rxprobe %ld: baton max %ld sum %ld ticks",
                  (long)i, (long)pr->baton_max, (long)pr->baton_sum);

        for (j = 0; j < AMI_RXPROBE_BUCKETS; j++)
        {
            if (pr->baton_hist[j] != 0)
                AMI_ERROR("rxprobe %ld: baton < 2^%ld ticks %ld",
                          (long)i, (long)j, (long)pr->baton_hist[j]);
        }

        for (j = 0; j <= AMI_SANA2_RX_MAX_DEPTH; j++)
        {
            if (pr->avail_hist[j] != 0)
                AMI_ERROR("rxprobe %ld: avail %ld -> %ld drains",
                          (long)i, (long)j, (long)pr->avail_hist[j]);
        }

        for (j = 0; j <= AMI_SANA2_RX_MAX_DEPTH; j++)
        {
            if (pr->backlog_hist[j] != 0)
                AMI_ERROR("rxprobe %ld: backlog %ld -> %ld drains",
                          (long)i, (long)j, (long)pr->backlog_hist[j]);
        }

        for (j = 0; j < AMI_RXPROBE_WORST; j++)
        {
            if (pr->worst_backlog[j] != 0)
                AMI_ERROR("rxprobe %ld: worst backlog %ld avail %ld "
                          "since last wake %ld baton %ld ticks",
                          (long)i, (long)pr->worst_backlog[j],
                          (long)pr->worst_avail[j], (long)pr->worst_when[j],
                          (long)pr->worst_baton[j]);
        }
    }

    /*
     * The device's own receive count against ours. iface->stats.packets_received
     * is incremented in ami_sana2_rx_deliver(); the device's is what a2065.device
     * saw on the wire, so the difference is what never reached a CMD_READ.
     */
    AMI_ERROR("rxprobe dev: rx %ld tx %ld, shim rx %ld tx %ld, "
              "bad %ld ovr %ld unk %ld alloc %ld err %ld",
              (long)iface->probe_dev_rx, (long)iface->probe_dev_tx,
              (long)iface->stats.packets_received,
              (long)iface->stats.packets_sent,
              (long)iface->stats.bad_data, (long)iface->stats.overruns,
              (long)iface->stats.unknown_types,
              (long)iface->stats.alloc_failures,
              (long)iface->stats.rx_errors);

    if (!sp->armed)
    {
        AMI_ERROR("rxprobe seq: no bulk flow seen");
        return;
    }

    AMI_ERROR("rxprobe seq: peer %08lx %ld->%ld inorder %ld ahead %ld "
              "(%ld bytes) behind %ld ack %ld other %ld data %ld",
              (long)sp->peer, (long)sp->sport, (long)sp->dport,
              (long)sp->inorder, (long)sp->ahead, (long)sp->ahead_bytes,
              (long)sp->behind, (long)sp->pure_ack, (long)sp->other,
              (long)sp->data_frames);

    for (i = 0; i < sp->gaps && i < AMI_RXPROBE_GAPS; i++)
    {
        AMI_ERROR("rxprobe gap %ld: want %08lx got %08lx (%ld bytes) avail %ld "
                  "filled after %ld ticks",
                  (long)i, (long)sp->gap_want[i], (long)sp->gap_got[i],
                  (long)(sp->gap_got[i] - sp->gap_want[i]),
                  (long)sp->gap_avail[i], (long)sp->gap_fill[i]);
    }
}

#endif /* AMINETXDUO_RXPROBE */

/* ------------------------------------------------------------- delivery */

/*
 * Hand one Ethernet-shaped packet to NetX Duo. Both the cooked path (header
 * synthesised above) and the raw path (header came off the wire) end here, so
 * the two modes are required to produce the same thing.
 *
 * The deferred entry points are used rather than _nx_ip_packet_receive: the
 * reader is not the IP thread, and the deferred variants are the supported way
 * to queue work onto it (see nx_api.h and nx_ram_network_driver.c, which
 * dispatches by EtherType the same way).
 */
VOID ami_sana2_rx_deliver(AmiSana2If *iface, NX_PACKET *packet,
                          const AmiRxSlot *slot)
{
    UINT type;

#ifndef AMINETXDUO_RX_VERIFY
    /* The sum the copy carried is only consulted by the verify path, so
       without it this is the one caller that takes a slot and reads nothing
       from it. */
    (VOID)slot;
#endif

#ifdef AMINETXDUO_BPF
    /*
     * The receive tap, where the cooked and raw paths converge and before the
     * link header is stripped below, so one call covers both modes and the
     * frame is a complete link-layer frame in one contiguous run.  One load
     * and one compare when nothing is capturing on this interface.
     */
    ami_bpf_tap_rx(iface, packet->nx_packet_prepend_ptr,
                   packet->nx_packet_length);
#endif

#ifdef AMINETXDUO_RXPROBE
    ami_sana2_rxprobe_deliver(iface, packet->nx_packet_prepend_ptr,
                              packet->nx_packet_length);
#endif

    if (packet->nx_packet_length < AMI_ETH_HEADER_SIZE)
    {
        nx_packet_release(packet);
        iface->stats.rx_errors++;
        iface->stats.rx_err_runt++;
        return;
    }

    type = (((UINT)packet->nx_packet_prepend_ptr[12]) << 8) |
           ((UINT)packet->nx_packet_prepend_ptr[13]);

    packet->nx_packet_address.nx_packet_interface_ptr = iface->interface_ptr;

    /* Clean off the link header before handing the frame upwards. */
    packet->nx_packet_prepend_ptr += AMI_ETH_HEADER_SIZE;
    packet->nx_packet_length      -= AMI_ETH_HEADER_SIZE;

    switch (type)
    {
    case AMI_ETHERTYPE_IPV4:
#ifdef AMINETXDUO_RX_VERIFY
        /*
         * Verify here and tell the stack what was verified, so it does not
         * walk the payload a second time.  A frame this declines carries no
         * bits and the stack checks it exactly as it always has; a frame it
         * rejects never reaches the stack at all.  See
         * src/net68k/n68k_rx_verify.c.
         */
        {
            UINT    drop =  NX_FALSE;
            ULONG   caps;

            /*
             * The copy hook already summed this frame out of the loads the
             * copy was doing, so hand that over rather than walking it again.
             * A slot that did not sum (misaligned, or no slot at all) passes
             * zero and the verifier walks, exactly as before.
             */
            if ((slot != NULL) && (slot->summed != FALSE))
                caps = n68k_rx_verify_sum(packet, slot->sum, slot->copied,
                                          &drop);
            else
                caps = n68k_rx_verify(packet, &drop);

            if (drop != NX_FALSE)
            {
#ifdef AMINETXDUO_RXPROBE
                ami_sana2_rxprobe_drop(packet, slot);
#endif
                nx_packet_release(packet);
                iface->stats.rx_errors++;
                iface->stats.rx_err_verify++;
                return;
            }

            packet->nx_packet_interface_capability_flag = caps;
        }
#endif
        iface->stats.packets_received++;
        _nx_ip_packet_deferred_receive(iface->ip, packet);
        break;

    case AMI_ETHERTYPE_IPV6:
        iface->stats.packets_received++;
        _nx_ip_packet_deferred_receive(iface->ip, packet);
        break;

#ifndef NX_DISABLE_IPV4
    case AMI_ETHERTYPE_ARP:
        iface->stats.packets_received++;
        _nx_arp_packet_deferred_receive(iface->ip, packet);
        break;

    case AMI_ETHERTYPE_RARP:
        iface->stats.packets_received++;
        _nx_rarp_packet_deferred_receive(iface->ip, packet);
        break;
#endif /* !NX_DISABLE_IPV4 */

    default:
        iface->stats.unknown_types++;
        nx_packet_release(packet);
        break;
    }
}

/* ------------------------------------------------------------ slot arming */

/*
 * The pad is what makes every longword access above this line legal.
 *
 * nx_packet_data_start is a multiple of NX_PACKET_ALIGNMENT (4),
 * _nx_packet_pool_create() rounds it, and the IP header sits at
 * data_start + AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE in both modes.  If that
 * sum is not a multiple of 4, every IP, TCP and UDP header field NetX Duo
 * reads as a ULONG is misaligned, n68k_checksum.c's `long_ptr` walks the
 * payload from an unaligned start (its end-pointer rounding assumes it does
 * not), and on a 68000 an odd one is an address error rather than a slow path.
 * 2 + 14 == 16 is not a coincidence and is not free to change.
 */
_Static_assert(((AMI_SANA2_RX_PAD + AMI_ETH_HEADER_SIZE) & 3) == 0,
               "RX pad must land the IP header on a longword boundary");
_Static_assert((NX_PACKET_ALIGNMENT % 4) == 0,
               "packet payloads are not longword aligned");

/* Position the packet and work out where the copy hook must write. */
static VOID ami_sana2_rx_arm(AmiSana2If *iface, AmiRxSlot *slot)
{
    NX_PACKET *packet = slot->packet;
    UCHAR     *base   = packet->nx_packet_data_start + AMI_SANA2_RX_PAD;

    packet->nx_packet_prepend_ptr = base;
    packet->nx_packet_append_ptr  = base;
    packet->nx_packet_length      = 0;

    /* Cooked: leave room for the synthesised header. Raw: the device supplies
       it. */
    slot->dst = iface->raw_mode ? base : (base + AMI_ETH_HEADER_SIZE);

    slot->capacity = (ULONG)(packet->nx_packet_data_end - slot->dst);
    slot->copied   = 0;
#ifdef AMINETXDUO_RX_VERIFY
    /* And the sum with it.  ami_sana2_copy_to_buff() clears this on entry and
       sets it only on the aligned path, so a driver that never calls the copy
       hook -- it is optional in SANA-II, a device may hand the frame over
       another way -- would otherwise leave the previous frame's verdict here
       and have ami_sana2_rx_deliver() verify these bytes against that frame's
       accumulator.

       The guard is the member's: AmiRxSlot.summed only exists in an
       AMINETXDUO_RX_VERIFY build, so without it this line did not compile and
       -DAMINETXDUO_RX_VERIFY=OFF, which CMakeLists.txt offers, built nothing.
       No CI arm turned it off, so nothing said so. */
    slot->summed   = FALSE;
#endif
}

/* Post every idle slot that has, or can get, a packet. Returns how many reads
   are in flight afterwards. */
static UWORD ami_sana2_rx_post(AmiSana2Rx *rx)
{
    AmiSana2If *iface = rx->iface;
    UWORD       i;
    UWORD       live = 0;

    for (i = 0; i < rx->depth; i++)
    {
        AmiRxSlot *slot = &rx->slot[i];

        if (slot->posted)
        {
            live++;
            continue;
        }

        if (rx->stop || !iface->online)
            continue;

        if (slot->packet == NULL)
        {
            if (nx_packet_allocate(iface->pool, &slot->packet,
                                   NX_RECEIVE_PACKET, NX_NO_WAIT) != NX_SUCCESS)
            {
                slot->packet = NULL;
                iface->stats.alloc_failures++;
                continue;
            }
        }

        ami_sana2_rx_arm(iface, slot);

        slot->req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        slot->req.ios2_Req.io_Message.mn_ReplyPort    = rx->port;
        slot->req.ios2_Req.io_Command = CMD_READ;
        slot->req.ios2_Req.io_Flags   = iface->raw_mode ? SANA2IOF_RAW : 0;
        slot->req.ios2_Req.io_Error   = 0;
        slot->req.ios2_WireError      = 0;
        slot->req.ios2_PacketType     = rx->packet_type;
        slot->req.ios2_DataLength     = 0;
        slot->req.ios2_Data           = slot;
        slot->posted                  = TRUE;

        SendIO((struct IORequest *)&slot->req);
#ifdef AMINETXDUO_RXPROBE
        rx->probe.posts++;
        rx->probe.live++;
#endif
        live++;
    }

#ifdef AMINETXDUO_RXPROBE
    if (live == 0)
        rx->probe.post_zero++;
    else if (live < rx->depth)
        rx->probe.post_partial++;
#endif

    return live;
}

/* ------------------------------------------------------------ completion */

static VOID ami_sana2_rx_complete(AmiSana2Rx *rx, AmiRxSlot *slot)
{
    AmiSana2If *iface  = rx->iface;
    NX_PACKET  *packet = slot->packet;
    ULONG       length = slot->req.ios2_DataLength;
    UCHAR      *eth;

    if (packet == NULL)
        return;

    /* ios2_DataLength is the documented answer; fall back to what the copy
       hook took, for devices that fill only one of the two. */
    if (length == 0)
        length = slot->copied;

    if (length == 0 || length > slot->capacity)
    {
        /* Keep the packet: rearming is cheaper than a pool round trip. */
        iface->stats.rx_errors++;
        iface->stats.rx_err_length++;
        return;
    }

    if (!iface->raw_mode)
    {
        eth = packet->nx_packet_prepend_ptr;

        if (iface->addr_bytes == AMI_ETH_ADDR_SIZE)
        {
            if ((slot->req.ios2_Req.io_Flags & SANA2IOF_BCAST) != 0)
            {
                UWORD i;

                /* Drivers are inconsistent about what they leave in DstAddr
                   for a broadcast; the flag is the authority. */
                for (i = 0; i < AMI_ETH_ADDR_SIZE; i++)
                    eth[i] = 0xFF;
            }
            else
            {
                ami_sana2_copy_bytes(&eth[0], slot->req.ios2_DstAddr,
                                     AMI_ETH_ADDR_SIZE);
            }

            ami_sana2_copy_bytes(&eth[6], slot->req.ios2_SrcAddr,
                                 AMI_ETH_ADDR_SIZE);
        }
        else
        {
            UWORD i;

            /* No address field on this wire: keep the shape, zero the bytes. */
            for (i = 0; i < 12; i++)
                eth[i] = 0;
        }

        eth[12] = (UCHAR)(slot->req.ios2_PacketType >> 8);
        eth[13] = (UCHAR)(slot->req.ios2_PacketType);

        length += AMI_ETH_HEADER_SIZE;
    }

    packet->nx_packet_length     = length;
    packet->nx_packet_append_ptr = packet->nx_packet_prepend_ptr + length;

    slot->packet = NULL;     /* ownership passes to NetX Duo */
    ami_sana2_rx_deliver(iface, packet, slot);
}

static VOID ami_sana2_rx_drain(AmiSana2Rx *rx)
{
    struct Message *msg;

#ifdef AMINETXDUO_RXPROBE
    {
        AmiRxProbe *pr      = &rx->probe;
        UWORD       backlog = ami_rxprobe_backlog(rx->port);
        UWORD       avail;

        if (backlog > AMI_SANA2_RX_MAX_DEPTH)
            backlog = AMI_SANA2_RX_MAX_DEPTH;

        avail = (UWORD)((pr->live > (ULONG)backlog)
                            ? (pr->live - (ULONG)backlog) : 0UL);
        if (avail > AMI_SANA2_RX_MAX_DEPTH)
            avail = AMI_SANA2_RX_MAX_DEPTH;

        if (backlog != 0)
        {
            ULONG now  = ami_rxprobe_clock();
            ULONG span = now - pr->last_wake;

            pr->last_wake = now;
            pr->drains++;
            pr->backlog_hist[backlog]++;
            pr->avail_hist[avail]++;
            if (avail == 0)
                pr->dry++;

            /* Keep the deepest, smallest first, so one pass is enough. */
            if (backlog > pr->worst_backlog[0])
            {
                UWORD k = 0;

                while (k + 1 < AMI_RXPROBE_WORST &&
                       backlog > pr->worst_backlog[k + 1])
                {
                    pr->worst_backlog[k] = pr->worst_backlog[k + 1];
                    pr->worst_avail[k]   = pr->worst_avail[k + 1];
                    pr->worst_when[k]    = pr->worst_when[k + 1];
                    pr->worst_baton[k]   = pr->worst_baton[k + 1];
                    k++;
                }

                pr->worst_backlog[k] = backlog;
                pr->worst_avail[k]   = avail;
                pr->worst_when[k]    = span;
                pr->worst_baton[k]   = pr->baton_last;
            }
        }

        rx->iface->seq.avail = avail;
    }
#endif

    while ((msg = GetMsg(rx->port)) != NULL)
    {
#ifdef AMINETXDUO_RXPROBE
        if (rx->probe.live != 0)
            rx->probe.live--;
#endif
        /* The reply message is the slot: ios2_Req.io_Message is its first
           member's first member. */
        AmiRxSlot *slot = (AmiRxSlot *)msg;
        LONG       err  = (LONG)(BYTE)slot->req.ios2_Req.io_Error;

        slot->posted = FALSE;

        if (rx->stop)
            continue;

        if (err == 0)
        {
            ami_sana2_rx_complete(rx, slot);
        }
        else if (err == (LONG)IOERR_ABORTED)
        {
            /* Our own doing; nothing to count. */
        }
        else if (err == (LONG)S2ERR_OUTOFSERVICE)
        {
            /*
             * S2_OFFLINE returns every pending read this way, and so does a
             * driver whose card has gone away underneath, such as a pulled
             * cable on a2065.device.
             *
             * Stopping the reader is not enough. NetX Duo learns the link
             * state only from nx_interface_link_up, which the driver entry
             * sets on NX_LINK_ENABLE/DISABLE, that is, only when the stack
             * asks. Nothing asks when the wire is pulled, so without this the
             * interface stays marked up: ShowNetStatus reports LINKUP, ARP
             * keeps queueing, and every send fails in the shim with no way for
             * the layer above to know why.
             *
             * `Online` recovers it: netstack.c:2208 issues NX_LINK_ENABLE,
             * whose driver case sets nx_interface_link_up back to NX_TRUE.
             * There is no automatic retry here: this runs on the reader, and a
             * driver that has gone out of service should not be hammered with
             * S2_ONLINE from inside a receive loop.
             */
            rx->iface->online = FALSE;

            if (rx->iface->interface_ptr != NULL)
                rx->iface->interface_ptr->nx_interface_link_up = NX_FALSE;

            AMI_WARN("sana2: %s went out of service; link marked down",
                     rx->iface->device);
        }
        else
        {
            rx->iface->stats.rx_errors++;
            rx->iface->stats.rx_err_io++;
        }
    }
}

/* --------------------------------------------------------------- shutdown */

/*
 * CMD_FLUSH: "abort and return all queued I/O requests for this unit."
 *
 * Unit-wide rather than per-request, so it is tried second: it takes the other
 * reader's queued reads with it, costing a few frames during a shutdown that
 * was going to lose them anyway. Exec defines it for the case where AbortIO()
 * is a no-op, and several SANA-II drivers implement it without implementing
 * abort.
 */
static VOID ami_sana2_rx_flush(AmiSana2Rx *rx)
{
    struct MsgPort   *port;
    struct IOSana2Req req;

    port = CreateMsgPort();
    if (port == NULL)
        return;

    req = rx->iface->templ;
    req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    req.ios2_Req.io_Message.mn_ReplyPort    = port;
    req.ios2_Req.io_Message.mn_Length       = (UWORD)sizeof(struct IOSana2Req);
    req.ios2_Req.io_Command                 = CMD_FLUSH;
    req.ios2_Req.io_Error                   = 0;
    req.ios2_WireError                      = 0;

    /* DoIO() blocks in exec Wait(); nothing inside the bracket may touch
       ThreadX or NetX Duo state. */
    ami_sana2_block_enter();
    (VOID)DoIO((struct IORequest *)&req);
    ami_sana2_block_leave();

    DeleteMsgPort(port);
}

/*
 * Collect whatever the device has given back, waiting up to `tries` ticks of
 * 40 ms, and return how many requests it still owns.
 *
 * The deadline matters because WaitIO() has none and AbortIO() is only a
 * request: Commodore's a2065.device 2.16 does not honour it on a queued
 * CMD_READ. The top of CMakeLists.txt records the same behaviour from the
 * other end of the lifecycle, where the raw-framing probe posted a read,
 * aborted it and hung ami_sana2_open() for ever. A reader that trusted the
 * abort produced two `reader did not stop` warnings on every shutdown and ten
 * of the sixteen seconds a lone `curl --version` cost.
 *
 * GetMsg() does not block, so no ami_sana2_block_enter() bracket is needed
 * here; tx_thread_sleep() must be outside one, which is why the loop is shaped
 * this way.
 */
static UWORD ami_sana2_rx_reap(AmiSana2Rx *rx, UWORD tries)
{
    UWORD outstanding;
    UWORD i;
    UWORD t = 0;

    for (;;)
    {
        struct Message *msg;

        while ((msg = GetMsg(rx->port)) != NULL)
        {
            ((AmiRxSlot *)msg)->posted = FALSE;
#ifdef AMINETXDUO_RXPROBE
            if (rx->probe.live != 0)
                rx->probe.live--;
#endif
        }

        outstanding = 0;
        for (i = 0; i < rx->depth; i++)
        {
            if (rx->slot[i].posted)
                outstanding++;
        }

        if (outstanding == 0 || t >= tries)
            break;

        t++;
        tx_thread_sleep(2);
    }

    return outstanding;
}

/*
 * Reclaim every outstanding read, or report how many are still held.
 *
 * Three steps, since no one of them works on every driver:
 *
 *   1. AbortIO() on each. Cheap, and answered immediately by a well-behaved
 *      device; a2065.device 2.16 ignores it.
 *   2. CMD_FLUSH, which exec defines as "abort all queued requests for this
 *      unit" and SANA-II carries forward. Unit-wide rather than per-request,
 *      hence second: x-surf-100.device returns every opener's reads, not just
 *      ours. That is accepted rather than overlooked, the alternative is
 *      step 3, and another program losing its posted reads is recoverable
 *      where writing into memory we have freed is not.
 *   3. Give up and report it, having freed nothing the device can still write
 *      into. Freeing the reply port, releasing the pinned packets or letting
 *      ami_sana2_close() free the interface while the device still holds
 *      pointers into all three corrupts memory on the next frame.
 *
 * In practice this rarely fires, because ami_sana2_rx_stop() takes the wire
 * offline before stopping the readers and S2_OFFLINE returns every queued read
 * by itself.
 */
static VOID ami_sana2_rx_teardown(AmiSana2Rx *rx)
{
    UWORD outstanding;
    UWORD i;

    for (i = 0; i < rx->depth; i++)
    {
        if (rx->slot[i].posted)
            AbortIO((struct IORequest *)&rx->slot[i].req);
    }

    outstanding = ami_sana2_rx_reap(rx, AMI_SANA2_RX_REAP_TRIES);

    if (outstanding != 0)
    {
        AMI_WARN("sana2: %ld read(s) survived AbortIO; trying CMD_FLUSH",
                 (long)outstanding);
        ami_sana2_rx_flush(rx);
        outstanding = ami_sana2_rx_reap(rx, AMI_SANA2_RX_REAP_TRIES);
    }

    rx->orphans = outstanding;

    if (outstanding != 0)
    {
        /* Nothing below this line may run: each of those is a pointer the
           device still holds. */
        AMI_ERROR("sana2: %ld read(s) still owned by the device; leaking the "
                  "reader rather than corrupting memory", (long)outstanding);
        return;
    }

    for (i = 0; i < rx->depth; i++)
    {
        if (rx->slot[i].packet != NULL)
        {
            nx_packet_release(rx->slot[i].packet);
            rx->slot[i].packet = NULL;
        }
    }

    if (rx->port != NULL)
    {
        DeleteMsgPort(rx->port);
        rx->port = NULL;
    }
}

/* ----------------------------------------------------------- reader thread */

static VOID ami_sana2_rx_thread(ULONG argument)
{
    AmiSana2Rx *rx    = (AmiSana2Rx *)argument;
    AmiSana2If *iface = rx->iface;
    UWORD       i;

    rx->task = FindTask(NULL);
    rx->port = CreateMsgPort();

    if (rx->port == NULL)
    {
        rx->failed = TRUE;
        tx_semaphore_put(&rx->ready);
        tx_semaphore_put(&rx->exited);
        return;
    }

    rx->wake_mask = 1UL << rx->port->mp_SigBit;

    /*
     * TX reaping duty. One reader takes it, giving the transmit ring a context
     * that runs when nothing is being sent; see the header of sana2_tx.c.
     *
     * A failure here is not fatal: the interface falls back to reaping on the
     * next transmit, which leaves a lone unacknowledged segment unrecoverable,
     * hence the warning.
     */
    rx->reap_sigbit = -1;
    rx->reap_mask   = 0;

    if (rx->reap_tx)
    {
        BYTE bit = AllocSignal(-1);

        if (bit >= 0)
        {
            rx->reap_sigbit = bit;
            rx->reap_mask   = 1UL << (ULONG)bit;
            ami_sana2_tx_reap_bind(iface, rx->task, bit);
        }
        else
        {
            AMI_WARN("sana2: no signal for TX reaping; retransmission will "
                     "wait for the next send");
        }
    }

    /*
     * Every request is a copy of the opened one, which carries io_Device,
     * io_Unit and the device's own ios2_BufferManagement cookie. Only the
     * reply port and the command differ.
     */
    for (i = 0; i < rx->depth; i++)
    {
        rx->slot[i].req   = iface->templ;
        rx->slot[i].owner = rx;
        rx->slot[i].req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        rx->slot[i].req.ios2_Req.io_Message.mn_ReplyPort    = rx->port;
        rx->slot[i].req.ios2_Req.io_Message.mn_Length =
            (UWORD)sizeof(struct IOSana2Req);
    }

#ifdef AMINETXDUO_RXPROBE
    /* TimerBase is opened lazily; the probe's clock needs it before the first
       drain, not after. */
    (VOID)ami_millis();
#endif

    rx->running = TRUE;
    tx_semaphore_put(&rx->ready);

    while (!rx->stop)
    {
        /*
         * At the top of the loop rather than after the Wait(), because the
         * pool-empty path below continues without reaching a drain, and
         * releasing finished writes is one of the things that returns packets
         * to the pool it is waiting for.
         *
         * This does not reap; it asks the IP thread to. See sana2_tx.c.
         */
        if (rx->reap_mask != 0)
            ami_sana2_tx_defer(iface);

        if (ami_sana2_rx_post(rx) == 0)
        {
            /* Either the pool is empty or the interface is down. Back off
               rather than spin; ami_sana2_rx_stop() signals out of this. */
            tx_thread_sleep(2);
            continue;
        }

        ami_sana2_block_enter();
        Wait(rx->wake_mask | rx->reap_mask);
#ifdef AMINETXDUO_RXPROBE
        {
            AmiRxProbe *pr = &rx->probe;
            ULONG       t0 = ami_rxprobe_clock();
            ULONG       dt;

            ami_sana2_block_leave();

            dt = ami_rxprobe_clock() - t0;
            pr->baton_sum += dt;
            pr->baton_last = dt;
            if (dt > pr->baton_max)
                pr->baton_max = dt;
            pr->baton_hist[ami_rxprobe_bucket(dt)]++;
        }
#else
        ami_sana2_block_leave();
#endif

        ami_sana2_rx_drain(rx);

#ifdef AMINETXDUO_RXPROBE
        /* Keep probe_dev_rx within a few hundred frames of the truth: the
           report runs from NetStat, which cannot issue a device command. */
        if (rx->reap_tx && (rx->probe.drains & 31UL) == 0UL)
            ami_sana2_refresh_stats(iface);
#endif
    }

    /*
     * Hand the reply port back before anything else in the teardown: after
     * this returns no completion can signal this task, which makes freeing the
     * signal bit, and shortly the Task, safe.
     */
    if (rx->reap_mask != 0)
    {
        ami_sana2_tx_reap_unbind(iface);
        rx->reap_mask = 0;
    }

    if (rx->reap_sigbit >= 0)
    {
        FreeSignal(rx->reap_sigbit);
        rx->reap_sigbit = -1;
    }

    ami_sana2_rx_teardown(rx);

    rx->running = FALSE;
    tx_semaphore_put(&rx->exited);
}

/* ------------------------------------------------------------ start / stop */

static const ULONG ami_sana2_rx_types[AMI_SANA2_RX_READERS] =
{
    AMI_ETHERTYPE_IPV4,
    AMI_ETHERTYPE_ARP
#ifdef AMINETXDUO_IPV6
, AMI_ETHERTYPE_IPV6
#endif
};

static const UWORD ami_sana2_rx_depths[AMI_SANA2_RX_READERS] =
{
    AMI_SANA2_RX_DEPTH_IPV4,
    AMI_SANA2_RX_DEPTH_ARP
#ifdef AMINETXDUO_IPV6
, AMI_SANA2_RX_DEPTH_IPV6
#endif
};

static const CHAR *const ami_sana2_rx_names[AMI_SANA2_RX_READERS] =
{
    "sana2 rx ip",
    "sana2 rx arp"
#ifdef AMINETXDUO_IPV6
, "sana2 rx ip6"
#endif
};

/*
 * How deep the IPv4 read queue should be on this machine.
 *
 * The device discards every frame that arrives with no CMD_READ outstanding,
 * so this number is the receive window in frames, and bursts overrun it:
 * sixteen TCP connections opening at once answer with sixteen SYN/ACKs inside
 * a few hundred microseconds, and a 14 MHz 68020 cannot re-post a read between
 * them.  A fixed four lost six of those sixteen; see sana2_internal.h for the
 * measurements.
 *
 * Sized from the packet pool rather than fixed, because each outstanding read
 * pins a packet for its whole life and the pool is already sized from
 * AvailMem().  A four-megabyte machine gets the floor, an eight-megabyte one
 * the ceiling.
 */
static UWORD ami_sana2_rx_ipv4_depth(NX_PACKET_POOL *pool)
{
    ULONG depth;

    if (pool == NULL)
        return (UWORD)AMI_SANA2_RX_DEPTH_IPV4;

    depth = pool->nx_packet_pool_total / (ULONG)AMI_SANA2_RX_POOL_SHARE;

    if (depth < (ULONG)AMI_SANA2_RX_DEPTH_IPV4)
        depth = (ULONG)AMI_SANA2_RX_DEPTH_IPV4;
    if (depth > (ULONG)AMI_SANA2_RX_MAX_DEPTH)
        depth = (ULONG)AMI_SANA2_RX_MAX_DEPTH;

    return (UWORD)depth;
}

/*
 * A stack ThreadX will accept.
 *
 * tx_thread_create() refuses a stack that lies inside one it still has on its
 * created list, and a dead Task's adopted thread stays on that list holding a
 * range the allocator has since given back (tx_amiga_stack_in_use()). The
 * command that starts the network keeps bsdsocket.library open on purpose and
 * then exits, so there is always at least one, and an interface added at run
 * time is the first thing to allocate a stack afterwards: readers refused this
 * way left the interface attached with its link down, addressed and unable to
 * send anything.
 *
 * Refused blocks are held rather than freed, so the next attempt is somewhere
 * else, and given back once one lands clear.
 */
#define AMI_SANA2_STACK_TRIES   8

static APTR ami_sana2_alloc_stack(ULONG size)
{
    APTR  held[AMI_SANA2_STACK_TRIES];
    APTR  stack = NULL;
    UWORD n     = 0;
    UWORD i;

    while (n < (UWORD)AMI_SANA2_STACK_TRIES)
    {
        stack = ami_alloc_flags(size, MEMF_PUBLIC);
        if (stack == NULL)
            break;

        if (tx_amiga_stack_in_use(stack, size) != (UINT)TX_TRUE)
            break;

        held[n++] = stack;
        stack     = NULL;
    }

    for (i = 0; i < n; i++)
        ami_free(held[i]);

    if (stack == NULL && n != 0)
        AMI_ERROR("sana2: no reader stack clear of a dead task's registration "
                  "in %ld tries", (long)n);

    return stack;
}

LONG ami_sana2_rx_start(AmiSana2If *iface)
{
    UWORD i;
    UWORD ipv4_depth;
    UINT  txstatus;

    if (iface->rx_running)
        return 0;

    /* A reader the device still owns cannot be re-created on top of. */
    if (iface->rx_orphaned)
    {
        AMI_ERROR("sana2: interface has orphaned readers; not restarting");
        return -1;
    }

    if (iface->pool == NULL || iface->ip == NULL)
        return -1;

    ipv4_depth = ami_sana2_rx_ipv4_depth(iface->pool);
    AMI_INFO("sana2: IPv4 read queue %ld deep (pool %ld packets)",
             (long)ipv4_depth, (long)iface->pool->nx_packet_pool_total);

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        rx->iface       = iface;
        rx->packet_type = ami_sana2_rx_types[i];
        rx->depth       = (ami_sana2_rx_types[i] == AMI_ETHERTYPE_IPV4)
                              ? ipv4_depth
                              : ami_sana2_rx_depths[i];
        rx->stop        = FALSE;
        rx->failed      = FALSE;
        rx->running     = FALSE;
        rx->started     = FALSE;
        rx->reap_sigbit = -1;
        rx->reap_mask   = 0;
        /* Stale from the previous run: ami_sana2_rx_stop() Signal()s this mask
           at rx->task, and a reader that then fails to get a MsgPort would be
           poked on a bit it does not hold. */
        rx->wake_mask   = 0;
        rx->orphans     = 0;

        /* The first reader carries the TX reaping duty. It is the IPv4 one,
           the reader that always exists, but nothing depends on which: any
           thread that blocks in exec Wait() will do. */
        rx->reap_tx     = (i == 0) ? TRUE : FALSE;

        if (rx->depth > AMI_SANA2_RX_MAX_DEPTH)
            rx->depth = AMI_SANA2_RX_MAX_DEPTH;

        rx->stack = ami_sana2_alloc_stack((ULONG)AMI_SANA2_RX_STACK_SIZE);
        if (rx->stack == NULL)
        {
            AMI_ERROR("sana2: no memory for reader stack");
            ami_sana2_rx_stop(iface);
            return -1;
        }

        /*
         * One at a time, not `a() || b()`: the control blocks live inside the
         * ami_alloc()ed AmiSana2If, so a `ready` left created while `exited`
         * failed would stay on ThreadX's created list after ami_sana2_close()
         * freed the interface.
         */
        if (tx_semaphore_create(&rx->ready, (CHAR *)"s2rxrdy", 0) != TX_SUCCESS)
        {
            AMI_ERROR("sana2: cannot create reader semaphores");
            ami_sana2_rx_stop(iface);
            return -1;
        }

        if (tx_semaphore_create(&rx->exited, (CHAR *)"s2rxend", 0) != TX_SUCCESS)
        {
            AMI_ERROR("sana2: cannot create reader semaphores");
            tx_semaphore_delete(&rx->ready);
            ami_sana2_rx_stop(iface);
            return -1;
        }

        txstatus = tx_thread_create(&rx->thread, (CHAR *)ami_sana2_rx_names[i],
                                    ami_sana2_rx_thread, (ULONG)rx,
                                    rx->stack, AMI_SANA2_RX_STACK_SIZE,
                                    AMI_SANA2_RX_PRIORITY,
                                    AMI_SANA2_RX_PRIORITY,
                                    TX_NO_TIME_SLICE, TX_AUTO_START);
        if (txstatus != TX_SUCCESS)
        {
            AMI_ERROR("sana2: cannot create reader %ld (%ld), stack %lx",
                      (long)i, (long)txstatus, (unsigned long)rx->stack);
            tx_semaphore_delete(&rx->ready);
            tx_semaphore_delete(&rx->exited);
            ami_sana2_rx_stop(iface);
            return -1;
        }

        rx->started = TRUE;

        /* Wait for the reader to own a MsgPort before posting anything. */
        if (tx_semaphore_get(&rx->ready, NX_IP_PERIODIC_RATE) != TX_SUCCESS ||
            rx->failed)
        {
            AMI_ERROR("sana2: reader %ld failed to start", (long)i);
            ami_sana2_rx_stop(iface);
            return -1;
        }
    }

    iface->rx_running = TRUE;
    return 0;
}

VOID ami_sana2_rx_stop(AmiSana2If *iface)
{
    UWORD i;

#ifdef AMINETXDUO_RXPROBE
    ami_sana2_rxprobe_report(iface);
#endif

    /*
     * The order of these three phases matters.
     *
     * S2_OFFLINE returns every queued CMD_READ with S2ERR_OUTOFSERVICE (see
     * ami_sana2_rx_drain()), and it is the only mechanism that works on a
     * device which ignores AbortIO(), as Commodore's a2065.device 2.16 does.
     * Taking the interface offline after stopping the readers, the shape
     * ami_sana2_close(), NX_LINK_DISABLE and NX_LINK_UNINITIALIZE all use,
     * issues that command ten seconds after the readers gave up waiting, and
     * tears them down, threads terminated and stacks freed, with reads still
     * queued. Measured on an A1200 profile: `curl --version` took 16.22 s when
     * nothing else held bsdsocket.library open and 0.32 s when AddNetInterface
     * did, the difference being two five-second `reader did not stop` timeouts
     * on every last close. Doing it here rather than at each call site keeps
     * the next caller from getting it wrong; ami_sana2_offline() is
     * idempotent, so the offline() callers still do afterwards costs nothing.
     *
     * Going offline before setting `stop` leaves a window in which a reader is
     * still running and still posting fresh CMD_READs onto a device that is
     * now offline, so they are never returned, and the drain that follows
     * finds them outstanding, times out after five seconds and orphans the
     * reader. The window is widest for an Offline() issued within ~20 ms of
     * link-up, while the reader is filling all its slots at once;
     * docs/RESEARCH.md 56 records it as the reason a DHCP test had to wait
     * 200 ms before taking the wire away.
     *
     * Three phases, each needing the one before it:
     *   1. stop posting , `stop` seen at the top of the reader's loop
     *   2. offline, S2_OFFLINE returns every read still queued
     *   3. join, every reader is then guaranteed to reach its exit
     */
    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        if (!rx->started)
            continue;

        rx->stop = TRUE;

        /* Kick the reader out of Wait(). Signalling a bit the reader has
           already freed with its MsgPort is harmless; the Task stays alive
           until it has put the "exited" semaphore. */
        if (rx->task != NULL && rx->wake_mask != 0)
            Signal(rx->task, rx->wake_mask);
    }

    (VOID)ami_sana2_offline(iface);

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        if (rx->started)
        {
            if (tx_semaphore_get(&rx->exited,
                                 5 * NX_IP_PERIODIC_RATE) != TX_SUCCESS)
            {
                /*
                 * The reader is still inside its own teardown, which has its own
                 * deadline, so this should not happen. If it does, the thread is
                 * running on `rx->stack` and the ThreadX control block is live,
                 * so neither may be freed: leaking 4 KB is recoverable, a thread
                 * executing freed memory is not.
                 */
                AMI_ERROR("sana2: reader %ld did not stop; leaking its stack "
                          "rather than freeing memory it is running on", (long)i);
                iface->rx_orphaned = TRUE;
                continue;
            }

            /*
             * The reader exited, but ami_sana2_rx_teardown() kept its reply
             * port because the device would not give every read back. That
             * port's mp_SigTask is this thread's Task and exec will Signal()
             * through it on the next matching frame, so the Task has to outlive
             * the port: no terminate, no delete, no stack free. Its slots and
             * packets are inside the interface, which rx_orphaned then keeps
             * alive as well.
             */
            if (rx->orphans != 0)
            {
                AMI_ERROR("sana2: reader %ld left %ld read(s) with the device; "
                          "leaking its thread and stack, the reply port they "
                          "will complete through signals that Task",
                          (long)i, (long)rx->orphans);
                iface->rx_orphaned = TRUE;
                continue;
            }

            /*
             * Give the thread time to run off the end of its entry function
             * before the control block and stack go away. This is the one place
             * the shim relies on port behaviour it cannot yet verify
             * (docs/RESEARCH.md §6.2).
             */
            tx_thread_sleep(5);

            tx_thread_terminate(&rx->thread);
            tx_thread_delete(&rx->thread);
            tx_semaphore_delete(&rx->ready);
            tx_semaphore_delete(&rx->exited);
        }

        /*
         * Outside the started gate. A reader whose semaphores or thread would
         * not create has a stack and nothing else, and ami_sana2_rx_start()
         * unwinds by calling this rather than by hand; before, that stack was
         * lost, and again on every Online retry (sana2_driver.c, NX_LINK_ENABLE
         * re-enters and overwrites the pointer).
         */
        if (rx->stack != NULL)
        {
            ami_free(rx->stack);
            rx->stack = NULL;
        }

        rx->started = FALSE;
        rx->task    = NULL;
    }

    iface->rx_running = FALSE;
}
