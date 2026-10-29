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
        slot->gso     = AMI_SANA2_GSO_NONE;

#ifdef AMINETXDUO_RXPROBE
        slot->write_at = 0UL;
#endif
    }

    iface->tx_pend_head  = 0;
    iface->tx_pend_count = 0;
    iface->tx_kicking    = FALSE;

#ifdef AMINETXDUO_TX_RUN
    /* The run flush: the opened request's device, unit and cookie, the
       write port for the reply a device that queues it would post, and the
       private command.  tx_more_ok is settled by the open (sana2_device.c). */
    iface->tx_flush_req = iface->templ;
    iface->tx_flush_req.ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    iface->tx_flush_req.ios2_Req.io_Message.mn_ReplyPort    = &iface->tx_port;
    iface->tx_flush_req.ios2_Req.io_Message.mn_Length =
        (UWORD)sizeof(struct IOSana2Req);
    iface->tx_flush_req.ios2_Req.io_Command = ANXD_CMD_TX_FLUSH;
    iface->tx_holder     = NULL;
    iface->tx_held       = 0;
    iface->tx_flush_busy = 0;
#endif

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

static VOID __attribute__((noinline))
ami_sana2_tx_reap_pending(AmiSana2If *iface)
{
    struct Message *msg;
    struct List     batch;
    struct Node    *node;
    struct Node    *next;

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
#ifdef AMINETXDUO_TX_RUN
        /* The one request on this port that is not a write: the run flush a
           device chose to queue. */
        if (((struct IOSana2Req *)msg)->ios2_Req.io_Command ==
            ANXD_CMD_TX_FLUSH)
        {
            ami_sana2_tx_flush_replied(iface);
            continue;
        }
#endif
        /* ios2_Req.io_Message is the first member of the first member of
           AmiTxSlot, so the reply message is the slot. */
        ami_sana2_tx_complete(iface, (AmiTxSlot *)msg);
    }

    /* The slots just handed back are what the queued writes were waiting
       for. */
    if (iface->tx_pend_count != 0)
        ami_sana2_tx_kick(iface);
}

VOID ami_sana2_tx_reap(AmiSana2If *iface)
{
    /* Keep the empty-port check small enough to inline into the send path.
       A reply arriving after it is picked up by the next send or reader.
       Only a nonempty port needs the batch frame and Disable()/Enable();
       the pending helper rechecks under Disable() because another reaper
       may have drained the port meanwhile. */
    if (iface->tx_port.mp_MsgList.lh_Head->ln_Succ != NULL)
        ami_sana2_tx_reap_pending(iface);
}

/*
 * One finished write: the reply's verdict, the packet back in the shape NetX
 * Duo handed over, the slot handed back.  From the reap for a write the device
 * replied, and from ami_sana2_tx_send() itself for one it completed inside
 * BeginIO() with IOF_QUICK kept (tx_quick_ok).
 */
static VOID ami_sana2_gso_complete(AmiSana2If *iface, AmiTxSlot *slot,
                                   LONG err);

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
        {
            iface->stats.packets_sent++;
            AMI_SANA2_COUNT_BYTES(iface->stats.tx_bytes_hi,
                                  iface->stats.tx_bytes,
                                  slot->total - slot->hdr_len);
        }

        /* A GSO child counted above as the frame it is; the parent is the
           block's, and goes back with its last child. */
        if (slot->gso != AMI_SANA2_GSO_NONE)
        {
            ami_sana2_gso_complete(iface, slot, err);
        }
        else if (slot->packet != NULL)
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

/* ------------------------------------------------------------- TCP GSO */

/*
 * `*p += n` and `*p -= n` as one read-modify-write instruction, which a task
 * switch cannot split on the one CPU; the subtraction says whether it left
 * zero, from the Z flag of the same instruction.  The host build's atomics
 * stand in for the tests.
 */
static inline VOID __attribute__((always_inline))
ami_sana2_gso_add(UBYTE *p, UBYTE n)
{
#if defined(__mc68000__) || defined(__m68k__)
    __asm__ __volatile__("add.b %1,%0" : "+m"(*p) : "d"(n) : "cc", "memory");
#else
    (VOID)__atomic_add_fetch(p, n, __ATOMIC_SEQ_CST);
#endif
}

