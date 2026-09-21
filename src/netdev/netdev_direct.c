/*
 * anxnet.device: the private SANA-II single-copy receive handshake.
 *
 * Kept separate from the device romtag so the claim-and-complete transaction
 * can run as an ordinary host test.  The chip cores call these from interrupt
 * context with only the Ethernet header in hand.
 *
 * There is no unclaim.  Neither supported port core has a recoverable error
 * once its drain has begun (netdev_nic.h states the contract), so the only
 * restore paths are the declines inside netdev_rx_claim() itself, which put
 * the CMD_READ back with AddHead before any hardware state has moved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/ports.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include "netdev_internal.h"
#include "aminetxduo/anxs2ext.h"

/*
 * The header is longword-aligned, but its source address starts six bytes in.
 * Three word moves retain the aligned fast path without telling the compiler
 * that this 2-mod-4 source is suitable for a ULONG load.
 */
static VOID direct_addr6(UBYTE *to, const UBYTE *from)
{
    *(UWORD *)(APTR)to       = *(const UWORD *)(CONST_APTR)from;
    *(UWORD *)(APTR)(to + 2) = *(const UWORD *)(CONST_APTR)(from + 2);
    *(UWORD *)(APTR)(to + 4) = *(const UWORD *)(CONST_APTR)(from + 4);
}

/* netdev_track_find() and netdev_take() moved to netdev_internal.h as static
   inlines: once a frame each, one call site each in the hot path, and both
   were in this file while that site is in netdev_device.c.  See the note
   there. */

/*
 * Which read would take this type, without taking it?  netdev_rx_claim() must
 * not steal a frame that a second opener would also have received, so it has
 * to scan EVERY opener before it takes anything.
 *
 * It returns the node rather than a yes/no because the answer is the same
 * node netdev_take() would have found -- the first of that type -- and the
 * list cannot change between the two: this runs in the core's interrupt
 * context with the frame already in hand.  Answering BOOL here meant walking
 * the winning opener's list twice per received frame, once to decide and
 * once to unlink.
 */
static struct IOSana2Req *netdev_peek_take(struct List *list, ULONG type)
{
    struct Node *n;

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)n;

        if (io->ios2_PacketType == type)
            return io;
    }

    return NULL;
}

/*
 * The direct-receive claim, from the core's serialized service context, with
 * only the frame header in hand.  Success means the payload's destination is returned
 * and the CMD_READ is off its queue with its address fields already filled;
 * the core then drains the hardware straight into the answer and finishes
 * with netdev_rx_claimed().  A claim cannot be cancelled after that point:
 * both supported port cores have already advanced hardware state once the
 * drain begins.
 *
 * The claim declines whenever the staging path would have done anything the
 * direct path cannot reproduce: a second opener wanting a copy of the same
 * type, a raw opener (whose buffer wants the header this function has already
 * consumed the meaning of), a filter hook (which must see the whole frame
 * before the copy), or an opener that never offered the direct pair.
 */
