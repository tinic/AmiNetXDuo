/* Native task-level call boundary for the research bridge.
 * SPDX-License-Identifier: MIT */
#include "tx_bridge_exec.h"
#include <exec/execbase.h>
#include <proto/exec.h>

_Static_assert(__builtin_offsetof(struct ExecBase,TDNestCnt)==TX_AMIGA_OFF_TDNESTCNT,"TDNestCnt ABI");
_Static_assert(__builtin_offsetof(struct ExecBase,AttnResched)==TX_AMIGA_OFF_ATTNRESCHED,"AttnResched ABI");

static void enter(void *arg) { (void)arg; Forbid(); }
static void leave(void *arg) { (void)arg; Permit(); }
static uintptr_t caller(void *arg) { (void)arg; return (uintptr_t)FindTask(0); }
static int can_pause(void *arg,unsigned levels)
{
    (void)arg;
    return levels<=127 && SysBase->IDNestCnt<0 && SysBase->TDNestCnt>=0 &&
        (unsigned)SysBase->TDNestCnt+1==levels;
}
static void panic(void *arg,const char *text)
{
    (void)arg; (void)text;
    /* A violation is a failed disposable guest, never a successful stub. */
    Alert(0x80000001UL);
    for (;;) {}
}

const AnxTxPlatform *anx_tx_exec_platform(void)
{
    static const AnxTxPlatform p={enter,leave,caller,panic,0,can_pause};
    return &p;
}

/* Same tail condition as the existing port; the inline TX_RESTORE already
 * decremented nesting. Re-enter then use Exec's Permit for its dispatch test. */
VOID _tx_amiga_permit_finish(void)
{
    if (SysBase->TDNestCnt<0 && SysBase->IDNestCnt<0 && SysBase->AttnResched) {
        Forbid();
        Permit();
    }
}
