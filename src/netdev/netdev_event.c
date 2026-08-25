/*
 * anxnet.device: S2_ONEVENT delivery and the S2_PacketFilter hook.
 *
 * A queued request matches on any bit in common with what was posted, not on
 * equality, and the completed request is given the WHOLE posted mask
 * (sana2device.spec, S2_ONEVENT NOTES and RESULTS).  S2EVENT_ERROR is a
 * qualifier accompanying the code that says where the error was, and is never
 * posted by itself.
 *
 * S2_PacketFilter is a standard utility.library Hook:
 *   keep = PacketFilter(hook, ios2, data)
 *   d0                   a0    a2    a1
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>

#include <proto/exec.h>

#include "netdev_internal.h"

/* ------------------------------------------------------------- the mask -- */

/*
 * nu_EventMask is the OR of every queued request's mask on this unit, so a
 * driver nobody is watching tests one word and returns.  A word, not a long: it
 * is written under Disable() and read without one, and a 68000 reads a longword
 * as two bus cycles.  Every S2EVENT_* bit is in 0..7.
 */
static UWORD netdev_event_scan(NetdevUnit *unit)
{
    struct Node *n;
    UWORD        mask = 0;

    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener *op = (NetdevOpener *)n;
        struct Node  *e;

        for (e = op->op_Events.lh_Head; e->ln_Succ != NULL; e = e->ln_Succ)
            mask |= (UWORD)((struct IOSana2Req *)e)->ios2_WireError;
    }

    return mask;
}

VOID netdev_event_rescan(NetdevUnit *unit)
{
    Disable();
    unit->nu_EventMask = netdev_event_scan(unit);
    Enable();
}

/*
 * Complete a state event which is already true, or queue an S2_ONEVENT.  The
 * state test, the AddTail and the nu_EventMask update are one transaction: a
 * transition between a separate test and queue would post to nobody and leave
 * a waiter behind for a state already reached.
 */
VOID netdev_event_wait(NetdevUnit *unit, struct IOSana2Req *io)
{
    NetdevOpener *op = NETDEV_OPENER(io->ios2_Req.io_Unit);
    ULONG         now;

    Disable();
    now = unit->nu_Online ? S2EVENT_ONLINE : S2EVENT_OFFLINE;
    if ((io->ios2_WireError & now) != 0)
    {
        Enable();
        netdev_reply(io, 0, io->ios2_WireError & now);
        return;
    }

    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    AddTail(&op->op_Events, &io->ios2_Req.io_Message.mn_Node);
    unit->nu_EventMask |= (UWORD)io->ios2_WireError;
    Enable();
}

/* ------------------------------------------------------------ the post --- */

/*
 * Almost every caller runs at interrupt level, so there is no allocation, no
 * Forbid(), no semaphore and nothing here that can Wait().  Disable() rather
 * than Forbid(), because the lists are touched by the INT2 server; it nests.
 * Each request is removed from the list BEFORE it is replied: once ReplyMsg()
 * has run the node is its owner's business again.
 */
VOID netdev_event(NetdevUnit *unit, ULONG mask)
{
    struct Node *n;
    BOOL         removed = FALSE;

    if ((unit->nu_EventMask & (UWORD)mask) == 0)
        return;

    Disable();
    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener *op = (NetdevOpener *)n;
        struct Node  *e  = op->op_Events.lh_Head;

        while (e->ln_Succ != NULL)
        {
            struct IOSana2Req *io   = (struct IOSana2Req *)e;
            struct Node       *next = e->ln_Succ;

            if ((io->ios2_WireError & mask) != 0)
            {
                Remove(e);
                removed = TRUE;
                netdev_reply(io, 0, mask);
            }
            e = next;
        }
    }

    /* Only when the set changed.  The common post matches nothing. */
    if (removed)
        unit->nu_EventMask = netdev_event_scan(unit);
    Enable();
}

/* ----------------------------------------------------------- the filter -- */

/*
 * What the opener is shown, and what is copied to it: the frame from byte 0 for
 * a RAW request, the payload past the 14-byte Ethernet header otherwise.  The
 * filter sees the same data CopyToBuff would (copybuff.spec autodoc).
 */
const UBYTE *netdev_payload(const NetdevOpener *op, const struct IOSana2Req *io,
                            const UBYTE *frame, UWORD len, ULONG *plen)
{
    if (netdev_io_is_raw(op, io))
    {
        *plen = len;
        return frame;
    }

    *plen = (ULONG)(len - NETDEV_HDR_LEN);
    return frame + NETDEV_HDR_LEN;
}

/*
 * TRUE when the packet can be handed over.  The hook itself runs at interrupt
 * level, in the middle of the card's own service, and the autodoc requires it:
 * "This function must be callable from interupts."
 */
BOOL netdev_filter_ok(NetdevOpener *op, struct IOSana2Req *io,
                      const UBYTE *data)
{
    if (op->op_Filter == NULL)
        return TRUE;

    return netdev_hook_call(op->op_Filter, io, (APTR)data);
}
