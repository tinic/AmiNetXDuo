/* Exec lists, for the bsdsocket host tests.  SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_EXEC_LISTS_H
#define AMINETXDUO_BSD_TEST_EXEC_LISTS_H
#include <exec/nodes.h>

struct List {
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
    UBYTE        lh_Type;
    UBYTE        l_pad;
};

struct MinList {
    struct MinNode *mlh_Head;
    struct MinNode *mlh_Tail;
    struct MinNode *mlh_TailPred;
};

#endif
