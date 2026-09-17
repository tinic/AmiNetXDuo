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
 * Three places, in order.  A self-contained installation ships its own copy
 * under AmiNetXDuo:Libs, and that drawer is the LAST member of LIBS:, so a
 * bare name would open AmiTCP's or Roadshow's copy from the system first.
 * OpenLibrary() given a path strips it to look through the libraries already
 * in memory, so a usergroup.library that is already loaded is the one every
 * caller gets whichever name is used.  Then the bare name, which is LIBS:.
 * Then where AmiTCP keeps it.  The two named paths run with requesters off:
 * on a system install neither assign need exist.
 */
static struct Library *bsd_usergroup_base;

static struct Library *bsd_usergroup_try(const char *path)
{
    struct Process *me;
    APTR            saved;
    struct Library *base;

    me = (struct Process *)FindTask(NULL);
    if (me == NULL || me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
        return NULL;

    saved = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1L;
    base = OpenLibrary((STRPTR)path, 0);
    me->pr_WindowPtr = saved;

    return base;
}

VOID bsd_usergroup_open(VOID)
{
    if (bsd_usergroup_base != NULL)
        return;

    bsd_usergroup_base = bsd_usergroup_try("AmiNetXDuo:Libs/usergroup.library");
    if (bsd_usergroup_base != NULL)
        return;

    bsd_usergroup_base = OpenLibrary((STRPTR)"usergroup.library", 0);
    if (bsd_usergroup_base != NULL)
        return;

    bsd_usergroup_base = bsd_usergroup_try("AmiTCP:libs/usergroup.library");
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

    if (DOSBase != NULL)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}
