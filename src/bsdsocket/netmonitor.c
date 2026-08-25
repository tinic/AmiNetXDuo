/*
 * bsdsocket.library, the network monitoring hooks.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "netmonitor.h"

#include <proto/exec.h>

/*
 * One list per type. MHT_ICMP is 0 and MHT_Bind is 6, so the type doubles as
 * the index. The _Static_asserts below check that, because a renumbering
 * otherwise files hooks under the wrong kind.
 */
_Static_assert(MHT_ICMP        == 0, "MHT_* numbering is the list index");
_Static_assert(MHT_UDP         == 1, "MHT_* numbering is the list index");
_Static_assert(MHT_TCP_Connect == 2, "MHT_* numbering is the list index");
_Static_assert(MHT_Connect     == 3, "MHT_* numbering is the list index");
_Static_assert(MHT_Send        == 4, "MHT_* numbering is the list index");
_Static_assert(MHT_Packet      == 5, "MHT_* numbering is the list index");
_Static_assert(MHT_Bind        == 6, "MHT_* numbering is the list index");

#define BSD_MHT_COUNT   (MHT_Bind + 1)

static struct MinList bsd_mon_list[BSD_MHT_COUNT];
static BOOL           bsd_mon_ready;
static LONG           bsd_mon_count;

/*
 * The lists are walked from the SANA-II receive path and from socket calls on
 * any task. A hook install mutates them. Forbid() rather than a semaphore: the
 * in-stack dispatch runs where blocking is not allowed, and the critical
 * sections are a few instructions.
 */
static VOID bsd_mon_setup(VOID)
{
    UWORD i;

    if (bsd_mon_ready)
        return;

    for (i = 0; i < (UWORD)BSD_MHT_COUNT; i++)
    {
        /* NewMinList(), open-coded: amiga.lib is not available here. */
        bsd_mon_list[i].mlh_Head     = (struct MinNode *)&bsd_mon_list[i].mlh_Tail;
        bsd_mon_list[i].mlh_Tail     = NULL;
        bsd_mon_list[i].mlh_TailPred = (struct MinNode *)&bsd_mon_list[i];
    }

    bsd_mon_ready = TRUE;
}

BOOL bsd_netmon_busy(VOID)
{
    return (bsd_mon_count > 0) ? TRUE : FALSE;
}

/* Is this hook already in any list? Called under Forbid(). */
static BOOL bsd_mon_installed(const struct Hook *hook)
{
    UWORD i;

    for (i = 0; i < (UWORD)BSD_MHT_COUNT; i++)
    {
        struct MinNode *node;

        for (node = bsd_mon_list[i].mlh_Head; node->mln_Succ != NULL;
             node = node->mln_Succ)
        {
            if ((const struct Hook *)node == hook)
                return TRUE;
        }
    }

    return FALSE;
}

LONG bsd_AddNetMonitorHookTagList(register LONG type __asm("d0"),
                                  register struct Hook *hook __asm("a0"),
                                  register struct TagItem *tags __asm("a1"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)tags;

    if (hook == NULL || hook->h_Entry == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    /*
     * "[EINVAL] The monitor type is not supported."
     */
    if (type < 0 || type >= BSD_MHT_COUNT)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (type != MHT_Connect && type != MHT_Bind && type != MHT_Send)
        return bsd_fail(SocketBase, AMI_EINVAL);

    Forbid();

    bsd_mon_setup();

    if (bsd_mon_installed(hook))
    {
        Permit();
        return bsd_fail(SocketBase, AMI_EBUSY);
    }

    AddTail((struct List *)&bsd_mon_list[type], (struct Node *)&hook->h_MinNode);
    bsd_mon_count++;

    Permit();

    return 0;
}

VOID bsd_RemoveNetMonitorHook(register struct Hook *hook __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    if (hook == NULL)
        return;

    Forbid();

    /*
     * Searched rather than trusted. Remove() on a node that is not in a list
     * writes through whatever mln_Succ and mln_Pred happen to hold. A remove
     * that takes no type invites a caller to remove the same hook twice.
     */
    if (bsd_mon_ready && bsd_mon_installed(hook))
    {
        Remove((struct Node *)&hook->h_MinNode);
        bsd_mon_count--;
    }

    Permit();
}

/*
 * The hook itself, called the way the autodoc lays it out: the Hook in A0,
 * NULL in A2, the message in A1. Declared with register annotations rather
 */
typedef LONG (*BsdMonitorFn)(register struct Hook *hook __asm("a0"),
                             register APTR reserved __asm("a2"),
                             register APTR message __asm("a1"));

/*
 * utility/hooks.h declares h_Entry as `ULONG (*)()`, no parameters, because
 * a Hook carries whatever shape the installer agreed on. The real shape is a
 */
typedef union BsdMonitorEntry
{
    ULONG        (*bme_Raw)(VOID);
    BsdMonitorFn   bme_Fn;
} BsdMonitorEntry;

/*
 * Every hook of one type, in install order. Stops at the first that does not
 * answer 0.
 */
LONG bsd_netmon_dispatch(LONG type, APTR message)
{
    struct MinNode *node;
    LONG            result = 0;

    if (!bsd_mon_ready || bsd_mon_count == 0 ||
        type < 0 || type >= BSD_MHT_COUNT)
        return 0;

    Forbid();

    for (node = bsd_mon_list[type].mlh_Head; node->mln_Succ != NULL;
         node = node->mln_Succ)
    {
        struct Hook     *hook = (struct Hook *)node;
        BsdMonitorEntry  entry;

        entry.bme_Raw = hook->h_Entry;

        result = entry.bme_Fn(hook, NULL, message);
        if (result != 0)
            break;
    }

    Permit();

    return result;
}

BOOL bsd_netmon_have(LONG type)
{
    BOOL any;

    if (!bsd_mon_ready || bsd_mon_count == 0 ||
        type < 0 || type >= BSD_MHT_COUNT)
        return FALSE;

    Forbid();
    any = (bsd_mon_list[type].mlh_Head->mln_Succ != NULL) ? TRUE : FALSE;
    Permit();

    return any;
}

/*
 * The name a program chose to be known by, for the cmm_Caller/bmm_Caller
 * field. SBTC_LOGTAGPTR is the only place an application can say what it is
 */
STRPTR bsd_netmon_caller(struct AmiSocketBase *base)
{
    return (base != NULL) ? (STRPTR)base->sb_LogTag : NULL;
}
