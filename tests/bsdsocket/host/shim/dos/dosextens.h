/* struct Process and friends, for the bsdsocket host tests.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_DOS_DOSEXTENS_H
#define AMINETXDUO_BSD_TEST_DOS_DOSEXTENS_H
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <dos/dos.h>

struct Process {
    struct Task     pr_Task;
    struct MsgPort  pr_MsgPort;
    WORD            pr_Pad;
    BPTR            pr_SegList;
    LONG            pr_StackSize;
    APTR            pr_GlobVec;
    LONG            pr_TaskNum;
    BPTR            pr_StackBase;
    LONG            pr_Result2;
    BPTR            pr_CurrentDir;
    BPTR            pr_CIS;
    BPTR            pr_COS;
    APTR            pr_ConsoleTask;
    APTR            pr_FileSystemTask;
    BPTR            pr_CLI;
    APTR            pr_ReturnAddr;
    APTR            pr_PktWait;
    APTR            pr_WindowPtr;
};

struct DosPacket {
    struct Message *dp_Link;
    struct MsgPort *dp_Port;
    LONG            dp_Type;
    LONG            dp_Res1;
    LONG            dp_Res2;
    LONG            dp_Arg1;
    LONG            dp_Arg2;
    LONG            dp_Arg3;
    LONG            dp_Arg4;
};

struct DeviceNode {
    BPTR   dn_Next;
    ULONG  dn_Type;
    APTR   dn_Task;
    BPTR   dn_Lock;
    BSTR   dn_Handler;
    ULONG  dn_StackSize;
    LONG   dn_Priority;
    BPTR   dn_Startup;
    BPTR   dn_SegList;
    BPTR   dn_GlobalVec;
    BSTR   dn_Name;
};
#define DLT_DEVICE 0
#endif
