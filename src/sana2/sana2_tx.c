/*
 * AmiNetXDuo, SANA-II transmit path.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sana2_internal.h"
#include "aminetxduo/nxstatus.h"
#include "aminetxduo/anxs2ext.h"

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

    /* WRITEREQUESTS was applied by ami_sana2_open(); an interface built any
       other way claims every slot. */
    if (iface->tx_slots == 0 || iface->tx_slots > (UWORD)AMI_SANA2_TX_SLOTS)
        iface->tx_slots = (UWORD)AMI_SANA2_TX_SLOTS;

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

    iface->tx_pend_head  = 0;
    iface->tx_pend_count = 0;
    iface->tx_kicking    = FALSE;

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
static VOID ami_sana2_tx_complete(AmiSana2If *iface, AmiTxSlot *slot);

VOID ami_sana2_tx_reap(AmiSana2If *iface)
{
    struct Message *msg;
    struct List     batch;
    struct Node    *node;
    struct Node    *next;

    /*
     * NOT A GetMsg() PER REPLY, AND NOT ONE AT ALL WHEN THERE IS NOTHING.
     * This runs at the head of every send, and GetMsg() is a Disable() pair
     * around one unlink -- on Emu68 a 5.5 us trap, paid even for an empty
     * port: 8% of a transmit profile sat here.  The emptiness test is one
     * load with no lock (a reply landing after it is picked up by the next
     * send, or by the reader); when there is something, the whole list is
     * spliced off under one Disable() and walked from here, the shape
     * ami_sana2_rx_drain() uses for the read replies.
     */
    if (iface->tx_port.mp_MsgList.lh_Head->ln_Succ == NULL)
        return;

    NewList(&batch);
    Disable();
    if (iface->tx_port.mp_MsgList.lh_Head->ln_Succ != NULL)
    {
        struct List *pl = &iface->tx_port.mp_MsgList;

        batch.lh_Head              = pl->lh_Head;
        batch.lh_TailPred          = pl->lh_TailPred;
        batch.lh_Head->ln_Pred     = (struct Node *)&batch.lh_Head;
        batch.lh_TailPred->ln_Succ = (struct Node *)&batch.lh_Tail;
        NewList(pl);
    }
    Enable();

    for (node = batch.lh_Head; (next = node->ln_Succ) != NULL; node = next)
    {
        msg = (struct Message *)node;
        /* ios2_Req.io_Message is the first member of the first member of
           AmiTxSlot, so the reply message is the slot. */
        ami_sana2_tx_complete(iface, (AmiTxSlot *)msg);
    }

    /* The slots just handed back are what the queued writes were waiting
       for. */
    if (iface->tx_pend_count != 0)
        ami_sana2_tx_kick(iface);
}

/*
 * One finished write: the reply's verdict, the packet back in the shape NetX
 * Duo handed over, the slot handed back.  From the reap for a write the device
 * replied, and from ami_sana2_tx_send() itself for one it completed inside
 * BeginIO() with IOF_QUICK kept (tx_quick_ok).
 */
