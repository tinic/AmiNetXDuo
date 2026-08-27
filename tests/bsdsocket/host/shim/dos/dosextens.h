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

/* The whole of it, not only the two members bsd_opener_name() reads: a
   partial one is a structure a later reader can be wrong about silently. */
struct CommandLineInterface {
    LONG   cli_Result2;
    BSTR   cli_SetName;
    BPTR   cli_CommandDir;
    LONG   cli_ReturnCode;
    BSTR   cli_CommandName;
    LONG   cli_FailLevel;
    BSTR   cli_Prompt;
    BPTR   cli_StandardInput;
    BPTR   cli_CurrentInput;
    BSTR   cli_CommandFile;
    LONG   cli_Interactive;
    LONG   cli_Background;
    BPTR   cli_CurrentOutput;
    LONG   cli_DefaultStack;
    BPTR   cli_StandardOutput;
    BPTR   cli_Module;
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

/* The one struct tcp_handler.c reaches through a BPTR.  Whole, in the NDK's
   order: fh_Arg1, fh_Type and fh_Port are what the FIND packet fills in, and
   a partial copy would put them at the wrong offsets for a later reader. */
struct FileHandle {
    struct Message *fh_Link;
    struct MsgPort *fh_Port;
    struct MsgPort *fh_Type;
    LONG            fh_Buf;
    LONG            fh_Pos;
    LONG            fh_End;
    LONG            fh_Funcs;
    LONG            fh_Func2;
    LONG            fh_Func3;
    LONG            fh_Args;
    LONG            fh_Arg2;
};
#define fh_Func1 fh_Funcs
#define fh_Arg1  fh_Args

/* The packet vocabulary TCP: answers, values from NDK dos/dosextens.h.  Only
   the actions tcp_handler.c names plus ACTION_INFO, which it must NOT answer
   (Workbench and `Info` walk the DOS list asking every device for one). */
#define ACTION_DIE              5
#define ACTION_LOCATE_OBJECT    8
#define ACTION_READ             'R'
#define ACTION_WRITE            'W'
#define ACTION_WAIT_CHAR        20
#define ACTION_EXAMINE_OBJECT   23
#define ACTION_EXAMINE_NEXT     24
#define ACTION_DISK_INFO        25
#define ACTION_INFO             26
#define ACTION_FLUSH            27
#define ACTION_PARENT           29
#define ACTION_CHANGE_SIGNAL    995
#define ACTION_FINDUPDATE       1004
#define ACTION_FINDINPUT        1005
#define ACTION_FINDOUTPUT       1006
#define ACTION_END              1007
#define ACTION_SEEK             1008
#define ACTION_IS_FILESYSTEM    1027
#define ACTION_EXAMINE_FH       1034

#define LDB_READ    0
#define LDB_WRITE   1
#define LDB_DEVICES 2
#define LDB_VOLUMES 3
#define LDB_ASSIGNS 4
#define LDF_READ    (1L << LDB_READ)
#define LDF_WRITE   (1L << LDB_WRITE)
#define LDF_DEVICES (1L << LDB_DEVICES)
#define LDF_VOLUMES (1L << LDB_VOLUMES)
#define LDF_ASSIGNS (1L << LDB_ASSIGNS)

struct DosList {
    BPTR   dol_Next;
    LONG   dol_Type;
    APTR   dol_Task;
    BPTR   dol_Lock;
    BSTR   dol_Name;
};

#define DLT_DEVICE 0
#endif
