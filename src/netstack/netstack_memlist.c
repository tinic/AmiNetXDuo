/*
 * AmiNetXDuo, Exec's memory list for the packet pool: which Fast RAM
 * headers there are and at what priority, so the pool is sized from the
 * fastest class of memory and not from everything (pool.h).
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/pool.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>

extern struct ExecBase *SysBase;

ULONG ami_ns_fast_headers(LONG *pri, ULONG *free, ULONG max)
{
    struct MemHeader *mh;
    ULONG             n = 0;

    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL && n < max;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
    {
        if ((mh->mh_Attributes & MEMF_FAST) == 0)
            continue;
        pri[n]  = (LONG)mh->mh_Node.ln_Pri;
        free[n] = mh->mh_Free;
        n++;
    }
    Permit();

    return n;
}
