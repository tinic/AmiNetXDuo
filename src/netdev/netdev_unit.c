/*
 * anxnet.device: the three things that are about a UNIT and an OPENER rather
 * than about a frame.
 *
 * WHY THEY ARE HERE AND NOT IN netdev_device.c, WHICH IS WHERE THEY WERE.
 * That file cannot be compiled off-target and never will be: it carries the
 * romtag as a raw `asm(" moveq #-1,%d0 ")` at its head and reaches
 * <exec/execbase.h>, so a host compile stops before it starts.  These three
 * need neither.  Split out for the same reason and in the same shape as
 * src/usergroup/ug_str.c, so src/netdev/test/test_netdev_unit.c can drive
 * them: the multicast hash decides whether a card accepts the router's
 * neighbour solicitation at all, and it had no test.
 *
 * NOTHING ELSE CHANGES BY MOVING THEM.  All three were already non-static and
 * already called from other translation units -- netdev_reply from
 * netdev_cmds.c, netdev_event.c and netdev_direct.c, the other two from
 * netdev_cmds.c -- so no call site changes and no inlining opportunity is
 * lost that -flto did not already have.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "netdev_mcaf.h"

#include <proto/exec.h>

VOID netdev_reply(struct IOSana2Req *io, LONG err, ULONG wire)
{
    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError    = wire;

    if ((io->ios2_Req.io_Flags & IOF_QUICK) != 0)
        return;

    ReplyMsg(&io->ios2_Req.io_Message);
}

/* --------------------------------------------------- the hash filter -- */

VOID netdev_rebuild_filter(NetdevUnit *unit)
{
    UBYTE mar[8];
    UWORD i;

    netdev_mar_clear(mar);

    if (unit->nu_Nic.promisc || unit->nu_AllMulti != 0)
    {
        netdev_mar_all(mar);
    }
    else
    {
        for (i = 0; i < NETDEV_MCAST_MAX; i++)
        {
            if (unit->nu_Mcast[i].refs != 0)
                netdev_mar_set(mar, unit->nu_Mcast[i].addr);
        }
    }

    for (i = 0; i < 8; i++)
        unit->nu_Nic.mar[i] = mar[i];

    Disable();
    unit->nu_Nic.ops->setfilter(&unit->nu_Nic);
    Enable();
}

/*
 * Take one opener's CMD_WRITEs off the unit's queue.  Disable(), because the
 * interrupt server walks the same list and AddTail() on it is Disable()d too --
 * that is the only arbitration this driver has.
 */
VOID netdev_drop_writes(NetdevUnit *unit, NetdevOpener *op)
{
    struct Node *n;

    Disable();
    n = unit->nu_Writes.lh_Head;
    while (n->ln_Succ != NULL)
    {
        struct IOSana2Req *io   = (struct IOSana2Req *)n;
        struct Node       *next = n->ln_Succ;

        if (NETDEV_OPENER(io->ios2_Req.io_Unit) == op)
        {
            Remove(n);
            netdev_reply(io, IOERR_ABORTED, 0);
        }
        n = next;
    }
    Enable();
}
