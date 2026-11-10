/*
 * CreateAmiNetXDuoStatusReport: where each waiting task is, and who holds the
 * stack's two locks.
 *
 * Everything is read, under Forbid(), from memory Exec already owns: the
 * seglists of bsdsocket.library and of the processes in the task list, a copy
 * of each waiting task's saved stack, and the published health mark.  A task
 * row is some time old when its stack is read, so the task is looked up in
 * Exec's lists again first; one that has gone gives no frames.  What a line
 * says is decided in statusreport_text.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

#include <stddef.h>

#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <dos/dosextens.h>

#include "statusreport_text.h"

#include "aminetxduo/health.h"

extern struct ExecBase *SysBase;

static SrRanges sr_code;
static UBYTE    sr_stack[SR_STACK_BYTES];

/* An SrPeek.  Only memory in Exec's own lists is read: a seglist that is in
   ROM, or is not a seglist, ends its walk here. */
static BOOL sr_mem_peek(APTR user, ULONG address, ULONG *value)
{
    (VOID)user;

    if (address == 0UL || (address & 1UL) != 0UL ||
        TypeOfMem((APTR)address) == 0UL)
        return FALSE;

    *value = *(const ULONG *)address;
    return TRUE;
}

/* Under Forbid().  The running task is in neither list. */
static BOOL sr_task_listed(const struct Task *task)
{
    const struct List *lists[2];
    const struct Node *node;
    ULONG              i;

    lists[0] = &SysBase->TaskReady;
    lists[1] = &SysBase->TaskWait;

    for (i = 0; i < 2; i++)
    {
        for (node = lists[i]->lh_Head; node->ln_Succ != NULL;
             node = node->ln_Succ)
        {
            if (node == &task->tc_Node)
                return TRUE;
        }
    }
    return FALSE;
}

/*
 * bsdsocket.library's seglist, from the tag it keeps in its base for a
 * profiler (struct BsdProfSegTag, src/bsdsocket/bsdsocket_internal.h): magic,
 * size, base, seglist, and a fifth longword that brings the sum to zero.
 * Found by scanning, as tools/profiler does; 0 when it is not there.
 */
#define SR_SEGTAG_MAGIC 0x50534731UL    /* 'PSG1' */
#define SR_SEGTAG_SIZE  20UL

static ULONG sr_library_seglist(const struct Library *lib)
{
    const UWORD *p = (const UWORD *)lib;
    const UWORD *end;

    if (lib->lib_PosSize < SR_SEGTAG_SIZE)
        return 0;

    end = (const UWORD *)((const UBYTE *)lib + lib->lib_PosSize - SR_SEGTAG_SIZE);
    for (; p <= end; p++)
    {
        const ULONG *t = (const ULONG *)p;

        if (t[0] == SR_SEGTAG_MAGIC && t[1] == SR_SEGTAG_SIZE &&
            t[2] == (ULONG)lib && t[0] + t[1] + t[2] + t[3] + t[4] == 0UL)
            return t[3];
    }
    return 0;
}

VOID sr_code_begin(VOID)
{
    struct Library *lib;

    sr_ranges_init(&sr_code, sr_mem_peek, NULL);

    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList,
                                     (CONST_STRPTR)"bsdsocket.library");
    if (lib != NULL && tool_stack_is_ours(lib))
        (VOID)sr_ranges_add(&sr_code, "bsdsocket", TRUE,
                            sr_library_seglist(lib));
    Permit();
}

/* A Shell's command is cli_Module; any other process keeps its seglist in
   entry 3 of the pr_SegList array, whose entry 0 is the entry count. */
VOID sr_code_add_process(ULONG task, const char *name)
{
    const struct Process *p = (const struct Process *)task;
    ULONG                 seglist = 0;
    ULONG                 entries;

    Forbid();
    if ((&p->pr_Task == SysBase->ThisTask || sr_task_listed(&p->pr_Task)) &&
        p->pr_Task.tc_Node.ln_Type == NT_PROCESS)
    {
        if (p->pr_CLI != 0 &&
            !sr_mem_peek(NULL, ((ULONG)p->pr_CLI << 2) +
                               offsetof(struct CommandLineInterface, cli_Module),
                         &seglist))
            seglist = 0;

        if (seglist == 0 && p->pr_SegList != 0)
        {
            ULONG array = (ULONG)p->pr_SegList << 2;

            if (!sr_mem_peek(NULL, array, &entries) || entries < 3UL ||
                entries > 64UL ||
                !sr_mem_peek(NULL, array + 12UL, &seglist))
                seglist = 0;
        }

        (VOID)sr_ranges_add(&sr_code, name, FALSE, seglist);
    }
    Permit();
}

