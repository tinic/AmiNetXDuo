/*
 * AmiNetXDuo -- SANA-II transmit path.
 *
 * Framing note, because it contradicts the usual telling of the story: NetX
 * Duo does *not* prepend the Ethernet header before calling the driver. It
 * reserves NX_PHYSICAL_HEADER (16) bytes of headroom and leaves the link
 * header to the driver -- see nx_ram_network_driver.c, which builds all 14
 * bytes itself. So "strip the Ethernet header on TX" costs us nothing: in
 * cooked mode we simply never build one, and hand SANA-II exactly what it
 * wants -- ios2_PacketType from the driver command, ios2_DstAddr from
 * nx_ip_driver_physical_address_msw/lsw, and the payload from the prepend
 * pointer.
 *
 * Only the (default-off) raw path has to build a header, and it undoes that
 * before releasing the packet, because TCP hands the same NX_PACKET back for
 * retransmission.
 *
 * Writes go out with SendIO, never DoIO: the sending thread is the IP thread
 * or an application thread inside nx_tcp_socket_send, and neither may block on
 * the wire.
 *
 * REAPING IS A LIFECYCLE PROBLEM, NOT A BOOKKEEPING ONE, And it cost us
 * RETRANSMISSION ENTIRELY.
 *
 * nx_packet_transmit_release() does not free a queued TCP segment -- it marks
 * it NX_DRIVER_TX_DONE (nx_packet_transmit_release.c) -- and
 * _nx_tcp_socket_retransmit() will only resend a packet carrying that mark.
 * Every NetX Duo reference driver sets it inside NX_LINK_PACKET_SEND, because
 * their sends are synchronous. Ours cannot: a SANA-II CMD_WRITE completes long
 * after the driver entry returns, so the release happens in
 * ami_sana2_tx_reap() instead.
 *
 * This used to be called only from the transmit path, which meant a packet was
 * released by the NEXT packet. On a link that goes quiet -- a lost HTTP GET, a
 * lost TLS ClientHello, any request/response protocol with a single request
 * segment -- there is no next packet, so the segment stayed un-reaped for ever
 * and TCP believed the driver still had it. docs/RESEARCH.md 27.4 measured the
 * result: eleven seconds of total silence after one unacknowledged segment.
 *
 * So completions are reaped When they complete, in two hops:
 *
 *   1. The reply port raises a signal on one of the SANA-II reader threads
 *      (ami_sana2_tx_reap_bind), which is the only thread in this shim that
 *      blocks in exec Wait() and can therefore be woken by a device at all.
 *   2. That thread does not touch the packet. It calls
 *      nx_ip_driver_deferred_processing(), which is the mechanism NetX Duo
 *      provides for exactly this -- a driver saying "I have work, run me on
 *      the IP thread" -- and the IP thread comes back into
 *      ami_sana2_driver_entry() with NX_LINK_DEFERRED_PROCESSING and reaps.
 *
 * The second hop is the point. Releasing a packet is a mutation of NetX Duo's
 * transmit queue and of the packet's own prepend pointer -- transmit_release
 * strips the IP header back off -- and doing that from a reader thread would
 * interleave it with whatever the IP thread was in the middle of. On the IP
 * thread it runs where every other send runs, under nx_ip_protection, and the
 * question does not arise. docs/RESEARCH.md 27.4 noted that this shim never
 * asked for NX_LINK_DEFERRED_PROCESSING; this is what it is for.
 *
 * The transmit path pays almost nothing, and that is measured rather than
 * asserted: the reader only asks for deferred processing when the reply port
 * is Not already empty, and during a bulk transfer the next ami_sana2_tx_send()
 * has already drained it -- so the common case costs one pointer compare in a
 * thread that was going to wake anyway, and the IP thread is never disturbed.
 * The hop only happens when the link goes quiet, which is the case that was
 * broken.
 *
 * The GetMsg() at the top of ami_sana2_tx_send() stays, and is what keeps the
 * shim correct with no reader bound at all -- open-time probing, an interface
 * not yet enabled, or a reader that could not get a signal bit.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

/* _nx_ip_driver_deferred_processing(): the sanctioned way for a driver to ask
   for a callback on the IP thread. Declared in nx_api.h with no nx_ alias, so
   it is spelled with the underscore -- exactly as sana2_rx.c already spells
   _nx_ip_packet_deferred_receive(). */
#include "nx_ip.h"

#ifdef AMINETXDUO_BPF
#include "aminetxduo/bpf.h"
#endif

#include <proto/exec.h>

