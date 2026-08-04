/*
 * Exec list nodes, for the bsdsocket host tests.  Layout matters: several
 * structures below embed these by value, so a wrong size moves every field
 * after it and the tests would be measuring a different struct than the one
 * that ships.  From the NDK, unchanged since 1.2.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BSD_TEST_EXEC_NODES_H
#define AMINETXDUO_BSD_TEST_EXEC_NODES_H

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

#define NT_TASK      1
#define NT_LIBRARY   9
#define NT_MESSAGE   5

#endif
