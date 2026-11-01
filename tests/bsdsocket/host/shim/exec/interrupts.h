/* <exec/interrupts.h> for the bsdsocket host tests: the software interrupt
   the base's timer port replies through (select.c).  From the NDK.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_EXEC_INTERRUPTS_H
#define AMINETXDUO_BSD_TEST_EXEC_INTERRUPTS_H

#include <exec/types.h>
#include <exec/nodes.h>

struct Interrupt
{
    struct Node is_Node;
    APTR        is_Data;
    VOID      (*is_Code)();
};

#endif