static inline BOOL __attribute__((always_inline))
ami_sana2_gso_sub_zero(UBYTE *p, UBYTE n)
{
#if defined(__mc68000__) || defined(__m68k__)
    UBYTE z;

    __asm__ __volatile__("sub.b %2,%1\n\tseq %0"
                         : "=d"(z), "+m"(*p) : "d"(n) : "cc", "memory");
    return (z != 0) ? TRUE : FALSE;
#else
    return (__atomic_sub_fetch(p, n, __ATOMIC_SEQ_CST) == 0) ? TRUE : FALSE;
#endif
}

/*
 * `n` children of block `idx` are done with the parent: written, refused by
 * the device, or never launched (`lost`).  Whichever is last releases it,
 * once; the context is free from that store on.
 *
 * No Forbid(), for tx_complete()'s reason.  The reader's reap, the IP thread
 * and a sender's own reap or IOF_QUICK completion, and drain or the offline
 * kick, can all reach this for one block at once.  What they share is the
 * count, and each change to it is one instruction (above), so the task that
 * takes it to zero is the only one.  `lost` is added before the count drops,
 * so that task reads every child's verdict.  Until it stores `parent` NULL no
 * gso_send() can take the context, and the barrier keeps the reads above that
 * store.
 */
static VOID ami_sana2_gso_put(AmiSana2If *iface, UWORD idx, UWORD n, BOOL lost)
{
    AmiTxGso  *g = &iface->gso[idx - 1];
    NX_PACKET *parent;
    UWORD      dropped;
    UWORD      segs;

    if (lost)
        ami_sana2_gso_add(&g->lost, (UBYTE)n);
    if (!ami_sana2_gso_sub_zero(&g->outstanding, (UBYTE)n))
        return;

    parent  = g->parent;
    dropped = g->lost;
    segs    = g->segs;
    __asm__ __volatile__("" ::: "memory");
    g->parent = NULL;

    if (parent == NULL)
        return;

    if (dropped != 0)
        AMI_WARN("sana2: a GSO block lost %ld of its %ld segments. TCP "
                 "sends them again", (LONG)dropped, (LONG)segs);

    nx_packet_transmit_release(parent);
}

static VOID ami_sana2_gso_complete(AmiSana2If *iface, AmiTxSlot *slot,
                                   LONG err)
{
    UWORD idx = slot->gso;

    slot->packet = NULL;
    slot->gso    = AMI_SANA2_GSO_NONE;
    ami_sana2_gso_put(iface, idx, 1, (BOOL)(err != 0));
}

/*
 * Abort anything still in flight and reap it.  AbortIO() is a request a driver
 * may decline; slot->req and its reply port live inside AmiSana2If, so
 * tx_orphaned stops ami_sana2_close() from freeing it.
 */
/* Reap, then count what the device still holds: every busy write slot and
   the queued run flush.  The drain below and the retained sweep. */
UWORD ami_sana2_tx_collect(AmiSana2If *iface)
{
    UWORD i;
    UWORD busy = 0;

    ami_sana2_tx_reap(iface);

    for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
    {
        if (iface->tx[i].busy)
            busy++;
    }
#ifdef AMINETXDUO_TX_RUN
    if (iface->tx_flush_busy)
        busy++;
#endif

    return busy;
}

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
        UWORD      gso    = AMI_SANA2_GSO_NONE;

        Forbid();
        if (iface->tx_pend_count != 0)
        {
            packet = iface->tx_pend[iface->tx_pend_head].packet;
            gso    = iface->tx_pend[iface->tx_pend_head].gso;
            iface->tx_pend_head = (UWORD)((iface->tx_pend_head + 1) %
                                          AMI_SANA2_TX_PEND);
            iface->tx_pend_count--;
        }
        Permit();

        if (packet == NULL)
            break;
        /* A GSO child owns only its share: the parent goes back once the
           children in slots are done with it too. */
        if (gso != AMI_SANA2_GSO_NONE)
            ami_sana2_gso_put(iface, gso, 1, TRUE);
        else
            AMI_NX_CLEANUP(nx_packet_transmit_release(packet));
        iface->stats.tx_errors++;
    }

    for (i = 0; i < AMI_SANA2_TX_SLOTS; i++)
    {
        if (iface->tx[i].busy)
            AbortIO((struct IORequest *)&iface->tx[i].req);
    }
