/* <exec/ports.h> for the sana2 host test.
   SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_SANA2_TEST_EXEC_PORTS_H
#define AMINETXDUO_SANA2_TEST_EXEC_PORTS_H

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>

struct Task;

struct MsgPort
{
    struct Node   mp_Node;
    UBYTE         mp_Flags;
    UBYTE         mp_SigBit;
    void         *mp_SigTask;
    struct List   mp_MsgList;
};

struct Message
{
    struct Node     mn_Node;
    struct MsgPort *mn_ReplyPort;
    UWORD           mn_Length;
};

#endif