VOID ami_sana2_tx_init(AmiSana2If *iface)
{
    UWORD i;

    /* PA_IGNORE until a reader claims the duty: an interface is opened,
       queried and probed before any reader exists, and those writes must
       still complete somewhere. */
    ami_sana2_port_init(&iface->tx_port, NULL, 0, PA_IGNORE);

    for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
    {
        AmiTxSlot *slot = &iface->tx[i];

        slot->req    = iface->templ;
        slot->iface  = iface;
        slot->busy   = FALSE;
        slot->packet = NULL;

        slot->req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        slot->req.ios2_Req.io_Message.mn_ReplyPort    = &iface->tx_port;
        slot->req.ios2_Req.io_Message.mn_Length =
            (UWORD)sizeof(struct IOSana2Req);
        slot->req.ios2_Data = slot;
    }
}

/*
 * Hand the reply port a task to signal, so a completion wakes somebody.
 *
 *   The two alternatives were a periodic sweep and a thread of our own. A
 *   sweep adds latency to every retransmission in exchange for work done when
 *   there is none to do, and a thread costs a context switch per frame on a
 *   machine where docs/RESEARCH.md 16 budgets 8.0 ms per segment at 14 MHz.
 *   A signal costs the reader one extra bit in a Wait() it was making anyway,
 *   and the sender one Signal() inside exec's ReplyMsg -- which is the same
 *   cost the receive path has always paid.
 *
 *   The task is one of the SANA-II readers because it is the only thread in
 *   this shim that blocks in exec rather than in ThreadX: the NX_IP thread
 *   waits on ThreadX event flags, which Signal() cannot break. Which reader
 *   does not matter, so it is the first one -- see ami_sana2_rx_start().
 *
 * Disable() rather than Forbid(): a device may ReplyMsg from its interrupt,
 * and exec's PutMsg reads mp_Flags, mp_SigTask and mp_SigBit as one Disabled
 * unit. Three stores, so the region is a handful of instructions.
 */
VOID ami_sana2_tx_reap_bind(AmiSana2If *iface, struct Task *task, BYTE sigbit)
{
    if (iface == NULL || task == NULL || sigbit < 0)
        return;

    Disable();
    iface->tx_port.mp_SigTask = task;
    iface->tx_port.mp_SigBit  = (UBYTE)sigbit;
    iface->tx_port.mp_Flags   = PA_SIGNAL;
    Enable();
}

/*
 * Give it back. MUST happen before the task exits or its signal bit is freed:
 * after this the port is inert again and completions simply queue up for the
 * next ami_sana2_tx_reap(), which ami_sana2_tx_drain() performs at shutdown.
 */
VOID ami_sana2_tx_reap_unbind(AmiSana2If *iface)
{
    if (iface == NULL)
        return;

    Disable();
    iface->tx_port.mp_Flags   = PA_IGNORE;
    iface->tx_port.mp_SigTask = NULL;
    iface->tx_port.mp_SigBit  = 0;
    Enable();
}

/*
 * The reader's whole contribution: if anything has completed, ask NetX Duo to
 * run the driver on the IP thread, which is where ami_sana2_tx_reap() is
 * allowed to touch a packet.
 *
 * The emptiness test is a single pointer compare and needs no Forbid(). It can
 * only be wrong in the safe direction: exec's PutMsg links the message and
 * raises the signal inside one Disable()d region, so a reader that has been
 * woken always sees a non-empty list. A reader that sees an empty one was
 * woken by a completion somebody else has already collected -- the transmit
 * path, almost always -- and there is nothing left to defer.
 */
VOID ami_sana2_tx_defer(AmiSana2If *iface)
{
    struct List *list;

    if (iface == NULL || iface->ip == NULL)
        return;

    /* This NDK has no IsMsgPortEmpty(); an exec List is empty when its
       TailPred points back at the header. */
    list = &iface->tx_port.mp_MsgList;
    if (list->lh_TailPred == (struct Node *)list)
        return;

    _nx_ip_driver_deferred_processing(iface->ip);
}

/*
 * Reap finished writes. Non-blocking by construction: GetMsg() on an empty
 * port returns NULL.
 *
 * Callable from any thread and from several at once: GetMsg() is atomic, so a
 * slot belongs to exactly one caller from the moment it is dequeued, and
 * nx_packet_transmit_release() does its own TX_DISABLE. That is what lets the
 * reader reap concurrently with a transmit in progress on the IP thread.
 */
