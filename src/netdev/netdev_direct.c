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
 * The direct-receive claim, from the core's interrupt context, with only the
 * frame header in hand.  Success means the payload's destination is returned
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

    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener      *op  = (NetdevOpener *)n;
        struct IOSana2Req *hit = netdev_peek_take(&op->op_Reads, type);

        if (hit == NULL)
            continue;
        if (cand != NULL)
            return NULL;        /* two takers: everyone gets the staging copy */
        cand    = op;
        cand_io = hit;
    }

    if (cand == NULL)
    {
        /* Nobody has a read of this type posted.  If a direct-path opener
           has been reading it, its reader is behind rather than gone, and a
           core with a ring may hold the frame for it. */
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
        return NULL;
    }
    if (cand->op_RxDirect == NULL || cand->op_RxFilled == NULL)
        return NULL;
    if (cand->op_Filter != NULL)
        return NULL;

    /* Already located above; netdev_take() would walk to the same node. */
    io = cand_io;
    nd_remove(&io->ios2_Req.io_Message.mn_Node);

    /* RAW is also a per-request flag.  The direct destination starts after
       the Ethernet header, so accepting this request would omit fourteen
       bytes and report the payload length where SANA-II promises the whole
       frame.  Put it back for the ordinary hand-over immediately below. */
    if (netdev_io_is_raw(cand, io))
    {
        nd_addhead(&cand->op_Reads, &io->ios2_Req.io_Message.mn_Node);
        return NULL;
    }

    dst = ((AnxdS2RxDirect)cand->op_RxDirect)(io->ios2_Data, plen);
    if (dst == NULL)
    {
        AddHead(&cand->op_Reads, &io->ios2_Req.io_Message.mn_Node);
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

VOID netdev_rx_claimed(APTR arg, APTR token, ULONG sum, UBYTE flags)
{
    NetdevUnit        *unit = (NetdevUnit *)arg;
    struct IOSana2Req *io   = (struct IOSana2Req *)token;
    NetdevOpener      *op   = NETDEV_OPENER(io->ios2_Req.io_Unit);
    NetdevTrack       *tr   = netdev_track_find(op, io->ios2_PacketType);
    UWORD              len  = (UWORD)(io->ios2_DataLength + NETDEV_HDR_LEN);

    /* Only the bits this opener asked for, beyond the one every opener has
       always understood (aminetxduo/anxs2ext.h). */
    ((AnxdS2RxFilled)op->op_RxFilled)(io->ios2_Data, io->ios2_DataLength,
                                      sum,
                                      (UBYTE)(flags & (ANXD_S2_RXF_SUMMED |
                                                       op->op_RxFlags)));

    unit->nu_Stats.PacketsReceived++;
    if (tr != NULL)
    {
        tr->st.PacketsReceived++;
        tr->st.BytesReceived += len;
    }
    unit->nu_RxDirect++;

    if (unit->nu_Nic.reply_batch)
    {
        /* Held for the pass's one reply (netdev_rx_flush_replies); a full
           array flushes early, which cannot happen with a ring the size of
           the array but costs nothing to say. */
        if (unit->nu_PendingCount >= NETDEV_PENDING_MAX)
            netdev_rx_flush_replies(unit);
        io->ios2_Req.io_Error = 0;
        io->ios2_WireError    = 0;
        unit->nu_Pending[unit->nu_PendingCount++] = io;
        return;
    }

    netdev_reply(io, 0, 0);
}

/*
 * THE PASS'S ONE REPLY.  What ReplyMsg() does per message -- NT_REPLYMSG,
 * AddTail on the port's list under Disable(), Signal() the port's task --
 * done once per port for every request held: one Disable() pair and one
 * Signal() for a whole burst instead of one of each per frame.  Only a
 * PA_SIGNAL port is answered this way; any other action goes through
 * ReplyMsg() as before, so a port that wants a software interrupt or nothing
 * gets exactly that.  The direct pair's opener owns its port and the port
 * outlives every posted read (the reader reaps them before it goes), so
 * mp_SigTask is live.
 *
 * Interrupt context: Disable() nests inside the bottom half's own, and
 * Signal() is made for this.  The array is swept in port order, one pass per
 * distinct port -- three at most, one per reader, and nearly always one.
 */
VOID netdev_rx_flush_replies(APTR arg)
{
    NetdevUnit *unit = (NetdevUnit *)arg;
    UWORD       n    = unit->nu_PendingCount;
    UWORD       i;

    if (n == 0)
        return;
    unit->nu_PendingCount = 0;

    for (i = 0; i < n; i++)
    {
        struct IOSana2Req *io   = unit->nu_Pending[i];
        struct MsgPort    *port;
        UWORD              j;

        if (io == NULL)
            continue;
        port = io->ios2_Req.io_Message.mn_ReplyPort;
        if (port == NULL || (port->mp_Flags & PF_ACTION) != PA_SIGNAL ||
            port->mp_SigTask == NULL)
        {
            unit->nu_Pending[i] = NULL;
            ReplyMsg(&io->ios2_Req.io_Message);
            continue;
        }

        Disable();
        for (j = i; j < n; j++)
        {
            struct IOSana2Req *q = unit->nu_Pending[j];

            if (q != NULL && q->ios2_Req.io_Message.mn_ReplyPort == port)
            {
                q->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
                AddTail(&port->mp_MsgList, &q->ios2_Req.io_Message.mn_Node);
                unit->nu_Pending[j] = NULL;
            }
        }
        Enable();
        Signal((struct Task *)port->mp_SigTask, 1UL << port->mp_SigBit);
    }
}
