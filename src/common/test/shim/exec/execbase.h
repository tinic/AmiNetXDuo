/*
 * struct ExecBase for the src/common host tests.
 *
 * tests/bsdsocket/host/shim has one already and it is not enough here:
 * ami_random.c's gather_exec() reads VBlankFrequency, PowerSupplyFrequency,
 * ex_EClockFrequency and LastAlert[], which that shim does not carry because
 * nothing in bsdsocket reads them.  The offsets do not have to be the real
 * ones -- nothing here casts a real SysBase -- so this is the fields and not
 * the layout.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_COMMON_TEST_EXEC_EXECBASE_H
#define AMINETXDUO_COMMON_TEST_EXEC_EXECBASE_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/tasks.h>

struct ExecBase
{
    ULONG        IdleCount;
    ULONG        DispCount;
    UWORD        Quantum;
    UWORD        Elapsed;
    UWORD        SysFlags;
    UWORD        AttnFlags;
    UWORD        VBlankFrequency;
    UWORD        PowerSupplyFrequency;
    ULONG        ex_EClockFrequency;
    ULONG        LastAlert[4];
    struct Task *ThisTask;
    struct List  TaskReady;
    struct List  TaskWait;
};

extern struct ExecBase *SysBase;

#endif
