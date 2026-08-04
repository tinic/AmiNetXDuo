/* struct Library, for the bsdsocket host tests.  SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_EXEC_LIBRARIES_H
#define AMINETXDUO_BSD_TEST_EXEC_LIBRARIES_H
#include <exec/nodes.h>

struct Library {
    struct Node lib_Node;
    UBYTE       lib_Flags;
    UBYTE       lib_pad;
    UWORD       lib_NegSize;
    UWORD       lib_PosSize;
    UWORD       lib_Version;
    UWORD       lib_Revision;
    char       *lib_IdString;
    ULONG       lib_Sum;
    UWORD       lib_OpenCnt;
};

#define LIBF_SUMMING   (1<<0)
#define LIBF_CHANGED   (1<<1)
#define LIBF_SUMUSED   (1<<2)
#define LIBF_DELEXP    (1<<3)

#endif
