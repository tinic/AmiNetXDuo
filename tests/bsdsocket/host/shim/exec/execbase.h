/* struct ExecBase, for the bsdsocket host tests.  Only the fields the tree
   reads; the layout before them is padded so their offsets are the real ones.
   SPDX-License-Identifier: MIT */
#ifndef AMINETXDUO_BSD_TEST_EXEC_EXECBASE_H
#define AMINETXDUO_BSD_TEST_EXEC_EXECBASE_H
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/tasks.h>

struct ExecBase {
    struct Library  LibNode;
    UWORD           SoftVer;
    WORD            LowMemChkSum;
    ULONG           ChkBase;
    APTR            ColdCapture;
    APTR            CoolCapture;
    APTR            WarmCapture;
    APTR            SysStkUpper;
    APTR            SysStkLower;
    ULONG           MaxLocMem;
    APTR            DebugEntry;
    APTR            DebugData;
    APTR            AlertData;
    APTR            MaxExtMem;
    UWORD           ChkSum;
    UBYTE           pad0[12 * 6];
    struct Task    *ThisTask;
    ULONG           IdleCount;
    ULONG           DispCount;
    UWORD           Quantum;
    UWORD           Elapsed;
    UWORD           SysFlags;
    BYTE            IDNestCnt;
    BYTE            TDNestCnt;
    UWORD           AttnFlags;
};

#define AFF_68010   (1<<0)
#define AFF_68020   (1<<1)
#define AFF_68030   (1<<2)
#define AFF_68040   (1<<3)
#define AFF_68881   (1<<4)
#define AFF_68882   (1<<5)
#define AFF_FPU40   (1<<6)
#define AFF_68060   (1<<7)

#endif
