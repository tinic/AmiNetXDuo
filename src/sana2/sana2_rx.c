/*
 * AmiNetXDuo -- SANA-II receive pipeline.
 *
 * SANA-II devices have no buffers of their own: a frame that arrives with no
 * matching CMD_READ outstanding is dropped, and CMD_READ is per packet type.
 * So the shim runs one reader thread per type -- 0x0800, 0x0806 and, when
 * AMINETXDUO_IPV6 is defined, 0x86DD -- each keeping several reads in flight
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
 * One of these threads also Notices transmit completions, which is not where
 * anyone would look for it. The reason is that they are the only threads in
 * the shim that block in exec Wait() rather than on a ThreadX object, so they
 * are the only ones a device's ReplyMsg can wake -- and without a thread that
 * wakes on a completion, TCP never learns that the driver has finished with a
 * segment and never retransmits it. The reader does not touch the packet: it
 * asks NetX Duo for deferred processing and the IP thread does the work.
 * sana2_tx.c carries the full account; the mechanism here is one extra signal
 * bit in a Wait() the reader was making anyway.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

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

/* ------------------------------------------------------------- delivery */

/*
 * Hand one Ethernet-shaped packet to NetX Duo. Both the cooked path (header
 * synthesised above) and the raw path (header came off the wire) end up here,
 * which is what keeps the two modes honest about producing the same thing.
 *
 * The deferred entry points are used rather than _nx_ip_packet_receive: the
 * reader is not the IP thread, and the deferred variants are the sanctioned
 * way to queue work onto it (see nx_api.h and nx_ram_network_driver.c, which
 * dispatches by EtherType exactly like this).
 */