UBYTE *netdev_rx_claim(APTR arg, const UBYTE *hdr, UWORD frame_len,
                       APTR *token, UBYTE *wanted)
{
    NetdevUnit        *unit = (NetdevUnit *)arg;
    NetdevOpener      *cand = NULL;
    struct IOSana2Req *io;
    struct Node       *n;
    struct IOSana2Req *cand_io = NULL;
    ULONG              type;
    UBYTE              flags = 0;
    UBYTE             *dst;
    UWORD              plen;

    if (wanted != NULL)
        *wanted = 0;

    if (frame_len < NETDEV_HDR_LEN)
        return NULL;

    type = *(const UWORD *)(CONST_APTR)(hdr + 12);
    plen = (UWORD)(frame_len - NETDEV_HDR_LEN);

    /* BeginIO and AbortIO change these queues from arbitrary tasks.  The
       service lock keeps the opener objects alive; this short mask makes the
       select-and-unlink one transaction without covering a callback or copy. */
    Disable();
    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener      *op  = (NetdevOpener *)n;
        struct IOSana2Req *hit = netdev_peek_take(&op->op_Reads, type);

        if (hit == NULL)
            continue;
        if (cand != NULL)
        {
            Enable();
            return NULL;        /* two takers: everyone gets the staging copy */
        }
        cand    = op;
        cand_io = hit;
    }

    if (cand == NULL)
    {
        /* Nobody has a read of this type posted.  If a direct-path opener
           has been reading it, its reader is behind rather than gone, and a
           core with a ring may hold the frame for it.  Only do that while it
           is the unit's sole opener.  Holding the head of a hardware ring for
           one lagging private reader must never delay unrelated traffic for
           a second SANA-II opener. */
        if (unit->nu_Openers != 1)
        {
            Enable();
            return NULL;
        }
        for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL;
             n = n->ln_Succ)
        {
            NetdevOpener *op = (NetdevOpener *)n;

            if (op->op_RxDirect != NULL && op->op_RxFilled != NULL &&
                op->op_Filter == NULL && !op->op_Raw &&
                netdev_reads_type(op, type))
            {
                *token = NETDEV_CLAIM_BEHIND;
                break;
            }
        }
        Enable();
        return NULL;
    }
    io = cand_io;
    nd_remove(&io->ios2_Req.io_Message.mn_Node);
    Enable();

    if (cand->op_RxDirect == NULL || cand->op_RxFilled == NULL)
    {
        Disable();
        nd_addhead(&cand->op_Reads, &io->ios2_Req.io_Message.mn_Node);
        Enable();
        return NULL;
    }
    if (cand->op_Filter != NULL)
    {
        Disable();
        nd_addhead(&cand->op_Reads, &io->ios2_Req.io_Message.mn_Node);
        Enable();
        return NULL;
    }

    /*
     * A batch: the frame goes to its next cookie and the request stays
     * queued -- it is answered when full (netdev_batch_filled) or when the
     * pass that filled it ends (netdev_batch_flush).  No raw or filter case
     * to consider: netdev_queue_batch() refused those openers, and the link
     * header is always written because the batch has no per-frame request
     * fields to carry the addresses.
     */
    if (netdev_is_batch(io))
    {
        AnxdS2RxBatch *b = (AnxdS2RxBatch *)io->ios2_Data;

        dst = ((AnxdS2RxDirect)cand->op_RxDirect)(b->Cookie[b->Filled], plen);
        if (dst == NULL)
        {
            Disable();
            nd_addhead(&cand->op_Reads, &io->ios2_Req.io_Message.mn_Node);
            Enable();
            return NULL;
        }
        {
            UBYTE *lh = dst - NETDEV_HDR_LEN;
            UWORD  i;

            for (i = 0; i < NETDEV_HDR_LEN; i++)
                lh[i] = hdr[i];
        }
        /* The one per-frame field a batch request does carry, from the
           claim to its completion: the payload length RxFilled() reports. */
        io->ios2_DataLength = plen;
        *token = io;
        if (wanted != NULL)
            *wanted = cand->op_RxFlags;
        return dst;
    }

    /* RAW is also a per-request flag.  The direct destination starts after
       the Ethernet header, so accepting this request would omit fourteen
       bytes and report the payload length where SANA-II promises the whole
       frame.  Put it back for the ordinary hand-over immediately below. */
    if (netdev_io_is_raw(cand, io))
    {
        Disable();
        nd_addhead(&cand->op_Reads, &io->ios2_Req.io_Message.mn_Node);
        Enable();
        return NULL;
    }

    dst = ((AnxdS2RxDirect)cand->op_RxDirect)(io->ios2_Data, plen);
    if (dst == NULL)
    {
        Disable();
        AddHead(&cand->op_Reads, &io->ios2_Req.io_Message.mn_Node);
        Enable();
        return NULL;
    }

    if ((hdr[0] & 1) != 0)
    {
        flags = (UBYTE)((*(const ULONG *)(CONST_APTR)hdr == 0xffffffffUL &&
                         *(const UWORD *)(CONST_APTR)(hdr + 4) == 0xffffu)
                        ? SANA2IOF_BCAST : SANA2IOF_MCAST);
    }

    /*
     * The link header, written where the opener will read it, instead of
     * taken apart into the request for the opener to put back together.
     * `dst` is the payload, so the header is the fourteen bytes in front of
     * it -- which is only true because the raw case was refused above.
     *
     * The request fields are filled either way: SANA-II promises them, and an
     * opener that asked for the header may still read ios2_SrcAddr.
     */
    if (cand->op_RxLinkHdr)
    {
        UBYTE *lh = dst - NETDEV_HDR_LEN;
        UWORD  i;

        for (i = 0; i < NETDEV_HDR_LEN; i++)
            lh[i] = hdr[i];
    }

    direct_addr6(io->ios2_DstAddr, hdr);
    direct_addr6(io->ios2_SrcAddr, hdr + NETDEV_ADDR_LEN);
    io->ios2_PacketType = type;
    io->ios2_DataLength = plen;
    io->ios2_Req.io_Flags =
        (UBYTE)((io->ios2_Req.io_Flags & ~(SANA2IOF_BCAST | SANA2IOF_MCAST)) |
                flags);

    *token = io;
    if (wanted != NULL)
        *wanted = cand->op_RxFlags;
    return dst;
}

