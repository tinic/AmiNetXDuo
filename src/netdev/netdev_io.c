/*
 * anxnet.device, the two Exec entry points that take an IORequest.
 *
 * Split out of netdev_device.c because what BeginIO does to a request's
 * fields before it dispatches is the whole of one shipped defect and had no
 * test: ios2_WireError was cleared for every command, and S2_ONEVENT carries
 * the caller's event mask IN that field, so every event request this driver
 * ever received named no condition and was refused.  The host tier enters
 * here now -- src/netdev/test/test_netdev_beginio.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"

#include <proto/exec.h>     /* ReplyMsg(): a short request is answered here */

VOID netdev_begin_io(register struct Device     *dev NETDEV_REG_A6,
                     register struct IOSana2Req *io  NETDEV_REG_A1)
{
    NetdevOpener *op;

    (VOID)dev;

    /*
     * NSCMD_DEVICEQUERY BEFORE ANYTHING SANA-II.  Its caller sends a plain
     * IOStdReq, and every field this function reads or writes below the
     * command number is either past that request's end (ios2_BufferManagement
     * at 84 of 48) or its io_Actual (ios2_WireError).  Deriving the opener
     * first found none in a copied request, and the query came back
     * IOERR_BADADDRESS: mcastfilter called this device "not NewStyle" while
     * the command list said otherwise.  The query needs no opener.
     */
    if (io->ios2_Req.io_Command == NSCMD_DEVICEQUERY)
    {
        io->ios2_Req.io_Error = 0;
        netdev_nsd_query((struct IOStdReq *)io);
        return;
    }

    /* Any other command in a request that short has no SANA-II fields to act
       on.  Answered within the IOStdReq it is, never past it. */
    if (NETDEV_IO_IS_SHORT(io))
    {
        struct IOStdReq *std = (struct IOStdReq *)io;

        std->io_Actual = 0;
        std->io_Error  = IOERR_NOCMD;
        if ((std->io_Flags & IOF_QUICK) == 0)
            ReplyMsg(&std->io_Message);
        return;
    }

    op = (io->ios2_Req.io_Unit != NULL &&
          io->ios2_Req.io_Unit != (struct Unit *)-1 &&
          io->ios2_BufferManagement != NULL)
         ? NETDEV_IO_OPENER(io) : NULL;

    io->ios2_Req.io_Error = 0;
    /*
     * ios2_WireError is an OUTPUT for every command but one.  S2_ONEVENT
     * carries the event mask the caller is waiting for IN it, so zeroing it
     * here handed netdev_cmds.c a mask of zero, which that code correctly
     * refuses as naming no condition: every S2_ONEVENT ever issued to this
     * device came back S2ERR_NOT_SUPPORTED/S2WERR_BAD_EVENT.  The events the
     * driver posts had no reachable waiter at all.
     */
    if (io->ios2_Req.io_Command != S2_ONEVENT)
        io->ios2_WireError = 0;

    /*
     * CMD_READ straight to its handler.  It is one per received frame, from
     * ami_sana2_rx_post_slot(), and it is most of what this device is ever
     * asked to do; netdev_perform() reaches the same three stores and AddHead
     * through a 40-byte frame, a movem of five registers and a jump table.
     * Every other command still goes the long way, and netdev_perform() keeps
     * its own CMD_READ case, so nothing that calls it directly changes
     * behaviour -- src/netdev/test enters at both.
     *
     * THE SIZE OF THIS IS NOT MEASURED, AND THE COMMIT MESSAGE SAYS SO.  The
     * rate cannot see it: six rounds gave rx -0.23% with the positions split
     * (+0.45%, -1.07%), which is what an effect of a few tenths looks like
     * against a ~1% resolution.  The profile could not settle it either --
     * exec.library/Dispatch was 10.6% in the branch run and absent from main's,
     * which deflates every other share by about that much, so
     * _netdev_perform 2.1% against _netdev_queue_read 1.1% is mostly a changed
     * denominator rather than a changed cost.
     *
     * What IS certain is that the frame, the movem of five registers and the
     * twenty-case jump table no longer execute on this path, because the path
     * no longer runs that code.  Kept on that basis and on the user's
     * direction that micro improvements accumulate toward a measurable total
     * -- not on a number.
     */
    if (op != NULL)
    {
        UWORD cmd = io->ios2_Req.io_Command;

        if (cmd == CMD_READ)
        {
            netdev_queue_read(op, io, CMD_READ);
            return;
        }

        /*
         * AND CMD_WRITE, WHICH ON A RECEIVE IS THE ACKNOWLEDGEMENT PATH.
         * An inbound bulk transfer sends roughly one frame for every two it
         * takes, and those sends are what reopen the window the far end is
         * filling; _netdev_perform still carries 1.8% of the real-path profile
         * with CMD_READ already bypassing it, and on an inbound-only transfer
         * there is nothing else going through it at that rate.
         *
         * S2_MULTICAST and S2_BROADCAST share the handler and are rare, so
         * they stay on the generic path and pay the jump table.
         */
        if (cmd == CMD_WRITE)
        {
            netdev_write_cmd(op, io, CMD_WRITE);
            return;
        }
    }

    netdev_perform(op, io);
}

LONG netdev_abort_io(register struct Device     *dev NETDEV_REG_A6,
                     register struct IOSana2Req *io  NETDEV_REG_A1)
{
    NetdevOpener *op;

    (VOID)dev;

    /* A query is answered inside BeginIO and never queued; a short request
       has no opener field to read.  Neither can be in progress. */
    if (io->ios2_Req.io_Command == NSCMD_DEVICEQUERY || NETDEV_IO_IS_SHORT(io))
        return -1;

    op = (io->ios2_Req.io_Unit != NULL &&
          io->ios2_Req.io_Unit != (struct Unit *)-1 &&
          io->ios2_BufferManagement != NULL)
         ? NETDEV_IO_OPENER(io) : NULL;

    return netdev_abort(op, io) ? 0 : -1;
}