VOID ami_sana2_tx_reap(AmiSana2If *iface)
{
    struct Message *msg;

    while ((msg = GetMsg(&iface->tx_port)) != NULL)
    {
        /* ios2_Req.io_Message is the first member of the first member of
           AmiTxSlot, so the reply message *is* the slot. */
        AmiTxSlot *slot = (AmiTxSlot *)msg;
        LONG       err  = (LONG)(BYTE)slot->req.ios2_Req.io_Error;

        if (err != 0)
            iface->stats.tx_errors++;
        else
            iface->stats.packets_sent++;

        if (slot->packet != NULL)
        {
            /* Put the packet back the way NetX Duo handed it to us before
               releasing it -- a queued TCP segment gets sent again. */
            if (slot->hdr_len != 0)
            {
                slot->packet->nx_packet_prepend_ptr += slot->hdr_len;
                slot->packet->nx_packet_length      -= slot->hdr_len;
                slot->hdr_len = 0;
            }

            nx_packet_transmit_release(slot->packet);
            slot->packet = NULL;
        }

        slot->cursor   = NULL;
        slot->consumed = 0;
        slot->total    = 0;
        slot->busy     = FALSE;
    }
}

/* Abort anything still in flight and reap it. Used on shutdown. */
VOID ami_sana2_tx_drain(AmiSana2If *iface)
{
    UWORD i;
    UWORD spins;

    for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
    {
        if (iface->tx[i].busy)
            AbortIO((struct IORequest *)&iface->tx[i].req);
    }

    for (spins = 0; spins < 64; spins++)
    {
        BOOL any = FALSE;

        ami_sana2_tx_reap(iface);

        for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
        {
            if (iface->tx[i].busy)
                any = TRUE;
        }

        if (!any)
            return;

        tx_thread_sleep(1);
    }

    AMI_WARN("sana2: TX ring did not drain");
}

static AmiTxSlot *ami_sana2_tx_claim(AmiSana2If *iface)
{
    AmiTxSlot *slot = NULL;
    UWORD      i;

    /*
     * The driver entry is reachable from the IP thread and from any thread
     * inside nx_tcp_socket_send, so claiming a slot needs a critical section.
     * Forbid() rather than Disable(): long Disable() regions break serial,
     * floppy and audio (docs/RESEARCH.md §6.2).
     */
    Forbid();
    for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
    {
        if (!iface->tx[i].busy)
        {
            iface->tx[i].busy = TRUE;
            slot = &iface->tx[i];
            break;
        }
    }
    Permit();

    return slot;
}

/*
 * bpf_write()'s wire end. The payload is copied into a pool packet because the
 * caller's buffer is an application buffer and CMD_WRITE completes long after
 * this returns; ami_sana2_tx_send() releases the packet when it does.
 */
LONG ami_sana2_inject(AmiSana2If *iface, UWORD ether_type, const UBYTE *dst,
                      const UBYTE *payload, ULONG len)
{
    NX_PACKET *packet;
    ULONG      msw = 0;
    ULONG      lsw = 0;

    if (iface == NULL || payload == NULL || len == 0)
        return -1;

    if (!iface->online || iface->pool == NULL)
        return -1;

    if (iface->mtu != 0 && len > iface->mtu)
        return -1;

    if (nx_packet_allocate(iface->pool, &packet, NX_PHYSICAL_HEADER,
                           NX_NO_WAIT) != NX_SUCCESS)
        return -1;

    if (nx_packet_data_append(packet, (VOID *)payload, len, iface->pool,
                              NX_NO_WAIT) != NX_SUCCESS)
    {
        nx_packet_release(packet);
        return -1;
    }

    packet->nx_packet_address.nx_packet_interface_ptr = iface->interface_ptr;

    if (dst != NULL && iface->addr_bytes == AMI_ETH_ADDR_SIZE)
    {
        msw = ((ULONG)dst[0] << 8) | (ULONG)dst[1];
        lsw = ((ULONG)dst[2] << 24) | ((ULONG)dst[3] << 16) |
              ((ULONG)dst[4] << 8) | (ULONG)dst[5];
    }

    /* tx_send() owns the packet from here, success or failure. */
    return (ami_sana2_tx_send(iface, packet, ether_type, msw, lsw) == NX_SUCCESS)
               ? 0 : -1;
}

