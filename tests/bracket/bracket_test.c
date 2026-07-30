/*
 * AmiNetXDuo -- the ThreadX/Exec adoption layer, on its own.
 *
 * WHY THIS EXISTS
 *
 * The defect of docs/RESEARCH.md 79 was one pointer read: ami_netstack_enter()
 * asked tx_thread_identify() whether the caller was already inside, and that
 * returns _tx_thread_current_ptr, which on this port is the GLOBAL baton holder
 * rather than an answer about the caller. A second Task arriving while the
 * first held the baton read "already a thread", skipped adoption and entered
 * NetX Duo unbracketed. Two Exec Tasks inside the stack at once, on a machine
 * with no memory protection.
 *
 * It survived every automated harness, and the reason is worth stating: the
 * tests that are concurrent do not go through this layer, and the tests that go
 * through this layer are not concurrent. tests/soak has four adopted Tasks and
 * deliberate adopt/orphan churn -- and never asks the question this file asks.
 *
 * So this tests the layer itself: no sockets, no NetX Duo, no SANA-II driver,
 * no interface. That is not minimalism for its own sake. It means this can run
 * in public CI, where anything reaching bsdsocket.library through its LVOs
 * cannot, because those need a2065.device and Commodore's driver is not
 * redistributable.
 *
 * WHAT IT ASSERTS
 *
 * The invariant that failed, first and by name: while one Task holds the baton,
 * an unrelated Task must be told it does NOT. Everything else here is secondary
 * to that one, and t_baton_is_not_shared() can fail against the old code --
 * which is the property that makes it a test rather than a description.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * tx_api.h first, before any NDK header: tx_port.h typedefs VOID, CHAR and
 * UCHAR itself, and exec/types.h getting there first makes those a redefinition
 * rather than a match. tests/soak/soak_test.c orders them the same way.
 */
#include "tx_api.h"
#include "tx_amiga.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/lists.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* --------------------------------------------------------------- the shape -- */

#ifndef BT_WORKERS
#define BT_WORKERS      6           /* churn phase: unrelated Exec Tasks      */
#endif
#ifndef BT_ROUNDS
#define BT_ROUNDS       200         /* adopt/orphan cycles per worker         */
#endif

#define BT_STACK        4096UL
#define BT_PRI          0

#define BT_SIG_GO       SIGF_SINGLE

/* ------------------------------------------------------------- the reporting -- */

static ULONG t_checks;
static ULONG t_failures;

/*
 * Flushed per line. The emulator runner reads stdout out of a file after the
 * run, so an unflushed line is a line that does not exist if the program wedges
 * -- and wedging is one of the things this file is here to catch. A sibling
 * harness lost two emulator runs to exactly that.
 */
static VOID t_log(const char *fmt, LONG a, LONG b)
{
    LONG args[2];

    args[0] = a;
    args[1] = b;
    (VOID)VPrintf((STRPTR)fmt, args);
    (VOID)Flush(Output());
}

static VOID t_check(LONG ok, const char *what, LONG detail)
{
    t_checks++;
    if (ok)
    {
        t_log("  ok   %s\n", (LONG)what, 0);
    }
    else
    {
        t_failures++;
        t_log("  FAIL %s (%ld)\n", (LONG)what, detail);
    }
}

/* ----------------------------------------------------------- spawning tasks -- */

/*
 * The stack is a SEPARATE AllocMem() from the task structure, and the MemList
 * covers only the task: RemTask() frees the task's own MemList entries, and a
 * list covering both frees one address twice. tests/soak/soak_test.c carries
 * the full account of what that looked like -- AN_FreeTwice when Exec noticed,
 * recycled memory executing as code when it did not.
 */
typedef struct BtTask
{
    struct Task    *bt_Task;
    APTR            bt_Stack;
    ULONG           bt_StackSize;
    struct Task    *bt_Parent;

    volatile UWORD  bt_Ready;       /* adopted and holding, for the rendezvous */
    volatile UWORD  bt_Done;
    volatile LONG   bt_Failures;
    volatile LONG   bt_Rounds;
    volatile LONG   bt_Saw;         /* what caller_is_thread() said, for a log */

    TX_THREAD       bt_Thread;
} BtTask;

static BtTask bt_worker[BT_WORKERS];
static BtTask bt_holder;