VOID ami_sana2_rx_deliver(AmiSana2If *iface, NX_PACKET *packet)
{
    UINT type;

#ifdef AMINETXDUO_BPF
    /*
     * The receive tap, at the convergence of the cooked and raw paths and
     * BEFORE the link header is stripped below -- so both modes are covered by
     * the one call and the frame is a complete link-layer frame in one
     * contiguous run.  A no-op (one load, one compare) when nothing is
     * capturing on this interface.
     */
    ami_bpf_tap_rx(iface, packet->nx_packet_prepend_ptr,
                   packet->nx_packet_length);
#endif

    if (packet->nx_packet_length < AMI_ETH_HEADER_SIZE)
    {
        nx_packet_release(packet);
        iface->stats.rx_errors++;
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

/* Position the packet and work out where the copy hook must write. */
static VOID ami_sana2_rx_arm(AmiSana2If *iface, AmiRxSlot *slot)
{
    NX_PACKET *packet = slot->packet;
    UCHAR     *base   = packet->nx_packet_data_start + AMI_SANA2_RX_PAD;

    packet->nx_packet_prepend_ptr = base;
    packet->nx_packet_append_ptr  = base;
    packet->nx_packet_length      = 0;

    /* Cooked: leave room for the header we are about to invent. Raw: the
       device supplies it. */
    slot->dst = iface->raw_mode ? base : (base + AMI_ETH_HEADER_SIZE);

    slot->capacity = (ULONG)(packet->nx_packet_data_end - slot->dst);
    slot->copied   = 0;
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
        live++;
    }

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

    /* ios2_DataLength is the documented answer, but fall back to what the
       copy hook actually took for devices that only fill one of the two. */
    if (length == 0)
        length = slot->copied;

    if (length == 0 || length > slot->capacity)
    {
        /* Keep the packet: rearming is cheaper than a pool round trip. */
        iface->stats.rx_errors++;
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

            /* No address field on this wire -- keep the shape, zero the bytes. */
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
    ami_sana2_rx_deliver(iface, packet);
}

static VOID ami_sana2_rx_drain(AmiSana2Rx *rx)
{
    struct Message *msg;

    while ((msg = GetMsg(rx->port)) != NULL)
    {
        /* The reply message is the slot -- ios2_Req.io_Message is its first
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
             * driver whose card has gone away underneath us -- a pulled cable
             * on a2065.device is the case that matters.
             *
             * Stopping the reader is not enough on its own. NetX Duo learns
             * the link state ONLY from nx_interface_link_up, which the driver
             * entry point sets on NX_LINK_ENABLE/DISABLE -- that is, only
             * when the stack asks. Nothing asks when the wire is pulled, so
             * without this the interface stayed marked up: ShowNetStatus
             * reported LINKUP, ARP kept queueing, and every send failed in
             * the shim with no way for the layer above to know why. Reporting
             * an interface as working when it is not is worse than reporting
             * it as down, because it is the one state a user cannot diagnose.
             *
             * `Online` recovers it: netstack.c:1393 issues NX_LINK_ENABLE,
             * whose driver case sets nx_interface_link_up back to NX_TRUE.
             * There is deliberately no automatic retry here -- this runs on
             * the reader, and a driver that has gone out of service is not
             * one to hammer with S2_ONLINE from inside a receive loop.
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
        }
    }
}

/* --------------------------------------------------------------- shutdown */

/*
 * CMD_FLUSH: "abort and return all queued I/O requests for this unit."
 *
 * Unit-wide rather than per-request, so it is the second thing tried and not
 * the first -- it takes the other reader's queued reads with it, which costs
 * a few frames during a shutdown that was going to lose them anyway. It is
 * the command exec defines for precisely the case where AbortIO() is a no-op,
 * and several SANA-II drivers implement it when they do not implement abort.
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
 * 40 ms, and answer how many requests it still owns.
 *
 *   It used to be, and WaitIO() has no deadline. Commodore's a2065.device
 *   2.16 does not honour AbortIO() on a queued CMD_READ -- the top of
 *   CMakeLists.txt records the same discovery from the other end of the
 *   lifecycle, where the raw-framing probe posted a read, aborted it, and hung
 *   ami_sana2_open() for ever -- so the abort above is a request, not a
 *   guarantee, and a reader that trusted it never came back. That is what put
 *   the two `reader did not stop` warnings in every shutdown, and ten of the
 *   sixteen seconds a lone `curl --version` was costing.
 *
 *   GetMsg() does not block, so no ami_sana2_block_enter() bracket is needed
 *   here; tx_thread_sleep() must be outside one, which is the other half of
 *   why the loop is shaped this way.
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
            ((AmiRxSlot *)msg)->posted = FALSE;

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
 * Return every outstanding read, or say how many are still gone.
 *
 * Three steps, most polite first, because no one of them works on every
 * driver:
 *
 *   1. AbortIO() on each. Correct, cheap, and what a well-behaved device
 *      answers immediately; a2065.device 2.16 ignores it.
 *   2. CMD_FLUSH, which exec defines as "abort all queued requests for this
 *      unit" and which SANA-II carries forward. Unit-wide rather than
 *      per-request, which is why it is second.
 *   3. Give up and SAY SO, having freed nothing the device can still write
 *      into. That is the part that matters: the old code freed the reply
 *      port, released the pinned packets and let ami_sana2_close() free the
 *      whole interface while the device still held pointers into all three.
 *      On a machine with no memory protection that is not a leak, it is a
 *      corruption waiting for the next frame to arrive.
 *
 * In practice none of this fires any more, because ami_sana2_rx_stop() now
 * takes the wire offline BEFORE stopping the readers and S2_OFFLINE returns
 * every queued read by itself. This is the belt to that pair of braces.
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
        /* Nothing below this line may run: every one of those is a pointer
           the device still holds. */
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
     * Tx reaping duty. One reader takes it, and this is where the transmit
     * ring finally acquires a context that runs when nothing is being sent --
     * see the header of sana2_tx.c for what the absence of one cost.
     *
     * A failure here is not fatal: the interface falls back to reaping on the
     * next transmit, which is exactly the behaviour that made a lone
     * unacknowledged segment unrecoverable, so it is worth a warning.
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
     * Every request is a copy of the opened one: that is what carries
     * io_Device, io_Unit and the device's own ios2_BufferManagement cookie.
     * Only the reply port and the command differ.
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

    rx->running = TRUE;
    tx_semaphore_put(&rx->ready);

    while (!rx->stop)
    {
        /*
         * At the top of the loop rather than after the Wait(), because the
         * pool-empty path below continues without ever reaching a drain --
         * and releasing finished writes is one of the things that puts
         * packets back in the pool it is waiting for.
         *
         * This does not reap. It asks the IP thread to; see sana2_tx.c.
         */
        if (rx->reap_mask != 0)
            ami_sana2_tx_defer(iface);

        if (ami_sana2_rx_post(rx) == 0)
        {
            /* Either the pool is empty or the interface is down. Back off
               rather than spin; ami_sana2_rx_stop() signals us out of this. */
            tx_thread_sleep(2);
            continue;
        }

        ami_sana2_block_enter();
        Wait(rx->wake_mask | rx->reap_mask);
        ami_sana2_block_leave();

        ami_sana2_rx_drain(rx);
    }

    /*
     * Hand the reply port back BEFORE anything else in the teardown: after
     * this returns, no completion can signal this task, which is what makes
     * freeing the signal bit -- and, shortly, the Task -- safe.
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
 * How deep the IPv4 read queue should be on THIS machine.
 *
 * Every frame that arrives with no CMD_READ outstanding is discarded by the
 * device, so this number is the receive window measured in frames, and the
 * thing that overruns it is a burst: sixteen TCP connections opening at once
 * answer with sixteen SYN/ACKs inside a few hundred microseconds, and a
 * 14 MHz 68020 cannot re-post a read between them.  A fixed four lost six of
 * those sixteen -- see the note in sana2_internal.h for the measurements.
 *
 * It is sized from the packet pool rather than fixed because each outstanding
 * read pins a packet for its whole life, and the pool is already sized from
 * AvailMem().  A machine with four megabytes gets the floor and no more; one
 * with eight gets the ceiling.  Taking a fixed share means the answer moves
 * with the memory rather than with a constant somebody has to remember to
 * revisit.
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

LONG ami_sana2_rx_start(AmiSana2If *iface)
{
    UWORD i;
    UWORD ipv4_depth;

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

        /* The first reader carries the TX reaping duty. It is the IPv4 one,
           which is the reader that always exists, but nothing depends on
           which: any thread that blocks in exec Wait() will do. */
        rx->reap_tx     = (i == 0) ? TRUE : FALSE;

        if (rx->depth > AMI_SANA2_RX_MAX_DEPTH)
            rx->depth = AMI_SANA2_RX_MAX_DEPTH;

        rx->stack = ami_alloc_flags(AMI_SANA2_RX_STACK_SIZE, MEMF_PUBLIC);
        if (rx->stack == NULL)
        {
            AMI_ERROR("sana2: no memory for reader stack");
            ami_sana2_rx_stop(iface);
            return -1;
        }

        if (tx_semaphore_create(&rx->ready, (CHAR *)"s2rxrdy", 0) != TX_SUCCESS ||
            tx_semaphore_create(&rx->exited, (CHAR *)"s2rxend", 0) != TX_SUCCESS)
        {
            AMI_ERROR("sana2: cannot create reader semaphores");
            ami_sana2_rx_stop(iface);
            return -1;
        }

        if (tx_thread_create(&rx->thread, (CHAR *)ami_sana2_rx_names[i],
                             ami_sana2_rx_thread, (ULONG)rx,
                             rx->stack, AMI_SANA2_RX_STACK_SIZE,
                             AMI_SANA2_RX_PRIORITY, AMI_SANA2_RX_PRIORITY,
                             TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
        {
            AMI_ERROR("sana2: cannot create reader thread");
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

    /*
     * OFFLINE FIRST, And the order is the whole fix.
     *
     * S2_OFFLINE returns every queued CMD_READ with S2ERR_OUTOFSERVICE --
     * ami_sana2_rx_drain() has said so in a comment since the day it was
     * written -- and it is the only mechanism that works on a device which
     * ignores AbortIO(), which Commodore's a2065.device 2.16 does.
     *
     * Every caller took the interface offline ALREADY, and every one of them
     * did it AFTER stopping the readers: ami_sana2_close(), NX_LINK_DISABLE
     * and NX_LINK_UNINITIALIZE all read `rx_stop(); tx_drain(); offline();`.
     * So the one command that would have freed the readers was issued ten
     * seconds after they had given up waiting, and the readers were torn down
     * -- threads terminated, stacks freed -- with reads still queued.
     *
     * Measured, A1200 profile: `curl --version` took 16.22 s when nothing
     * else held bsdsocket.library open and 0.32 s when AddNetInterface did.
     * The difference was two five-second `reader did not stop` timeouts, one
     * per reader, on every last close.
     *
     * Doing it here rather than at each call site means the next caller
     * cannot get it wrong. ami_sana2_offline() is idempotent, so the
     * offline() the callers still do afterwards costs nothing.
     *
     * Tell the readers to stop before taking the wire offline, not after.
     *
     * This used to go offline first and set `stop` in the loop below, which
     * left a window: between the two, a reader is still running and still
     * posting fresh CMD_READs -- onto a device that is now offline, so they
     * are never returned, and the drain that follows finds them outstanding,
     * times out after five seconds and orphans the reader.
     *
     * The window is widest exactly where it was found: an Offline() issued
     * within ~20 ms of link-up, when the reader is filling all of its slots
     * at once. docs/RESEARCH.md 56 records it as the reason a DHCP test had
     * to wait 200 ms before taking the wire away, and it is reachable by a
     * user typing Offline while the interface is still coming up.
     *
     * Three phases now, and each needs the one before it:
     *   1. stop posting  -- `stop` seen at the top of the reader's loop
     *   2. offline       -- S2_OFFLINE returns every read still queued
     *   3. join          -- every reader is now guaranteed to reach its exit
     */
    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        if (!rx->started)
            continue;

        rx->stop = TRUE;

        /*
         * Kick the reader out of Wait(). Signalling a bit that the reader has
         * already freed with its MsgPort is harmless -- the Task itself stays
         * alive until it has put the "exited" semaphore.
         */
        if (rx->task != NULL && rx->wake_mask != 0)
            Signal(rx->task, rx->wake_mask);
    }

    (VOID)ami_sana2_offline(iface);

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        if (!rx->started)
            continue;

        if (tx_semaphore_get(&rx->exited, 5 * NX_IP_PERIODIC_RATE) != TX_SUCCESS)
        {
            /*
             * The reader is still somewhere inside its own teardown, which
             * now has a deadline of its own, so this should not happen. If it
             * does, the thread is running on `rx->stack` and the ThreadX
             * control block is live -- so neither may be freed. Say so and
             * leave them: an interface that leaks 32 KB is recoverable and a
             * thread executing freed memory is not.
             */
            AMI_ERROR("sana2: reader %ld did not stop; leaking its stack "
                      "rather than freeing memory it is running on", (long)i);
            iface->rx_orphaned = TRUE;
            continue;
        }

        /*
         * Give the thread time to run off the end of its entry function before
         * the control block and stack go away. This is the one place the shim
         * leans on port behaviour it cannot yet verify (docs/RESEARCH.md §6.2).
         */
        tx_thread_sleep(5);

        tx_thread_terminate(&rx->thread);
        tx_thread_delete(&rx->thread);
        tx_semaphore_delete(&rx->ready);
        tx_semaphore_delete(&rx->exited);

        /* The reader exited, but it may have exited leaving reads the device
           would not give back. Its slots, its packets and its reply port are
           inside this interface, so the interface itself cannot be freed. */
        if (rx->orphans != 0)
            iface->rx_orphaned = TRUE;

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
