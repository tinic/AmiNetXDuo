/*
 * AmiNetXDuo, SANA-II transmit path.
 *
 * Framing: NetX Duo does not prepend the Ethernet header before calling the
 * driver. It reserves NX_PHYSICAL_HEADER (16) bytes of headroom and leaves the
 * link header to the driver, see nx_ram_network_driver.c, which builds all
 * 14 bytes itself. Cooked mode therefore never builds one, and hands SANA-II
 * what it wants: ios2_PacketType from the driver command, ios2_DstAddr from
 * nx_ip_driver_physical_address_msw/lsw, and the payload from the prepend
 * pointer.
 *
 * A raw write builds a header, and it undoes that before releasing the packet,
 * because TCP hands the same NX_PACKET back for retransmission.
 *
 * Writes are queued, never DoIO: the sending thread is the IP thread or an
 * application thread inside nx_tcp_socket_send, and neither can block on the
 * wire. BeginIO rather than SendIO, for the reason at the post below.
 *
 * Reaping is a lifecycle problem. A mistake in it disables retransmission.
 *
 * nx_packet_transmit_release() does not free a queued TCP segment. It marks it
 * NX_DRIVER_TX_DONE (nx_packet_transmit_release.c), and
 * _nx_tcp_socket_retransmit() only resends a packet carrying that mark.
 * Every NetX Duo reference driver sets it inside NX_LINK_PACKET_SEND, because
 * their sends are synchronous. This one cannot: a SANA-II CMD_WRITE completes
 * long after the driver entry returns, so the release happens in
 * ami_sana2_tx_reap() instead.
 *
 * Calling that only from the transmit path means a packet is released by the
 * next packet. On a link that goes quiet, a lost HTTP GET, a lost TLS
 * ClientHello, any request/response protocol with a single request segment,
 * there is no next packet, so the segment stays un-reaped and TCP believes the
 * driver still has it. docs/RESEARCH.md 27.4 measured eleven seconds of
 * silence after one unacknowledged segment.
 *
 * Completions are therefore reaped when they complete, in two hops:
 *
 *   1. The reply port raises a signal on one of the SANA-II reader threads
 *      (ami_sana2_tx_reap_bind), the only thread in this shim that blocks in
 *      exec Wait() and can therefore be woken by a device.
 *   2. That thread does not touch the packet. It calls
 *      nx_ip_driver_deferred_processing(), NetX Duo's mechanism for a driver
 *      to request a callback on the IP thread, which comes back into
 *      ami_sana2_driver_entry() with NX_LINK_DEFERRED_PROCESSING and reaps.
 *
 * The second hop matters because releasing a packet mutates NetX Duo's
 * transmit queue and the packet's own prepend pointer (transmit_release strips
 * the IP header back off). From a reader thread that interleaves with whatever
 * the IP thread is doing. On the IP thread it runs where every other send
 * runs, under nx_ip_protection.
 *
 * The transmit path pays almost nothing. The reader only asks for deferred
 * processing when the reply port is non-empty, and during a bulk transfer the
 * next ami_sana2_tx_send() has already drained it. The common case is one
 * pointer compare in a thread that was going to wake anyway. The hop only
 * happens when the link goes quiet.
 *
 * The GetMsg() at the top of ami_sana2_tx_send() keeps the shim correct with
 * no reader bound at all: open-time probing, an interface not yet enabled, or
 * a reader that could not get a signal bit.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "aminetxduo/budget.h"

/* _nx_ip_driver_deferred_processing(): the supported way for a driver to ask
   for a callback on the IP thread. Declared in nx_api.h with no nx_ alias, so
   it is spelled with the underscore, as sana2_rx.c spells
   _nx_ip_packet_deferred_receive(). */
#include "nx_ip.h"

#ifdef AMINETXDUO_BPF
#include "aminetxduo/bpf.h"
#endif

#include <proto/exec.h>
/* BeginIO(): a macro over the device's own vector, no amiga.lib to link. */
#include <inline/alib.h>

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

        slot->hdr_len = 0;
        slot->pad_len = 0;

