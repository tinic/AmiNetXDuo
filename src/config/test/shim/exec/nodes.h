/*
 * Host test shim, the list nodes usergroup_internal.h embeds. See
 * exec/types.h. Never compiled for the Amiga.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_EXEC_NODES_H
#define AMINETXDUO_TEST_EXEC_NODES_H

#include <exec/types.h>

struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    UBYTE        ln_Type;
    BYTE         ln_Pri;
    char        *ln_Name;
};

struct MinNode {
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
};

/* ln_Type values, at the NDK's numbering.  ug_db.c refuses to Open() from a
   bare Task, which has no pr_ fields, so it checks for NT_PROCESS. */
#define NT_TASK     1
#define NT_PROCESS  13

#endif
