/*
 * AmiNetXDuo, SANA-II receive pipeline.
 *
 * SANA-II devices have no buffers of their own: a frame that arrives with no
 * matching CMD_READ outstanding is dropped, and CMD_READ is per packet type.
 * The shim therefore runs one reader thread per type, 0x0800, 0x0806 and, when
 * AMINETXDUO_IPV6 is defined, 0x86DD, each keeping several reads in flight
 * so the pipeline never empties.
 *
 * Because S2_CopyToBuff is called at interrupt level, the NX_PACKET a read
 * lands in must already exist when the read is posted. Each outstanding
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
 * object, so they are the only ones a device's ReplyMsg can wake. Without a
 * thread that wakes on a completion, TCP never learns that the driver has
 * finished with a segment, and never retransmits it. The reader does not touch
 * the packet: it asks NetX Duo for deferred processing and the IP thread does
 * the work. See sana2_tx.c. The mechanism here is one extra signal bit in a
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

#include "aminetxduo/random.h"

#include <proto/exec.h>
/* BeginIO(): a macro over the device's own vector, no amiga.lib to link. */
#include <inline/alib.h>

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

        /* A hole closing sub-millisecond is a frame that came late. One
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
 * fault is the sender's or a bridge's, not this stack's or the driver's.  This
 * line attributed the 2026-08 read-collapse rig: every rejected segment was a
 * burst first-transmission carrying the pseudo-header sum.
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
     * The device's own receive count against the shim's.
     * iface->stats.packets_received is incremented in ami_sana2_rx_deliver().
     * The device's count is what a2065.device saw on the wire, so the
     * difference is what never reached a CMD_READ.
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
         * Check here and tell the stack what was checked, so it does not walk
         * the payload a second time.  A frame this declines carries no bits,
         * and the stack checks it exactly as it always has.  A frame it
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
 * reads as a ULONG is misaligned.  n68k_checksum.c's `long_ptr` then walks the
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
       sets it only on the aligned path.  A driver that never calls the copy
       hook -- it is optional in SANA-II, and a device can hand the frame over
       another way -- otherwise leaves the previous frame's verdict here, and
       ami_sana2_rx_deliver() checks these bytes against that frame's
       accumulator.

       The guard is the member's: AmiRxSlot.summed only exists in an
       AMINETXDUO_RX_VERIFY build, so without it this line did not compile, and
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

        /* BeginIO(), not SendIO(): SendIO() zeroes io_Flags and drops the
           SANA2IOF_RAW just set. Both lines it runs are above. */
        BeginIO((struct IORequest *)&slot->req);
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

/* Reconcile the device's documented ios2_DataLength answer with the number of
   bytes its CopyToBuff call actually initialized.  A smaller reported length
   is safe, but its carried checksum covered the longer copy and cannot be
   reused.  A larger reported length would expose stale packet-pool bytes. */
BOOL ami_sana2_rx_resolve_length(AmiRxSlot *slot, ULONG *length)
{
    if (slot == NULL || length == NULL)
        return FALSE;

    if (slot->copied == 0)
        return FALSE;

    if (*length == 0)
        *length = slot->copied;

    if (*length > slot->capacity || *length > slot->copied)
        return FALSE;

#ifdef AMINETXDUO_RX_VERIFY
    if (*length != slot->copied)
        slot->summed = FALSE;
#endif

    return TRUE;
}

