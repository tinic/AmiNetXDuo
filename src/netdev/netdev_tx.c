/*
 * anxnet.device and anxgenet.device: the write path.
 *
 * Framing a CMD_WRITE, issuing it, the queue pump and the task-level
 * transmit, out of netdev_device.c for the reason netdev_unit.c is: that
 * file cannot be compiled off target, and this is where a batch write
 * (ANXD_S2F_TX_BATCH) pauses and resumes, which a host test must see.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "aminetxduo/anxs2ext.h"
#include "dp8390.h"

#include <proto/exec.h>

/* ------------------------------------------------------------- transmit --- */

/*
 * Frame one CMD_WRITE into nu_TxBuf.  Returns the wire length, or 0 with the
 * request already answered.
 */
UWORD netdev_tx_build(NetdevUnit *unit, struct IOSana2Req *io,
                      NetdevOpener *op)
{
    UBYTE *buf;
    ULONG  len = io->ios2_DataLength;
    UWORD  total;

    /* A core whose transmit buffer the CPU can address takes the frame
       directly.  Everything else is framed in the unit's own staging buffer
       and copied across by ops->tx. */
    buf = (unit->nu_Nic.tx_at != NULL) ? unit->nu_Nic.tx_at(&unit->nu_Nic)
                                       : NULL;
    if (buf == NULL)
        buf = (UBYTE *)unit->nu_TxBuf;
    unit->nu_TxAt = buf;

    if (netdev_io_is_raw(op, io))
    {
        if (len < NETDEV_HDR_LEN || len > NETDEV_FRAME_MAX)
        {
            netdev_reply(io, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
            return 0;
        }
        if (!netdev_copy_call(op->op_CopyFrom, buf, io->ios2_Data, len))
        {
            netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
            netdev_event(unit,
                         S2EVENT_ERROR | S2EVENT_TX | S2EVENT_BUFF);
            return 0;
        }
        total = (UWORD)len;
    }
    else
    {
        /*
         * nu_TxBuf is the unit's last field, so an unbounded ios2_DataLength
         * copies past the buffer and past the whole unit.
         */
        if (len > NETDEV_MTU)
        {
            netdev_reply(io, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
            /* slip.device posts S2EVENT_TX for exactly this refusal and
               cnet.device posts it from its .toobig arm. */
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
            return 0;
        }

        /* Aligned moves, as on the receive side: nu_TxBuf is ULONG[] and both
           addresses sit at even offsets. */
        nd_addr6(buf, io->ios2_DstAddr);
        nd_addr6(buf + NETDEV_ADDR_LEN, unit->nu_Nic.mac);
        *(UWORD *)(APTR)(buf + 12) = (UWORD)io->ios2_PacketType;

        if (len != 0 &&
            !netdev_copy_call(op->op_CopyFrom, buf + NETDEV_HDR_LEN,
                              io->ios2_Data, len))
        {
            netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_BUFF);
            return 0;
        }
        total = (UWORD)(len + NETDEV_HDR_LEN);
    }

    /*
     * The chip pads a short frame to the Ethernet minimum out of whatever is in
     * the buffer, so the padding must be written: left out, up to 46 bytes of
     * the last frame go on the wire behind every ARP.
     */
    if (total < NETDEV_FRAME_MIN)
    {
        nd_zero(buf + total, (ULONG)(NETDEV_FRAME_MIN - total));
        total = NETDEV_FRAME_MIN;
    }

    return total;
}

/*
 * The write's per-write metadata: the checksum the card is to finish and
 * whether a run may hold its start, into the core's fields, and the flags
 * as the opener answered them, so a caller can tell a batch write.  Per-write metadata comes through the
 * versioned extension, never through an unassigned bit in Exec/SANA-II's
 * io_Flags.  The callback is a plain load from the opener-owned request
 * cookie and is safe when a queued write advances from interrupt context.
 */
/* Inline where it is used: inside netdev_tx_issue(), where an ordinary
   write has always paid for it, and ahead of the build only for an opener
   granted TX_BATCH, which must know before framing.  Called ahead of the
   build for every write it was a call of its own on each one in the
   release LTO image. */
static inline UBYTE __attribute__((always_inline))
netdev_tx_meta(NetdevUnit *unit, struct IOSana2Req *io, NetdevOpener *op)
{
    UBYTE flags = 0;

    unit->nu_Nic.tx_csum = 0;
    unit->nu_Nic.tx_more = 0;
    /* Only when a transmit feature was accepted (ANX-013): an opener that
       supplied TxFlags but was granted nothing it governs is not called. */
    if (op->op_TxFlags != NULL &&
        (op->op_Extensions & (ANXD_S2F_TX_CSUM_TCP | ANXD_S2F_TX_CSUM_UDP |
                              ANXD_S2F_TX_MORE | ANXD_S2F_TX_BATCH)) != 0)
    {
        flags = ((AnxdS2TxFlags)op->op_TxFlags)(io->ios2_Data);

        unit->nu_Nic.tx_csum = (UBYTE)(flags & op->op_TxCsum);
        /* A run in progress: a core with a flush may hold its start. */
        unit->nu_Nic.tx_more = (UBYTE)((op->op_Extensions &
                                        ANXD_S2F_TX_MORE) != 0 &&
                                       (flags & ANXD_S2_TXF_MORE) != 0 &&
                                       unit->nu_Nic.tx_flush != NULL);
        /* A batch only where one was agreed, and never raw. */
        if ((op->op_Extensions & ANXD_S2F_TX_BATCH) == 0 ||
            netdev_io_is_raw(op, io))
            flags &= (UBYTE)~ANXD_S2_TXF_BATCH;
    }

    return flags;
}

/* The chip half, and the only half that needs the mask.  The metadata is
   read here, except for an opener granted TX_BATCH: the pump and the
   task-level write read it before the build for every write of such an
   opener, so its TxFlags callback runs once a write either way. */
LONG netdev_tx_issue(NetdevUnit *unit, struct IOSana2Req *io,
                     NetdevOpener *op, UWORD total)
{
    NetdevTrack *tr;
    LONG         rc;

    if ((op->op_Extensions & ANXD_S2F_TX_BATCH) == 0)
        (VOID)netdev_tx_meta(unit, io, op);
    rc = unit->nu_Nic.ops->tx(&unit->nu_Nic, unit->nu_TxAt, total);
    if (rc != 0)
        return rc;

    unit->nu_Stats.PacketsSent++;
    tr = netdev_track_find(op, io->ios2_PacketType);
    if (tr != NULL)
    {
        tr->st.PacketsSent++;
        tr->st.BytesSent += total;
    }

    return 0;
}

#ifndef NETDEV_TIME
#define netdev_tx_timed_build(u, i, o)      netdev_tx_build((u), (i), (o))
#define netdev_tx_timed_issue(u, i, o, t)   netdev_tx_issue((u), (i), (o), (t))
#endif

/* ------------------------------------------------------- batch writes --- */

#if NETDEV_HAS_TX_TASK_LOCK
/*
 * ANXD_S2F_TX_BATCH (aminetxduo/anxs2ext.h): one CMD_WRITE, Count frames.
 * The request stays the device's until every frame is in the ring; its
 * cursor is the unit's (nu_TxBatchIo, nu_TxBatchNext), never the opener's
 * record.  A step posts what the ring and the caller's budget allow; a
 * batch that is not finished goes back to the head of nu_Writes, so it
 * resumes before any later write, from the next frame.
 */
#define NETDEV_BATCH_DONE       0   /* every frame posted: reply 0       */
#define NETDEV_BATCH_PAUSED     1   /* back to the head of the queue     */
#define NETDEV_BATCH_FAILED     2   /* answered here with the error      */

/* Frame `index` of the batch in hand, at buf: the link header, then the
   opener's copy of the payload. */
static UWORD netdev_tx_fill(APTR arg, UWORD index, UBYTE *buf)
{
    NetdevUnit        *unit = (NetdevUnit *)arg;
    struct IOSana2Req *io   = unit->nu_TxBatchIo;
    NetdevOpener      *op   = NETDEV_IO_OPENER(io);
    UWORD              len;

    len = ((UWORD)(index + 1u) == unit->nu_TxBatchInfo.Count)
              ? unit->nu_TxBatchInfo.LastLen
              : unit->nu_TxBatchInfo.SegLen;

    nd_addr6(buf, io->ios2_DstAddr);
    nd_addr6(buf + NETDEV_ADDR_LEN, unit->nu_Nic.mac);
    *(UWORD *)(APTR)(buf + 12) = (UWORD)io->ios2_PacketType;

    if (((AnxdS2TxChild)op->op_TxChild)(io->ios2_Data, (ULONG)index,
                                        buf + NETDEV_HDR_LEN,
                                        (ULONG)len) == 0)
        return 0;

    return (UWORD)(len + NETDEV_HDR_LEN);
}

/* The shape the opener answered, within what the contract allows: every
   frame one an ordinary write could have been. */
static BOOL netdev_tx_batch_ok(const AnxdS2TxBatchInfo *b, ULONG n)
{
    return (BOOL)(n == b->Count && n >= 2 && n <= ANXD_S2_TX_BATCH_MAX &&
                  b->SegLen >= NETDEV_FRAME_MIN - NETDEV_HDR_LEN &&
                  b->SegLen <= NETDEV_MTU &&
                  b->LastLen >= NETDEV_FRAME_MIN - NETDEV_HDR_LEN &&
                  b->LastLen <= NETDEV_MTU);
}

/*
 * At most `budget` more frames of batch `io` into the ring; *copied counts
 * them.  The caller has run netdev_tx_meta() for it, holds tx_busy or
 * Disable(), and has the request off nu_Writes.
 */
static UWORD netdev_tx_batch_step(NetdevUnit *unit, struct IOSana2Req *io,
                                  NetdevOpener *op, UWORD budget,
                                  UWORD *copied)
{
    NetdevNic   *nic = &unit->nu_Nic;
    NetdevTrack *tr;
    UWORD        first;
    UWORD        want;
    UWORD        posted;
    UBYTE        failed = 0;

    if (unit->nu_TxBatchIo != io)
    {
        AnxdS2TxBatchInfo info;
        ULONG             n;

        info.Count = info.SegLen = info.LastLen = info.Reserved = 0;
        n = ((AnxdS2TxBatch)op->op_TxBatch)(io->ios2_Data, &info);
        if (!netdev_tx_batch_ok(&info, n))
        {
            netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_GENERIC_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
            return NETDEV_BATCH_FAILED;
        }
        unit->nu_TxBatchIo   = io;
        unit->nu_TxBatchInfo = info;
        unit->nu_TxBatchNext = 0;
    }

    first = unit->nu_TxBatchNext;
    want  = (UWORD)(unit->nu_TxBatchInfo.Count - first);
    if (want > budget)
        want = budget;

    posted = (want != 0) ? nic->tx_run(nic, first, want, netdev_tx_fill,
                                       unit, &failed)
                         : 0;
    unit->nu_TxBatchNext = (UWORD)(first + posted);
    *copied = (UWORD)(*copied + posted);

    /* The committed prefix, once a run: what the opener counts as sent. */
    if (posted != 0)
        ((AnxdS2TxPosted)op->op_TxPosted)(io->ios2_Data,
                                          (ULONG)unit->nu_TxBatchNext);

    /* Per frame, as the ordinary write counts its one. */
    if (posted != 0)
    {
        ULONG bytes = (ULONG)posted *
                      (unit->nu_TxBatchInfo.SegLen + NETDEV_HDR_LEN);

        if (unit->nu_TxBatchNext == unit->nu_TxBatchInfo.Count)
            bytes = bytes - unit->nu_TxBatchInfo.SegLen +
                    unit->nu_TxBatchInfo.LastLen;
        unit->nu_Stats.PacketsSent += posted;
        tr = netdev_track_find(op, io->ios2_PacketType);
        if (tr != NULL)
        {
            tr->st.PacketsSent += posted;
            tr->st.BytesSent   += bytes;
        }
    }

    if (failed)
    {
        unit->nu_TxBatchIo = NULL;
        if (!nic->running)
        {
            netdev_reply(io, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
        }
        else
        {
            netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_BUFF);
        }
        return NETDEV_BATCH_FAILED;
    }

    if (unit->nu_TxBatchNext == unit->nu_TxBatchInfo.Count)
    {
        unit->nu_TxBatchIo = NULL;
        unit->nu_TxBatches++;
        return NETDEV_BATCH_DONE;
    }

    unit->nu_TxBatchPauses++;
    return NETDEV_BATCH_PAUSED;
}
#endif /* NETDEV_HAS_TX_TASK_LOCK */

/* ------------------------------------------------------------ the pump --- */

/*
 * A pass that spent its frame budget with writes still waiting and a slot
 * free is owed another, run by the software interrupt once this Disable()
 * ends and the machine's other interrupts have had their turn.  Never for a
 * full ring: the completions that free it are what run the next pass, and
 * nudging then is a loop that finds nothing to do.
 */
static VOID netdev_tx_owe(NetdevUnit *unit)
{
    if (unit->nu_PumpOwed)
        return;                     /* Caused already, not yet run */
    unit->nu_PumpOwed = 1;
    unit->nu_PumpNudges++;
    Cause(&unit->nu_Soft);
}

/* A full-looking ring is asked about before anything is called stuck.  The
   frames a run holds for company go to the chip first: held, they are
   never retired, and a ring full of them waited for the blank's kick.
   Inline: the test is every write's and was written at each site before;
   out of line it was one more call per ordinary write in the LTO image. */
static inline VOID __attribute__((always_inline))
netdev_tx_reclaim_full(NetdevUnit *unit)
{
    if (unit->nu_Nic.txb_inuse >= unit->nu_Nic.txb_cnt &&
        unit->nu_Nic.tx_reclaim != NULL)
    {
        if (unit->nu_Nic.tx_flush != NULL)
            unit->nu_Nic.tx_flush(&unit->nu_Nic);
        (VOID)unit->nu_Nic.tx_reclaim(&unit->nu_Nic);
    }
}

/*
 * Drain the queue into whatever transmit buffers the chip has free, out of
 * the masked section's budget (nu_PumpBudget, netdev_tx_section): every
 * write taken costs at least one, whether it is sent, refused or fails, so
 * a queue of bad writes is bounded too.  The caller holds Disable().
 * Stands off while a task-level build owns nu_TxBuf; that builder pumps
 * again once its own frame is issued, so nothing is stranded.
 */
VOID netdev_tx_pump(NetdevUnit *unit)
{
    /* A task is mid-transmit under Forbid() (tx_task_lock): it pumps for
       itself when it is done, and nothing here may touch the ring now. */
    if (unit->nu_Nic.tx_busy)
        return;

    netdev_tx_reclaim_full(unit);

    while (unit->nu_Nic.txb_inuse < unit->nu_Nic.txb_cnt)
    {
        struct IOSana2Req *io;
        NetdevOpener      *op;
        UWORD              total;
        LONG               rc;
        UBYTE              flags;

        if (unit->nu_TxBuilding || IsListEmpty(&unit->nu_Writes))
            return;
        if (unit->nu_PumpBudget == 0)
        {
            netdev_tx_owe(unit);
            return;
        }

        io = (struct IOSana2Req *)RemHead(&unit->nu_Writes);
        op = NETDEV_IO_OPENER(io);
        /* Only an opener granted TX_BATCH can have sent a batch. */
        flags = ((op->op_Extensions & ANXD_S2F_TX_BATCH) != 0)
                    ? netdev_tx_meta(unit, io, op) : 0;

#if NETDEV_HAS_TX_TASK_LOCK
        if ((flags & ANXD_S2_TXF_BATCH) != 0)
        {
            UWORD copied = 0;
            UWORD r = netdev_tx_batch_step(unit, io, op,
                                           unit->nu_PumpBudget, &copied);

            if (r == NETDEV_BATCH_PAUSED && copied == 0)
            {
                /* Nothing fitted: where it was, and the pass is over. */
                AddHead(&unit->nu_Writes, &io->ios2_Req.io_Message.mn_Node);
                return;
            }
            /* A refusal costs a frame's worth: it took the callbacks. */
            if (copied == 0)
                copied = 1;
            unit->nu_PumpBudget = (UBYTE)((copied < unit->nu_PumpBudget)
                                          ? unit->nu_PumpBudget - copied : 0);
            if (r == NETDEV_BATCH_PAUSED)
            {
                /* Where it was: ahead of every later write. */
                AddHead(&unit->nu_Writes, &io->ios2_Req.io_Message.mn_Node);
                continue;
            }
            if (r == NETDEV_BATCH_DONE)
                netdev_reply(io, 0, 0);
            continue;
        }
#else
        (VOID)flags;
#endif

        unit->nu_PumpBudget--;
        total = netdev_tx_timed_build(unit, io, op);
        if (total == 0)
            continue;

        rc = netdev_tx_timed_issue(unit, io, op, total);
        if (rc == DP8390_TX_BUSY)
        {
            AddHead(&unit->nu_Writes, &io->ios2_Req.io_Message.mn_Node);
            return;
        }
        if (rc != 0)
        {
            netdev_reply(io, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
            continue;
        }

        netdev_reply(io, 0, 0);
    }
}

/* The software interrupt's half of netdev_tx_owe(), under its Disable() and
   out of its section's budget. */
BOOL netdev_tx_owed(NetdevUnit *unit)
{
    if (!unit->nu_PumpOwed)
        return FALSE;
    unit->nu_PumpOwed = 0;
    netdev_tx_pump(unit);
    return TRUE;
}

/*
 * The blank's backstop, under its Disable(): writes waiting and a slot free
 * is a pass nobody else may run -- a quiet wire raises no interrupt, and a
 * core's tick only asks for a pump when it retired something, which an
 * empty ring never does.
 */
BOOL netdev_tx_tick(NetdevUnit *unit)
{
    if (unit->nu_Nic.tx_busy || IsListEmpty(&unit->nu_Writes) ||
        unit->nu_Nic.txb_inuse >= unit->nu_Nic.txb_cnt)
        return FALSE;
    netdev_tx_pump(unit);
    return TRUE;
}

ULONG netdev_tx_service(NetdevUnit *unit)
{
    ULONG mine;

    netdev_tx_section(unit);
    mine = netdev_interrupt(unit);
    (VOID)netdev_tx_owed(unit);
    return mine;
}

/* The blank's pump: the core's tick when it retired something, else the
   backstop above.  Out of the blank's one section, which netdev_tick_c()
   opens at its entry. */
VOID netdev_tx_blank(NetdevUnit *unit)
{
    if (unit->nu_Nic.ops->tick != NULL &&
        unit->nu_Nic.ops->tick(&unit->nu_Nic))
        netdev_tx_pump(unit);
    else
        (VOID)netdev_tx_tick(unit);
}

/* ------------------------------------------------------ the write call --- */

/*
 * From BeginIO at task level.  Claim nu_TxBuf under the mask, frame outside it,
 * take the mask again only for the chip: the opener's CopyFrom is 135 us of the
 * 219 us a transmit spends here and must not run with interrupts off.
 */
#if NETDEV_HAS_TX_TASK_LOCK
static VOID netdev_tx_direct_task(NetdevUnit *unit, struct IOSana2Req *io);
#endif

VOID netdev_tx_direct(NetdevUnit *unit, struct IOSana2Req *io)
{
    NetdevOpener *op = NETDEV_IO_OPENER(io);
    UWORD         total;
    LONG          rc;

#if NETDEV_HAS_TX_TASK_LOCK
    /* The GENET and ZZ9000 cores ask for this, and only their images carry
       it: anxnet.device's cores keep the arm below and its image its size. */
    if (unit->nu_Nic.tx_task_lock)
    {
        netdev_tx_direct_task(unit, io);
        return;
    }
#endif

    Disable();
    netdev_tx_section(unit);
    /* The command-table check is outside this critical section.  If OFFLINE
       drained the queue between that check and here, adding the write now
       would strand it on a stopped unit with no completion left to pump it. */
    if (!unit->nu_Online || !unit->nu_Nic.running)
    {
        Enable();
        netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        return;
    }

    /* Same as the pump: a full-looking ring is asked about before a write
       is queued behind it. */
    netdev_tx_reclaim_full(unit);

    if (unit->nu_TxBuilding || !IsListEmpty(&unit->nu_Writes) ||
        unit->nu_Nic.txb_inuse >= unit->nu_Nic.txb_cnt)
    {
        netdev_queue_tail(&unit->nu_Writes, io);
        netdev_tx_pump(unit);
        Enable();
        return;
    }
    unit->nu_TxBuilding = 1;
    /* No batch on these cores: TX_BATCH needs tx_run, which only a
       tx_task_lock core offers (netdev_extension_supported). */
    if (!unit->nu_Nic.tx_short_build)
    {
        Enable();
        total = netdev_tx_timed_build(unit, io, op);
        Disable();
    }
    else
    {
        /* One section: the copy is cheaper than the mask on this machine. */
        total = netdev_tx_timed_build(unit, io, op);
    }
    unit->nu_TxBuilding = 0;
    if (total == 0)
    {
        netdev_tx_pump(unit);       /* the build failed and answered io */
        Enable();
        return;
    }

    rc = netdev_tx_timed_issue(unit, io, op, total);
    if (rc == DP8390_TX_BUSY)
    {
        netdev_queue_head(&unit->nu_Writes, io);
        Enable();
        return;
    }
    netdev_tx_pump(unit);           /* anything queued while we built */
    Enable();

    if (rc != 0)
    {
        netdev_reply(io, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
        netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
    }
    else
    {
        netdev_reply(io, 0, 0);
    }
}

/*
 * The same write for a core whose interrupt never produces (tx_task_lock):
 * Forbid() keeps other tasks out, tx_busy keeps the interrupt's reclaim and
 * the blank's pump off the ring for the whole of the build and the issue,
 * and Disable() is taken only around the unit's write list, which the pump
 * shares -- the rare path, a ring that is full.  On Emu68 the Disable()
 * pair this replaces was a 5.5 us trap per frame against a 0.4 us copy.
 *
 * A batch write is one Forbid() and one tx_busy for up to
 * ANXD_S2_TX_BATCH_MAX frames, the copies of all of them inside it, then
 * at most one masked pump pass (NETDEV_PUMP_FRAMES) for what waits behind.
 */
#if NETDEV_HAS_TX_TASK_LOCK
static VOID netdev_tx_direct_task(NetdevUnit *unit, struct IOSana2Req *io)
{
    NetdevOpener *op = NETDEV_IO_OPENER(io);
    UWORD         total;
    LONG          rc;
    BOOL          queued = FALSE;
    UBYTE         flags;

    Forbid();
    unit->nu_Nic.tx_busy = 1;

    if (!unit->nu_Online || !unit->nu_Nic.running)
    {
        unit->nu_Nic.tx_busy = 0;
        Permit();
        netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        return;
    }

    /* A full-looking ring is asked about first: the reclaim is this task's
       now, the interrupt's stands off. */
    netdev_tx_reclaim_full(unit);

    /* Behind whatever the blank's pump still holds, or into a full ring:
       onto the list, under the mask the pump takes for it. */
    if (unit->nu_TxBuilding || !IsListEmpty(&unit->nu_Writes) ||
        unit->nu_Nic.txb_inuse >= unit->nu_Nic.txb_cnt)
    {
        Disable();
        netdev_tx_section(unit);
        netdev_queue_tail(&unit->nu_Writes, io);
        unit->nu_Nic.tx_busy = 0;
        netdev_tx_pump(unit);
        Enable();
        Permit();
        return;
    }

    unit->nu_TxBuilding = 1;
    /* Only an opener granted TX_BATCH can have sent a batch. */
    flags = ((op->op_Extensions & ANXD_S2F_TX_BATCH) != 0)
                ? netdev_tx_meta(unit, io, op) : 0;
    if ((flags & ANXD_S2_TXF_BATCH) != 0)
    {
        UWORD copied = 0;
        UWORD r = netdev_tx_batch_step(unit, io, op,
                                       (UWORD)ANXD_S2_TX_BATCH_MAX, &copied);

        unit->nu_TxBuilding = 0;
        unit->nu_Nic.tx_busy = 0;
        if (r == NETDEV_BATCH_PAUSED)
        {
            /* The ring took part of it: the rest resumes from the head,
               before any later write, when the pump next runs. */
            Disable();
            netdev_queue_head(&unit->nu_Writes, io);
            Enable();
        }
        if (!IsListEmpty(&unit->nu_Writes))
        {
            Disable();
            netdev_tx_section(unit);
            netdev_tx_pump(unit);
            Enable();
        }
        Permit();

        if (r == NETDEV_BATCH_DONE)
            netdev_reply(io, 0, 0);
        return;
    }

    total = netdev_tx_timed_build(unit, io, op);
    if (total != 0)
    {
        rc = netdev_tx_timed_issue(unit, io, op, total);
        if (rc == DP8390_TX_BUSY)
        {
            Disable();
            netdev_queue_head(&unit->nu_Writes, io);
            Enable();
            queued = TRUE;
        }
    }
    else
    {
        rc = 0;                     /* the build failed and answered io */
        queued = TRUE;
    }
    unit->nu_TxBuilding = 0;
    unit->nu_Nic.tx_busy = 0;

    /* Anything the blank queued while this task held the ring. */
    if (!IsListEmpty(&unit->nu_Writes))
    {
        Disable();
        netdev_tx_section(unit);
        netdev_tx_pump(unit);
        Enable();
    }
    Permit();

    if (queued)
        return;
    if (rc != 0)
    {
        netdev_reply(io, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
        netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
    }
    else
    {
        netdev_reply(io, 0, 0);
    }
}
#endif /* NETDEV_HAS_TX_TASK_LOCK */
