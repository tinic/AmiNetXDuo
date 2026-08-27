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

VOID netdev_begin_io(register struct Device     *dev NETDEV_REG_A6,
                     register struct IOSana2Req *io  NETDEV_REG_A1)
{
    NetdevOpener *op = (io->ios2_Req.io_Unit != NULL &&
                        io->ios2_Req.io_Unit != (struct Unit *)-1)
                       ? NETDEV_OPENER(io->ios2_Req.io_Unit) : NULL;

    (VOID)dev;

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

    netdev_perform(op, io);
}

LONG netdev_abort_io(register struct Device     *dev NETDEV_REG_A6,
                     register struct IOSana2Req *io  NETDEV_REG_A1)
{
    NetdevOpener *op = (io->ios2_Req.io_Unit != NULL &&
                        io->ios2_Req.io_Unit != (struct Unit *)-1)
                       ? NETDEV_OPENER(io->ios2_Req.io_Unit) : NULL;

    (VOID)dev;

    return netdev_abort(op, io) ? 0 : -1;
}