static VOID ami_sana2_rx_complete(AmiSana2Rx *rx, AmiRxSlot *slot)
{
    AmiSana2If *iface  = rx->iface;
    NX_PACKET  *packet = slot->packet;
    ULONG       length = slot->req.ios2_DataLength;
    UCHAR      *eth;

    if (packet == NULL)
        return;

    /* ios2_DataLength is the documented answer. Fall back to what the copy
       hook took, for devices that fill only one of the two, but never extend
       the packet past the bytes the hook initialized. */
    if (!ami_sana2_rx_resolve_length(slot, &length))
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
                   for a broadcast. The flag is the authority. */
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

    /*
     * When this frame arrived is the only thing this machine knows that is
     * not in its own boot image, and the entropy pool cannot clear
     * AMI_RANDOM_MIN_BITS without it -- see the arrival section of
     * src/common/ami_random.c.  A load and a branch once the pool is over the
     * bar, which is within seconds of the interface carrying traffic.
     */
    ami_random_arrival();

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
            /* Asked for here, so nothing to count. */
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
             * driver that has gone out of service must not be sent S2_ONLINE
             * over and over from inside a receive loop.
             */
            rx->iface->online = FALSE;

            if (rx->iface->interface_ptr != NULL)
                rx->iface->interface_ptr->nx_interface_link_up = NX_FALSE;

            AMI_WARN("sana2: %s went out of service. The link is marked down",
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

    /* DoIO() blocks in exec Wait(). Nothing inside the bracket can touch
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
 * here. tx_thread_sleep() must be outside one, which is why the loop is shaped
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
 *   1. AbortIO() on each. Cheap, and answered at once by a well-behaved
 *      device. a2065.device 2.16 ignores it.
 *   2. CMD_FLUSH, which exec defines as "abort all queued requests for this
 *      unit" and SANA-II carries forward. Unit-wide rather than per-request,
 *      hence second: x-surf-100.device returns every opener's reads, not just
 *      this stack's. That is accepted rather than overlooked: the alternative
 *      is step 3, and another program losing its posted reads is recoverable
 *      where a write into freed memory is not.
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
        /* Nothing below this line can run: each of those is a pointer the
           device still holds. */
        AMI_ERROR("sana2: %ld read(s) still owned by the device. The "
                  "reader leaks. A free here corrupts memory",
                  (long)outstanding);
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
     * TX reaping duty. One reader takes it, which gives the transmit ring a
     * context that runs when nothing is being sent. See the header of
     * sana2_tx.c.
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
            AMI_WARN("sana2: no signal for TX reaping. Retransmission will "
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
    /* TimerBase is opened lazily. The probe's clock needs it before the first
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
         * This does not reap. It asks the IP thread to. See sana2_tx.c.
         */
        if (rx->reap_mask != 0)
            ami_sana2_tx_defer(iface);

        if (ami_sana2_rx_post(rx) == 0)
        {
            /* Either the pool is empty or the interface is down. Back off
               rather than spin. ami_sana2_rx_stop() signals out of this. */
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

static const CHAR *const ami_sana2_rx_names[AMI_SANA2_RX_READERS] =
{
    "sana2 rx ip",
    "sana2 rx arp"
#ifdef AMINETXDUO_IPV6
, "sana2 rx ip6"
#endif
};

/* ------------------------------------------------------------- read depth */

/*
 * The most reads a wire this slow can ever need.
 *
 * S2_DEVICEQUERY reports a line rate and AROSTCP scales its read pool up with
 * it, on the reasoning that a faster wire delivers a burst sooner.  That is
 * true and it is not the binding constraint here, which is why this table caps
 * rather than scales, and why every Ethernet row in it is the ceiling.
 *
 * A read queue has to hold the burst a peer is allowed to send.  Whether the
 * queue drains faster than the burst arrives is a race between the wire and
 * the CPU, and on the machines this stack runs on the CPU loses at every
 * Ethernet rate there is: an A1200 receives at 4.9 Mbit/s flat out
 * (tests/tools/run-iperf.sh, a2065, 8 MB, guest receiving), which is 2.4 ms of
 * CPU per full frame against the 1.2 ms a 10BASE-T wire takes to deliver the
 * next one.  Ten megabits is already faster than the reader; a hundred is not
 * differently faster.  Measured, n=3 interleaved, medians in kbit/s, guest
 * receiving, depth forced:
 *
 *      depth              4      8     16     32
 *      a2065     8 MB  1588   2254   4896   4843
 *      a2065     0 MB  1917   1929   1910   1900
 *      xsurf100  8 MB  1457   1976   3962   3964
 *      xsurf100  0 MB  1554   1589   1586   1586
 *
 * The card that reports ten megabits and the card that reports a hundred want
 * the same depth, and what separates the two rows of each is memory.
 *
 * That the lab's cards are unpaced is not what makes those rows agree.  The
 * emulated a2065 delivers as fast as the host can push unless
 * AMIBERRY_A2065_KBIT is set, so it was set: paced to a real ten megabits, the
 * same machine reads 1683 / 2502 / 4798 / 4672 kbit/s at the same four depths,
 * which is the unpaced row again.  A ten-megabit wire is not a constraint on a
 * receiver that cannot fill it.
 *
 * A wire SLOWER than the receiver is the one case where the reported rate
 * decides anything, and that is measured too.  Paced to two megabits, below
 * the 4.9 Mbit/s the reader can manage, the same four depths read 1749 / 1579
 * / 1580 / 1571 kbit/s: the floor is as good as the ceiling, and the twenty
 * eight packets the ceiling would pin buy nothing.  Hence a cap and a
 * threshold below the reader's own rate rather than a scale.
 *
 * No Ethernet board in src/netdev/netdev_cards.c reports under ten megabits,
 * so nothing this project can boot takes the bottom row.  The wires that would
 * are slip.device and rs485.device, which have no emulated board to run on
 * (tests/tools/cards.sh); the paced card above is what stands in for them.
 */
typedef struct AmiRxSpeedStep
{
    ULONG   bps;        /* the fastest wire this row covers */
    UWORD   depth;      /* the most reads it can ever need  */
} AmiRxSpeedStep;

static const AmiRxSpeedStep ami_sana2_rx_ladder[] =
{
    {     4000000UL,  AMI_SANA2_RX_DEPTH_IPV4 },
    { 0xFFFFFFFFUL,   AMI_SANA2_RX_MAX_DEPTH  }
};

static UWORD ami_sana2_rx_wire_depth(ULONG bps)
{
    UWORD i;
    UWORD last = (UWORD)(sizeof(ami_sana2_rx_ladder) /
                         sizeof(ami_sana2_rx_ladder[0]) - 1);

    /* 0 is "the device did not say": either it left BPS alone or it supplied a
       short block that stopped before it, and ami_sana2_query() zeroes the
       block first, so both arrive as 0.  Nothing else needs a special case --
       the bottom step covers every nonsense value a device can answer, and
       0xFFFFFFFF, which is what a rate that did not fit a ULONG looks like,
       falls off the top and is treated as very fast. */
    if (bps == 0)
        bps = (ULONG)AMI_SANA2_BPS_DEFAULT;

    for (i = 0; i < last; i++)
    {
        if (bps <= ami_sana2_rx_ladder[i].bps)
            return ami_sana2_rx_ladder[i].depth;
    }

    return ami_sana2_rx_ladder[last].depth;
}

/*
 * What each reader gets: the smaller of what the wire asks for and what the
 * pool can spare.
 *
 * WHAT THE POOL CAN SPARE.  Each outstanding read pins one NX_PACKET for the
 * whole life of the request, and the pool is sized from AvailMem()
 * (src/netstack/netstack.c).  The budget is therefore over all three readers
 * together and not one each: with the IPv6 reader built in, the old per-reader
 * arithmetic pinned 4 + 2 + 2 on a machine whose whole pool was seventeen
 * packets, and called it "the four".
 *
 * The floors are never given up.  A machine too small to spare them still gets
 * them, because a reader below its floor cannot absorb the smallest burst
 * there is -- the SYN/ACKs of a handful of connections opening at once -- and
 * the failure is a connection that never completes, not a slow one.
 *
 * IPv4 is served before IPv6 out of what is left over, and the IPv6 reader has
 * a cap of its own on top of that (AMI_SANA2_RX_WANT_IPV6).  On a 2 MB machine
 * the spare runs out first and the IPv6 reader keeps exactly the depth it had
 * before this function existed; on an 8 MB one the cap is what stops it.
 *
 * THE IPv6 READER IS NOT A NEIGHBOUR-DISCOVERY READER.  Its packet type is
 * 0x86DD, which is the whole protocol: every IPv6 TCP segment this machine
 * receives arrives through it.  It was two deep, against IPv4's thirty-two, on
 * the same wire and for the same traffic, and that is a cliff rather than a
 * handicap: 386 kbit/s against 1959 at eight, measured on the same boot as the
 * IPv4 transfer beside it.
 *
 * WHAT THE WHOLE CHANGE COSTS THE CARDS.  tests/tools/run-iperf.sh against a
 * peer off the box, every board in tests/tools/cards.sh, both memory arms,
 * arms alternating direction between reps, n=3, medians: no card's receive
 * rate moves more than 2.7 per cent either way and the sign is not consistent
 * -- by bytes rather than by rate the worst cell is a different one.  Transmit
 * is inside 1.5 per cent.  Bring-up, the AddNetInterface block, is identical
 * to within 20 ms on every card: iface->bps was already read at open.
 *
 * ARP STAYS AT ITS FLOOR, deliberately.  Two is a request and its reply, which
 * is the whole of what that reader carries; a deeper queue buys tolerance of a
 * broadcast storm and nothing else, costs a pinned packet per slot on the
 * smallest machine there is, and no workload in tests/ can overrun two.  The
 * one that would is a LAN sweeping the segment while this machine is opening a
 * connection, and nothing here measures it.  The test beside this asserts the
 * flatness rather than leaving it to be inferred from its absence.
 */
static UWORD ami_sana2_rx_window_depth(ULONG pool_total)
{
    ULONG frames = pool_total / (ULONG)AMI_SANA2_RX_POOL_SHARE;

    if (frames < (ULONG)AMI_SANA2_RX_DEPTH_IPV4)
        frames = (ULONG)AMI_SANA2_RX_DEPTH_IPV4;
    if (frames > (ULONG)AMI_SANA2_RX_MAX_DEPTH)
        frames = (ULONG)AMI_SANA2_RX_MAX_DEPTH;

    return (UWORD)frames;
}

VOID ami_sana2_rx_plan(ULONG bps, ULONG pool_total, BOOL dual_stack,
                       AmiRxDepths *out)
{
    UWORD want;
    UWORD cap;
    ULONG budget;
    ULONG floors;
    ULONG spare;
    UWORD give;

    if (out == NULL)
        return;

    want = ami_sana2_rx_window_depth(pool_total);
    cap  = ami_sana2_rx_wire_depth(bps);
    if (want > cap)
        want = cap;

    out->ipv4 = (UWORD)AMI_SANA2_RX_DEPTH_IPV4;
    out->arp  = (UWORD)AMI_SANA2_RX_DEPTH_ARP;
    out->ipv6 = dual_stack ? (UWORD)AMI_SANA2_RX_DEPTH_IPV6 : (UWORD)0;

    budget = pool_total / (ULONG)AMI_SANA2_RX_BUDGET_SHARE;
    floors = (ULONG)out->ipv4 + (ULONG)out->arp + (ULONG)out->ipv6;
    spare  = (budget > floors) ? (budget - floors) : 0UL;

    give = (want > out->ipv4) ? (UWORD)(want - out->ipv4) : (UWORD)0;
    if ((ULONG)give > spare)
        give = (UWORD)spare;
    out->ipv4 = (UWORD)(out->ipv4 + give);
    spare    -= (ULONG)give;

    if (dual_stack)
    {
        UWORD want6 = want;

        if (want6 > (UWORD)AMI_SANA2_RX_WANT_IPV6)
            want6 = (UWORD)AMI_SANA2_RX_WANT_IPV6;

        give = (want6 > out->ipv6) ? (UWORD)(want6 - out->ipv6) : (UWORD)0;
        if ((ULONG)give > spare)
            give = (UWORD)spare;
        out->ipv6 = (UWORD)(out->ipv6 + give);
    }
}

/*
 * A stack ThreadX accepts.
 *
 * tx_thread_create() refuses a stack that lies inside one it still has on its
 * created list, and a dead Task's adopted thread stays on that list holding a
 * range the allocator has since given back (tx_amiga_stack_in_use()). The
 * command that starts the network keeps bsdsocket.library open on purpose and
 * then exits, so there is always at least one. An interface added at run time
 * is the first thing to allocate a stack afterwards, and readers refused this
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
    UWORD       i;
    AmiRxDepths depths;
    UINT        txstatus;

    if (iface->rx_running)
        return 0;

    /* A reader the device still owns cannot be re-created on top of. */
    if (iface->rx_orphaned)
    {
        AMI_ERROR("sana2: interface has orphaned readers. "
                  "It is not restarted");
        return -1;
    }

    if (iface->pool == NULL || iface->ip == NULL)
        return -1;

    /*
     * iface->bps is already here: ami_sana2_query() reads it at open, before
     * any reader exists, so the line rate costs this path no device round trip
     * and bring-up is not asked for one more answer than it was.
     */
    ami_sana2_rx_plan(iface->bps, iface->pool->nx_packet_pool_total,
                      (BOOL)(AMI_SANA2_RX_READERS == 3), &depths);

    AMI_INFO("sana2: read queues ip %ld arp %ld ip6 %ld "
             "(pool %ld packets, %ld bps)",
             (long)depths.ipv4, (long)depths.arp, (long)depths.ipv6,
             (long)iface->pool->nx_packet_pool_total, (long)iface->bps);

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        rx->iface       = iface;
        rx->packet_type = ami_sana2_rx_types[i];
        if (ami_sana2_rx_types[i] == AMI_ETHERTYPE_IPV4)
            rx->depth = depths.ipv4;
        else if (ami_sana2_rx_types[i] == AMI_ETHERTYPE_ARP)
            rx->depth = depths.arp;
        else
            rx->depth = depths.ipv6;
        rx->stop        = FALSE;
        rx->failed      = FALSE;
        rx->running     = FALSE;
        rx->started     = FALSE;
        rx->reap_sigbit = -1;
        rx->reap_mask   = 0;
        /* Stale from the previous run: ami_sana2_rx_stop() Signal()s this mask
           at rx->task, and a reader that then fails to get a MsgPort is
           signalled on a bit it does not hold. */
        rx->wake_mask   = 0;
        rx->orphans     = 0;

        /* The first reader carries the TX reaping duty. It is the IPv4 one,
           the reader that always exists, but nothing depends on which: any
           thread that blocks in exec Wait() serves. */
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
         * failed stays on ThreadX's created list after ami_sana2_close() frees
         * the interface.
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
    ULONG zombies;

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
     * on every last close. This happens here rather than at each call site, so
     * the next caller cannot get it wrong. ami_sana2_offline() is idempotent,
     * so the offline() calls that still follow cost nothing.
     *
     * If the offline happens before `stop` is set, a window is left in which a
     * reader is still running and still posting fresh CMD_READs onto a device
     * that is now offline, so they are never returned, and the drain that
     * follows finds them outstanding, times out after five seconds and orphans
     * the reader. The window is widest for an Offline() issued within ~20 ms
     * of link-up, while the reader is filling all its slots at once.
     * docs/RESEARCH.md 56 records it as the reason a DHCP test had to wait
     * 200 ms before taking the wire away.
     *
     * Three phases, each needing the one before it:
     *   1. stop posting, `stop` is seen at the top of the reader's loop
     *   2. offline, S2_OFFLINE returns every read still queued
     *   3. join, every reader is then guaranteed to reach its exit
     */
    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        if (!rx->started)
            continue;

        rx->stop = TRUE;

        /* Wake the reader out of Wait(). A signal on a bit the reader has
           already freed with its MsgPort is harmless. The Task stays alive
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
                 * deadline, so this does not normally happen. If it does, the
                 * thread is running on `rx->stack` and the ThreadX control block
                 * is live, so neither can be freed: a 4 KB leak is recoverable,
                 * a thread that runs on freed memory is not.
                 */
                AMI_ERROR("sana2: reader %ld did not stop. Its stack leaks. A "
                          "free here corrupts memory the reader runs on",
                          (long)i);
                iface->rx_orphaned = TRUE;
                continue;
            }

            /*
             * The reader exited, but ami_sana2_rx_teardown() kept its reply
             * port because the device would not give every read back. That
             * port's mp_SigTask is this thread's Task and exec signals through
             * it on the next matching frame, so the Task has to outlive
             * the port: no terminate, no delete, no stack free. Its slots and
             * packets are inside the interface, which rx_orphaned then keeps
             * alive as well.
             */
            if (rx->orphans != 0)
            {
                AMI_ERROR("sana2: reader %ld left %ld read(s) with the "
                          "device. Its thread and stack leak. The reply "
                          "port they "
                          "will complete through signals that Task",
                          (long)i, (long)rx->orphans);
                iface->rx_orphaned = TRUE;
                continue;
            }

            /*
             * Give the thread time to run off the end of its entry function
             * before the control block and stack go away. This is the one place
             * the shim relies on port behaviour it cannot yet check
             * (docs/RESEARCH.md §6.2).
             */
            tx_thread_sleep(5);

            /* The total is monotonic.  The live gauge can stay unchanged if
               an older zombie exits while this delete creates a new one,
               which would make this code free the new zombie's live stack. */
            zombies = tx_amiga_zombie_tasks();

            tx_thread_terminate(&rx->thread);
            tx_thread_delete(&rx->thread);

            /*
             * tx_thread_delete() removes the Task by asking it to destroy
             * itself and gives up after two seconds. A reader that has put
             * `exited` is on its way out of its entry function with no Exec
             * call left to block in, so it always arrives. A failure leaves a
             * zombie still running on rx->stack, and this frees the memory
             * under it. The monotonic zombie count is the signal that this
             * happened (tx_amiga.h); unlike the live gauge it cannot be
             * cancelled by some older zombie exiting at the same time. Leak
             * the stack when it moves, as the two paths above do.
             */
            if (tx_amiga_zombie_tasks() != zombies)
            {
                AMI_ERROR("sana2: reader %ld cannot be removed. Its stack "
                          "leaks. A free here corrupts memory the reader "
                          "runs on",
                          (long)i);
                iface->rx_orphaned = TRUE;
                continue;
            }

            tx_semaphore_delete(&rx->ready);
            tx_semaphore_delete(&rx->exited);
        }

        /*
         * Outside the started gate. A reader whose semaphores or thread would
         * not create has a stack and nothing else, and ami_sana2_rx_start()
         * unwinds by calling this rather than by hand. Before, that stack was
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
