/*
 * anxnet.device: S2_ONEVENT delivery and the S2_PacketFilter hook.
 *
 * Its own file for the same reason netdev_mcaf.c is: it needs no chip, no bus
 * and no register, so a host test can drive it.  What it decides cannot be
 * seen from an emulator -- an event posted from the wrong context corrupts a
 * list on a machine under load and nowhere else, and a filter hook that runs
 * on the wrong side of the copy-out drops traffic silently.
 *
 * WHAT THE SPECIFICATION SAYS, since none of it is inventable:
 *
 *   "All pending requests for a particular event will be returned when that
 *    event occurs."                       sana2device.spec, S2_ONEVENT NOTES
 *
 *   "All event types that cover a particular condition are returned when that
 *    condition occures.  For instance, if an error is returned by a buffer
 *    management function during receive processing, events of types
 *    S2EVENT_ERROR, S2EVENT_RX and S2EVENT_BUFF would be returned if
 *    pending."                            sana2device.spec, S2_ONEVENT NOTES
 *
 * So a queued request matches on ANY bit in common with what was posted, not
 * on equality, and S2EVENT_ERROR is a qualifier that rides along with the code
 * that says where the error was -- it is never posted by itself.  That is the
 * convention every posting site in this driver follows.
 *
 *   "ios2_WireError - Mask of events that occured."
 *                                         sana2device.spec, S2_ONEVENT RESULTS
 *
 * The completed request is given the WHOLE posted mask, not the part of it the
 * caller asked about.  Commodore's own slip.device does that (DoEvent, in the
 * SANA-II developer archive: `ios2->ios2_WireError = events`); cnet.device
 * hands back the intersection instead, which loses the "which of the three"
 * the sentence above exists to convey.
 *
 * THE FILTER.  S2_PacketFilter is a standard utility.library Hook, not a bare
 * function pointer:
 *
 *   "S2_PacketFilter [optional] - This is a pointer to a standard Hook to be
 *    called before S2_CopyToBuff is done."          SANA-II standard.txt
 *
 *   keep = PacketFilter(hook, ios2, data)
 *   d0                   a0    a2    a1
 *   ... TRUE if the driver should provide the packet to the protocol stack,
 *   FALSE if the packet should be ignored.          copybuff.spec autodoc
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
 * nu_EventMask is the OR of every queued request's mask on this unit, and it
 * exists so that a driver nobody is watching does no work at all: an interrupt
 * that would post an event tests one word and returns.  Without it every
 * dropped frame walked the opener list and every opener's event list to find
 * them empty.
 *
 * A WORD, not a long.  It is written under Disable() and read without one, and
 * a 68000 reads a longword as two bus cycles: an interrupt landing between
 * them yields half of one value and half of another.  Every S2EVENT_* bit is
 * in 0..7, so one word holds the whole of it and every 68k reads a word in a
 * single cycle.
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
 * Queue an S2_ONEVENT.  The mask has to join nu_EventMask under the same
 * Disable() as the AddTail: an interrupt between the two would see the request
 * on the list and the gate still saying nobody is waiting, and skip it.
 */
VOID netdev_event_wait(NetdevUnit *unit, struct IOSana2Req *io)
{
    NetdevOpener *op = NETDEV_OPENER(io->ios2_Req.io_Unit);

    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;

    Disable();
    AddTail(&op->op_Events, &io->ios2_Req.io_Message.mn_Node);
    unit->nu_EventMask |= (UWORD)io->ios2_WireError;
    Enable();
}

/* ------------------------------------------------------------ the post --- */

/*
 * INTERRUPT CONTEXT.  Almost every caller is one: the card's INT2 server, the
 * vertical blank watchdog at INT3, and card.resource's removal callback.  What
 * that permits is the whole of what happens here -- Disable(), a list walk,
 * Remove(), and ReplyMsg(), which is a PutMsg() and a Signal() and is
 * documented callable from interrupts.  There is no allocation, no Forbid(),
 * no semaphore and nothing that can Wait().
 *
 * Disable() rather than Forbid(): the lists are touched by the INT2 server, so
 * task-level exclusion is not exclusion.  It nests, so the callers that
 * already hold it (netdev_tick, netdev_tx_pump) are not a special case.
 *
 * The requests belong to other tasks.  Each is removed from the list BEFORE it
 * is replied: once ReplyMsg() has run the owner may be running and the node is
 * its business again, so a Remove() after the reply is a walk through memory
 * somebody else now owns.
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

    /* Only when the set changed; the common post matches nothing. */
    if (removed)
        unit->nu_EventMask = netdev_event_scan(unit);
    Enable();
}

/* ----------------------------------------------------------- the filter -- */

/*
 * What the opener is shown, and what is copied to it: the frame from byte 0
 * for a RAW request, the payload past the 14-byte Ethernet header otherwise.
 * The autodoc is explicit that the filter sees the same thing CopyToBuff
 * would -- "The data should NOT include any hardware specific headers (unless
 * of course the CMD_READ request wanted RAW packets)" -- so there is one
 * answer here and both callers use it.
 */
const UBYTE *netdev_payload(const NetdevOpener *op, const struct IOSana2Req *io,
                            const UBYTE *frame, UWORD len, ULONG *plen)
{
    if (op->op_Raw || (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)
    {
        *plen = len;
        return frame;
    }

    *plen = (ULONG)(len - NETDEV_HDR_LEN);
    return frame + NETDEV_HDR_LEN;
}

/*
 * TRUE when the packet may be handed over.  No hook installed is the answer
 * for every opener that has not asked to filter, and it costs one pointer test
 * on a receive path that is already holding the opener.
 *
 * The hook itself runs at interrupt level, in the middle of the card's own
 * service, and the autodoc says so: "This function must be callable from
 * interupts."  Nothing here can make that true for a caller who ignored it.
 */
BOOL netdev_filter_ok(NetdevOpener *op, struct IOSana2Req *io,
                      const UBYTE *data)
{
    if (op->op_Filter == NULL)
        return TRUE;

    return netdev_hook_call(op->op_Filter, io, (APTR)data);
}