static VOID bt_newlist(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = (struct Node *)0;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static struct Task *bt_spawn(BtTask *bt, VOID (*entry)(VOID), const char *name)
{
    struct MemList *memlist;
    struct Task    *task;
    ULONG           tsize = (ULONG)sizeof(struct Task);

    bt->bt_StackSize = BT_STACK;
    bt->bt_Stack     = AllocMem(bt->bt_StackSize, MEMF_PUBLIC | MEMF_CLEAR);
    if (bt->bt_Stack == NULL)
        return NULL;

    memlist = (struct MemList *)AllocMem((ULONG)sizeof(struct MemList),
                                         MEMF_PUBLIC | MEMF_CLEAR);
    if (memlist == NULL)
    {
        FreeMem(bt->bt_Stack, bt->bt_StackSize);
        bt->bt_Stack = NULL;
        return NULL;
    }

    task = (struct Task *)AllocMem(tsize, MEMF_PUBLIC | MEMF_CLEAR);
    if (task == NULL)
    {
        FreeMem((APTR)memlist, (ULONG)sizeof(struct MemList));
        FreeMem(bt->bt_Stack, bt->bt_StackSize);
        bt->bt_Stack = NULL;
        return NULL;
    }

    memlist->ml_NumEntries      = 1;
    memlist->ml_ME[0].me_Addr   = (APTR)task;
    memlist->ml_ME[0].me_Length = tsize;

    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri  = BT_PRI;
    task->tc_Node.ln_Name = (char *)name;
    task->tc_SPLower      = bt->bt_Stack;
    task->tc_SPUpper      = (APTR)(((UBYTE *)bt->bt_Stack) + bt->bt_StackSize);
    task->tc_SPReg        = task->tc_SPUpper;
    task->tc_UserData     = (APTR)bt;

    bt_newlist(&task->tc_MemEntry);
    AddTail(&task->tc_MemEntry, (struct Node *)memlist);

    if (AddTask(task, (APTR)entry, (APTR)0) == NULL)
    {
        FreeMem((APTR)task, tsize);
        FreeMem((APTR)memlist, (ULONG)sizeof(struct MemList));
        FreeMem(bt->bt_Stack, bt->bt_StackSize);
        bt->bt_Stack = NULL;
        return NULL;
    }

    bt->bt_Task = task;
    return task;
}

/* ------------------------------------------------------- the holder rendezvous -- */

/*
 * Adopt, say so, and keep holding until released. Nothing else: the point is to
 * have exactly one Task legitimately inside while somebody else asks the
 * question.
 */
static VOID bt_holder_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);

    if (tx_amiga_adopt_thread(&bt->bt_Thread, (CHAR *)"bracket holder", 20)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt->bt_Done = 1U;
        Signal(bt->bt_Parent, BT_SIG_GO);
        return;
    }

    /* Adopted: from here until the orphan below, this Task is the baton. */
    if (tx_amiga_caller_is_thread() == (UINT)TX_FALSE)
        bt->bt_Failures++;              /* the holder must see itself */

    bt->bt_Ready = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);

    Wait(BT_SIG_GO);                    /* held until main has looked */

    (VOID)tx_amiga_orphan_thread(&bt->bt_Thread);
    bt->bt_Done = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);
}

/* ------------------------------------------------------------ the churn body -- */

/*
 * Adopt and orphan in a loop, checking the layer's answer either side. Under
 * BT_WORKERS Tasks doing this at once the baton changes hands constantly, which
 * is the state in which the old code answered the wrong question.
 */
static VOID bt_worker_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;
    LONG         i;

    Wait(BT_SIG_GO);

    for (i = 0; i < BT_ROUNDS; i++)
    {
        /* Not adopted yet: whatever any other Task is doing, the answer for
           THIS Task is no. This is the assertion the released defect broke. */
        if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
        {
            bt->bt_Failures++;
            bt->bt_Saw = 1;
            break;
        }

        if (tx_amiga_adopt_thread(&bt->bt_Thread, (CHAR *)"bracket worker", 20)
            != TX_SUCCESS)
        {
            bt->bt_Failures++;
            break;
        }

        if (tx_amiga_caller_is_thread() == (UINT)TX_FALSE)
        {
            bt->bt_Failures++;          /* adopted and not recognised */
            (VOID)tx_amiga_orphan_thread(&bt->bt_Thread);
            break;
        }

        if (tx_amiga_adopted_thread() != &bt->bt_Thread)
        {
            bt->bt_Failures++;          /* somebody else's TX_THREAD */
            (VOID)tx_amiga_orphan_thread(&bt->bt_Thread);
            break;
        }

        if (tx_amiga_orphan_thread(&bt->bt_Thread) != TX_SUCCESS)
        {
            bt->bt_Failures++;
            break;
        }

        if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
        {
            bt->bt_Failures++;          /* orphaned and still counted in */
            break;
        }

        bt->bt_Rounds = i + 1;
    }

    bt->bt_Done = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);
}

/*
 * ThreadX calls this from tx_kernel_enter() and the link fails without it. This
 * layer is what is under test, so there is nothing to define: every thread here
 * arrives by adoption from an Exec Task, which is the whole point.
 */
VOID tx_application_define(VOID *first_unused_memory)
{
    (VOID)first_unused_memory;
}

/* ------------------------------------------------------------------- the main -- */

