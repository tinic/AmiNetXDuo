/*
 * Host test shim, struct Task, referenced only through pointers. See
 * exec/types.h. Never compiled for the Amiga.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_EXEC_TASKS_H
#define AMINETXDUO_TEST_EXEC_TASKS_H

#include <exec/types.h>
#include <exec/nodes.h>

struct Task {
    struct Node tc_Node;
    UBYTE       tc_Flags;
    UBYTE       tc_State;
    BYTE        tc_IDNestCnt;
    BYTE        tc_TDNestCnt;
    ULONG       tc_SigAlloc;
    ULONG       tc_SigWait;
    ULONG       tc_SigRecvd;
    ULONG       tc_SigExcept;
};

#endif
