/*
 * Host test shim, struct Library, which every AmiNetXDuo library base starts
 * with. See exec/types.h. Never compiled for the Amiga.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TEST_EXEC_LIBRARIES_H
#define AMINETXDUO_TEST_EXEC_LIBRARIES_H

#include <exec/types.h>
#include <exec/nodes.h>

struct Library {
    struct Node lib_Node;
    UBYTE       lib_Flags;
    UBYTE       lib_pad;
    UWORD       lib_NegSize;
    UWORD       lib_PosSize;
    UWORD       lib_Version;
    UWORD       lib_Revision;
    APTR        lib_IdString;
    ULONG       lib_Sum;
    UWORD       lib_OpenCnt;
};

#endif
