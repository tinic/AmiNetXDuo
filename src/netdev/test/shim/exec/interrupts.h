/* <exec/interrupts.h> for the netdev host test: struct Interrupt, which
   NetdevUnit embeds twice and the host never arms.
   SPDX-License-Identifier: MIT */

#ifndef AMINETXDUO_NETDEV_TEST_EXEC_INTERRUPTS_H
#define AMINETXDUO_NETDEV_TEST_EXEC_INTERRUPTS_H

#include <exec/types.h>
#include <exec/nodes.h>

struct Interrupt
{
    struct Node is_Node;
    APTR        is_Data;
    VOID      (*is_Code)();
};

#endif
