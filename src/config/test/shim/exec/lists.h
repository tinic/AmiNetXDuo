/*
 * Host test shim, the list heads usergroup_internal.h embeds. See
 * exec/types.h. Never compiled for the Amiga.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_EXEC_LISTS_H
#define AMINETXDUO_TEST_EXEC_LISTS_H

#include <exec/types.h>
#include <exec/nodes.h>

struct List {
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
    UBYTE        lh_Type;
    UBYTE        lh_pad;
};

struct MinList {
    struct MinNode *mlh_Head;
    struct MinNode *mlh_Tail;
    struct MinNode *mlh_TailPred;
};

#endif