static VOID bt_reap(BtTask *bt)
{
    if (bt->bt_Stack != NULL)
    {
        /* RemTask() freed the task and its MemList when the entry returned;
           the stack was deliberately not on that list. */
        FreeMem(bt->bt_Stack, bt->bt_StackSize);
        bt->bt_Stack = NULL;
    }
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    UINT         status;
    UWORD        i;
    UWORD        spawned = 0;
    LONG         rounds  = 0;

    (VOID)argc; (VOID)argv;

    t_log("bracket: the ThreadX/Exec adoption layer, %ld workers, %ld rounds\n",
          (LONG)BT_WORKERS, (LONG)BT_ROUNDS);

    status = tx_amiga_kernel_start();
    t_check(status == TX_SUCCESS, "ThreadX kernel started", (LONG)status);
    if (status != TX_SUCCESS)
    {
        t_log("%ld checks, %ld failures -- FAIL\n", (LONG)t_checks,
              (LONG)t_failures);
        return 20;
    }

    /* main() has adopted nothing, and nothing else is running yet. */
    t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
            "an unadopted Task is not the baton holder", 0);

    /* ---- the regression case, on its own and deterministic -------------- */

    bt_holder.bt_Parent = me;
    SetSignal(0, BT_SIG_GO);

    t_check(bt_spawn(&bt_holder, bt_holder_entry, "bracket-holder") != NULL,
            "spawned the holder Task", 0);

    if (bt_holder.bt_Task != NULL)
    {
        Signal(bt_holder.bt_Task, BT_SIG_GO);
        Wait(BT_SIG_GO);                /* until it is adopted and holding */

        t_check(bt_holder.bt_Ready != 0U, "holder adopted a thread", 0);

        /*
         * THE ONE THAT MATTERS. Another Task holds the baton right now. main()
         * has adopted nothing, so the layer must say no. The released defect
         * said yes here, and everything downstream -- a caller skipping
         * adoption and entering NetX Duo unbracketed -- followed from it.
         */
        t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
                "another Task holding the baton does not make us a thread", 0);

        t_check(tx_amiga_adopted_thread() != &bt_holder.bt_Thread,
                "we are not handed somebody else's TX_THREAD", 0);

        Signal(bt_holder.bt_Task, BT_SIG_GO);   /* release it */
        Wait(BT_SIG_GO);

        t_check(bt_holder.bt_Failures == 0, "holder saw itself as the baton",
                bt_holder.bt_Failures);

        t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
                "still not a thread once the holder has orphaned", 0);

        bt_reap(&bt_holder);
    }

    /* ---- the churn, where the baton changes hands under load ------------ */

    SetSignal(0, BT_SIG_GO);

    for (i = 0; i < BT_WORKERS; i++)
    {
        bt_worker[i].bt_Parent = me;
        if (bt_spawn(&bt_worker[i], bt_worker_entry, "bracket-worker") != NULL)
            spawned++;
    }
    t_check(spawned == BT_WORKERS, "spawned every worker", (LONG)spawned);

    for (i = 0; i < BT_WORKERS; i++)
        if (bt_worker[i].bt_Task != NULL)
            Signal(bt_worker[i].bt_Task, BT_SIG_GO);

    /*
     * Poll the done flags rather than counting signals. Exec signals are BITS,
     * not counters: six workers setting SIGF_SINGLE can coalesce into one wake,
     * and a Wait() per worker then blocks for ever on a signal that was already
     * merged. This harness did exactly that and hung in four runs out of five,
     * which read like a defect in the layer under test.
     */
    {
        ULONG waited = 0;

        for (;;)
        {
            UWORD done = 0;

            for (i = 0; i < BT_WORKERS; i++)
                if (bt_worker[i].bt_Task == NULL || bt_worker[i].bt_Done != 0U)
                    done++;

            if (done >= BT_WORKERS)
                break;

            if (waited >= (ULONG)(60 * 50))
            {
                for (i = 0; i < BT_WORKERS; i++)
                    if (bt_worker[i].bt_Task != NULL && bt_worker[i].bt_Done == 0U)
                        t_log("  WEDGED worker %ld after %ld rounds\n",
                              (LONG)i, bt_worker[i].bt_Rounds);
                t_check(0, "every worker finished inside the deadline",
                        (LONG)waited);
                break;
            }

            Delay(5);
            waited += 5;
        }
    }

    for (i = 0; i < BT_WORKERS; i++)
    {
        if (bt_worker[i].bt_Task == NULL)
            continue;

        t_check(bt_worker[i].bt_Failures == 0, "worker kept every invariant",
                bt_worker[i].bt_Failures);
        rounds += bt_worker[i].bt_Rounds;
        bt_reap(&bt_worker[i]);
    }

    t_check(rounds == (LONG)BT_WORKERS * BT_ROUNDS,
            "every worker finished every round", rounds);

    /* And main is still what it was before any of that. */
    t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
            "unadopted after the churn, as before it", 0);

    t_log("%ld checks, %ld failures -- ", (LONG)t_checks, (LONG)t_failures);
    t_log("%s\n", (LONG)((t_failures == 0UL) ? "PASS" : "FAIL"), 0);

    return (t_failures == 0UL) ? 0 : 20;
}
