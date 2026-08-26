/*
 * AmiNetXDuo, SANA-II transmit path.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "aminetxduo/budget.h"

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

#ifdef AMINETXDUO_TX_LAZY_COLLECT
    iface->tx_lazy_timer_up  = FALSE;
    iface->tx_lazy_parked    = FALSE;
    iface->tx_lazy_last_send = 0UL;
#endif
}

/*
 * The signalled task must be a SANA-II reader: the only thread here that
 * blocks in exec Wait() rather than on ThreadX event flags, which Signal()
 * cannot break.  Disable(), not Forbid(): a device may ReplyMsg from interrupt.
 */
VOID ami_sana2_tx_reap_bind(AmiSana2If *iface, struct Task *task, BYTE sigbit)
{
    if (iface == NULL || task == NULL || sigbit < 0)
        return;

    Disable();
    iface->tx_port.mp_SigTask = task;
    iface->tx_port.mp_SigBit  = (UBYTE)sigbit;
    iface->tx_port.mp_Flags   = PA_SIGNAL;
#ifdef AMINETXDUO_TX_LAZY_COLLECT
    /* The port is armed, whatever parking a previous reader's tenure left. */
    iface->tx_lazy_parked = FALSE;
#endif
    Enable();
}

/*
 * Must happen before the task exits or its signal bit is freed.  After this the
 * port is inert and completions queue for the next ami_sana2_tx_reap().
 */
VOID ami_sana2_tx_reap_unbind(AmiSana2If *iface)
{
    if (iface == NULL)
        return;

    Disable();
    iface->tx_port.mp_Flags   = PA_IGNORE;
    iface->tx_port.mp_SigTask = NULL;
    iface->tx_port.mp_SigBit  = 0;
#ifdef AMINETXDUO_TX_LAZY_COLLECT
    /* PA_IGNORE now means "no reader", not "parked": the lazy tick must not
       hand this task-less port a PA_SIGNAL back. */
    iface->tx_lazy_parked = FALSE;
#endif
    Enable();
}

/*
 * The emptiness test needs no Forbid(): PutMsg links the message and raises the
 * signal inside one Disable()d region, so it can only be wrong in the safe
 * direction.
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

#ifdef AMINETXDUO_TX_LAZY_COLLECT
/*
 * Lazy collection: ami_sana2_tx_send() parks the reply port PA_IGNORE while
 * sends are flowing, so this one-tick timer is what collects a lone completion
 * on a quiet link and hands PA_SIGNAL back once the sends stop.
 */
static VOID ami_sana2_tx_lazy_tick(ULONG argument)
{
    AmiSana2If *iface = (AmiSana2If *)argument;

    if (iface->tx_lazy_parked &&
        (tx_time_get() - iface->tx_lazy_last_send) > 1UL)
    {
        Disable();
        if (iface->tx_port.mp_SigTask != NULL)
            iface->tx_port.mp_Flags = PA_SIGNAL;
        iface->tx_lazy_parked = FALSE;
        Enable();
    }

    /* Restoring PA_SIGNAL raises nothing for a reply already queued, so the
       collection below is not conditional on the parking above. */
    ami_sana2_tx_defer(iface);
}

VOID ami_sana2_tx_lazy_start(AmiSana2If *iface)
{
    if (iface == NULL || iface->tx_lazy_timer_up)
        return;

    iface->tx_lazy_parked    = FALSE;
    iface->tx_lazy_last_send = 0UL;

    if (tx_timer_create(&iface->tx_lazy_timer, (CHAR *)"anxd tx lazy",
                        ami_sana2_tx_lazy_tick, (ULONG)iface,
                        1UL, 1UL, TX_AUTO_ACTIVATE) == TX_SUCCESS)
    {
        iface->tx_lazy_timer_up = TRUE;
    }
    else
    {
        AMI_WARN("sana2: no lazy-collect tick. Completions signal per "
                 "write, the shipped design");
    }
}

VOID ami_sana2_tx_lazy_stop(AmiSana2If *iface)
{
    if (iface == NULL || !iface->tx_lazy_timer_up)
        return;

    tx_timer_deactivate(&iface->tx_lazy_timer);
    tx_timer_delete(&iface->tx_lazy_timer);
    iface->tx_lazy_timer_up = FALSE;

    /* Hand the port back to the shipped arrangement: signalling whenever a
       reader is bound. Without the tick no parking is safe. */
    Disable();
    if (iface->tx_port.mp_SigTask != NULL)
        iface->tx_port.mp_Flags = PA_SIGNAL;
    iface->tx_lazy_parked = FALSE;
    Enable();
}
#endif /* AMINETXDUO_TX_LAZY_COLLECT */