#ifdef AMINETXDUO_RXPROBE
        slot->write_at = 0UL;
#endif
    }
}

/*
 * Hand the reply port a task to signal, so a completion wakes a thread.
 *
 * The alternatives were a periodic sweep and a dedicated thread. A sweep adds
 * latency to every retransmission in exchange for work done when there is
 * none. A thread costs a context switch per frame on a machine where
 * docs/RESEARCH.md 16 budgets 8.0 ms per segment at 14 MHz. A signal costs the
 * reader one extra bit in a Wait() it was making anyway, and the sender one
 * Signal() inside exec's ReplyMsg, the same cost the receive path pays.
 *
 * The task is one of the SANA-II readers because it is the only thread in this
 * shim that blocks in exec rather than in ThreadX: the NX_IP thread waits on
 * ThreadX event flags, which Signal() cannot break. Which reader does not
 * matter, so it is the first one, see ami_sana2_rx_start().
 *
 * Disable() rather than Forbid(): a device can ReplyMsg from its interrupt,
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
 * Give it back. Must happen before the task exits or its signal bit is freed.
 * After this the port is inert and completions queue up for the next
 * ami_sana2_tx_reap(), which ami_sana2_tx_drain() runs at shutdown.
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
 * run the driver on the IP thread, where ami_sana2_tx_reap() can touch a
 * packet.
 *
 * The emptiness test is a single pointer compare and needs no Forbid(). It can
 * only be wrong in the safe direction: exec's PutMsg links the message and
 * raises the signal inside one Disable()d region, so a woken reader always
 * sees a non-empty list. A reader that sees an empty one was woken by a
 * completion somebody else has already collected, usually the transmit path,
 * and there is nothing left to defer.
 */
