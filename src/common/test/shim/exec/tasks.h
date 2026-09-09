/*
 * struct Task for the src/common host tests.  Wider than the config shim's:
 * ami_random.c's gather_tasks() hashes tc_SPReg and tc_SPUpper of every task
 * on TaskReady and TaskWait, and a Task without them does not compile.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_COMMON_TEST_EXEC_TASKS_H
#define AMINETXDUO_COMMON_TEST_EXEC_TASKS_H

#include <exec/types.h>
#include <exec/nodes.h>

struct Task
{
    struct Node tc_Node;
    UBYTE       tc_Flags;
    UBYTE       tc_State;
    APTR        tc_SPReg;
    APTR        tc_SPLower;
    APTR        tc_SPUpper;
};

#define NT_TASK     1
#define NT_PROCESS  13

#endif
