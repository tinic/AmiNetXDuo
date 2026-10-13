/* Installed RAM for the Installer, including allocations, on Exec V37+.
 * SPDX-License-Identifier: MIT
 */
#ifndef NETPROBE_MEMORY_H
#define NETPROBE_MEMORY_H

#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>

extern struct ExecBase *SysBase;

static ULONG netprobe_memory_total(void)
{
    struct MemHeader *mh;
    ULONG total = 0;

    /* MEMF_TOTAL was added in V39. Earlier Exec versions ignore that flag
       and return free memory, so read the managed ranges instead. */
    if (SysBase->LibNode.lib_Version >= 39)
        return AvailMem(MEMF_TOTAL);

    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        total += (ULONG)mh->mh_Upper - (ULONG)mh->mh_Lower;
    Permit();
    return total;
}

#endif
