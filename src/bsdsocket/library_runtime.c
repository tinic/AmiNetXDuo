/*
 * bsdsocket.library, runtime pieces a shared library has to supply itself.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"

#include "aminetxduo/random.h"

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>

__attribute__((weak)) struct DosLibrary *DOSBase;

__attribute__((weak)) void srand(unsigned int seed)
{
    ami_random_srand(seed);
}

__attribute__((weak)) int rand(void)
{
    return ami_random_rand();
}

/*
 * Called from bsd_lib_init(), that is from InitResident() on the first
 * OpenLibrary(): a normal task context where OpenLibrary() is legal.
 */
BOOL bsd_runtime_open(VOID)
{
    if (DOSBase == NULL)
        DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);

    ami_random_init();

    return (DOSBase != NULL) ? TRUE : FALSE;
}

/*
 * usergroup.library, opened and held for as long as this library is loaded.
 *
 */
static struct Library *bsd_usergroup_base;
static BOOL            bsd_amitcp_tried;

/*
 * AmiTCP: -> SYS:, if nothing else has claimed the name.
 */
static VOID bsd_amitcp_assign(VOID)
{
    struct Process *me;
    APTR            saved;
    BPTR            lock;

    me = (struct Process *)FindTask(NULL);
    if (me == NULL || me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return;

    saved              = me->pr_WindowPtr;
    me->pr_WindowPtr   = (APTR)-1L;
    lock               = Lock((STRPTR)"AmiTCP:", SHARED_LOCK);
    me->pr_WindowPtr   = saved;

    if (lock != 0)
    {
        UnLock(lock);
        return;
    }

    lock = Lock((STRPTR)"SYS:", SHARED_LOCK);
    if (lock == 0)
        return;

    /* AssignLock() takes the lock on success and leaves it on failure. */
    if (AssignLock((STRPTR)"AmiTCP", lock) == DOSFALSE)
        UnLock(lock);
}

VOID bsd_usergroup_open(VOID)
{
    struct Process *me;
    APTR            saved;

    if (!bsd_amitcp_tried)
    {
        bsd_amitcp_tried = TRUE;
        bsd_amitcp_assign();
    }

    if (bsd_usergroup_base != NULL)
        return;

    bsd_usergroup_base = OpenLibrary((STRPTR)"usergroup.library", 0);
    if (bsd_usergroup_base != NULL)
        return;

    me = (struct Process *)FindTask(NULL);
    if (me == NULL || me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return;

    saved = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1L;
    bsd_usergroup_base =
        OpenLibrary((STRPTR)"AmiTCP:libs/usergroup.library", 0);
    me->pr_WindowPtr = saved;
}

VOID bsd_runtime_close(VOID)
{
    /* ami_millis() opened timer.device against a timerequest that is a
       file-scope static in this segment. Expunge is about to UnLoadSeg() it. */
    ami_timer_close();

    if (bsd_usergroup_base != NULL)
    {
        CloseLibrary(bsd_usergroup_base);
        bsd_usergroup_base = NULL;
    }

    bsd_amitcp_tried = FALSE;

    if (DOSBase != NULL)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}
