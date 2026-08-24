/*
 * anxnet.device: the private SANA-II single-copy receive handshake.
 *
 * Kept separate from the device romtag so the claim, completion and rollback
 * transaction can run as an ordinary host test.  The chip cores call these
 * from interrupt context with only the Ethernet header in hand.
 *
 * SPDX-License-Identifier: MIT
 */

#include <proto/exec.h>

#include "netdev_internal.h"
#include "aminetxduo/anxs2ext.h"

static VOID direct_addr6(UBYTE *to, const UBYTE *from)
{
    *(UWORD *)(APTR)to       = *(const UWORD *)(const APTR)from;
    *(UWORD *)(APTR)(to + 2) = *(const UWORD *)(const APTR)(from + 2);
    *(UWORD *)(APTR)(to + 4) = *(const UWORD *)(const APTR)(from + 4);
}

/* Bounded by the highest slot ever taken, not by the array.  The profile put
   this at 26% of the old hand-over, because it scanned all sixteen entries for
   every opener on every frame, and usually nothing is tracked. */
NetdevTrack *netdev_track_find(NetdevOpener *op, ULONG type)
{
    UWORD i;

    for (i = 0; i < op->op_TrackHigh; i++)
    {
        if (op->op_Track[i].used && op->op_Track[i].type == type)
            return &op->op_Track[i];
    }

    return NULL;
}

struct IOSana2Req *netdev_take(struct List *list, ULONG type)
{
    struct Node *n;

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)n;

        if (type == (ULONG)-1 || io->ios2_PacketType == type)
        {
            Remove(n);
            return io;
        }
    }

    return NULL;
}

/* Is there a read for this type without taking it?  A direct claim must not
   steal a frame that a second opener would also have received. */
static BOOL netdev_would_take(const struct List *list, ULONG type)
{
    const struct Node *n;

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        const struct IOSana2Req *io = (const struct IOSana2Req *)n;

        if (io->ios2_PacketType == type)
            return TRUE;
    }

    return FALSE;
}

UBYTE *netdev_rx_claim(APTR arg, const UBYTE *hdr, UWORD frame_len,
                       APTR *token)
{
    NetdevUnit        *unit = (NetdevUnit *)arg;
    NetdevOpener      *cand = NULL;
    struct IOSana2Req *io;
    struct Node       *n;
    ULONG              type;
    UBYTE              flags = 0;
    UBYTE             *dst;
    UWORD              plen;

    if (frame_len < NETDEV_HDR_LEN)
        return NULL;

    type = *(const UWORD *)(const APTR)(hdr + 12);
    plen = (UWORD)(frame_len - NETDEV_HDR_LEN);

    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener *op = (NetdevOpener *)n;

        if (!netdev_would_take(&op->op_Reads, type))
            continue;
        if (cand != NULL)
            return NULL;
        cand = op;
    }

    if (cand == NULL || cand->op_RxDirect == NULL || cand->op_RxFilled == NULL)
        return NULL;
    if (cand->op_Raw || cand->op_Filter != NULL)
        return NULL;

    io = netdev_take(&cand->op_Reads, type);
    if (io == NULL)
        return NULL;

    dst = ((AnxdS2RxDirect)cand->op_RxDirect)(io->ios2_Data, plen);
    if (dst == NULL)
    {
        AddHead(&cand->op_Reads, &io->ios2_Req.io_Message.mn_Node);
        return NULL;
    }

    if ((hdr[0] & 1) != 0)
    {
        flags = (UBYTE)((*(const ULONG *)(const APTR)hdr == 0xffffffffUL &&
                         *(const UWORD *)(const APTR)(hdr + 4) == 0xffffu)
                        ? SANA2IOF_BCAST : SANA2IOF_MCAST);
    }

    direct_addr6(io->ios2_DstAddr, hdr);
    direct_addr6(io->ios2_SrcAddr, hdr + NETDEV_ADDR_LEN);
    io->ios2_PacketType = type;
    io->ios2_DataLength = plen;
    io->ios2_Req.io_Flags =
        (UBYTE)((io->ios2_Req.io_Flags & ~(SANA2IOF_BCAST | SANA2IOF_MCAST)) |
                flags);

    *token = io;
    return dst;
}

VOID netdev_rx_claimed(APTR arg, APTR token, ULONG sum, UBYTE summed)
{
    NetdevUnit        *unit = (NetdevUnit *)arg;
    struct IOSana2Req *io   = (struct IOSana2Req *)token;
    NetdevOpener      *op   = NETDEV_OPENER(io->ios2_Req.io_Unit);
    NetdevTrack       *tr   = netdev_track_find(op, io->ios2_PacketType);
    UWORD              len  = (UWORD)(io->ios2_DataLength + NETDEV_HDR_LEN);

    ((AnxdS2RxFilled)op->op_RxFilled)(io->ios2_Data, io->ios2_DataLength,
                                      sum, summed);

    unit->nu_Stats.PacketsReceived++;
    if (tr != NULL)
    {
        tr->st.PacketsReceived++;
        tr->st.BytesReceived += len;
    }
    unit->nu_RxDirect++;

    netdev_reply(io, 0, 0);
}

VOID netdev_rx_unclaim(APTR arg, APTR token)
{
    struct IOSana2Req *io = (struct IOSana2Req *)token;
    NetdevOpener      *op = NETDEV_OPENER(io->ios2_Req.io_Unit);

    (VOID)arg;
    AddHead(&op->op_Reads, &io->ios2_Req.io_Message.mn_Node);
}
