/* struct Task, for the bsdsocket host tests.  SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_EXEC_TASKS_H
#define AMINETXDUO_BSD_TEST_EXEC_TASKS_H
#include <exec/nodes.h>
#include <exec/lists.h>

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
    UWORD       tc_TrapAlloc;
    UWORD       tc_TrapAble;
    APTR        tc_ExceptData;
    APTR        tc_ExceptCode;
    APTR        tc_TrapData;
    APTR        tc_TrapCode;
    APTR        tc_SPReg;
    APTR        tc_SPLower;
    APTR        tc_SPUpper;
    VOID      (*tc_Switch)(void);
    VOID      (*tc_Launch)(void);
    struct List tc_MemEntry;
    APTR        tc_UserData;
};

#endif
