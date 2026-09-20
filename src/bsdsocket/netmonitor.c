/*
 * bsdsocket.library, the network monitoring hooks.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "netmonitor.h"

#include <exec/memory.h>
#include <proto/exec.h>

#ifdef AMINETXDUO_NETMONITOR

_Static_assert(MHT_ICMP        == 0, "MHT_* numbering is the list index");
_Static_assert(MHT_UDP         == 1, "MHT_* numbering is the list index");
_Static_assert(MHT_TCP_Connect == 2, "MHT_* numbering is the list index");
_Static_assert(MHT_Connect     == 3, "MHT_* numbering is the list index");
_Static_assert(MHT_Send        == 4, "MHT_* numbering is the list index");
_Static_assert(MHT_Packet      == 5, "MHT_* numbering is the list index");
_Static_assert(MHT_Bind        == 6, "MHT_* numbering is the list index");

#define BSD_MHT_COUNT   (MHT_Bind + 1)

typedef struct BsdMonitorCall
{
    struct MinNode  bmc_Node;
    struct Task    *bmc_Task;
} BsdMonitorCall;

typedef struct BsdMonitorNode
{
    struct MinNode        bmn_Node;
    struct Hook          *bmn_Hook;
    struct AmiSocketBase *bmn_Owner;
    struct MinList        bmn_Calls;
    struct Task          *bmn_Waiter;
    ULONG                 bmn_WaitMask;
    ULONG                 bmn_Id;
    volatile UWORD        bmn_Refs;
    UBYTE                 bmn_Removed;
} BsdMonitorNode;

static struct MinList bsd_mon_list[BSD_MHT_COUNT];
static BOOL           bsd_mon_ready;
static LONG           bsd_mon_count;
static ULONG          bsd_mon_next_id = 1;

static VOID bsd_mon_newlist(struct MinList *list)
{
    list->mlh_Head     = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail     = NULL;
    list->mlh_TailPred = (struct MinNode *)list;
}

static VOID bsd_mon_setup(VOID)
{
    UWORD i;

    if (bsd_mon_ready)
        return;
    for (i = 0; i < (UWORD)BSD_MHT_COUNT; i++)
        bsd_mon_newlist(&bsd_mon_list[i]);
    bsd_mon_ready = TRUE;
}

BOOL bsd_netmon_busy(VOID)
{
    return (bsd_mon_count > 0) ? TRUE : FALSE;
}

/* Called only under Forbid(). */
static BsdMonitorNode *bsd_mon_find(const struct Hook *hook)
{
    UWORD i;

    for (i = 0; i < (UWORD)BSD_MHT_COUNT; i++)
    {
        struct MinNode *n;

        for (n = bsd_mon_list[i].mlh_Head; n->mln_Succ != NULL;
             n = n->mln_Succ)
        {
            BsdMonitorNode *mn = (BsdMonitorNode *)n;

            if (mn->bmn_Hook == hook)
                return mn;
        }
    }
    return NULL;
}