/*
 * A frame landed in a batch's next cookie.  Count it; a full batch is
 * answered here and now, a partial one when the pass ends.  Called with the
 * frame's bytes already in place, from the direct completion below or from
 * the staging copy (netdev_batch_stage).
 */
static VOID netdev_batch_filled(NetdevUnit *unit, NetdevOpener *op,
                                struct IOSana2Req *io, UWORD len,
                                ULONG sum, UBYTE flags)
{
    AnxdS2RxBatch *b  = (AnxdS2RxBatch *)io->ios2_Data;
    NetdevTrack   *tr = netdev_track_find(op, io->ios2_PacketType);

    ((AnxdS2RxFilled)op->op_RxFilled)(b->Cookie[b->Filled], len, sum,
                                      (UBYTE)(flags & (ANXD_S2_RXF_SUMMED |
                                                       op->op_RxFlags)));
    if ((flags & op->op_RxFlags & ANXD_S2_RXF_VERIFIED) != 0)
        unit->nu_Nic.rx_verified++;

    unit->nu_Stats.PacketsReceived++;
    if (tr != NULL)
    {
        tr->st.PacketsReceived++;
        tr->st.BytesReceived += (ULONG)len + NETDEV_HDR_LEN;
    }
    unit->nu_RxDirect++;

    if (b->Filled == 0)
        unit->nu_BatchPending++;
    b->Filled++;
    if (b->Filled >= b->Count)
    {
        if (unit->nu_BatchPending != 0)
            unit->nu_BatchPending--;
        netdev_reply(io, 0, 0);
    }
    else
    {
        Disable();
        nd_addhead(&op->op_Reads, &io->ios2_Req.io_Message.mn_Node);
        Enable();
    }
}

/*
 * The staging path's way into a batch: a frame the direct claim declined
 * (a second opener also reads the type, or the core has no claim at all)
 * is copied into the batch's next cookie by the opener's own CopyToBuff
 * hook, the link header is written in front of it, and RxFilled() reports
 * the frame the way the direct path does -- without SUMMED, so the opener
 * verifies it in software.  The batch keeps its place in op_Reads.
 */
