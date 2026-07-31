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
#include <exec/io.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstack.h"

/* The bracket under test, and the counter ThreadX reads to decide whether it is
   in an interrupt.  Declared here rather than pulled in from tx_thread.h, which
   wants TX_SOURCE_CODE. */
VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);
extern volatile ULONG _tx_thread_system_state;

/* --------------------------------------------------------------- the shape -- */

#ifndef BT_WORKERS
#define BT_WORKERS      6           /* churn phase: unrelated Exec Tasks      */
#endif
#ifndef BT_ROUNDS
#define BT_ROUNDS       200         /* adopt/orphan cycles per worker         */
#endif

#ifndef BT_BATON_TASKS
#define BT_BATON_TASKS  3           /* baton phase: as many as a SANA-II iface */
#endif
#ifndef BT_BATON_TICKS
#define BT_BATON_TICKS  250         /* how long the baton phase runs, in ticks */
#endif

#define BT_STACK        4096UL
#define BT_PRI          0
#define BT_PROBE_PRI    5           /* above every Task the phase creates      */
#define BT_PROBE_MICRO  2000UL      /* probe period; 500 samples a second      */

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

/*
 * How far the holder got. It runs as a plain Task, so it cannot print -- and a
 * bounded wait that says nothing is no better than the unbounded one it
 * replaced.
 */
static volatile ULONG bt_mark;

static VOID bt_newlist(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = (struct Node *)0;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static struct Task *bt_spawn(BtTask *bt, VOID (*entry)(VOID), const char *name,
                             BYTE pri)
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
    task->tc_Node.ln_Pri  = pri;
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

    bt_mark = 1UL;
    Wait(BT_SIG_GO);                    /* held until main has looked */

    bt_mark = 2UL;
    (VOID)tx_amiga_orphan_thread(&bt->bt_Thread);
    bt_mark = 3UL;
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

/* ------------------------------------------ the shared interrupt state -- */

/*
 * _tx_thread_system_state is one global counter, and every Task in the machine
 * reads it. ThreadX treats a non-zero value as "an ISR is running", and every
 * _tx_thread_system_return() is behind TX_THREAD_SYSTEM_RETURN_CHECK, which
 * tests it. So a Task that reaches _tx_thread_system_suspend() while somebody
 * else has the counter raised does not suspend: it returns still linked into
 * the object's suspension list, with the object's suspended count already
 * incremented. List and count then disagree, and the next
 * _tx_event_flags_set() walks past the end of the list into offset 0x80 of
 * address zero -- the Enforcer hits this phase exists for.
 *
 * The port's rule is therefore that the counter may only be raised under an
 * unbroken Forbid(): tx_thread_context_save.c states it, tx_amiga_adopt.c
 * repeats it. netstack_baton.c raised it and then Permit()ed, which is the
 * defect.
 *
 * Catching that needs an observer that runs INSIDE the window, and no Task can
 * poll its way in -- Exec will not switch between two Tasks of the same
 * priority except on a quantum boundary. The probe below is dispatched the way
 * the SANA-II readers are on real hardware: it is the highest-priority Task in
 * the phase and it wakes on a device interrupt, so Exec dispatches it at the
 * first instruction where switching is allowed. Under the old code that is the
 * Permit() in the middle of the bracket.
 *
 * Two independent detectors, because they catch it from opposite sides:
 *
 *   bt_probe_shared      the probe ran while the counter was raised
 *   bs_StateShared       a Task entering the bracket found it already raised
 *
 * Both must be zero. Under the old code both fire within the first second.
 */

static volatile ULONG bt_probe_samples;
static volatile ULONG bt_probe_shared;
static volatile ULONG bt_probe_worst;
static volatile UWORD bt_phase_stop;

static BtTask bt_probe;
static BtTask bt_baton[BT_BATON_TASKS];

static VOID bt_probe_entry(VOID)
{
    struct Task        *me = FindTask(NULL);
    BtTask             *bt = (BtTask *)me->tc_UserData;
    struct MsgPort     *port;
    struct timerequest *tr;

    port = CreateMsgPort();
    tr   = (port != NULL)
         ? (struct timerequest *)CreateIORequest(port,
                                                 (ULONG)sizeof(struct timerequest))
         : NULL;

    if (tr == NULL ||
        OpenDevice((CONST_STRPTR)"timer.device", (ULONG)UNIT_MICROHZ,
                   (struct IORequest *)tr, 0UL) != 0)
    {
        bt->bt_Failures++;
        if (tr != NULL)
            DeleteIORequest((struct IORequest *)tr);
        if (port != NULL)
            DeleteMsgPort(port);
        bt->bt_Done = 1U;
        Signal(bt->bt_Parent, BT_SIG_GO);
        return;
    }

    while (bt_phase_stop == 0U)
    {
        ULONG state;

        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs    = 0UL;
        tr->tr_time.tv_micro   = BT_PROBE_MICRO;
        (VOID)DoIO((struct IORequest *)tr);

        /* First thing after the wake, before anything can lower it again. */
        state = _tx_thread_system_state;

        bt_probe_samples++;
        if (state != 0UL)
        {
            bt_probe_shared++;
            if (state > bt_probe_worst)
                bt_probe_worst = state;
        }
    }

    CloseDevice((struct IORequest *)tr);
    DeleteIORequest((struct IORequest *)tr);
    DeleteMsgPort(port);

    bt->bt_Done = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);
}