VOID sr_code_modules(SrOut *o)
{
    sr_modules(o, &sr_code);
}

VOID sr_code_task_frames(SrOut *o, ULONG index, ULONG task)
{
    const struct Task *t = (const struct Task *)task;
    ULONG              n = 0;

    /* The library's ranges are the first in the table; without them no line
       would print. */
    if (sr_code.count == 0 || !sr_code.r[0].library)
        return;

    Forbid();
    if (sr_task_listed(t))
    {
        ULONG sp    = (ULONG)t->tc_SPReg;
        ULONG lower = (ULONG)t->tc_SPLower;
        ULONG upper = (ULONG)t->tc_SPUpper;

        if ((sp & 1UL) == 0UL && sp >= lower && sp < upper &&
            TypeOfMem((APTR)sp) != 0UL)
        {
            n = upper - sp;
            if (n > SR_STACK_BYTES)
                n = SR_STACK_BYTES;
            CopyMem((APTR)sp, sr_stack, n);
        }
    }
    Permit();

    if (n >= 4UL)
        (VOID)sr_task_frames(o, index, &sr_code, sr_stack, n);
}

/* ----------------------------------------------------------------- locks --- */

VOID sr_locks(SrOut *o)
{
    static AmiBatonStats baton;
    const AmiHealthMark *mark;
    ULONG                holder = 0;
    ULONG                owner  = 0;
    LONG                 nest   = 0;
    LONG                 queue  = 0;
    BOOL                 have   = FALSE;
    BOOL                 lock   = FALSE;

    /* Forbid(), never ObtainSemaphore(): tool_health_mark() says why. */
    Forbid();
    mark = (const AmiHealthMark *)FindSemaphore((STRPTR)AMI_HEALTH_NAME);
    if (mark != NULL &&
        mark->hm_Magic   == AMI_HEALTH_MAGIC &&
        mark->hm_Version == (UWORD)AMI_HEALTH_VERSION &&
        mark->hm_Size    == (UWORD)sizeof(AmiHealthMark) &&
        mark->hm_Baton   != NULL &&
        mark->hm_Holder  != NULL)
    {
        const struct SignalSemaphore *sem =
            (const struct SignalSemaphore *)mark->hm_SbLock;

        baton  = *(const AmiBatonStats *)mark->hm_Baton;
        holder = (ULONG)*(VOID * const *)mark->hm_Holder;
        have   = TRUE;

        if (sem != NULL)
        {
            owner = (ULONG)sem->ss_Owner;
            nest  = (LONG)sem->ss_NestCount;
            queue = (LONG)sem->ss_QueueCount;
            lock  = TRUE;
        }
    }
    Permit();

    if (!have)
    {
        sr_str(o, "baton", NULL);
        sr_str(o, "lock.sb", NULL);
        return;
    }

    sr_hex(o, "baton.holder_task", holder);
    sr_ulong(o, "baton.live", baton.bs_Live);
    sr_ulong(o, "baton.live_max", baton.bs_LiveMax);
    sr_ulong(o, "baton.full", baton.bs_Full);
    sr_ulong(o, "baton.transitions", baton.bs_Transitions);
    sr_ulong(o, "baton.state_max", baton.bs_StateMax);
    sr_ulong(o, "baton.moved", baton.bs_BatonMoved);
    sr_ulong(o, "baton.state_shared", baton.bs_StateShared);
    sr_ulong(o, "baton.reclaimed", baton.bs_Reclaimed);

    if (!lock)
    {
        sr_str(o, "lock.sb", NULL);
        return;
    }

    /* ss_QueueCount is -1 when nobody holds it and 0 with an owner and no
       waiter. */
    sr_hex(o, "lock.sb.owner_task", owner);
    sr_long(o, "lock.sb.nest", nest);
    sr_long(o, "lock.sb.queue", queue);
}