NetdevRxResult netdev_batch_stage(NetdevUnit *unit, NetdevOpener *op,
                                  struct IOSana2Req *io, const UBYTE *frame,
                                  UWORD len)
{
    AnxdS2RxBatch *b    = (AnxdS2RxBatch *)io->ios2_Data;
    APTR           cookie = b->Cookie[b->Filled];
    UWORD          plen = (UWORD)(len - NETDEV_HDR_LEN);
    UBYTE         *dst;
    UWORD          i;

    dst = ((AnxdS2RxDirect)op->op_RxDirect)(cookie, plen);
    if (dst == NULL)
        return NETDEV_RX_FAILED;
    if (!netdev_copy_call(op->op_CopyTo, cookie, (APTR)(frame + NETDEV_HDR_LEN),
                          plen))
        return NETDEV_RX_FAILED;
    for (i = 0; i < NETDEV_HDR_LEN; i++)
        dst[(LONG)i - NETDEV_HDR_LEN] = frame[i];
    netdev_batch_filled(unit, op, io, plen, 0, 0);
    return NETDEV_RX_TAKEN;
}

/*
 * THE PASS'S ONE REPLY.  Every batch that took a frame during this pass and
 * is still queued -- not full -- is answered now, so a burst of N frames is
 * one ReplyMsg() per batch that held any of them and no frame waits for a
 * later pass.  nu_BatchPending is the number of such batches, kept by
 * netdev_batch_filled(); a batch aborted while pending leaves the count one
 * high until this scan finds nothing and resets it, which costs one walk of
 * the read lists and nothing else.  Each unlink is masked; replies are not.
 */
VOID netdev_batch_flush(NetdevUnit *unit)
{
    while (unit->nu_BatchPending != 0)
    {
        struct IOSana2Req *found = NULL;
        struct Node       *n;

        Disable();
        for (n = unit->nu_OpenerList.lh_Head;
             n->ln_Succ != NULL && found == NULL; n = n->ln_Succ)
        {
            NetdevOpener *op = (NetdevOpener *)n;
            struct Node  *r;

            for (r = op->op_Reads.lh_Head; r->ln_Succ != NULL;
                 r = r->ln_Succ)
            {
                struct IOSana2Req *io = (struct IOSana2Req *)r;

                if (netdev_is_batch(io) &&
                    ((AnxdS2RxBatch *)io->ios2_Data)->Filled != 0)
                {
                    nd_remove(r);
                    found = io;
                    break;
                }
            }
        }
        if (found != NULL && unit->nu_BatchPending != 0)
            unit->nu_BatchPending--;
        else if (found == NULL)
            unit->nu_BatchPending = 0;
        Enable();

        if (found != NULL)
            netdev_reply(found, 0, 0);
    }
}

VOID netdev_rx_claimed(APTR arg, APTR token, ULONG sum, UBYTE flags)
{
    NetdevUnit        *unit = (NetdevUnit *)arg;
    struct IOSana2Req *io   = (struct IOSana2Req *)token;
    NetdevOpener      *op   = NETDEV_IO_OPENER(io);
    NetdevTrack       *tr;
    UWORD              len;

    if (netdev_is_batch(io))
    {
        /* The claim left the payload length in ios2_DataLength. */
        netdev_batch_filled(unit, op, io, (UWORD)io->ios2_DataLength, sum,
                            flags);
        return;
    }

    tr  = netdev_track_find(op, io->ios2_PacketType);
    len = (UWORD)(io->ios2_DataLength + NETDEV_HDR_LEN);

    /* Only the bits this opener asked for, beyond the one every opener has
       always understood (aminetxduo/anxs2ext.h). */
    ((AnxdS2RxFilled)op->op_RxFilled)(io->ios2_Data, io->ios2_DataLength,
                                      sum,
                                      (UBYTE)(flags & (ANXD_S2_RXF_SUMMED |
                                                       op->op_RxFlags)));

    if ((flags & op->op_RxFlags & ANXD_S2_RXF_VERIFIED) != 0)
        unit->nu_Nic.rx_verified++;

    unit->nu_Stats.PacketsReceived++;
    if (tr != NULL)
    {
        tr->st.PacketsReceived++;
        tr->st.BytesReceived += len;
    }
    unit->nu_RxDirect++;

    netdev_reply(io, 0, 0);
}