/*
 * Non-blocking by construction: GetMsg() on an empty port returns NULL.
 * Callable from any thread and from several at once: GetMsg() is atomic and
 * nx_packet_transmit_release() does its own TX_DISABLE.
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
        if (slot->write_at != 0UL)
        {
            ami_budget_ack(ami_budget_clock() - slot->write_at);
            slot->write_at = 0UL;
        }
#endif

        if (err != 0)
        {
            /*
             * A raw write this shim asked for on its own initiative, refused:
             * latch it so the next frame of that type is cooked.  raw_mode is
             * the operator asking for raw outright and is not downgraded here.
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
 * Abort anything still in flight and reap it.  AbortIO() is a request a driver
 * may decline; slot->req and its reply port live inside AmiSana2If, so
 * tx_orphaned stops ami_sana2_close() from freeing it.
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
 * Ethernet's 60-byte minimum frame.  Cooked mode's nx_packet_length is the
 * payload, so the minimum is 46 and the driver adds 14; raw mode's is the whole
 * frame.  ami_sana2_tx_reap() takes the zeroes back off before releasing.
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
     * Reachable from the IP thread and from any thread inside
     * nx_tcp_socket_send.  Forbid() rather than Disable(): long Disable()
     * regions break serial, floppy and audio.
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
 * bpf_write()'s wire end.  The payload is copied into a pool packet because the
 * caller's buffer is an application buffer and CMD_WRITE outlives this call.
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
#ifdef AMINETXDUO_RXPROBE
    ULONG probe_t0 = ami_budget_clock();
    ULONG probe_t1;
    ULONG probe_t2;
#endif

    if (iface == NULL || packet == NULL)
        return NX_PTR_ERROR;

    /* Before the reap walk: xmit is the stack's cost of emitting this frame,
       and reap is already its own leg. */
    ami_budget_xmit(probe_t0);

    /* Cheap when the reader has already emptied the port, and the only reaping
       that happens when no reader is bound. */
    ami_sana2_tx_reap(iface);

#ifdef AMINETXDUO_RXPROBE
    probe_t1 = ami_budget_clock();
#endif

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
     * ariadne.device 1.50 compares ios2_PacketType against 1500 SIGNED, so any
     * type with bit 15 set takes its 802.3 arm and gets a length written into
     * the type field.  Those go out raw instead; below 0x8000 nothing changes.
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
         * The deferred transmit checksum must be computed BEFORE the link
         * header goes on: everything downstream reads the datagram at
         * nx_packet_prepend_ptr and finds none behind 14 bytes of Ethernet.
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
     * `slot->hdr_len` doubles as `has_link_header`: either the 14 bytes are in
     * the packet or the tap synthesises them.  Nothing is written into the
     * packet, which is often a segment handed back for retransmission.
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
     * Broadcast and multicast go out as CMD_WRITE with the address the IP layer
     * chose: the spec warns S2_BROADCAST/S2_MULTICAST are "not supported by all
     * networks and/or network interfaces".
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

#ifdef AMINETXDUO_TX_LAZY_COLLECT
    /*
     * Park before BeginIO: a device is free to complete the write synchronously
     * inside it, and that completion is the one this send makes silent.  Only
     * with the tick live, and only over a PA_SIGNAL port.
     */
    iface->tx_lazy_last_send = tx_time_get();
    if (iface->tx_lazy_timer_up && !iface->tx_lazy_parked)
    {
        Disable();
        if (iface->tx_port.mp_SigTask != NULL &&
            iface->tx_port.mp_Flags == PA_SIGNAL)
        {
            iface->tx_port.mp_Flags = PA_IGNORE;
            iface->tx_lazy_parked   = TRUE;
        }
        Enable();
    }
#endif

    /*
     * BeginIO() and not SendIO(): SendIO() zeroes io_Flags, which takes
     * SANA2IOF_RAW with it, and the device would then build a second Ethernet
     * header in front of this one.  IOF_QUICK stays clear.
     */
#ifdef AMINETXDUO_RXPROBE
    probe_t2       = ami_budget_clock();
    slot->write_at = probe_t2;
#endif

    BeginIO((struct IORequest *)&slot->req);

#ifdef AMINETXDUO_RXPROBE
    ami_budget_reap(probe_t1 - probe_t0);
    ami_budget_stuff(probe_t2 - probe_t1);
    ami_budget_post(ami_budget_clock() - probe_t2);
#endif

    return NX_SUCCESS;
}
