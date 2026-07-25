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
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"

#include "nx_ip.h"
#ifndef NX_DISABLE_IPV4
#include "nx_arp.h"
#include "nx_rarp.h"
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
            /* S2_OFFLINE returns every pending read this way. Stop posting
               until the interface comes back. */
            rx->iface->online = FALSE;
        }
        else
        {
            rx->iface->stats.rx_errors++;
        }
    }
}

/* --------------------------------------------------------------- shutdown */

static VOID ami_sana2_rx_teardown(AmiSana2Rx *rx)
{
    UWORD i;

    for (i = 0; i < rx->depth; i++)
    {
        if (rx->slot[i].posted)
            AbortIO((struct IORequest *)&rx->slot[i].req);
    }

    /* WaitIO() blocks in exec Wait(); nothing inside this bracket may touch
       ThreadX or NetX Duo state. */
    ami_sana2_block_enter();
    for (i = 0; i < rx->depth; i++)
    {
        if (rx->slot[i].posted)
        {
            WaitIO((struct IORequest *)&rx->slot[i].req);
            rx->slot[i].posted = FALSE;
        }
    }
    ami_sana2_block_leave();

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
        if (ami_sana2_rx_post(rx) == 0)
        {
            /* Either the pool is empty or the interface is down. Back off
               rather than spin; ami_sana2_rx_stop() signals us out of this. */
            tx_thread_sleep(2);
            continue;
        }

        ami_sana2_block_enter();
        Wait(rx->wake_mask);
        ami_sana2_block_leave();

        ami_sana2_rx_drain(rx);
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

LONG ami_sana2_rx_start(AmiSana2If *iface)
{
    UWORD i;

    if (iface->rx_running)
        return 0;

    if (iface->pool == NULL || iface->ip == NULL)
        return -1;

    for (i = 0; i < AMI_SANA2_RX_READERS; i++)
    {
        AmiSana2Rx *rx = &iface->rx[i];

        rx->iface       = iface;
        rx->packet_type = ami_sana2_rx_types[i];
        rx->depth       = ami_sana2_rx_depths[i];
        rx->stop        = FALSE;
        rx->failed      = FALSE;
        rx->running     = FALSE;
        rx->started     = FALSE;

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

        if (tx_semaphore_get(&rx->exited, 5 * NX_IP_PERIODIC_RATE) != TX_SUCCESS)
            AMI_WARN("sana2: reader %ld did not stop", (long)i);

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