/*
 * A SANA-II reader, with the device taken out: adopt once, then hand the baton
 * back and take it again for as long as the phase lasts. That is exactly what
 * ami_sana2_block_enter()/leave() do around the Wait() for an IORequest.
 */
static VOID bt_baton_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;
    LONG         rounds = 0;

    Wait(BT_SIG_GO);

    if (tx_amiga_adopt_thread(&bt->bt_Thread, (CHAR *)"bracket baton", 20)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt->bt_Done = 1U;
        Signal(bt->bt_Parent, BT_SIG_GO);
        return;
    }

    while (bt_phase_stop == 0U)
    {
        ami_netstack_baton_release();

        /* Where the reader's Wait() for a packet goes. Nothing here may touch
           ThreadX: the bracket has taken this Task off the ready list. */
        if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
            bt->bt_Failures++;

        ami_netstack_baton_acquire();

        if (tx_amiga_adopted_thread() != &bt->bt_Thread)
            bt->bt_Failures++;

        rounds++;
        bt->bt_Rounds = rounds;
    }

    (VOID)tx_amiga_orphan_thread(&bt->bt_Thread);

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

/*
 * Poll a flag with a deadline rather than Wait() on a signal. Signals are bits:
 * two set before the first Wait() arrive as one, and the second Wait() then
 * blocks for ever on a wake that already happened. Reporting how far the holder
 * got is the difference between a diagnosis and a harness that stops.
 */
