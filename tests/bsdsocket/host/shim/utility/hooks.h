/* struct Hook, for the bsdsocket host tests.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_UTILITY_HOOKS_H
#define AMINETXDUO_BSD_TEST_UTILITY_HOOKS_H
#include <exec/nodes.h>
struct Hook {
    struct MinNode h_MinNode;
    ULONG (*h_Entry)(void);
    ULONG (*h_SubEntry)(void);
    APTR    h_Data;
};
#endif