UINT ami_sana2_tx_send(AmiSana2If *iface, NX_PACKET *packet, UWORD ether_type,
                       ULONG dst_msw, ULONG dst_lsw)
{
    AmiTxSlot *slot;
    UWORD      spins;
    ULONG      length;

    if (iface == NULL || packet == NULL)
        return NX_PTR_ERROR;

    /* Belt to the reader's braces: cheap when the reader has already emptied
       the port, and the only reaping there is when no reader is bound. */
    ami_sana2_tx_reap(iface);

    if (!iface->online)
    {
        nx_packet_transmit_release(packet);
        iface->stats.tx_errors++;
        return NX_NOT_ENABLED;
    }

    slot = ami_sana2_tx_claim(iface);
    for (spins = 0; slot == NULL && spins < AMI_SANA2_TX_WAIT_TICKS; spins++)
    {
        tx_thread_sleep(1);
        ami_sana2_tx_reap(iface);
        slot = ami_sana2_tx_claim(iface);
    }

    if (slot == NULL)
    {
        /* A full ring is congestion, not an error: drop and let the upper
           layers retransmit. */
        nx_packet_transmit_release(packet);
        iface->stats.tx_errors++;
        return NX_TX_QUEUE_DEPTH;
    }

    slot->hdr_len = 0;

    if (iface->raw_mode)
    {
        UCHAR *eth;

        if ((ULONG)(packet->nx_packet_prepend_ptr -
                    packet->nx_packet_data_start) < AMI_ETH_HEADER_SIZE)
        {
            slot->busy = FALSE;
            nx_packet_transmit_release(packet);
            iface->stats.tx_errors++;
            return NX_UNDERFLOW;
        }

        packet->nx_packet_prepend_ptr -= AMI_ETH_HEADER_SIZE;
        packet->nx_packet_length      += AMI_ETH_HEADER_SIZE;
        slot->hdr_len = AMI_ETH_HEADER_SIZE;

        eth    = packet->nx_packet_prepend_ptr;
        eth[0] = (UCHAR)(dst_msw >> 8);
        eth[1] = (UCHAR)(dst_msw);
        eth[2] = (UCHAR)(dst_lsw >> 24);
        eth[3] = (UCHAR)(dst_lsw >> 16);
        eth[4] = (UCHAR)(dst_lsw >> 8);
        eth[5] = (UCHAR)(dst_lsw);
        ami_sana2_copy_bytes(&eth[6], iface->mac, AMI_ETH_ADDR_SIZE);
        eth[12] = (UCHAR)(ether_type >> 8);
        eth[13] = (UCHAR)(ether_type);
    }

#ifdef AMINETXDUO_BPF
    /*
     * The transmit tap, after the raw block above so one call site serves both
     * modes.  `iface->raw_mode` is exactly `has_link_header`: in raw mode the
     * 14 bytes are now in the packet, in cooked mode they exist nowhere and
     * the tap synthesises them from the three facts the CMD_WRITE below is
     * about to carry.  Nothing is written into the packet -- it is very often
     * a queued TCP segment that will be handed back for retransmission.
     */
    ami_bpf_tap_tx(iface, packet, iface->raw_mode, ether_type,
                   dst_msw, dst_lsw, iface->mac);
#endif

    length = packet->nx_packet_length;

    slot->packet     = packet;
    slot->cursor     = NULL;      /* the copy hook rewinds on first call */
    slot->cursor_off = 0;
    slot->consumed   = 0;
    slot->total      = length;

    slot->req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    slot->req.ios2_Req.io_Message.mn_ReplyPort    = &iface->tx_port;
    slot->req.ios2_Req.io_Command = CMD_WRITE;
    slot->req.ios2_Req.io_Flags   = iface->raw_mode ? SANA2IOF_RAW : 0;
    slot->req.ios2_Req.io_Error   = 0;
    slot->req.ios2_WireError      = 0;
    slot->req.ios2_PacketType     = (ULONG)ether_type;
    slot->req.ios2_DataLength     = length;
    slot->req.ios2_Data           = slot;

    /*
     * Broadcast and multicast both go out as CMD_WRITE with the destination
     * address the IP layer chose. S2_BROADCAST/S2_MULTICAST exist, but the
     * spec warns they are "not supported by all networks and/or network
     * interfaces", and on Ethernet they buy nothing over an ff:ff:... write --
     * which is also the only way ARP requests can go out, since NetX Duo sends
     * those as NX_LINK_ARP_SEND rather than NX_LINK_PACKET_BROADCAST.
     */
    if (iface->addr_bytes == AMI_ETH_ADDR_SIZE)
    {
        slot->req.ios2_DstAddr[0] = (UBYTE)(dst_msw >> 8);
        slot->req.ios2_DstAddr[1] = (UBYTE)(dst_msw);
        slot->req.ios2_DstAddr[2] = (UBYTE)(dst_lsw >> 24);
        slot->req.ios2_DstAddr[3] = (UBYTE)(dst_lsw >> 16);
        slot->req.ios2_DstAddr[4] = (UBYTE)(dst_lsw >> 8);
        slot->req.ios2_DstAddr[5] = (UBYTE)(dst_lsw);
    }
    else
    {
        UWORD i;

        /* Point-to-point wires (PPP, SLIP) have no address field at all. */
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            slot->req.ios2_DstAddr[i] = 0;
    }

    SendIO((struct IORequest *)&slot->req);

    return NX_SUCCESS;
}