static VOID bt_wait_for(volatile UWORD *flag, const char *what)
{
    ULONG waited = 0;

    while (*flag == 0U)
    {
        if (waited >= (ULONG)(30 * 50))
        {
            t_check(0, what, (LONG)bt_mark);
            return;
        }
        Delay(5);
        waited += 5;
    }
}

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

    t_check(bt_spawn(&bt_holder, bt_holder_entry, "bracket-holder", BT_PRI) != NULL,
            "spawned the holder Task", 0);

    if (bt_holder.bt_Task != NULL)
    {
        Signal(bt_holder.bt_Task, BT_SIG_GO);
        bt_wait_for(&bt_holder.bt_Ready, "the holder to adopt");

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
        bt_wait_for(&bt_holder.bt_Done, "the holder to orphan");

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
        if (bt_spawn(&bt_worker[i], bt_worker_entry, "bracket-worker", BT_PRI) != NULL)
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

    /* ---- the bracket, watched from inside its own window ---------------- */

    SetSignal(0, BT_SIG_GO);
    bt_phase_stop = 0U;
    spawned       = 0;

    bt_probe.bt_Parent = me;
    t_check(bt_spawn(&bt_probe, bt_probe_entry, "bracket-probe",
                     BT_PROBE_PRI) != NULL, "spawned the probe Task", 0);

    for (i = 0; i < BT_BATON_TASKS; i++)
    {
        bt_baton[i].bt_Parent = me;
        if (bt_spawn(&bt_baton[i], bt_baton_entry, "bracket-baton", BT_PRI) != NULL)
            spawned++;
    }
    t_check(spawned == BT_BATON_TASKS, "spawned every baton Task", (LONG)spawned);

    for (i = 0; i < BT_BATON_TASKS; i++)
        if (bt_baton[i].bt_Task != NULL)
            Signal(bt_baton[i].bt_Task, BT_SIG_GO);

    Delay(BT_BATON_TICKS);
    bt_phase_stop = 1U;

    {
        ULONG waited = 0;

        for (;;)
        {
            UWORD done = (UWORD)((bt_probe.bt_Task == NULL ||
                                  bt_probe.bt_Done != 0U) ? 1 : 0);

            for (i = 0; i < BT_BATON_TASKS; i++)
                if (bt_baton[i].bt_Task == NULL || bt_baton[i].bt_Done != 0U)
                    done++;

            if (done >= (UWORD)(BT_BATON_TASKS + 1))
                break;

            if (waited >= (ULONG)(60 * 50))
            {
                t_check(0, "the baton phase finished inside the deadline",
                        (LONG)waited);
                break;
            }

            Delay(5);
            waited += 5;
        }
    }

    rounds = 0;
    for (i = 0; i < BT_BATON_TASKS; i++)
    {
        if (bt_baton[i].bt_Task == NULL)
            continue;
        t_check(bt_baton[i].bt_Failures == 0,
                "baton Task kept every invariant", bt_baton[i].bt_Failures);
        rounds += bt_baton[i].bt_Rounds;
        bt_reap(&bt_baton[i]);
    }
    t_check(rounds > 0, "the baton actually changed hands", rounds);

    t_check(bt_probe.bt_Failures == 0, "the probe opened timer.device",
            bt_probe.bt_Failures);

    /* Without this, zero violations would mean nothing: a probe that never ran
       cannot see one. */
    t_check(bt_probe_samples >= 100UL, "the probe sampled often enough",
            (LONG)bt_probe_samples);

    /*
     * THE ONE THAT MATTERS. Every one of these is a moment where ThreadX was
     * told an interrupt was in progress while Exec was free to dispatch any
     * Task in the machine -- and the Task it dispatches is holding the baton,
     * one blocking call away from being left on a suspension list it has been
     * taken off.
     */
    t_check(bt_probe_shared == 0UL,
            "_tx_thread_system_state is never raised with switching enabled",
            (LONG)bt_probe_shared);

    t_check(ami_baton_stats.bs_StateShared == 0UL,
            "no bracket found the interrupt state already raised",
            (LONG)ami_baton_stats.bs_StateShared);

    t_check(ami_baton_stats.bs_Full == 0UL, "the baton table never filled",
            (LONG)ami_baton_stats.bs_Full);
    t_check(ami_baton_stats.bs_BatonMoved == 0UL,
            "release() always found the baton was ours",
            (LONG)ami_baton_stats.bs_BatonMoved);

    t_log("  baton phase: %ld rounds, %ld probe samples\n",
          rounds, (LONG)bt_probe_samples);
    t_log("  shared state: %ld probe (worst %ld)\n",
          (LONG)bt_probe_shared, (LONG)bt_probe_worst);
    t_log("  shared state: %ld bracket, %ld transitions\n",
          (LONG)ami_baton_stats.bs_StateShared,
          (LONG)ami_baton_stats.bs_Transitions);

    bt_reap(&bt_probe);

    t_log("%ld checks, %ld failures -- ", (LONG)t_checks, (LONG)t_failures);
    t_log("%s\n", (LONG)((t_failures == 0UL) ? "PASS" : "FAIL"), 0);

    return (t_failures == 0UL) ? 0 : 20;
}