LONG bsd_AddNetMonitorHookTagList(register LONG type __asm("d0"),
                                  register struct Hook *hook __asm("a0"),
                                  register struct TagItem *tags __asm("a1"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    BsdMonitorNode *mn;

    (VOID)tags;
    if (hook == NULL || hook->h_Entry == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);
    if (type < 0 || type >= BSD_MHT_COUNT ||
        (type != MHT_Connect && type != MHT_Bind && type != MHT_Send))
        return bsd_fail(SocketBase, AMI_EINVAL);

    mn = AllocMem(sizeof(*mn), MEMF_PUBLIC | MEMF_CLEAR);
    if (mn == NULL)
        return bsd_fail(SocketBase, AMI_ENOMEM);
    mn->bmn_Hook  = hook;
    mn->bmn_Owner = SocketBase;
    bsd_mon_newlist(&mn->bmn_Calls);

    Forbid();
    bsd_mon_setup();
    if (bsd_mon_find(hook) != NULL)
    {
        Permit();
        FreeMem(mn, sizeof(*mn));
        return bsd_fail(SocketBase, AMI_EBUSY);
    }
    mn->bmn_Id = bsd_mon_next_id++;
    if (bsd_mon_next_id == 0)
        bsd_mon_next_id = 1;
    AddTail((struct List *)&bsd_mon_list[type], (struct Node *)&mn->bmn_Node);
    bsd_mon_count++;
    Permit();
    return 0;
}

/* Entered under Forbid(); returns after restoring scheduling. */
static VOID bsd_mon_remove_node(BsdMonitorNode *mn,
                                struct AmiSocketBase *base)
{
    struct Task *me = FindTask(NULL);
    BOOL         self = FALSE;
    BOOL         free_now = FALSE;

    Remove((struct Node *)&mn->bmn_Node);
    mn->bmn_Removed = 1;

    if (mn->bmn_Refs == 0)
    {
        if (bsd_mon_count > 0)
            bsd_mon_count--;
        free_now = TRUE;
    }
    else
    {
        struct MinNode *n;

        for (n = mn->bmn_Calls.mlh_Head; n->mln_Succ != NULL;
             n = n->mln_Succ)
        {
            if (((BsdMonitorCall *)n)->bmc_Task == me)
            {
                self = TRUE;
                break;
            }
        }
        if (!self)
        {
            mn->bmn_Waiter   = me;
            mn->bmn_WaitMask = base != NULL ? base->sb_EventSigMask : 0;
        }
    }

    Permit();

    if (!self && !free_now)
    {
        ULONG mask = mn->bmn_WaitMask;

        while (mn->bmn_Refs != 0)
        {
            if (mask != 0)
                (VOID)Wait(mask);
            else
            {
                /* This fallback is only reachable from an invalid master-base
                   use. Yield without letting the wrapper or Hook escape. */
                Forbid();
                Permit();
            }
        }
        free_now = TRUE;
    }

    if (free_now)
        FreeMem(mn, sizeof(*mn));
}

VOID bsd_RemoveNetMonitorHook(register struct Hook *hook __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    BsdMonitorNode *mn;

    if (hook == NULL)
        return;
    Forbid();
    mn = bsd_mon_ready ? bsd_mon_find(hook) : NULL;
    if (mn == NULL)
    {
        Permit();
        return;
    }
    bsd_mon_remove_node(mn, SocketBase);
}

VOID bsd_netmon_drop_owner(struct AmiSocketBase *owner)
{
    for (;;)
    {
        BsdMonitorNode *found = NULL;
        UWORD           i;

        Forbid();
        if (bsd_mon_ready)
        {
            for (i = 0; i < (UWORD)BSD_MHT_COUNT && found == NULL; i++)
            {
                struct MinNode *n;

                for (n = bsd_mon_list[i].mlh_Head; n->mln_Succ != NULL;
                     n = n->mln_Succ)
                {
                    BsdMonitorNode *mn = (BsdMonitorNode *)n;

                    if (mn->bmn_Owner == owner)
                    {
                        found = mn;
                        break;
                    }
                }
            }
        }
        if (found == NULL)
        {
            Permit();
            return;
        }
        bsd_mon_remove_node(found, owner);
    }
}

typedef LONG (*BsdMonitorFn)(register struct Hook *hook __asm("a0"),
                             register APTR reserved __asm("a2"),
                             register APTR message __asm("a1"));

typedef union BsdMonitorEntry
{
    ULONG        (*bme_Raw)(VOID);
    BsdMonitorFn   bme_Fn;
} BsdMonitorEntry;

LONG bsd_netmon_dispatch(LONG type, APTR message)
{
    ULONG last = 0;
    ULONG end;
    LONG  result = 0;

    if (!bsd_mon_ready || bsd_mon_count == 0 ||
        type < 0 || type >= BSD_MHT_COUNT)
        return 0;

    Forbid();
    end = bsd_mon_next_id;
    Permit();

    for (;;)
    {
        BsdMonitorNode *mn = NULL;
        BsdMonitorCall  call;
        BsdMonitorEntry entry;
        struct Hook    *hook;
        BOOL            free_after = FALSE;
        struct MinNode *n;

        Forbid();
        for (n = bsd_mon_list[type].mlh_Head; n->mln_Succ != NULL;
             n = n->mln_Succ)
        {
            BsdMonitorNode *candidate = (BsdMonitorNode *)n;

            if (candidate->bmn_Id > last && candidate->bmn_Id < end)
            {
                mn = candidate;
                break;
            }
        }
        if (mn == NULL)
        {
            Permit();
            break;
        }

        last = mn->bmn_Id;
        call.bmc_Task = FindTask(NULL);
        AddTail((struct List *)&mn->bmn_Calls, (struct Node *)&call.bmc_Node);
        mn->bmn_Refs++;
        hook = mn->bmn_Hook;
        entry.bme_Raw = hook->h_Entry;
        Permit();

        result = entry.bme_Fn(hook, NULL, message);

        Forbid();
        Remove((struct Node *)&call.bmc_Node);
        if (mn->bmn_Refs != 0)
            mn->bmn_Refs--;
        if (mn->bmn_Removed && mn->bmn_Refs == 0)
        {
            if (bsd_mon_count > 0)
                bsd_mon_count--;
            if (mn->bmn_Waiter != NULL && mn->bmn_WaitMask != 0)
                Signal(mn->bmn_Waiter, mn->bmn_WaitMask);
            else
                free_after = TRUE;
        }
        Permit();
        if (free_after)
            FreeMem(mn, sizeof(*mn));
        if (result != 0)
            break;
    }
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

STRPTR bsd_netmon_caller(struct AmiSocketBase *base)
{
    return (base != NULL) ? (STRPTR)base->sb_LogTag : NULL;
}

#else /* !AMINETXDUO_NETMONITOR */

LONG bsd_AddNetMonitorHookTagList(register LONG type __asm("d0"),
                                  register struct Hook *hook __asm("a0"),
                                  register struct TagItem *tags __asm("a1"),
                                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)type; (VOID)hook; (VOID)tags;
    return bsd_fail(SocketBase, AMI_ENOSYS);
}

VOID bsd_RemoveNetMonitorHook(register struct Hook *hook __asm("a0"),
                              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)hook; (VOID)SocketBase;
}

LONG bsd_netmon_dispatch(LONG type, APTR message)
{
    (VOID)type; (VOID)message;
    return 0;
}

BOOL bsd_netmon_have(LONG type)
{
    (VOID)type;
    return FALSE;
}

STRPTR bsd_netmon_caller(struct AmiSocketBase *base)
{
    return (base != NULL) ? (STRPTR)base->sb_LogTag : NULL;
}

BOOL bsd_netmon_busy(VOID)
{
    return FALSE;
}

VOID bsd_netmon_drop_owner(struct AmiSocketBase *owner)
{
    (VOID)owner;
}

#endif /* AMINETXDUO_NETMONITOR */
