/*
 * Host test shim -- struct Task, referenced only through pointers. See
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
};

#endif