#ifdef AMINETXDUO_TX_RUN
    /* A run left open by a sender the interface went down under, and the
       one flush a queuing device may still hold. */
    iface->tx_holder = NULL;
    iface->tx_held   = 0;
    if (iface->tx_flush_busy)
        AbortIO((struct IORequest *)&iface->tx_flush_req);
#endif

    /* The flush request is the device's until its reply is reaped, exactly
       as a write is: it lives in the interface and answers on tx_port, so
       it is counted with the writes below and holds tx_orphaned the same
       way.  AbortIO() may answer it later rather than now. */
    spins = 0;
    for (;;)
    {
        busy = ami_sana2_tx_collect(iface);

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
        AMI_ERROR("sana2: %ld request(s) still owned by the device. The "
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

/* Up to `want` free slots for a GSO block's children, marked busy, as a mask
   of their indices (AMI_SANA2_TX_SLOTS is at most 32).  Under the caller's
   Forbid(), for the reason ami_sana2_tx_claim() takes its own. */
static ULONG ami_sana2_tx_claim_run(AmiSana2If *iface, UWORD want)
{
    ULONG mask = 0;
    UWORD i;

    for (i = 0; i < iface->tx_slots && want != 0; i++)
    {
        if (!iface->tx[i].busy)
        {
            iface->tx[i].busy = TRUE;
            mask |= 1UL << i;
            want--;
        }
    }

    return mask;
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
 * because the collection is what launches the next one.  `count` children
 * of GSO block `gso` go in together or not at all, so a block never waits
 * with a hole in it.
 */
static BOOL ami_sana2_tx_enqueue(AmiSana2If *iface, NX_PACKET *packet,
                                 UWORD ether_type, ULONG dst_msw,
                                 ULONG dst_lsw, UWORD gso, UWORD count)
{
    BOOL  queued = FALSE;
    UWORD i;

    Forbid();
    if ((UWORD)AMI_SANA2_TX_PEND - iface->tx_pend_count >= count)
    {
        for (i = 0; i < count; i++)
        {
            AmiTxPending *pend =
                &iface->tx_pend[(iface->tx_pend_head + iface->tx_pend_count) %
                                AMI_SANA2_TX_PEND];

            pend->packet     = packet;
            pend->dst_msw    = (UWORD)dst_msw;
            pend->dst_lsw    = dst_lsw;
            pend->ether_type = ether_type;
            pend->gso        = gso;
            iface->tx_pend_count++;
        }
        queued = TRUE;
    }
    Permit();

    if (!queued)
        return FALSE;

    iface->stats.tx_queued += count;

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
            /* The interface went down while the write waited: teardown.  A
               GSO child gives back its share of the parent and no more. */
            slot->busy = FALSE;
            if (pend.gso != AMI_SANA2_GSO_NONE)
                ami_sana2_gso_put(iface, pend.gso, 1, TRUE);
            else
                AMI_NX_CLEANUP(nx_packet_transmit_release(pend.packet));
            iface->stats.tx_errors++;
        }
        else
        {
            /* The verdict is the launch's own: a packet that fails here was
               released by it, as one from the direct path is. */
            slot->gso = pend.gso;
            (VOID)ami_sana2_tx_launch(iface, slot, pend.packet, pend.ether_type,
                                      pend.dst_msw, pend.dst_lsw);
        }

        iface->tx_kicking = FALSE;
    }
}

/* ----------------------------------------------------------- transmit runs */
#ifdef AMINETXDUO_TX_RUN

/*
 * The first opener is the holder; a second thread's bracket (another
 * program sending on the same interface) is a no-op, and its writes start
 * whatever the holder has waiting, which is no worse than today.  Forbid()
 * and not a ThreadX object: the test is two loads in the common case, and
 * the ThreadX bracket the callers hold is not a lock between threads.
 */
VOID ami_sana2_tx_run_begin(AmiSana2If *iface)
{
    TX_THREAD *me;

    if (iface == NULL || !iface->tx_more_ok)
        return;

    me = tx_thread_identify();
    if (me == NULL)
        return;

    Forbid();
    if (iface->tx_holder == NULL)
        iface->tx_holder = me;
    Permit();
}

/*
 * Start what the run has left with the device.  The command is quick by
 * contract (anxs2ext.h); a device that queues it instead gets one and no
 * more: the reply is collected by the reap, and until then, and after a
 * refusal, runs are off for the interface.  A write of another thread may
 * already have started the held frames; the command is then a register
 * write that finds nothing, cheaper than knowing.
 */
VOID ami_sana2_tx_run_flush(AmiSana2If *iface)
{
    if (iface == NULL || iface->tx_holder != tx_thread_identify() ||
        !iface->tx_held)
        return;

    iface->tx_held = 0;
    if (iface->tx_flush_busy || !iface->online)
        return;

    iface->tx_flush_req.ios2_Req.io_Flags = IOF_QUICK;
    iface->tx_flush_req.ios2_Req.io_Error = 0;
    BeginIO((struct IORequest *)&iface->tx_flush_req);
    if ((iface->tx_flush_req.ios2_Req.io_Flags & IOF_QUICK) == 0)
    {
        iface->tx_flush_busy = 1;
        iface->tx_more_ok    = 0;
    }
    else if (iface->tx_flush_req.ios2_Req.io_Error != 0)
    {
        iface->tx_more_ok = 0;
    }
}

VOID ami_sana2_tx_run_end(AmiSana2If *iface)
{
    if (iface == NULL || iface->tx_holder == NULL ||
        iface->tx_holder != tx_thread_identify())
        return;

    ami_sana2_tx_run_flush(iface);
    iface->tx_holder = NULL;
}

VOID ami_sana2_tx_flush_replied(AmiSana2If *iface)
{
    iface->tx_flush_busy = 0;
    /* Stays off: a device that queued one would queue the next. */
}
#endif /* AMINETXDUO_TX_RUN */

/*
 * A TCP GSO block (aminetxduo/tcpgso.h) as its k child writes, in order:
 * into free slots while nothing waits, the rest queued behind them in one
 * piece.  Owns the parent from here, success or failure; a context holds it
 * until the last child is done with it (ami_sana2_gso_put).  Out of line:
 * the ordinary write pays one flag test for it.
 */
static UINT __attribute__((noinline))
ami_sana2_gso_send(AmiSana2If *iface, NX_PACKET *packet, UWORD ether_type,
                   ULONG dst_msw, ULONG dst_lsw)
{
    AmiTxGso   blk;
    AmiTxSlot *slot;
    ULONG      claimed = 0;
    UWORD      idx = AMI_SANA2_GSO_NONE;
    UWORD      at;
    UWORD      i;
    UWORD      s;

    /* Cooked IPv4 only: the copy hook is what cuts the block, and neither a
       raw write nor an IPv6 datagram is cut there.  The capability says
       neither, so the core is not to send one; one that arrives is dropped
       rather than put on the wire whole. */
    if (iface->raw_mode || ether_type != AMI_ETHERTYPE_IPV4 ||
        iface->gso == NULL || !ami_sana2_gso_parse(packet, iface->mtu, &blk))
    {
        nx_packet_transmit_release(packet);
        iface->stats.tx_errors++;
        return NX_INVALID_PACKET;
    }

    /* Blocks finish roughly in the order they started, so the search starts
       after the last one taken and usually stops at once. */
    Forbid();
    at = iface->gso_next;
    for (i = 0; i < iface->gso_count; i++)
    {
        if (at >= iface->gso_count)
            at = 0;
        if (iface->gso[at].parent == NULL)
        {
            blk.parent       = packet;
            blk.outstanding  = blk.segs;
            blk.lost         = 0;
            blk.next_seg     = 0;
            iface->gso[at]   = blk;
            idx              = (UWORD)(at + 1);
            iface->gso_next  = (UWORD)(at + 1);
            break;
        }
        at++;
    }
    /* The children's slots in the same region: one Forbid() per block, as
       the ordinary write takes one per frame.  Not past a queued write. */
    if (idx != AMI_SANA2_GSO_NONE && iface->tx_pend_count == 0)
        claimed = ami_sana2_tx_claim_run(iface, blk.segs);
    Permit();

    /* Cannot happen: a block in flight holds a slot or a queue entry, and
       there is a context for each.  Kept, counted as a full queue would be,
       rather than trusted. */
    if (idx == AMI_SANA2_GSO_NONE)
    {
        nx_packet_transmit_release(packet);
        iface->stats.tx_errors += blk.segs;
        iface->stats.tx_queue_full++;
        return NX_TX_QUEUE_DEPTH;
    }

    /* The slots claimed with the context, launched in order; then again only
       if one came straight back, from a device that finished it inside
       BeginIO().  The unlocked look costs nothing when it is wrong: a missed
       slot leaves the rest to the queue and the kick. */
    i = 0;
    while (claimed != 0)
    {
        BOOL back = FALSE;

        for (s = 0; claimed != 0; s++)
        {
            if ((claimed & (1UL << s)) == 0)
                continue;
            claimed &= ~(1UL << s);

            slot      = &iface->tx[s];
            slot->gso = idx;
            (VOID)ami_sana2_tx_launch(iface, slot, packet, ether_type,
                                      dst_msw, dst_lsw);
            if (!slot->busy)
                back = TRUE;
            i++;
        }

        if (back && i < blk.segs && iface->tx_pend_count == 0)
        {
            Forbid();
            if (iface->tx_pend_count == 0)
                claimed = ami_sana2_tx_claim_run(iface, (UWORD)(blk.segs - i));
            Permit();
        }
    }

    if (i < blk.segs)
    {
        if (!ami_sana2_tx_enqueue(iface, packet, ether_type, dst_msw, dst_lsw,
                                  idx, (UWORD)(blk.segs - i)))
        {
            /* The children already launched still own their share. */
            iface->stats.tx_errors += (ULONG)(blk.segs - i);
            iface->stats.tx_queue_full++;
            ami_sana2_gso_put(iface, idx, (UWORD)(blk.segs - i), TRUE);
            return (i == 0) ? NX_TX_QUEUE_DEPTH : NX_SUCCESS;
        }
        ami_sana2_tx_kick(iface);
    }

    return NX_SUCCESS;
}

UINT ami_sana2_tx_send(AmiSana2If *iface, NX_PACKET *packet, UWORD ether_type,
                       ULONG dst_msw, ULONG dst_lsw)
{
    /* ACKPACE: a pure acknowledgment may wait for its turn instead
       (sana2_ackpace.c).  One test when the option is off. */
    if (iface != NULL && iface->ack_tpkb != 0 && packet != NULL &&
        ether_type == AMI_ETHERTYPE_IPV4 &&
        ami_sana2_ack_hold(iface, packet, dst_msw, dst_lsw))
        return NX_SUCCESS;

    return ami_sana2_tx_send_now(iface, packet, ether_type, dst_msw, dst_lsw);
}

UINT ami_sana2_tx_send_now(AmiSana2If *iface, NX_PACKET *packet,
                           UWORD ether_type, ULONG dst_msw, ULONG dst_lsw)
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

    /* A TCP GSO block goes out as several writes (ami_sana2_gso_send). */
    if ((packet->nx_packet_interface_capability_flag &
         NX_INTERFACE_CAPABILITY_TCP_GSO) != 0)
        return ami_sana2_gso_send(iface, packet, ether_type, dst_msw, dst_lsw);

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
        if (!ami_sana2_tx_enqueue(iface, packet, ether_type, dst_msw, dst_lsw,
                                  AMI_SANA2_GSO_NONE, 1))
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

#ifdef AMINETXDUO_BPF
/* LTO otherwise reserves the capture view and synthetic link header in
   every launch, even when no capture channel is bound. */
static VOID __attribute__((noinline))
ami_sana2_tx_capture(AmiSana2If *iface, AmiTxSlot *slot, NX_PACKET *packet,
                      UWORD ether_type, ULONG dst_msw, ULONG dst_lsw)
{
    ami_bpf_tap_tx(iface, packet, slot->hdr_len != 0, ether_type,
                   dst_msw, dst_lsw, iface->mac);
}

/*
 * A GSO child as the wire sees it: the synthetic link header, the headers
 * the copy hook will write and the payload slice in place.  Never the
 * parent, which no wire ever carries.
 */
static VOID __attribute__((noinline))
ami_sana2_gso_capture(AmiSana2If *iface, AmiTxSlot *slot, ULONG length,
                      UWORD ether_type, ULONG dst_msw, ULONG dst_lsw)
{
    static const UBYTE zero[AMI_ETH_MIN_FRAME];
    const AmiTxGso *g = &iface->gso[slot->gso - 1];
    AmiBpfView      view;
    UBYTE           eth[AMI_ETH_HEADER_SIZE];
    UBYTE           hdr[120];
    NX_PACKET      *cur = slot->seg_buf;
    ULONG           off = slot->seg_buf_off;
    ULONG           hlen;
    ULONG           seg_len;
    ULONG           left;

    eth[0] = (UBYTE)(dst_msw >> 8);
    eth[1] = (UBYTE)(dst_msw);
    eth[2] = (UBYTE)(dst_lsw >> 24);
    eth[3] = (UBYTE)(dst_lsw >> 16);
    eth[4] = (UBYTE)(dst_lsw >> 8);
    eth[5] = (UBYTE)(dst_lsw);
    ami_sana2_copy_bytes(&eth[6], iface->mac, AMI_ETH_ADDR_SIZE);
    eth[12] = (UBYTE)(ether_type >> 8);
    eth[13] = (UBYTE)(ether_type);

    hlen = ami_sana2_gso_header(slot, hdr);
    (VOID)ami_sana2_gso_slice(g, slot->seg_index, &seg_len);
    left = seg_len;

    view.wirelen = AMI_ETH_HEADER_SIZE + length;
    view.caplen  = 0;
    view.nsegs   = 0;
    (VOID)ami_bpf_view_add(&view, eth, AMI_ETH_HEADER_SIZE);
    (VOID)ami_bpf_view_add(&view, hdr, hlen);

    while (left != 0 && cur != NULL)
    {
        ULONG have = (ULONG)(cur->nx_packet_append_ptr -
                             cur->nx_packet_prepend_ptr);
        ULONG take;

        if (off >= have)
        {
            cur = cur->nx_packet_next;
            off = 0;
            continue;
        }
        take = have - off;
        if (take > left)
            take = left;
        if (ami_bpf_view_add(&view, cur->nx_packet_prepend_ptr + off,
                             take) != 0)
            break;          /* caplen < wirelen says so */
        left -= take;
        off  += take;
    }

    if (left == 0 && length > hlen + seg_len)
        (VOID)ami_bpf_view_add(&view, zero, length - hlen - seg_len);

    ami_bpf_tap_view(iface, &view);
}
#endif

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

    /* A GSO child: cooked, nothing written into the parent, the frame the
       copy hook's.  The pad is the copy hook's zeroes too. */
    if (slot->gso != AMI_SANA2_GSO_NONE)
    {
        raw_write = FALSE;
        csum_hw   = (iface->tx_csum_ok & ANXD_S2_TXF_TCP) != 0;
        slot->tx_flags = csum_hw ? ANXD_S2_TXF_TCP : 0;
        slot->pad_len  = 0;
        length    = ami_sana2_gso_prepare(slot, packet);
        if (iface->hw_type == S2WireType_Ethernet &&
            length < (ULONG)(AMI_ETH_MIN_FRAME - AMI_ETH_HEADER_SIZE))
            length = (ULONG)(AMI_ETH_MIN_FRAME - AMI_ETH_HEADER_SIZE);
#ifdef AMINETXDUO_BPF
        if (ami_bpf_bound_channels != 0)
            ami_sana2_gso_capture(iface, slot, length, ether_type, dst_msw,
                                  dst_lsw);
#endif
        goto post;
    }

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
     * The transport checksum by the card: the pseudo-header
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
    if (ami_bpf_bound_channels != 0)
        ami_sana2_tx_capture(iface, slot, packet, ether_type, dst_msw, dst_lsw);
#endif

    length = packet->nx_packet_length;

post:
    slot->packet     = packet;
    slot->cursor     = NULL;      /* the copy hook rewinds on first call */
    slot->cursor_off = 0;
    slot->consumed   = 0;
    slot->total      = length;
    slot->tx_flags   = csum_hw ? ANXD_S2_TXF_TCP : 0;
#ifdef AMINETXDUO_TX_RUN
    /* One of the holder's run: the device may wait for the next before it
       starts this one.  The holder's own thread only -- the kick that
       launches a queued write from the reader, or an acknowledgement the IP
       thread sends meanwhile, is a plain write and starts what is held. */
    if (iface->tx_holder != NULL && iface->tx_holder == tx_thread_identify())
    {
        slot->tx_flags |= ANXD_S2_TXF_MORE;
        iface->tx_held  = 1;
    }
#endif

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
                                            (iface->tx_quick_ok ? IOF_QUICK : 0));
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
