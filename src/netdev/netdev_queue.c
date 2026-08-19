/*
 * anxnet.device, queued IORequest invariants.
 *
 * SPDX-License-Identifier: MIT
 */

#include <proto/exec.h>

#include "netdev_internal.h"


static VOID netdev_queue_prepare(struct IOSana2Req *io)
{
    io -> ios2_Req.io_Flags &=  (UBYTE) ~IOF_QUICK;
    io -> ios2_Req.io_Message.mn_Node.ln_Type =  NT_MESSAGE;
}


VOID netdev_queue_tail(struct List *list, struct IOSana2Req *io)
{
    netdev_queue_prepare(io);
    AddTail(list, &io -> ios2_Req.io_Message.mn_Node);
}


VOID netdev_queue_head(struct List *list, struct IOSana2Req *io)
{
    netdev_queue_prepare(io);
    AddHead(list, &io -> ios2_Req.io_Message.mn_Node);
}