static VOID ami_sana2_tx_complete(AmiSana2If *iface, AmiTxSlot *slot)
{
    LONG err = (LONG)(BYTE)slot->req.ios2_Req.io_Error;

    {

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
        }

        /*
         * Under Forbid(), because ami_sana2_tx_claim() scans `busy` under
         * Forbid() and this is the store that hands the slot back.  `busy` is
         * volatile and `packet` is not, so a compiler is free to sink the
         * plain store past the volatile one; a claim landing in that window
         * takes a slot whose `packet` this thread then NULLs, orphaning a
         * packet that is never released.  The pool drains, ami_sana2_rx_post()
         * starts returning zero and the reader sleeps -- a read that falls off
         * a cliff while writes carry on, which is a shape this stack has
         * shipped once before.
         *
         * It was unreachable until the reader began reaping for itself: reap
         * used to run only on the IP thread or under nx_ip_protection from a
         * sender, so it could not race the claim.  It can now.
         *
         * A COMPILER BARRIER, NOT Forbid().  This shipped as Forbid()/Permit()
         * in 92bff6b3.
         *
         * THE NUMBERS BELOW ARE NOT TRUSTWORTHY AND ARE KEPT ONLY SO THE NEXT
         * READER DOES NOT RE-DERIVE THEM.  They were taken with each arm built
         * in a DIFFERENT reused worktree, and one of those build directories
         * did not match its own commit: a null control at the same commit
         * measured 5.3% between two such trees on identical source
         * (d5323947).  So "2% of receive and 5.5% of transmit" is an artefact
         * of unknown size and sign, and re-measuring it wants clean builds and
         * an md5 on each arm.
         *
         * THE CHANGE STANDS ON ITS OWN WITHOUT THEM.  92bff6b3 closed a real
         * race and closed it with a scheduling region; the hazard it names is
         * a COMPILER one and there is one CPU, so a barrier closes the same
         * hole and is strictly less work per completion.  That argument needs
         * no rate number.
         *
         * What it was measured as: about 2% of receive and 5.5% of transmit,
         * interleaved against 242be840 in one sitting -- transmit hit harder
         * because the pair runs once per completed transmit and a receive run
         * only pays it on ACKs.
         *
         * THE RECEIVE FIGURE IS POSITION-CORRECTED AND THE FIRST ONE WAS NOT.
         * Fixed-order runs put base first and the other arm second and read
         * -4.3%, but this rig gives the SECOND arm of a pair 2.0% less
         * receive whatever is in it: six rounds alternating which tree runs
         * first measure base/first 5,528,920 against fix/first 5,528,520, a
         * difference of 0.007%, while second-position medians sit 2.0% below
         * first-position ones in both trees.  Transmit shows no such effect
         * (+0.3%), so its 5.5% stands as measured.  Alternate the order of any
         * pair on this rig; a fixed order silently charges the second arm 2%.
         *
         * Permit() ends a scheduling
         * region and can switch on the spot, and a context switch is the most
         * expensive thing on this port (the priority-inheritance commit
         * reverted in 8e63732e cost 18% of transmit by the same mechanism).
         *
         * The barrier is sufficient because the hazard named above is a
         * COMPILER one, and there is one CPU: a claim that lands between
         * these stores reads busy == TRUE and skips the slot, and one that
         * lands after the last store finds every field already cleared.  The
         * barrier is what stops the plain stores sinking past the volatile
         * one; ordering them is the whole requirement.
         */
        slot->packet   = NULL;
        slot->cursor   = NULL;
        slot->consumed = 0;
        slot->total    = 0;
        __asm__ __volatile__("" ::: "memory");
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

    /* Writes still waiting for a slot go back to the pool: the interface is
       going down and nothing will launch them. */
    for (;;)
    {
        NX_PACKET *packet = NULL;

        Forbid();
        if (iface->tx_pend_count != 0)
        {
            packet = iface->tx_pend[iface->tx_pend_head].packet;
            iface->tx_pend_head = (UWORD)((iface->tx_pend_head + 1) %
                                          AMI_SANA2_TX_PEND);
            iface->tx_pend_count--;
        }
        Permit();

        if (packet == NULL)
            break;
        AMI_NX_CLEANUP(nx_packet_transmit_release(packet));
        iface->stats.tx_errors++;
    }

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
    for (i = 0; i < iface->tx_slots; i++)
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

static UINT ami_sana2_tx_launch(AmiSana2If *iface, AmiTxSlot *slot,
                                NX_PACKET *packet, UWORD ether_type,
                                ULONG dst_msw, ULONG dst_lsw);

/*
 * Queue a write behind the ones already waiting.  FALSE when the queue is
 * full, and the caller drops the packet.  Un-parks the reply port: while
 * writes wait, every completion has to be collected at once, by the reader,
 * because the collection is what launches the next one.
 */
static BOOL ami_sana2_tx_enqueue(AmiSana2If *iface, NX_PACKET *packet,
                                 UWORD ether_type, ULONG dst_msw,
                                 ULONG dst_lsw)
{
    AmiTxPending *pend = NULL;

    Forbid();
    if (iface->tx_pend_count < (UWORD)AMI_SANA2_TX_PEND)
    {
        pend = &iface->tx_pend[(iface->tx_pend_head + iface->tx_pend_count) %
                               AMI_SANA2_TX_PEND];
        pend->packet     = packet;
        pend->dst_msw    = dst_msw;
        pend->dst_lsw    = dst_lsw;
        pend->ether_type = ether_type;
        iface->tx_pend_count++;
    }
    Permit();

    if (pend == NULL)
        return FALSE;

    iface->stats.tx_queued++;

#ifdef AMINETXDUO_TX_LAZY_COLLECT
    if (iface->tx_lazy_parked)
    {
        Disable();
        if (iface->tx_port.mp_SigTask != NULL)
            iface->tx_port.mp_Flags = PA_SIGNAL;
        iface->tx_lazy_parked = FALSE;
        Enable();
    }
#endif

    return TRUE;
}

VOID ami_sana2_tx_kick(AmiSana2If *iface)
{
    for (;;)
    {
        AmiTxSlot   *slot = NULL;
        AmiTxPending pend;
        UWORD        i;

        /* One launcher at a time keeps the queue's order on the wire: the
           flag is held from the dequeue through BeginIO().  A second task
           finding it held leaves the rest to the holder, which comes back
           round for anything queued behind its back; a write queued after
           the holder has gone is launched by its own sender's kick. */
        Forbid();
        if (!iface->tx_kicking && iface->tx_pend_count != 0)
        {
            for (i = 0; i < iface->tx_slots; i++)
            {
                if (!iface->tx[i].busy)
                {
                    iface->tx[i].busy = TRUE;
                    slot = &iface->tx[i];
                    break;
                }
            }
            if (slot != NULL)
            {
                iface->tx_kicking = TRUE;
                pend = iface->tx_pend[iface->tx_pend_head];
                iface->tx_pend_head = (UWORD)((iface->tx_pend_head + 1) %
                                              AMI_SANA2_TX_PEND);
                iface->tx_pend_count--;
            }
        }
        Permit();

        if (slot == NULL)
            return;

        if (!iface->online)
        {
            /* The interface went down while the write waited: teardown. */
            slot->busy = FALSE;
            AMI_NX_CLEANUP(nx_packet_transmit_release(pend.packet));
            iface->stats.tx_errors++;
        }
        else
        {
            /* The verdict is the launch's own: a packet that fails here was
               released by it, as one from the direct path is. */
            (VOID)ami_sana2_tx_launch(iface, slot, pend.packet, pend.ether_type,
                                      pend.dst_msw, pend.dst_lsw);
        }

        iface->tx_kicking = FALSE;
    }
}

UINT ami_sana2_tx_send(AmiSana2If *iface, NX_PACKET *packet, UWORD ether_type,
                       ULONG dst_msw, ULONG dst_lsw)
{
    AmiTxSlot *slot;
#ifdef AMINETXDUO_RXPROBE
    ULONG probe_t0 = ami_budget_clock();
    ULONG probe_t1;
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
    ami_budget_reap(probe_t1 - probe_t0);
#endif

    if (!iface->online)
    {
        nx_packet_transmit_release(packet);
        iface->stats.tx_errors++;
        return NX_NOT_ENABLED;
    }

    /*
     * Behind whatever is already waiting, or straight into a slot.  A full
     * ring is not a wait any more (it was a 20 ms tx_thread_sleep per spin,
     * sana2_internal.h, AMI_SANA2_TX_PEND) and not a drop until the queue is
     * full too: the write waits its turn and the completion that frees a
     * slot launches it.
     */
    if (iface->tx_pend_count != 0)
        slot = NULL;
    else
        slot = ami_sana2_tx_claim(iface);

    if (slot == NULL)
    {
        if (!ami_sana2_tx_enqueue(iface, packet, ether_type, dst_msw, dst_lsw))
        {
            /* Congestion, not an error: drop and let the upper layers
               retransmit. */
            nx_packet_transmit_release(packet);
            iface->stats.tx_errors++;
            iface->stats.tx_queue_full++;
            return NX_TX_QUEUE_DEPTH;
        }
        ami_sana2_tx_kick(iface);
        return NX_SUCCESS;
    }

    return ami_sana2_tx_launch(iface, slot, packet, ether_type, dst_msw,
                               dst_lsw);
}

/*
 * One write into a claimed slot: the link header where the wire needs it,
 * the pad, the tap, the request, BeginIO(), and the completion when the
 * device kept IOF_QUICK.  Owns the packet from here, success or failure.
 */
static UINT ami_sana2_tx_launch(AmiSana2If *iface, AmiTxSlot *slot,
                                NX_PACKET *packet, UWORD ether_type,
                                ULONG dst_msw, ULONG dst_lsw)
{
    ULONG      length;
    BOOL       raw_write;
    BOOL       csum_hw = FALSE;
#ifdef AMINETXDUO_RXPROBE
    ULONG probe_t1 = ami_budget_clock();
    ULONG probe_t2;
#endif

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

    /*
     * The transport checksum by the card (ANXD_S2_TX_CSUM): the pseudo-header
     * sum goes into the field now, the copy hook then copies the segment as
     * it is, and the write carries the flag.  Before the pad, whose zero
     * bytes add nothing to a ones-complement sum either way; after the raw
     * block, which has already handed such a packet to NetX Duo's walk.
     */
    if (iface->tx_csum_ok != 0 && !raw_write &&
        (packet->nx_packet_interface_capability_flag &
         NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM) != 0)
        csum_hw = ami_sana2_tx_pseudo_sum(packet);

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
    /*
     * IOF_QUICK, TO OUR OWN DRIVERS.  Exec's contract: a device that finishes
     * the request inside BeginIO() leaves the flag set and posts no reply; one
     * that queues it clears the flag and replies later.  anxnet.device and
     * anxgenet.device keep it (netdev_reply, netdev_queue_prepare), and a
     * kept flag is a ReplyMsg() -- a Disable() pair, two traps on Emu68 -- and
     * a reap splice the write never pays.  A third-party driver has only ever
     * seen SendIO()'s cleared flag on CMD_WRITE from the stacks it was written
     * for, so it does not get the offer.
     */
    slot->req.ios2_Req.io_Flags   = (UBYTE)((raw_write ? SANA2IOF_RAW : 0) |
                                            (iface->tx_quick_ok ? IOF_QUICK : 0) |
                                            (csum_hw ? ANXD_S2IOF_L4_CSUM : 0));
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
    if (iface->tx_lazy_timer_up && !iface->tx_lazy_parked &&
        iface->tx_pend_count == 0)
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

    /* Kept: finished in there, no reply coming.  Cleared: queued, the reap
       collects it.  Read after BeginIO(), which is where the device decides. */
    if (iface->tx_quick_ok &&
        (slot->req.ios2_Req.io_Flags & IOF_QUICK) != 0)
    {
        ami_sana2_tx_complete(iface, slot);
    }

#ifdef AMINETXDUO_RXPROBE
    ami_budget_stuff(probe_t2 - probe_t1);
    ami_budget_post(ami_budget_clock() - probe_t2);
#endif

    return NX_SUCCESS;
}