VOID ami_sana2_tx_defer(AmiSana2If *iface)
{
    struct List *list;

    if (iface == NULL || iface->ip == NULL)
        return;

    /* This NDK has no IsMsgPortEmpty(). An exec List is empty when its
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
 * nx_packet_transmit_release() does its own TX_DISABLE. This lets the reader
 * reap concurrently with a transmit in progress on the IP thread.
 */
VOID ami_sana2_tx_reap(AmiSana2If *iface)
{
    struct Message *msg;

    while ((msg = GetMsg(&iface->tx_port)) != NULL)
    {
        /* ios2_Req.io_Message is the first member of the first member of
           AmiTxSlot, so the reply message is the slot. */
        AmiTxSlot *slot = (AmiTxSlot *)msg;
        LONG       err  = (LONG)(BYTE)slot->req.ios2_Req.io_Error;

#ifdef AMINETXDUO_RXPROBE
        /*
         * The ack leg's closing stamp.  What it spans is the device's whole
         * transmit half as the stack experiences it: BeginIO to the moment
         * the reply is collected and the packet can be released, which is
         * what gates a queued TCP segment's retransmission.  A reply that
         * sat un-reaped across a quiet link exceeds the budget ceiling and
         * is discarded there rather than owning the maximum.
         */
        if (slot->write_at != 0UL)
        {
            ami_budget_ack(ami_budget_clock() - slot->write_at);
            slot->write_at = 0UL;
        }
#endif

        if (err != 0)
        {
            /*
             * A raw write this shim asked for on its own initiative, refused.
             * Latched, so the next frame of that type is cooked again and the
             * device is never asked twice. The frame itself is lost, which the
             * retransmission above it covers. iface->raw_mode is the operator
             * asking for raw framing outright and is not downgraded here.
             */
            if (!iface->raw_mode &&
                (slot->req.ios2_Req.io_Flags & SANA2IOF_RAW) != 0 &&
                !iface->raw_tx_refused)
            {
                iface->raw_tx_refused = TRUE;
                AMI_WARN("sana2: %s refuses raw writes. EtherType %lx goes "
                         "back to cooked framing",
                         iface->device,
                         (LONG)slot->req.ios2_PacketType);
            }

            iface->stats.tx_errors++;
            AMI_ERROR("sana2: CMD_WRITE failed err=%ld wire=%ld type=%lx "
                      "len=%ld dst=%lx%lx",
                      (LONG)err, (LONG)slot->req.ios2_WireError,
                      (LONG)slot->req.ios2_PacketType, (LONG)slot->total,
                      (LONG)(((ULONG)slot->req.ios2_DstAddr[0] << 8) |
                             slot->req.ios2_DstAddr[1]),
                      (LONG)(((ULONG)slot->req.ios2_DstAddr[2] << 24) |
                             ((ULONG)slot->req.ios2_DstAddr[3] << 16) |
                             ((ULONG)slot->req.ios2_DstAddr[4] << 8) |
                             slot->req.ios2_DstAddr[5]));
        }
        else
            iface->stats.packets_sent++;

        if (slot->packet != NULL)
        {
            /* Restore the packet to the shape NetX Duo handed over before
               releasing it. A queued TCP segment gets sent again. */
            if (slot->hdr_len != 0)
            {
                slot->packet->nx_packet_prepend_ptr += slot->hdr_len;
                slot->packet->nx_packet_length      -= slot->hdr_len;
                slot->hdr_len = 0;
            }

            if (slot->pad_len != 0)
            {
                slot->packet->nx_packet_append_ptr -= slot->pad_len;
                slot->packet->nx_packet_length     -= slot->pad_len;
                slot->pad_len = 0;
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

/*
 * Abort anything still in flight and reap it. Used on shutdown.
 *
 * The deadline is the same admission ami_sana2_rx_teardown() makes: AbortIO()
 * is a request, and a driver can decline it. slot->req and its mn_ReplyPort
 * both live inside AmiSana2If, so a device still holding a CMD_WRITE writes
 * into this allocation whenever it finishes. tx_orphaned stops
 * ami_sana2_close() from freeing it, exactly as rx_orphaned does.
 */
VOID ami_sana2_tx_drain(AmiSana2If *iface)
{
    UWORD i;
    UWORD spins;
    UWORD busy = 0;

    for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
    {
        if (iface->tx[i].busy)
            AbortIO((struct IORequest *)&iface->tx[i].req);
    }

    spins = 0;
    for (;;)
    {
        ami_sana2_tx_reap(iface);

        busy = 0;
        for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
        {
            if (iface->tx[i].busy)
                busy++;
        }

        if (busy == 0 || spins >= 64)
            break;

        spins++;
        tx_thread_sleep(1);
    }

    /* Assigned, not or'ed: a later drain that gets everything back clears it,
       which is what lets an interface bounce recover. */
    iface->tx_orphaned = (busy != 0) ? TRUE : FALSE;

    if (busy != 0)
    {
        AMI_ERROR("sana2: %ld write(s) still owned by the device. The "
                  "interface leaks. A free here corrupts memory the "
                  "device writes into",
                  (long)busy);
    }
}

/*
 * Ethernet's minimum frame, and which layer supplies it.
 *
 * A frame shorter than 60 bytes before the FCS is a runt and the wire cannot
 * carry one. Nothing in the SANA-II spec says whether the stack or the driver
 * pads, and the drivers disagree: ariadne_ii zero-fills a short CMD_WRITE,
 * a2065 raises its LANCE descriptor to 64 without supplying the bytes, and
 * ariadne 1.50 does neither, handing the chip exactly what it was given and
 * relying on the Am79C960's own APAD_XMT to fill the rest.
 *
 * Eight frames a boot are short enough to need this, all of them 28-byte ARP,
 * so all three of those answers are in use today and none of them belongs to
 * this stack. Each is a driver's private decision about a frame built here:
 * the one that supplies no bytes transmits the tail of its own ring buffer,
 * and the one that defers to APAD_XMT is one CSR4 bit away from a transmitter
 * that shuts down on the first ARP, because an Am79C960 with that bit clear
 * answers a short buffer with TX_BUFF|TX_UFLO and drops CSR0_TXON until the
 * chip is reinitialised.
 *
 * The minimum is not 60 on both arms. Cooked mode hands SANA-II the IP
 * datagram and the driver builds the 14-byte Ethernet header itself, so
 * nx_packet_length is the payload and the minimum is 46. Raw mode has already
 * prepended those 14 bytes above, so nx_packet_length is the whole frame and
 * the minimum is 60. One expression covers both: what goes on the wire is the
 * packet plus the header the driver adds, and the pad is 60 less that.
 *
 * The zeroes are written into the packet. A raise of ios2_DataLength on its
 * own is shorter, and it puts whatever the pool block held last on the wire.
 * That is a2065.device's own descriptor clamp: it lifts the byte count to 64
 * and copies nothing more, so the tail of its ring buffer is transmitted.
 * Padding here keeps that clamp from engaging on a frame from this stack, bar
 * the four bytes between 60 and its 64.
 *
 * Both callers of ami_sana2_tx_send() come through here: the driver entry's
 * NX_LINK_PACKET_SEND, PACKET_BROADCAST, ARP_SEND, ARP_RESPONSE_SEND and
 * RARP_SEND (sana2_driver.c), and ami_sana2_inject() for bpf_write().
 *
 * ami_sana2_tx_reap() takes the zeroes back off before releasing the packet,
 * exactly as it does the raw-mode header, because a queued TCP segment is
 * handed back for retransmission and must carry its own length.
 */
static VOID ami_sana2_tx_pad(AmiSana2If *iface, AmiTxSlot *slot,
                             NX_PACKET *packet)
{
    ULONG on_wire;
    ULONG pad;
    ULONG room;
    ULONG i;

    slot->pad_len = 0;

    if (iface->hw_type != S2WireType_Ethernet)
        return;

#ifndef NX_DISABLE_PACKET_CHAIN
    /* A chain is never this short, and the zeroes belong on its tail link
       rather than this one. */
    if (packet->nx_packet_next != NX_NULL)
        return;
#endif

    on_wire = packet->nx_packet_length +
              (slot->hdr_len != 0 ? 0UL : (ULONG)AMI_ETH_HEADER_SIZE);

    if (on_wire >= (ULONG)AMI_ETH_MIN_FRAME)
        return;

    pad  = (ULONG)AMI_ETH_MIN_FRAME - on_wire;
    room = (ULONG)(packet->nx_packet_data_end - packet->nx_packet_append_ptr);

    /* A pool block holds far more than 60 bytes and the frames this fires on
       are shorter than that, so the room is always there. If it ever is not,
       the frame goes out as a runt rather than writing past the packet. */
    if (room < pad)
        return;

    for (i = 0; i < pad; i++)
        packet->nx_packet_append_ptr[i] = 0;

    packet->nx_packet_append_ptr += pad;
    packet->nx_packet_length     += pad;
    slot->pad_len                 = (UWORD)pad;
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
 * this returns. ami_sana2_tx_send() releases the packet when it does.
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
    BOOL       raw_write;

    if (iface == NULL || packet == NULL)
        return NX_PTR_ERROR;

    /* Cheap when the reader has already emptied the port, and the only reaping
       that happens when no reader is bound. */
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

    /*
     * ariadne.device 1.50 builds the cooked Ethernet header with
     *
     *     move.w  38(a2),d0        ; low word of ios2_PacketType
     *     cmpi.w  #1500,d0
     *     ble.s   as_802_3         ; SIGNED
     *
     * so every type with bit 15 set reads as negative, takes the IEEE 802.3
     * arm, and gets ios2_DataLength written into the type field instead of the
     * type. 0x0800 and 0x0806 are positive and come out right, which is why
     * IPv4 works and IPv6 leaves as a length-framed frame no router answers:
     * no neighbour solicitation, no router solicitation, no global address, on
     * a card whose IPv4 works.
     *
     * Its raw arm is correct -- it tests io_Flags bit 7 and transmits the
     * buffer verbatim -- so the header goes on here for the types the cooked
     * arm cannot carry. Nothing below 0x8000 changes, which leaves IPv4, ARP
     * and RARP on the path they have always taken. A device that refuses
     * SANA2IOF_RAW says so on the write and ami_sana2_tx_reap() latches
     * raw_tx_refused, and everything after it goes back to cooked.
     */
    raw_write = iface->raw_mode;

    if (!raw_write
        && (ether_type & 0x8000U) != 0
        && iface->hw_type == S2WireType_Ethernet
        && iface->addr_bytes == AMI_ETH_ADDR_SIZE
        && !iface->raw_tx_refused)
        raw_write = TRUE;

    if (raw_write)
    {
        UCHAR *eth;

        /*
         * The deferred transmit checksum, before the link header goes on.
         *
         * Everything downstream that reads this packet as an IP datagram
         * starts at nx_packet_prepend_ptr: sana2_copy.c's fusion, and
         * _nx_ip_packet_checksum_compute() behind it. With fourteen bytes of
         * Ethernet in front of that, neither finds a datagram. Measured on the
         * wire: every IPv6 SYN left with cksum 0x0000 and nothing answered
         * one, while echo replies kept coming back, because ICMPv6 is
         * checksummed by the stack and TCP is the only thing deferred here.
         *
         * The cost is a pass over the segment that the fusion folds into the
         * copy, and it is paid on IPv6 TCP only. Nothing else reaches this
         * arm.
         */
        if ((packet->nx_packet_interface_capability_flag &
             NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) != 0)
            _nx_ip_packet_checksum_compute(packet);  /* clears the flag */

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

    /* After the raw block, so any header it prepended is already in
       nx_packet_length, and before the tap, so a capture shows the frame the
       wire sees. */
    ami_sana2_tx_pad(iface, slot, packet);

#ifdef AMINETXDUO_BPF
    /*
     * The transmit tap, after the raw block above so one call site serves both
     * modes.  `slot->hdr_len` doubles as `has_link_header`: the 14 bytes are
     * now in the packet, or they exist nowhere and the tap synthesises them
     * from the three facts the CMD_WRITE below carries.  Nothing is written
     * into the packet, which is often a queued TCP segment that is handed back
     * for retransmission.
     */
    ami_bpf_tap_tx(iface, packet, slot->hdr_len != 0, ether_type,
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
    slot->req.ios2_Req.io_Flags   = raw_write ? SANA2IOF_RAW : 0;
    slot->req.ios2_Req.io_Error   = 0;
    slot->req.ios2_WireError      = 0;
    slot->req.ios2_PacketType     = (ULONG)ether_type;
    slot->req.ios2_DataLength     = length;
    slot->req.ios2_Data           = slot;

    /*
     * Broadcast and multicast both go out as CMD_WRITE with the destination
     * address the IP layer chose. S2_BROADCAST/S2_MULTICAST exist, but the
     * spec warns they are "not supported by all networks and/or network
     * interfaces", and on Ethernet they add nothing over an ff:ff:... write,
     * which is also the only way ARP requests can go out since NetX Duo sends
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

        /* Addressless wires have no destination address field. */
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++)
            slot->req.ios2_DstAddr[i] = 0;
    }

    /*
     * BeginIO() and not SendIO(), because SendIO() is
     *
     *     ln_Type = NT_MESSAGE; io_Flags = 0; BeginIO()
     *
     * and that zero takes SANA2IOF_RAW with it. A raw write posted through
     * SendIO() arrives at the device as a cooked one, the device builds a
     * header in front of the header this shim just built, and the frame goes
     * out with fourteen bytes of Ethernet inside its own payload. Both lines
     * SendIO() runs are above. IOF_QUICK stays clear, so the request is queued
     * and replied to iface->tx_port exactly as before.
     */
#ifdef AMINETXDUO_RXPROBE
    /* The ack leg's opening stamp, as late as it can be taken: everything
       above is the shim's own framing cost, and the leg is the device's. */
    slot->write_at = ami_budget_clock();
#endif

    BeginIO((struct IORequest *)&slot->req);

    return NX_SUCCESS;
}
