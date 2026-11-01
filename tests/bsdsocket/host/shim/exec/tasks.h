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

/* NDK exec/tasks.h: tc_State.  bsd_event_post() signals a Task in a live
   state directly and walks Exec's lists for anything else (#383). */
#define TS_INVALID  0
#define TS_ADDED    1
#define TS_RUN      2
#define TS_READY    3
#define TS_WAIT     4
#define TS_EXCEPT   5
#define TS_REMOVED  6

/* NDK exec/tasks.h:94.  bsd_tcp_handler_start() waits on it for the control
   process to report whether TCP: was published. */
#define SIGB_SINGLE 4
#define SIGF_SINGLE (1L << SIGB_SINGLE)

#endif
