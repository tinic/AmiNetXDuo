/*
 * AmiNetXDuo, the ThreadX/Exec adoption layer, on its own: while one Task
 * holds the baton, an unrelated Task must be told it does NOT.  No sockets,
 * no NetX Duo, no SANA-II driver, so it runs in public CI.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * tx_api.h first, before any NDK header: tx_port.h typedefs VOID, CHAR and
 * UCHAR itself, and exec/types.h getting there first makes those a
 * redefinition rather than a match.
 */
#include "tx_api.h"
#include "tx_amiga.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/lists.h>
#include <exec/io.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstack.h"

/* Declared here rather than pulled in from tx_thread.h, which wants
   TX_SOURCE_CODE. */
VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);
BOOL ami_netstack_baton_abandon(TX_THREAD *thread);
BOOL ami_netstack_baton_reclaim_dead(VOID);
ULONG _tx_amiga_task_stamp(struct Task *task);
extern volatile ULONG _tx_thread_system_state;
extern TX_THREAD *_tx_thread_current_ptr;
extern struct ExecBase *SysBase;

/* --------------------------------------------------------------- the shape -- */

#ifndef BT_WORKERS
#define BT_WORKERS      6           /* churn phase: unrelated Exec Tasks      */
#endif
#ifndef BT_ROUNDS
#define BT_ROUNDS       200         /* adopt/orphan cycles per worker         */
#endif

#ifndef BT_BATON_TASKS
#define BT_BATON_TASKS  20          /* proves there is no sixteen-task ceiling */
#endif
#ifndef BT_BATON_TICKS
#define BT_BATON_TICKS  250         /* how long the baton phase runs, in ticks */
#endif

#define BT_STACK        4096UL
#define BT_PRI          0
#define BT_PROBE_PRI    5           /* above every Task the phase creates      */
#define BT_PROBE_MICRO  2000UL      /* probe period; 500 samples a second      */

#define BT_SIG_GO       SIGF_SINGLE
#define BT_SIG_ACQUIRE  SIGBREAKF_CTRL_E

/* ------------------------------------------------------------- the reporting -- */

static ULONG t_checks;
static ULONG t_failures;

/* Flushed per line: the emulator runner reads stdout from a file after the
   run, so an unflushed line is lost if the program wedges. */
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

/* The stack is a SEPARATE AllocMem() from the task structure, and the MemList
   covers only the task: RemTask() frees the task's own MemList entries, and a
   list covering both frees one address twice. */
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

    /* The port owns the storage; this is the handle it lent us. */
    TX_THREAD      *bt_Thread;
    ULONG           bt_Gen;
} BtTask;

static BtTask bt_worker[BT_WORKERS];
static BtTask bt_holder;
static BtTask bt_dead_holding;
static BtTask bt_dead_dormant;
static BtTask bt_dead_released;
static BtTask bt_dead_event;

/* Never adopted, never in the pool: a TX_THREAD that is definitely not the one
   under test.  A discarded handle will not do -- the pool hands that slot to
   the next adopter, so the "other" thread would be the very thread the check
   is trying to tell apart. */
static TX_THREAD bt_decoy_thread;

static TX_EVENT_FLAGS_GROUP bt_dead_flags;
static volatile UINT bt_dead_flags_status = TX_NOT_DONE;

static VOID bt_wait_for(volatile UWORD *flag, const char *what);
static VOID bt_reap(BtTask *bt);

/* The reaper case and everything only it uses.  A green thread has no native
   Exec task to become a reaper zombie, so the realm build neither calls it nor
   compiles it; see bt_test_no_signal_reap() and its call site. */
static TX_THREAD *bt_reap_owner;
static ULONG      bt_reap_owner_gen;
static TX_THREAD bt_reap_target;
static TX_THREAD bt_overlap_probe;
static volatile ULONG bt_reap_entry_calls;

static VOID bt_reap_target_entry(ULONG input)
{
    (VOID)input;
    bt_reap_entry_calls++;
}

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

/* The flag, the Signal and the RemTask() must be one indivisible step, so that
   main cannot see bt_Done set and still find this Task alive standing on a
   stack main is about to free.  Exec discards a removed task's forbid nesting. */
static VOID bt_finish(BtTask *bt)
{
    Forbid();
    bt->bt_Done = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);
    RemTask(NULL);

    for (;;)
        Wait(0UL);
}

/* ------------------------------------------------------- the holder rendezvous -- */

static VOID bt_holder_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);

    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen, (CHAR *)"bracket holder", 20,
                              (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    if (tx_amiga_caller_is_thread() == (UINT)TX_FALSE)
        bt->bt_Failures++;              /* the holder must see itself */

    bt->bt_Ready = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);

    bt_mark = 1UL;
    Wait(BT_SIG_GO);                    /* held until main has looked */

    bt_mark = 2UL;
    (VOID)tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen);
    bt_mark = 3UL;
    bt_finish(bt);
}

/* ------------------------------------------------------------ the churn body -- */

static VOID bt_worker_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;
    LONG         i;

    Wait(BT_SIG_GO);

    for (i = 0; i < BT_ROUNDS; i++)
    {
        if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
        {
            bt->bt_Failures++;
            bt->bt_Saw = 1;
            break;
        }

        if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen, (CHAR *)"bracket worker", 20,
                              (UINT)TX_FALSE)
            != TX_SUCCESS)
        {
            bt->bt_Failures++;
            break;
        }

        if (tx_amiga_caller_is_thread() == (UINT)TX_FALSE)
        {
            bt->bt_Failures++;          /* adopted and not recognised */
            (VOID)tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen);
            break;
        }

        if (tx_amiga_adopted_thread() != bt->bt_Thread)
        {
            bt->bt_Failures++;          /* somebody else's TX_THREAD */
            (VOID)tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen);
            break;
        }

        if (tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
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

    bt_finish(bt);
}

/* ---------------------------------------------------- forced task death -- */

static VOID bt_die_holding_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen, (CHAR *)"dead holding", 20,
                              (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
    }
    else
    {
        bt->bt_Ready = 1U;
    }

    bt_finish(bt);
}

static VOID bt_die_dormant_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen, (CHAR *)"dead dormant", 20,
                              (UINT)TX_FALSE)
        != TX_SUCCESS ||
        tx_amiga_adopt_suspend(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
    {
        bt->bt_Failures++;
    }
    else
    {
        bt->bt_Ready = 1U;
    }

    bt_finish(bt);
}

static VOID bt_die_released_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen, (CHAR *)"dead released", 20,
                              (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
    }
    else
    {
        ami_netstack_baton_release();
        bt->bt_Ready = 1U;
    }

    bt_finish(bt);
}

static VOID bt_die_event_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;
    ULONG        actual = 0UL;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen, (CHAR *)"dead event", 20,
                              (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    bt->bt_Ready = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);

    /* Never set. main removes this Exec Task after ThreadX has linked the
       TX_THREAD into bt_dead_flags' suspension list. */
    (VOID)tx_event_flags_get(&bt_dead_flags, 1UL, TX_OR_CLEAR, &actual,
                             TX_WAIT_FOREVER);

    bt->bt_Failures++;                 /* a dead task must not come back */
    bt_finish(bt);
}

/* Enter through the production request gate, then stop in the same kind of
   ThreadX suspension a blocking socket receive uses.  main removes the parked
   owner Task while this continuation is still active. */

static BOOL bt_wait_dead(BtTask *bt, const char *what)
{
    bt_wait_for(&bt->bt_Done, what);
    if (bt->bt_Done == 0U)
        return FALSE;

    t_check(bt->bt_Ready != 0U, "dead Task reached its target state",
            bt->bt_Ready);
    t_check(bt->bt_Failures == 0, "dead Task set up without an error",
            bt->bt_Failures);

    return TRUE;
}

static VOID bt_discard_dead(BtTask *bt, BOOL expect_baton_slot,
                            const char *what)
{
    BOOL had_slot;
    BOOL still_baton;
    UINT status;

    t_check(tx_amiga_stack_in_use(bt->bt_Stack, bt->bt_StackSize) == TX_TRUE,
            "dead Task's stack is still claimed before cleanup", 0);

    had_slot = ami_netstack_baton_abandon(bt->bt_Thread);
    t_check(had_slot == expect_baton_slot, what, (LONG)had_slot);

    status = tx_amiga_discard_thread(bt->bt_Thread, bt->bt_Gen);
    t_check(status == TX_SUCCESS, "foreign dead TX_THREAD was discarded",
            (LONG)status);
    t_check(bt->bt_Thread->tx_thread_id == 0UL,
            "discard deleted the dead TX_THREAD", 0);
    t_check(tx_amiga_stack_in_use(bt->bt_Stack, bt->bt_StackSize) == TX_FALSE,
            "discard released the dead Task's stack range", 0);

    Forbid();
    still_baton = (_tx_thread_current_ptr == bt->bt_Thread);
    Permit();
    t_check(still_baton == FALSE,
            "dead TX_THREAD is no longer the global baton holder", 0);

    bt_reap(bt);
}

/* ------------------------------------------ the shared interrupt state -- */

/* Port rule under test: _tx_thread_system_state may only be raised under an
   unbroken Forbid().  The probe is the highest-priority Task in the phase and
   wakes on a timer interrupt, so it lands inside any Permit() window. */

static volatile ULONG bt_probe_samples;
static volatile ULONG bt_probe_shared;
static volatile ULONG bt_probe_worst;
static volatile UWORD bt_phase_stop;
static volatile UWORD bt_baton_released;

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
        bt_finish(bt);
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

    bt_finish(bt);
}

static VOID bt_baton_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;
    LONG         rounds = 0;

    Wait(BT_SIG_GO);

    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen, (CHAR *)"bracket baton", 20,
                              (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    while (bt_phase_stop == 0U)
    {
        /* The storage belongs to this TX_THREAD, not to a global task table:
           a nested Exec wait extends the same bracket and only the matching
           outer acquire resumes it. */
        ami_netstack_baton_release();
        ami_netstack_baton_release();

        /* Make the capacity proof independent of CPU speed.  Every Task
           reaches the released state once before any of them reacquires;
           after that barrier the timed contention phase runs unchanged. */
        if (rounds == 0)
        {
            Forbid();
            bt_baton_released++;
            Permit();
            Wait(BT_SIG_ACQUIRE);
        }

        /* Where the reader's Wait() for a packet goes. Nothing here may touch
           ThreadX: the bracket has taken this Task off the ready list. */
        if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
            bt->bt_Failures++;

        ami_netstack_baton_acquire();
        if (tx_amiga_caller_is_thread() != (UINT)TX_FALSE)
            bt->bt_Failures++;
        ami_netstack_baton_acquire();

        if (tx_amiga_adopted_thread() != bt->bt_Thread)
            bt->bt_Failures++;

        rounds++;
        bt->bt_Rounds = rounds;
    }

    (VOID)tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen);

    bt_finish(bt);
}

/* --------------------------------------------- the dead-holder reclaim -- */

/* A Task removed by Exec while it holds the baton.  Production recovers it
   from the tick: ami_netstack_baton_reclaim_dead() runs in a ThreadX timer
   callback, on the tick task under Forbid() with the interrupt state raised,
   and discards the holder's TX_THREAD.  These cases run it from exactly that
   context, and from a plain Task for the answers that must not change. */

static BtTask   bt_rc_holder;           /* dies holding, reclaimed by timer */
static BtTask   bt_rc_parked;           /* adopts behind it, must resume    */
static BtTask   bt_rc_live;             /* alive and holding: not reclaimed */
static BtTask   bt_rc_stamped;          /* dies holding, address may recycle */
static BtTask   bt_rc_reuse;            /* the Task spawned in its place     */
static TX_TIMER bt_rc_timer;

static volatile ULONG bt_rc_timer_fired;
static volatile LONG  bt_rc_timer_result;
static volatile ULONG bt_rc_timer_state;    /* system_state at the callback */

static VOID bt_rc_timer_entry(ULONG id)
{
    (VOID)id;
    bt_rc_timer_state  = _tx_thread_system_state;
    bt_rc_timer_result = (LONG)ami_netstack_baton_reclaim_dead();
    bt_rc_timer_fired++;
}

/* Adopts while a dead Task holds the baton, so it parks in
   tx_amiga_adopt_thread(); bt_Ready marks the moment it came out. */
static VOID bt_rc_parked_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    bt->bt_Saw = 1;
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen, (CHAR *)"reclaim parked", 20,
                              (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    bt->bt_Ready = 1U;
    if (tx_amiga_caller_is_thread() == (UINT)TX_FALSE)
        bt->bt_Failures++;

    (VOID)tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen);
    bt_finish(bt);
}

/* An Exec Task that exists at its address and, like a program that reused
   the dead holder's, owns the same signal bit (bt_Saw, or -1 for none) until
   BT_SIG_ACQUIRE tells it to give the bit back. */
static VOID bt_rc_reuse_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;
    LONG         bit = bt->bt_Saw;

    if (bit >= 0 && AllocSignal(bit) != bit)
        bt->bt_Failures++;

    bt->bt_Ready = 1U;
    for (;;)
    {
        ULONG got = Wait(BT_SIG_GO | BT_SIG_ACQUIRE);

        if ((got & BT_SIG_ACQUIRE) != 0UL && bit >= 0)
        {
            FreeSignal(bit);
            bit = -1;
            bt->bt_Rounds = 1;
        }
        if ((got & BT_SIG_GO) != 0UL)
            break;
    }
    bt_finish(bt);
}

/* Bit number of a single-bit mask, or -1. */
static LONG bt_sigbit(ULONG mask)
{
    LONG bit;

    for (bit = 0; bit < 32; bit++)
        if (mask == (1UL << bit))
            return bit;
    return -1;
}

/* Ticks until *flag is set, or -1 past the deadline. */
static LONG bt_ticks_until(volatile UWORD *flag, ULONG deadline_ticks)
{
    ULONG waited = 0UL;

    while (*flag == 0U)
    {
        if (waited >= deadline_ticks)
            return -1;
        Delay(1);
        waited++;
    }
    return (LONG)waited;
}

static VOID bt_test_reclaim_dead_holder(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        before;
    ULONG        waited;
    LONG         ticks;
    BOOL         held;
    UINT         status;

    t_log("bracket: reclaiming the baton from a Task removed while holding\n",
          0, 0);

    /* ---- (a) the holder dies, the tick reclaims -------------------------- */

    /* BOTH Tasks are created before either dies.  RemTask() frees the struct
       Task, and Exec hands the next AllocMem() of that size the same address:
       a Task spawned after the holder died would land on top of it, and a
       recycled address is DECLINED by the predicate, not reclaimed.  Case (d)
       below is where that is the subject. */
    SetSignal(0, BT_SIG_GO);
    bt_rc_holder.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_holder, bt_die_holding_entry, "reclaim-holder",
                     BT_PRI) != NULL, "spawned the holder that dies holding", 0);
    bt_rc_parked.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_parked, bt_rc_parked_entry, "reclaim-parked",
                     BT_PRI) != NULL, "spawned the parked adopter", 0);
    if (bt_rc_holder.bt_Task == NULL)
        return;

    Signal(bt_rc_holder.bt_Task, BT_SIG_GO);
    if (!bt_wait_dead(&bt_rc_holder, "reclaim holder to exit"))
        return;

    Forbid();
    held = (_tx_thread_current_ptr == bt_rc_holder.bt_Thread);
    Permit();
    t_check(held, "the dead Task's TX_THREAD holds the baton", 0);

    /* ---- (b) the second Task adopts behind it and parks ------------------ */

    if (bt_rc_parked.bt_Task != NULL)
    {
        Signal(bt_rc_parked.bt_Task, BT_SIG_GO);
        Delay(10);
        t_check(bt_rc_parked.bt_Saw == 1 && bt_rc_parked.bt_Ready == 0U,
                "the adopter is parked behind the dead holder",
                (LONG)bt_rc_parked.bt_Ready);
    }

    before = ami_baton_stats.bs_Reclaimed;
    bt_rc_timer_fired  = 0UL;
    bt_rc_timer_result = -1;
    bt_rc_timer_state  = 99UL;

    status = tx_timer_create(&bt_rc_timer, (CHAR *)"reclaim", bt_rc_timer_entry,
                             0UL, 5UL, 0UL, TX_AUTO_ACTIVATE);
    t_check(status == TX_SUCCESS, "created the reclaim timer", (LONG)status);
    if (status != TX_SUCCESS)
        return;

    waited = 0UL;
    while ((bt_rc_timer_fired == 0UL ||
            bt_rc_holder.bt_Thread->tx_thread_id != 0UL) && waited < 50UL)
    {
        Delay(1);
        waited++;
    }

    t_check(bt_rc_timer_fired == 1UL, "the timer callback ran once",
            (LONG)bt_rc_timer_fired);
    t_check(bt_rc_timer_state == 1UL,
            "the callback ran at interrupt level, as the tick does",
            (LONG)bt_rc_timer_state);
    t_check(bt_rc_timer_result == 1, "reclaim reported a dead holder",
            bt_rc_timer_result);
    t_check(bt_rc_holder.bt_Thread->tx_thread_id == 0UL,
            "the dead holder's TX_THREAD was deleted within a second",
            (LONG)waited);
    Forbid();
    held = (_tx_thread_current_ptr == bt_rc_holder.bt_Thread);
    Permit();
    t_check(!held, "the dead holder no longer holds the baton", 0);
    t_check(tx_amiga_stack_in_use(bt_rc_holder.bt_Stack,
                                  bt_rc_holder.bt_StackSize) == TX_FALSE,
            "the dead holder's stack range was released", 0);
    t_check(ami_baton_stats.bs_Reclaimed == before + 1UL,
            "bs_Reclaimed counted one reclaim",
            (LONG)ami_baton_stats.bs_Reclaimed);
    t_log("  reclaim_timer_state=%ld reclaim_timer_result=%ld\n",
          (LONG)bt_rc_timer_state, bt_rc_timer_result);
    t_log("  reclaim_ticks=%ld reclaim_preempt_resets=%ld\n",
          (LONG)waited, (LONG)_tx_amiga_discard_preempt_resets);

    (VOID)tx_timer_delete(&bt_rc_timer);
    bt_reap(&bt_rc_holder);

    if (bt_rc_parked.bt_Task != NULL)
    {
        ticks = bt_ticks_until(&bt_rc_parked.bt_Ready, 50UL);
        t_check(ticks >= 0, "the parked adopter resumed within a second",
                ticks);
        t_log("  reclaim_parked_resume_ticks=%ld\n", ticks, 0);
        bt_wait_for(&bt_rc_parked.bt_Done, "the resumed adopter to exit");
        t_check(bt_rc_parked.bt_Failures == 0,
                "the resumed adopter held the baton and orphaned",
                bt_rc_parked.bt_Failures);
        bt_reap(&bt_rc_parked);
    }

    /* ---- (c) a live holder is left alone --------------------------------- */

    SetSignal(0, BT_SIG_GO);
    bt_rc_live.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_live, bt_holder_entry, "reclaim-live", BT_PRI)
            != NULL, "spawned the live holder", 0);
    if (bt_rc_live.bt_Task != NULL)
    {
        Signal(bt_rc_live.bt_Task, BT_SIG_GO);
        bt_wait_for(&bt_rc_live.bt_Ready, "the live holder to adopt");

        before = ami_baton_stats.bs_Reclaimed;
        t_check(ami_netstack_baton_reclaim_dead() == FALSE,
                "a live holder is not reclaimed", 0);
        t_check(bt_rc_live.bt_Thread->tx_thread_id != 0UL,
                "the live holder's TX_THREAD is intact", 0);
        t_check(ami_baton_stats.bs_Reclaimed == before,
                "bs_Reclaimed is unchanged by a live holder",
                (LONG)ami_baton_stats.bs_Reclaimed);

        /* The stamp is only ever compared for a Task that is alive, and a
           live holder's stamp is its own: taken at adoption, from its own
           context, and its bounds and name cannot move while it is inside a
           vector.  Unstamped (0) is no opinion, so liveness alone decides. */
        {
            ULONG saved = bt_rc_live.bt_Thread->tx_thread_amiga_task_stamp;
            UINT  dead;

            Forbid();
            dead = tx_amiga_adopted_task_dead(bt_rc_live.bt_Thread);
            Permit();
            t_check(dead == TX_FALSE, "a live holder is alive to the predicate",
                    (LONG)dead);
            t_check(saved == _tx_amiga_task_stamp(bt_rc_live.bt_Task),
                    "a live holder's stamp is its Task's stamp", (LONG)saved);

            Forbid();
            bt_rc_live.bt_Thread->tx_thread_amiga_task_stamp = 0UL;
            dead = tx_amiga_adopted_task_dead(bt_rc_live.bt_Thread);
            bt_rc_live.bt_Thread->tx_thread_amiga_task_stamp = saved;
            Permit();
            t_check(dead == TX_FALSE, "an unstamped live holder is alive",
                    (LONG)dead);
        }

        Signal(bt_rc_live.bt_Task, BT_SIG_GO);
        bt_wait_for(&bt_rc_live.bt_Done, "the live holder to orphan");
        t_check(bt_rc_live.bt_Failures == 0, "the live holder kept its baton",
                bt_rc_live.bt_Failures);
        bt_reap(&bt_rc_live);
    }

    /* ---- (d) the stamp, and a recycled Task address ---------------------- */

    {
        ULONG        s1 = _tx_amiga_task_stamp(me);
        ULONG        s2 = _tx_amiga_task_stamp(me);
        ULONG        old_stamp;
        ULONG        new_stamp;
        struct Task *dead_task;
        UINT         dead;

        t_log("  reclaim_stamp_self=%lu exec_version=%ld\n", (LONG)s1,
              (LONG)SysBase->LibNode.lib_Version);
        /* Commodore's exec never sets TF_ETASK; recorded per arm, not asserted. */
        t_log("  reclaim_etask_flag=%ld\n",
              (LONG)((me->tc_Flags & TF_ETASK) != 0), 0);
        t_check(s1 != 0UL, "this Task has a stamp", (LONG)s1);
        t_check(s1 == s2, "the stamp is stable", (LONG)s2);
        t_check(_tx_amiga_task_stamp(NULL) == 0UL, "no Task, no stamp", 0);

        SetSignal(0, BT_SIG_GO);
        bt_rc_stamped.bt_Parent = me;
        t_check(bt_spawn(&bt_rc_stamped, bt_die_holding_entry,
                         "reclaim-stamped", BT_PRI) != NULL,
                "spawned the holder whose address may recycle", 0);
        if (bt_rc_stamped.bt_Task == NULL)
            return;

        dead_task = bt_rc_stamped.bt_Task;
        Signal(dead_task, BT_SIG_GO);
        if (!bt_wait_dead(&bt_rc_stamped, "stamped holder to exit"))
            return;

        old_stamp = bt_rc_stamped.bt_Thread->tx_thread_amiga_task_stamp;
        t_log("  reclaim_stamp_holder=%lu\n", (LONG)old_stamp, 0);

        /* Same allocation sizes in the same order, right after the free, and
           owning the dead holder's run bit, so that only the stamp can tell
           the two apart. */
        bt_rc_reuse.bt_Parent = me;
        bt_rc_reuse.bt_Saw    = bt_sigbit(bt_rc_stamped.bt_Thread->tx_thread_amiga_run_signal);
        t_check(bt_rc_reuse.bt_Saw >= 0, "the dead holder had a run bit",
                bt_rc_reuse.bt_Saw);
        t_check(bt_spawn(&bt_rc_reuse, bt_rc_reuse_entry, "reclaim-reuse",
                         BT_PRI) != NULL, "spawned a Task of the same size", 0);
        if (bt_rc_reuse.bt_Task != NULL)
        {
            bt_wait_for(&bt_rc_reuse.bt_Ready, "the reuse Task to start");
            t_check(bt_rc_reuse.bt_Failures == 0,
                    "the reuse Task took the dead holder's run bit",
                    bt_rc_reuse.bt_Failures);
        }

        Forbid();
        dead = tx_amiga_adopted_task_dead(bt_rc_stamped.bt_Thread);
        new_stamp = (bt_rc_reuse.bt_Task != NULL)
                  ? _tx_amiga_task_stamp(bt_rc_reuse.bt_Task) : 0UL;
        Permit();

        if (bt_rc_reuse.bt_Task == dead_task && old_stamp != 0UL)
        {
            UINT collided;

            t_log("  reclaim_stamp_path=exercised reclaim_stamp_new=%lu\n",
                  (LONG)new_stamp, 0);
            t_check(new_stamp != old_stamp, "the recycled address carries a "
                    "new stamp", (LONG)new_stamp);
            /* THE STAMP IS A NEGATIVE TEST.  The address is occupied by
               something that does not look like our holder, so the reclaim
               DECLINES: a missed reclaim, never a wrong one.  The old code
               read this as dead. */
            t_check(dead == TX_FALSE,
                    "a live Task at the dead holder's address is declined, "
                    "not reclaimed", (LONG)dead);

            /* A collision, forced: the dead holder's stamp made equal to the
               new Task's, which also owns the run bit.  The predicate then
               says alive, which is the pre-reclaim behaviour, and the reclaim
               leaves it be.  A collision can only ever err this way. */
            Forbid();
            bt_rc_stamped.bt_Thread->tx_thread_amiga_task_stamp = new_stamp;
            collided = tx_amiga_adopted_task_dead(bt_rc_stamped.bt_Thread);
            Permit();
            t_check(collided == TX_FALSE,
                    "a colliding stamp reads as alive, never as dead",
                    (LONG)collided);
            before = ami_baton_stats.bs_Reclaimed;
            t_check(ami_netstack_baton_reclaim_dead() == FALSE,
                    "a colliding stamp is not reclaimed", 0);
            t_check(bt_rc_stamped.bt_Thread->tx_thread_id != 0UL,
                    "the collided TX_THREAD is left intact", 0);
            t_check(ami_baton_stats.bs_Reclaimed == before,
                    "bs_Reclaimed is unchanged by a collision",
                    (LONG)ami_baton_stats.bs_Reclaimed);

            /* The same collision once the new Task gives the run bit back:
               a Task without the holder's bit is not the holder. */
            Signal(bt_rc_reuse.bt_Task, BT_SIG_ACQUIRE);
            {
                ULONG waited = 0UL;

                while (bt_rc_reuse.bt_Rounds == 0 && waited < 250UL)
                {
                    Delay(1);
                    waited++;
                }
            }
            t_check(bt_rc_reuse.bt_Rounds == 1, "the reuse Task freed the bit",
                    bt_rc_reuse.bt_Rounds);
            Forbid();
            collided = tx_amiga_adopted_task_dead(bt_rc_stamped.bt_Thread);
            Permit();
            t_check(collided == TX_TRUE,
                    "without the run bit the recycled Task reads as dead",
                    (LONG)collided);

            Forbid();
            bt_rc_stamped.bt_Thread->tx_thread_amiga_task_stamp = old_stamp;
            Permit();

            /* Restored, the stamp mismatches again and the reclaim keeps
               declining -- until the Task at that address goes away, which is
               what turns the miss back into a reclaim. */
            t_check(ami_netstack_baton_reclaim_dead() == FALSE,
                    "the restored stamp still declines while the address is "
                    "taken", 0);

            Signal(bt_rc_reuse.bt_Task, BT_SIG_GO);
            bt_wait_for(&bt_rc_reuse.bt_Done, "the reuse Task to exit");
            bt_reap(&bt_rc_reuse);
            bt_rc_reuse.bt_Task = NULL;
        }
        else
        {
            t_log("  reclaim_stamp_path=not_exercised reclaim_stamp_reused=%ld\n",
                  (LONG)(bt_rc_reuse.bt_Task == dead_task), 0);
            t_check(dead == TX_TRUE, "the dead holder is seen as dead",
                    (LONG)dead);
        }

        before = ami_baton_stats.bs_Reclaimed;
        t_check(ami_netstack_baton_reclaim_dead() == TRUE,
                "reclaim discards the stamped holder", 0);
        t_check(bt_rc_stamped.bt_Thread->tx_thread_id == 0UL,
                "the stamped holder's TX_THREAD was deleted", 0);
        t_check(ami_baton_stats.bs_Reclaimed == before + 1UL,
                "bs_Reclaimed counted the stamped reclaim",
                (LONG)ami_baton_stats.bs_Reclaimed);
        t_check(ami_netstack_baton_reclaim_dead() == FALSE,
                "nothing left to reclaim", 0);

        if (bt_rc_reuse.bt_Task != NULL)
        {
            Signal(bt_rc_reuse.bt_Task, BT_SIG_GO);
            bt_wait_for(&bt_rc_reuse.bt_Done, "the reuse Task to exit");
            bt_reap(&bt_rc_reuse);
        }
        bt_reap(&bt_rc_stamped);
    }

    t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
            "unadopted after the reclaim cases", 0);
}

/* ------------------------------------------ the pool and the predicate -- */

/* Everything below is about the two things the first attempt at this got
   wrong: who owns the TX_THREAD, and what counts as proof of death. */

static BtTask   bt_rc_waiter;           /* alive, blocked in Wait(~0), holding */
static BtTask   bt_rc_onstack;          /* AmiNetCaller on its own stack       */
static BtTask   bt_rc_slot;             /* takes the slot the corpse freed     */
static BtTask   bt_rc_cached;           /* the suspend/resume form             */
static BtTask   bt_rc_mutex_owner;      /* dies owning a TX_MUTEX              */
static TX_MUTEX bt_rc_mutex;

#ifndef BT_POOL_EXTRA
#define BT_POOL_EXTRA   2               /* adopters beyond the pool's size     */
#endif
#define BT_POOL_MAX     24              /* room for any cap the port may have  */

static BtTask   bt_pool[BT_POOL_MAX];
static BtTask   bt_pool_reserved;       /* the caller that may not park        */

extern UINT _tx_thread_preempt_disable;


/* Adopts, then blocks in an exec Wait() the way a hook or a callback that
   forgot the rule does: alive, holding the baton, and parked on every signal
   bit it has -- including its own run bit.  The withdrawn TS_WAIT criterion
   called exactly this shape dead. */
static VOID bt_rc_wait_forever_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen,
                              (CHAR *)"live waiter", 20, (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    SetSignal(0UL, ~0UL);               /* nothing pending, so the Wait blocks */
    bt->bt_Ready = 1U;
    (VOID)Wait(~0UL);

    bt->bt_Rounds = 1;
    (VOID)tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen);
    bt_finish(bt);
}


/* The startup shape (netstack.c): the AmiNetCaller is on the Task's OWN stack,
   so removing the Task frees the record the adoption was made through. */
static VOID bt_rc_onstack_entry(VOID)
{
    struct Task  *me = FindTask(NULL);
    BtTask       *bt = (BtTask *)me->tc_UserData;
    AmiNetCaller  caller;

    Wait(BT_SIG_GO);

    caller.nc_Adopted = FALSE;
    caller.nc_Live    = FALSE;
    caller.nc_Task    = me;
    if (tx_amiga_adopt_thread(&caller.nc_Thread, &caller.nc_Gen,
                              (CHAR *)"on-stack caller", 20, (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    /* Published so main can still reach the handle once this stack is gone. */
    bt->bt_Thread = caller.nc_Thread;
    bt->bt_Gen    = caller.nc_Gen;
    bt->bt_Saw    = (LONG)(APTR)&caller;
    bt->bt_Ready  = 1U;

    bt_finish(bt);                      /* RemTask() while holding the baton */
}


/* Adopt with the reserved slot allowed -- what a Task inside ami_ns_lock does
   -- and report the instant a slot was taken, which is before the baton
   arrives.  bt_Saw is the free-slot count the claim left behind. */
static VOID bt_rc_hold_reserved_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    bt->bt_Rounds = 1;                  /* about to ask */
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen,
                              (CHAR *)"reserved holder", 20, (UINT)TX_TRUE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    bt->bt_Ready = 1U;
    Wait(BT_SIG_ACQUIRE);

    if (tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
        bt->bt_Failures++;
    bt_finish(bt);
}

/* Adopt, hold the baton, and give it back when told. */
static VOID bt_rc_hold_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen,
                              (CHAR *)"pool holder", 20, (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    bt->bt_Ready = 1U;
    Wait(BT_SIG_ACQUIRE);

    if (tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
        bt->bt_Failures++;
    bt->bt_Rounds = 1;
    bt_finish(bt);
}


/* The cached form: the slot is kept across a suspend, and the handle keeps
   naming it.  This is the NXCACHE path of src/bsdsocket/netx_call.c. */
static VOID bt_rc_cached_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen,
                              (CHAR *)"cached caller", 20, (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    if (tx_amiga_adopt_suspend(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
        bt->bt_Failures++;

    bt->bt_Ready = 1U;                  /* dormant; main looks at the slot */
    Wait(BT_SIG_ACQUIRE);

    if (tx_amiga_adopt_resume(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
        bt->bt_Failures++;
    if (tx_amiga_caller_is_thread() == (UINT)TX_FALSE)
        bt->bt_Failures++;
    if (tx_amiga_adopt_suspend(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
        bt->bt_Failures++;
    if (tx_amiga_adopt_resume(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
        bt->bt_Failures++;
    if (tx_amiga_orphan_thread(bt->bt_Thread, bt->bt_Gen) != TX_SUCCESS)
        bt->bt_Failures++;

    bt->bt_Rounds = 1;
    bt_finish(bt);
}


/* Dies owning a TX_MUTEX as well as the baton.  NX_IP's nx_ip_protection IS a
   TX_MUTEX (nx_api.h), so what releases one releases the other. */
static VOID bt_rc_mutex_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen,
                              (CHAR *)"mutex corpse", 20, (UINT)TX_FALSE)
        != TX_SUCCESS)
    {
        bt->bt_Failures++;
        bt_finish(bt);
    }

    if (tx_mutex_get(&bt_rc_mutex, TX_WAIT_FOREVER) != TX_SUCCESS)
        bt->bt_Failures++;
    else
        bt->bt_Ready = 1U;

    bt_finish(bt);
}


/* P0-2.  A LIVE holder blocked in Wait(~0UL) matched the withdrawn criterion
   "TS_WAIT with the run bit in tc_SigWait", and would have been discarded out
   from under a Task that was still running. */
static VOID bt_test_live_waiter_not_dead(VOID)
{
    struct Task *me = FindTask(NULL);
    struct Task *victim;
    ULONG        before;
    ULONG        runsig;
    UINT         old_criterion;
    UINT         dead;

    t_log("bracket: a live holder blocked in Wait() is not a corpse\n", 0, 0);

    SetSignal(0, BT_SIG_GO);
    bt_rc_waiter.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_waiter, bt_rc_wait_forever_entry, "reclaim-waiter",
                     BT_PRI) != NULL, "spawned the live waiting holder", 0);
    if (bt_rc_waiter.bt_Task == NULL)
        return;

    victim = bt_rc_waiter.bt_Task;
    Signal(victim, BT_SIG_GO);
    bt_wait_for(&bt_rc_waiter.bt_Ready, "the live holder to adopt");
    if (bt_rc_waiter.bt_Ready == 0U || bt_rc_waiter.bt_Thread == TX_NULL)
        return;

    Delay(5);                           /* let it reach the Wait() */

    runsig = bt_rc_waiter.bt_Thread->tx_thread_amiga_run_signal;

    Forbid();
    Disable();
    /* The withdrawn criterion, spelled out here so the regression is visible:
       it is TRUE for this Task, and this Task is alive. */
    old_criterion = ((runsig != 0UL) && (victim->tc_State == TS_WAIT) &&
                     ((victim->tc_SigWait & runsig) != 0UL))
                  ? (UINT)TX_TRUE : (UINT)TX_FALSE;
    Enable();
    dead = tx_amiga_adopted_task_dead(bt_rc_waiter.bt_Thread);
    Permit();

    t_check(old_criterion == (UINT)TX_TRUE,
            "the live holder matches the withdrawn TS_WAIT criterion",
            (LONG)old_criterion);
    t_check(dead == TX_FALSE,
            "a live holder blocked in Wait(~0) is NOT dead", (LONG)dead);

    before = ami_baton_stats.bs_Reclaimed;
    t_check(ami_netstack_baton_reclaim_dead() == FALSE,
            "a live holder blocked in Wait(~0) is not reclaimed", 0);
    t_check(bt_rc_waiter.bt_Thread->tx_thread_id != 0UL,
            "its TX_THREAD is intact", 0);
    t_check(ami_baton_stats.bs_Reclaimed == before,
            "bs_Reclaimed is unchanged", (LONG)ami_baton_stats.bs_Reclaimed);

    Signal(victim, BT_SIG_ACQUIRE);
    bt_wait_for(&bt_rc_waiter.bt_Done, "the live holder to orphan and exit");
    t_check(bt_rc_waiter.bt_Rounds == 1 && bt_rc_waiter.bt_Failures == 0,
            "the live holder came back and gave the baton up",
            bt_rc_waiter.bt_Failures);
    bt_reap(&bt_rc_waiter);
}


/* P0-1.  The holder's AmiNetCaller was on its own stack.  The reclaim has to
   work after that stack is freed, which it can only do if the TX_THREAD was
   never in it.  Then: the slot it frees is handed out again, and the handle
   the corpse left behind must not be honoured. */
static VOID bt_test_onstack_caller_and_slot_reuse(VOID)
{
    struct Task *me = FindTask(NULL);
    UBYTE       *stack_lo;
    UBYTE       *stack_hi;
    TX_THREAD   *stale_thread;
    ULONG        stale_gen;
    ULONG        before;
    BOOL         rec_in_stack;
    BOOL         thread_in_stack;
    UINT         valid;
    UINT         st_discard;
    UINT         st_orphan;

    t_log("bracket: a holder whose AmiNetCaller was on its own stack\n", 0, 0);

    SetSignal(0, BT_SIG_GO);
    bt_rc_onstack.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_onstack, bt_rc_onstack_entry, "reclaim-onstack",
                     BT_PRI) != NULL, "spawned the on-stack caller", 0);
    if (bt_rc_onstack.bt_Task == NULL)
        return;

    Signal(bt_rc_onstack.bt_Task, BT_SIG_GO);
    if (!bt_wait_dead(&bt_rc_onstack, "on-stack caller to exit"))
        return;

    stack_lo = (UBYTE *)bt_rc_onstack.bt_Stack;
    stack_hi = stack_lo + bt_rc_onstack.bt_StackSize;

    rec_in_stack = ((UBYTE *)(APTR)bt_rc_onstack.bt_Saw >= stack_lo &&
                    (UBYTE *)(APTR)bt_rc_onstack.bt_Saw <  stack_hi);
    thread_in_stack = ((UBYTE *)bt_rc_onstack.bt_Thread >= stack_lo &&
                       (UBYTE *)bt_rc_onstack.bt_Thread <  stack_hi);

    t_check(rec_in_stack,
            "the AmiNetCaller really was on the dead Task's stack", 0);
    t_check(!thread_in_stack,
            "its TX_THREAD was not: the port owns that storage", 0);

    /* Freed BEFORE the reclaim runs.  Whatever the reclaim reads, it is not
       this; under caller-owned storage it would have been. */
    FreeMem(bt_rc_onstack.bt_Stack, bt_rc_onstack.bt_StackSize);
    bt_rc_onstack.bt_Stack = NULL;

    stale_thread = bt_rc_onstack.bt_Thread;
    stale_gen    = bt_rc_onstack.bt_Gen;

    before = ami_baton_stats.bs_Reclaimed;
    t_check(ami_netstack_baton_reclaim_dead() == TRUE,
            "the on-stack caller's baton was reclaimed after its stack went", 0);
    t_check(stale_thread->tx_thread_id == 0UL,
            "its TX_THREAD was deleted", 0);
    t_check(ami_baton_stats.bs_Reclaimed == before + 1UL,
            "bs_Reclaimed counted it", (LONG)ami_baton_stats.bs_Reclaimed);

    Forbid();
    valid = tx_amiga_adopt_handle_valid(stale_thread, stale_gen);
    Permit();
    t_check(valid == (UINT)TX_FALSE,
            "the discarded handle stopped naming a live adoption", (LONG)valid);

    /* ---- the freed slot, handed out again ------------------------------- */

    SetSignal(0, BT_SIG_GO);
    bt_rc_slot.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_slot, bt_rc_hold_entry, "reclaim-slot", BT_PRI)
            != NULL, "spawned the next adopter", 0);
    if (bt_rc_slot.bt_Task == NULL)
        return;

    Signal(bt_rc_slot.bt_Task, BT_SIG_GO);
    bt_wait_for(&bt_rc_slot.bt_Ready, "the next adopter to take a slot");
    if (bt_rc_slot.bt_Ready == 0U)
        return;

    t_log("  reclaim_slot_reused=%ld reclaim_slot_gen_moved=%ld\n",
          (LONG)(bt_rc_slot.bt_Thread == stale_thread),
          (LONG)(bt_rc_slot.bt_Gen != stale_gen));
    t_check(bt_rc_slot.bt_Thread == stale_thread,
            "the freed slot was the one handed out next", 0);
    t_check(bt_rc_slot.bt_Gen != stale_gen,
            "it came with a new generation", (LONG)bt_rc_slot.bt_Gen);

    Forbid();
    valid      = tx_amiga_adopt_handle_valid(stale_thread, stale_gen);
    st_discard = tx_amiga_discard_thread(stale_thread, stale_gen);
    st_orphan  = tx_amiga_orphan_thread(stale_thread, stale_gen);
    Permit();

    t_check(valid == (UINT)TX_FALSE,
            "the stale handle does not name the new adoption", (LONG)valid);
    t_check(st_discard == TX_THREAD_ERROR,
            "a discard on the stale handle is refused", (LONG)st_discard);
    t_check(st_orphan == TX_THREAD_ERROR,
            "an orphan on the stale handle is refused", (LONG)st_orphan);
    t_check(bt_rc_slot.bt_Thread->tx_thread_id != 0UL,
            "the new adoption survived both", 0);

    Signal(bt_rc_slot.bt_Task, BT_SIG_ACQUIRE);
    bt_wait_for(&bt_rc_slot.bt_Done, "the next adopter to orphan and exit");
    t_check(bt_rc_slot.bt_Failures == 0 && bt_rc_slot.bt_Rounds == 1,
            "the new adoption orphaned cleanly", bt_rc_slot.bt_Failures);
    bt_reap(&bt_rc_slot);
}


/* Every slot taken, and two more Tasks asking.  They must WAIT -- never be
   handed storage of their own -- and they must come back once a slot frees. */
static VOID bt_test_pool_exhaustion(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        slots   = tx_amiga_adopt_slots();
    ULONG        reserve = tx_amiga_adopt_reserve();
    ULONG        free_before;
    ULONG        ordinary;
    ULONG        total;
    ULONG        i;
    ULONG        ready;
    ULONG        done;
    ULONG        parks_before;
    ULONG        waiting;

    t_log("bracket: more adopters than slots\n", 0, 0);
    t_log("  pool_slots=%lu pool_reserve=%lu\n", (LONG)slots,
          (LONG)tx_amiga_adopt_reserve());

    /* Also a leak check on everything above: by now every case has orphaned
       or discarded what it adopted, so every slot should be back. */
    free_before = tx_amiga_adopt_slots_free();
    t_check(free_before == slots, "every case above gave its slot back",
            (LONG)free_before);

    if (free_before <= reserve ||
        free_before + (ULONG)BT_POOL_EXTRA > (ULONG)BT_POOL_MAX)
    {
        t_check(0, "the pool is a size this test can cover", (LONG)free_before);
        return;
    }

    /* Everything an ORDINARY caller may have.  The reserved tail is not part
       of it, whether the caller caches its bracket or not: the reserve follows
       who holds ami_ns_lock, which no Task here does. */
    ordinary     = free_before - reserve;
    total        = ordinary + (ULONG)BT_POOL_EXTRA;
    parks_before = _tx_amiga_adopt_parks;

    SetSignal(0, BT_SIG_GO);

    for (i = 0UL; i < total; i++)
    {
        bt_pool[i].bt_Parent = me;
        if (bt_spawn(&bt_pool[i], bt_rc_hold_entry, "pool", BT_PRI) == NULL)
        {
            t_check(0, "spawned every pool adopter", (LONG)i);
            total = i;
            break;
        }
    }

    /* The first `ordinary` claim one each; only one of them holds the baton,
       the rest are parked inside adopt with a slot taken.  The last two find
       everything an ordinary caller may have is gone, and park in the port. */
    for (i = 0UL; i < ordinary; i++)
        Signal(bt_pool[i].bt_Task, BT_SIG_GO);

    Delay(25);

    t_check(tx_amiga_adopt_slots_free() == reserve,
            "ordinary adopters stop at the reserved tail",
            (LONG)tx_amiga_adopt_slots_free());

    for (i = ordinary; i < total; i++)
        Signal(bt_pool[i].bt_Task, BT_SIG_GO);

    Delay(25);

    Forbid();
    waiting = _tx_amiga_adopt_waiting;
    Permit();

    ready = 0UL;
    for (i = ordinary; i < total; i++)
        ready += (bt_pool[i].bt_Ready != 0U) ? 1UL : 0UL;

    t_check(waiting == (ULONG)BT_POOL_EXTRA,
            "the adopters beyond the pool are waiting for a slot",
            (LONG)waiting);
    t_check(ready == 0UL,
            "and none of them was given storage of its own", (LONG)ready);
    t_log("  pool_waiting=%lu pool_peak=%lu\n", (LONG)waiting,
          (LONG)_tx_amiga_adopt_peak);

    /* THE RESERVE, doing the one job it has.  Ordinary callers are blocked and
       the only free slot is the reserved one.  A caller entitled to it -- in
       the stack, a Task inside ami_ns_lock -- must still get a slot, at once
       and without joining the queue.  (It then waits for the BATON, which is
       a different thing and is what every adopter does.) */
    bt_pool_reserved.bt_Parent = me;
    t_check(bt_spawn(&bt_pool_reserved, bt_rc_hold_reserved_entry,
                     "pool-reserved", BT_PRI) != NULL,
            "spawned the caller that may not park", 0);
    if (bt_pool_reserved.bt_Task != NULL)
    {
        ULONG waited = 0UL;

        Signal(bt_pool_reserved.bt_Task, BT_SIG_GO);
        while (tx_amiga_adopt_slots_free() != 0UL && waited < 50UL)
        {
            Delay(1);
            waited++;
        }

        t_check(tx_amiga_adopt_slots_free() == 0UL,
                "the reserved caller took the slot ordinary callers cannot",
                (LONG)tx_amiga_adopt_slots_free());

        Forbid();
        waiting = _tx_amiga_adopt_waiting;
        Permit();
        t_check(waiting == (ULONG)BT_POOL_EXTRA,
                "and did not join the queue to do it", (LONG)waiting);
        t_log("  pool_reserved_claim_ticks=%lu pool_ordinary=%lu\n",
              (LONG)waited, (LONG)ordinary);
    }

    /* Let them all go.  Each orphan frees a slot and wakes a waiter. */
    for (i = 0UL; i < total; i++)
        Signal(bt_pool[i].bt_Task, BT_SIG_ACQUIRE);
    if (bt_pool_reserved.bt_Task != NULL)
        Signal(bt_pool_reserved.bt_Task, BT_SIG_ACQUIRE);

    done = 0UL;
    for (i = 0UL; i < total; i++)
    {
        bt_wait_for(&bt_pool[i].bt_Done, "every pool adopter to exit");
        done += (bt_pool[i].bt_Done != 0U) ? 1UL : 0UL;
    }

    t_check(done == total, "every adopter finished, including the ones that "
            "waited", (LONG)done);

    ready = 0UL;
    for (i = 0UL; i < total; i++)
        ready += (ULONG)bt_pool[i].bt_Failures;
    t_check(ready == 0UL, "no adopter reported a failure", (LONG)ready);

    t_check(_tx_amiga_adopt_parks >= parks_before + (ULONG)BT_POOL_EXTRA,
            "the port counted the waits",
            (LONG)(_tx_amiga_adopt_parks - parks_before));
    t_check(_tx_amiga_adopt_waiter_full == 0UL,
            "no adopter was refused for want of a waiter slot",
            (LONG)_tx_amiga_adopt_waiter_full);
    t_check(_tx_amiga_adopt_peak <= slots,
            "the pool never went over its size", (LONG)_tx_amiga_adopt_peak);

    if (bt_pool_reserved.bt_Task != NULL)
    {
        bt_wait_for(&bt_pool_reserved.bt_Done, "the reserved caller to exit");
        t_check(bt_pool_reserved.bt_Failures == 0,
                "the reserved caller held the baton and orphaned",
                bt_pool_reserved.bt_Failures);
        bt_reap(&bt_pool_reserved);
    }

    Forbid();
    waiting = _tx_amiga_adopt_waiting;
    Permit();
    t_check(waiting == 0UL, "nobody is left waiting", (LONG)waiting);
    t_check(tx_amiga_adopt_slots_free() == slots,
            "and every slot is back", (LONG)tx_amiga_adopt_slots_free());

    for (i = 0UL; i < total; i++)
        bt_reap(&bt_pool[i]);
}


/* The cached bracket, which keeps its slot across a suspend. */
static VOID bt_test_cached_handle(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        gen_while_dormant;
    BOOL         holding;

    t_log("bracket: the cached adoption keeps its slot across a suspend\n",
          0, 0);

    SetSignal(0, BT_SIG_GO);
    bt_rc_cached.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_cached, bt_rc_cached_entry, "reclaim-cached",
                     BT_PRI) != NULL, "spawned the cached caller", 0);
    if (bt_rc_cached.bt_Task == NULL)
        return;

    Signal(bt_rc_cached.bt_Task, BT_SIG_GO);
    bt_wait_for(&bt_rc_cached.bt_Ready, "the cached caller to go dormant");
    if (bt_rc_cached.bt_Ready == 0U || bt_rc_cached.bt_Thread == TX_NULL)
        return;

    Forbid();
    gen_while_dormant = tx_amiga_adopt_generation(bt_rc_cached.bt_Thread);
    holding = (_tx_thread_current_ptr == bt_rc_cached.bt_Thread);
    Permit();

    t_check(gen_while_dormant == bt_rc_cached.bt_Gen,
            "the slot is still the dormant caller's", (LONG)gen_while_dormant);
    t_check(!holding, "and it is not holding the baton while dormant", 0);

    Signal(bt_rc_cached.bt_Task, BT_SIG_ACQUIRE);
    bt_wait_for(&bt_rc_cached.bt_Done, "the cached caller to finish");
    t_check(bt_rc_cached.bt_Rounds == 1 && bt_rc_cached.bt_Failures == 0,
            "resume, suspend, resume and orphan all took the handle",
            bt_rc_cached.bt_Failures);

    Forbid();
    gen_while_dormant = tx_amiga_adopt_generation(bt_rc_cached.bt_Thread);
    Permit();
    t_check(gen_while_dormant == 0UL,
            "the orphan gave the slot back", (LONG)gen_while_dormant);

    bt_reap(&bt_rc_cached);
}


/* What the corpse leaves behind: a raised preemption lockout and a held
   mutex.  Discarding it has to undo both. */
static VOID bt_test_corpse_leftovers(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        resets_before;
    ULONG        before;
    UINT         status;
    UINT         preempt_after;
    ULONG        owners;

    t_log("bracket: what a discarded corpse leaves behind\n", 0, 0);

    status = tx_mutex_create(&bt_rc_mutex, (CHAR *)"reclaim mutex", TX_INHERIT);
    t_check(status == TX_SUCCESS, "created the mutex the corpse will hold",
            (LONG)status);
    if (status != TX_SUCCESS)
        return;

    SetSignal(0, BT_SIG_GO);
    bt_rc_mutex_owner.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_mutex_owner, bt_rc_mutex_entry, "reclaim-mutex",
                     BT_PRI) != NULL, "spawned the mutex-owning holder", 0);
    if (bt_rc_mutex_owner.bt_Task == NULL)
    {
        (VOID)tx_mutex_delete(&bt_rc_mutex);
        return;
    }

    Signal(bt_rc_mutex_owner.bt_Task, BT_SIG_GO);
    if (!bt_wait_dead(&bt_rc_mutex_owner, "the mutex owner to exit"))
    {
        (VOID)tx_mutex_delete(&bt_rc_mutex);
        return;
    }

    t_check(bt_rc_mutex.tx_mutex_ownership_count != 0UL,
            "the corpse still owns the mutex",
            (LONG)bt_rc_mutex.tx_mutex_ownership_count);

    /* The lockout a Task removed mid-service leaves raised.  Only the dead
       Task could have raised it, so the discard is entitled to clear it. */
    Forbid();
    resets_before = _tx_amiga_discard_preempt_resets;
    _tx_thread_preempt_disable = (UINT)1;
    Permit();

    before = ami_baton_stats.bs_Reclaimed;
    t_check(ami_netstack_baton_reclaim_dead() == TRUE,
            "the mutex-owning corpse was reclaimed", 0);
    t_check(ami_baton_stats.bs_Reclaimed == before + 1UL,
            "bs_Reclaimed counted it", (LONG)ami_baton_stats.bs_Reclaimed);

    Forbid();
    preempt_after = _tx_thread_preempt_disable;
    owners        = bt_rc_mutex.tx_mutex_ownership_count;
    Permit();

    t_check(preempt_after == (UINT)0,
            "the corpse's preemption lockout was cleared", (LONG)preempt_after);
    t_check(_tx_amiga_discard_preempt_resets == resets_before + 1UL,
            "and counted",
            (LONG)(_tx_amiga_discard_preempt_resets - resets_before));
    t_check(owners == 0UL,
            "terminate released the mutex the corpse held", (LONG)owners);
    t_check(bt_rc_mutex.tx_mutex_owner == TX_NULL,
            "and it has no owner", 0);
    t_log("  corpse_preempt_resets=%lu corpse_mutex_owners=%lu\n",
          (LONG)_tx_amiga_discard_preempt_resets, (LONG)owners);

    bt_reap(&bt_rc_mutex_owner);
    (VOID)tx_mutex_delete(&bt_rc_mutex);
}

/* P0-1.  A Task removed between taking a slot and creating the TX_THREAD in it
   leaves a busy slot with no thread.  The baton reclaim cannot see that -- it
   asks about a holder, and there is none -- so the tick sweeps the pool. */

static BtTask bt_rc_orphan_slot;

static VOID bt_rc_orphan_slot_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);

    /* Exactly the residue: claimed, never published. */
    if (tx_amiga_adopt_claim_orphan() == (UINT)TX_FALSE)
        bt->bt_Failures++;
    else
        bt->bt_Ready = 1U;

    bt_finish(bt);                      /* RemTask() with the slot held */
}

static VOID bt_test_unpublished_slot_sweep(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        free_before;
    ULONG        free_held;
    ULONG        free_after;
    ULONG        swept_before;

    t_log("bracket: a slot whose claimer died before it published\n", 0, 0);

    free_before  = tx_amiga_adopt_slots_free();
    swept_before = _tx_amiga_adopt_unpublished_freed;

    /* The sweep must never take a slot that is alive or published. */
    tx_amiga_adopt_sweep_unpublished();
    t_check(_tx_amiga_adopt_unpublished_freed == swept_before,
            "the sweep frees nothing while every slot is published",
            (LONG)(_tx_amiga_adopt_unpublished_freed - swept_before));
    t_check(tx_amiga_adopt_slots_free() == free_before,
            "and leaves the free count alone", (LONG)free_before);

    SetSignal(0, BT_SIG_GO);
    bt_rc_orphan_slot.bt_Parent = me;
    t_check(bt_spawn(&bt_rc_orphan_slot, bt_rc_orphan_slot_entry,
                     "reclaim-orphanslot", BT_PRI) != NULL,
            "spawned the Task that dies holding an unpublished slot", 0);
    if (bt_rc_orphan_slot.bt_Task == NULL)
        return;

    Signal(bt_rc_orphan_slot.bt_Task, BT_SIG_GO);
    bt_wait_for(&bt_rc_orphan_slot.bt_Done, "the unpublished claimer to exit");
    t_check(bt_rc_orphan_slot.bt_Ready != 0U &&
            bt_rc_orphan_slot.bt_Failures == 0,
            "it took a slot before it died", bt_rc_orphan_slot.bt_Failures);

    free_held = tx_amiga_adopt_slots_free();
    t_check(free_held == free_before - 1UL,
            "the slot is gone and no TX_THREAD accounts for it",
            (LONG)free_held);

    /* The baton reclaim has nothing to say about it: there is no holder. */
    t_check(ami_netstack_baton_reclaim_dead() == FALSE,
            "the baton reclaim cannot see an unpublished slot", 0);
    t_check(tx_amiga_adopt_slots_free() == free_held,
            "so the slot is still gone after it", (LONG)free_held);

    tx_amiga_adopt_sweep_unpublished();
    free_after = tx_amiga_adopt_slots_free();

    t_check(_tx_amiga_adopt_unpublished_freed == swept_before + 1UL,
            "the sweep gave the leaked slot back, once",
            (LONG)(_tx_amiga_adopt_unpublished_freed - swept_before));
    t_check(free_after == free_before, "the pool is whole again",
            (LONG)free_after);

    tx_amiga_adopt_sweep_unpublished();
    t_check(_tx_amiga_adopt_unpublished_freed == swept_before + 1UL,
            "a second sweep finds nothing",
            (LONG)(_tx_amiga_adopt_unpublished_freed - swept_before));
    t_log("  orphan_slot_total=%lu\n",
          (LONG)_tx_amiga_adopt_unpublished_freed, 0);

    bt_reap(&bt_rc_orphan_slot);
}

/* THE SHUTDOWN WAKE, and the one case that needs it.

   A waiter parked on a full pool is signalled from every slot release -- but
   only if it still carries the stamp it registered with.  On a mismatch the
   ordinary pass RETAINS the entry and stays silent, because dropping it is the
   one thing that could lose a live waiter's only wakeup.  "Retries on the next
   release" is a fine answer while releases keep coming.  After
   tx_amiga_kernel_stop() none ever will, so the shutdown pass drops the stamp
   test: it signals the entry anyway and empties the table.

   THE TRADE THAT MAKES: at shutdown a stamp mismatch is far more likely to mean
   "this waiter is long gone and its address was recycled" than "this waiter
   moved", so the Signal may land on a different Task.  It lands on a bit that
   Task itself allocated, so the worst it can do is return one Wait() early,
   which any correct Exec program tolerates.  The alternative is a Task parked
   for ever in a library that has shut down.  One spurious wakeup beats one
   permanent hang.

   The mismatch is forced here the way nothing in production does it, by
   changing the parked Task's name pointer: _tx_amiga_task_stamp() is built from
   the stack bounds and ln_Name, and ln_Name is the only one of those main can
   move without disturbing the Task. */

static BtTask bt_final[BT_POOL_MAX];
static BtTask bt_final_waiter;

static char bt_final_name_a[] = "final-waiter";
static char bt_final_name_b[] = "final-waiter-renamed-to-break-its-stamp";

/* Parks on a full pool and never gets a slot.  Records its own signal
   allocation across the whole episode: a bit leaked or a bit freed twice both
   show up as a changed tc_SigAlloc. */
static VOID bt_final_waiter_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;
    ULONG        sig_before;
    ULONG        sig_after;
    UINT         status;

    Wait(BT_SIG_GO);

    sig_before = me->tc_SigAlloc;
    bt->bt_Ready = 1U;                  /* about to park */

    status = tx_amiga_adopt_thread(&bt->bt_Thread, &bt->bt_Gen,
                                   (CHAR *)"final waiter", 20, (UINT)TX_FALSE);

    sig_after = me->tc_SigAlloc;

    /* The kernel went away under it, so it must come back empty-handed. */
    if (status == TX_SUCCESS || bt->bt_Thread != TX_NULL || bt->bt_Gen != 0UL)
        bt->bt_Failures++;

    /* Exactly one FreeSignal() for exactly one AllocSignal(): a leak leaves a
       bit set that was not set before, a double free clears one that was. */
    if (sig_after != sig_before)
        bt->bt_Failures++;

    bt->bt_Rounds  = (LONG)status;
    bt->bt_Saw     = (LONG)(sig_after ^ sig_before);
    bt_finish(bt);
}

static VOID bt_test_shutdown_wakes_mismatched_waiter(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG        slots   = tx_amiga_adopt_slots();
    ULONG        reserve = tx_amiga_adopt_reserve();
    ULONG        ordinary;
    ULONG        i;
    ULONG        waiting;
    ULONG        waited;
    UINT         status;

    t_log("bracket: the shutdown wake, and a waiter whose stamp moved\n", 0, 0);

    ordinary = tx_amiga_adopt_slots_free();
    if (ordinary <= reserve || ordinary > (ULONG)BT_POOL_MAX)
    {
        t_check(0, "the pool is a size this test can cover", (LONG)ordinary);
        return;
    }
    ordinary -= reserve;

    /* ---- fill everything an ordinary caller may have -------------------- */

    SetSignal(0, BT_SIG_GO);
    for (i = 0UL; i < ordinary; i++)
    {
        bt_final[i].bt_Parent = me;
        if (bt_spawn(&bt_final[i], bt_rc_hold_entry, "final-hold", BT_PRI)
            == NULL)
        {
            t_check(0, "spawned every filler", (LONG)i);
            ordinary = i;
            break;
        }
    }
    for (i = 0UL; i < ordinary; i++)
        Signal(bt_final[i].bt_Task, BT_SIG_GO);
    Delay(25);

    t_check(tx_amiga_adopt_slots_free() == reserve,
            "the pool is full for an ordinary caller",
            (LONG)tx_amiga_adopt_slots_free());

    /* ---- one more, which parks ------------------------------------------ */

    bt_final_waiter.bt_Parent = me;
    t_check(bt_spawn(&bt_final_waiter, bt_final_waiter_entry,
                     bt_final_name_a, BT_PRI) != NULL,
            "spawned the waiter that will be left behind", 0);
    if (bt_final_waiter.bt_Task == NULL)
        return;

    Signal(bt_final_waiter.bt_Task, BT_SIG_GO);
    waited = 0UL;
    while (_tx_amiga_adopt_waiting == 0UL && waited < 250UL)
    {
        Delay(1);
        waited++;
    }
    Forbid();
    waiting = _tx_amiga_adopt_waiting;
    Permit();
    t_check(waiting == 1UL, "it parked waiting for a slot", (LONG)waiting);
    if (waiting != 1UL)
        return;

    /* ---- break its stamp, then give every slot back --------------------- */

    Forbid();
    bt_final_waiter.bt_Task->tc_Node.ln_Name = bt_final_name_b;
    Permit();
    t_check(_tx_amiga_task_stamp(bt_final_waiter.bt_Task) != 0UL,
            "the renamed Task still has a stamp", 0);

    for (i = 0UL; i < ordinary; i++)
        Signal(bt_final[i].bt_Task, BT_SIG_ACQUIRE);
    for (i = 0UL; i < ordinary; i++)
        bt_wait_for(&bt_final[i].bt_Done, "every filler to orphan and exit");

    /* Every one of those orphans ran the ORDINARY wake.  None of them may have
       signalled the waiter, and none of them may have dropped its entry. */
    Forbid();
    waiting = _tx_amiga_adopt_waiting;
    Permit();
    t_check(tx_amiga_adopt_slots_free() == slots,
            "every slot came back", (LONG)tx_amiga_adopt_slots_free());
    t_check(waiting == 1UL,
            "the mismatched waiter was retained, not dropped", (LONG)waiting);
    t_check(bt_final_waiter.bt_Done == 0U,
            "and never woken, with the pool standing empty in front of it",
            (LONG)bt_final_waiter.bt_Done);

    for (i = 0UL; i < ordinary; i++)
        bt_reap(&bt_final[i]);

    /* ---- the shutdown pass is the only thing that can free it ----------- */

    status = tx_amiga_kernel_stop();
    t_check(status == TX_SUCCESS, "ThreadX kernel stopped", (LONG)status);

    bt_wait_for(&bt_final_waiter.bt_Done,
                "the shutdown wake released the mismatched waiter");
    t_check(bt_final_waiter.bt_Done != 0U,
            "the waiter woke and unwound", (LONG)bt_final_waiter.bt_Done);
    t_check(bt_final_waiter.bt_Rounds == (LONG)TX_NOT_DONE,
            "it came back TX_NOT_DONE, not holding a thread",
            bt_final_waiter.bt_Rounds);
    t_check(bt_final_waiter.bt_Failures == 0,
            "with its signal freed exactly once and no handle",
            bt_final_waiter.bt_Failures);
    t_log("  final_sigalloc_delta=%ld final_status=%ld\n",
          bt_final_waiter.bt_Saw, bt_final_waiter.bt_Rounds);

    Forbid();
    waiting = _tx_amiga_adopt_waiting;
    Permit();
    t_check(waiting == 0UL, "and the shutdown pass emptied the table",
            (LONG)waiting);

    bt_reap(&bt_final_waiter);
}

/* ThreadX calls this from tx_kernel_enter(); the link fails without it. */
VOID tx_application_define(VOID *first_unused_memory)
{
    (VOID)first_unused_memory;
    bt_dead_flags_status = tx_event_flags_create(&bt_dead_flags,
                                                  (CHAR *)"dead task wait");
}

/* ------------------------------------------------------------------- the main -- */

/* Poll with a deadline, never Wait(): Exec signals are bits, so two set before
   the first Wait() arrive as one and the second Wait() blocks for ever. */
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

/* tx_interrupt_control() sets a posture, it does not add a nesting level when
   the requested posture is already in force.  The old implementation called
   Forbid() for DISABLE unconditionally, so the ordinary save/restore idiom
   leaked two levels when its caller was already forbidden.  This test reads
   the classic Exec counter deliberately: that is the state the public API is
   required to leave unchanged, and a scheduling probe would have to Wait()
   under a deliberately broken Forbid() to observe the same defect. */
static VOID bt_test_interrupt_control_idempotent(VOID)
{
    BYTE before;
    BYTE after_set;
    BYTE after_restore;
    BYTE enabled_before;
    BYTE enabled_after_set;
    BYTE enabled_after_repeat;
    BYTE enabled_after_restore;
    UINT old;
    UINT repeated;

    t_log("bracket: interrupt posture is idempotent\n", 0, 0);

    Forbid();
    before = SysBase->TDNestCnt;
    old = tx_interrupt_control(TX_INT_DISABLE);
    after_set = SysBase->TDNestCnt;
    (VOID)tx_interrupt_control(old);
    after_restore = SysBase->TDNestCnt;
    Permit();

    t_check(old == TX_INT_DISABLE,
            "DISABLE reports an already-disabled posture", (LONG)old);
    t_check(after_set == before,
            "setting the current posture adds no nesting", (LONG)after_set);
    t_check(after_restore == before,
            "restoring that posture adds no nesting", (LONG)after_restore);

    enabled_before = SysBase->TDNestCnt;
    old = tx_interrupt_control(TX_INT_DISABLE);
    enabled_after_set = SysBase->TDNestCnt;
    repeated = tx_interrupt_control(TX_INT_DISABLE);
    enabled_after_repeat = SysBase->TDNestCnt;
    (VOID)tx_interrupt_control(old);
    enabled_after_restore = SysBase->TDNestCnt;

    t_check(old == TX_INT_ENABLE,
            "DISABLE reports an enabled posture", (LONG)old);
    t_check(repeated == TX_INT_DISABLE,
            "repeated DISABLE reports a disabled posture", (LONG)repeated);
    t_check(enabled_after_set == (BYTE)(enabled_before + 1),
            "DISABLE adds exactly one nesting level", (LONG)enabled_after_set);
    t_check(enabled_after_repeat == enabled_after_set,
            "repeated DISABLE adds no nesting", (LONG)enabled_after_repeat);
    t_check(enabled_after_restore == enabled_before,
            "ENABLE restores the original nesting", (LONG)enabled_after_restore);
}

/* The target's stack is only safe to release once the live-zombie count is back
   at its baseline.  Built only where it is called: this case exhausts a native
   thread's handshake signals and a green thread has none. */
static VOID bt_test_no_signal_reap(VOID)
{
    BYTE  held[32];
    BYTE  sig = -1;
    APTR  arena;
    ULONG arena_size = BT_STACK + 200UL;
    ULONG historic_before;
    ULONG historic_after;
    ULONG live_before;
    ULONG live_after;
    ULONG waited;
    UWORD held_count = 0U;
    UINT  status;
    UINT  created = TX_FALSE;
    UINT  deleted = TX_FALSE;

    t_log("bracket: no-signal native-thread reaper fallback\n", 0, 0);

    arena = AllocMem(arena_size, MEMF_PUBLIC | MEMF_CLEAR);
    t_check(arena != NULL, "allocated the contained-stack arena", 0);
    if (arena == NULL)
        return;

    status = tx_amiga_adopt_thread(&bt_reap_owner, &bt_reap_owner_gen,
                                   (CHAR *)"reaper test owner", 20,
                                   (UINT)TX_FALSE);
    t_check(status == TX_SUCCESS, "adopted the reaper test owner",
            (LONG)status);
    if (status != TX_SUCCESS)
    {
        FreeMem(arena, arena_size);
        return;
    }

    bt_reap_entry_calls = 0UL;
    status = tx_thread_create(&bt_reap_target, (CHAR *)"reaper target",
                              bt_reap_target_entry, 0UL,
                              (APTR)((UBYTE *)arena + 100UL), BT_STACK,
                              20U, 20U, TX_NO_TIME_SLICE, TX_DONT_START);
    t_check(status == TX_SUCCESS, "created a dormant native-backed thread",
            (LONG)status);
    if (status == TX_SUCCESS)
        created = TX_TRUE;

    if (created != TX_FALSE)
    {
        t_check(tx_amiga_stack_in_use(arena, arena_size) == TX_TRUE,
                "an enclosing range overlaps the target stack", 0);

        /* The overlap rejection is _txe_thread_create()'s, and the shipping
           build compiles argument checking out, so there the probe is really
           created and has to come straight back out: left on the created list
           it owns the arena that is freed below, and every later
           tx_amiga_stack_in_use() would answer for it. */
        status = tx_thread_create(&bt_overlap_probe,
                                  (CHAR *)"overlap probe",
                                  bt_reap_target_entry, 0UL,
                                  arena, arena_size,
                                  20U, 20U, TX_NO_TIME_SLICE,
                                  TX_DONT_START);
#ifdef TX_DISABLE_ERROR_CHECKING
        t_check(status == TX_SUCCESS,
                "without argument checking the overlapping stack is taken",
                (LONG)status);
        if (status == TX_SUCCESS)
        {
            (VOID)tx_thread_terminate(&bt_overlap_probe);
            (VOID)tx_thread_delete(&bt_overlap_probe);
        }
#else
        t_check(status == TX_PTR_ERROR,
                "ThreadX rejects a stack containing an existing stack",
                (LONG)status);
#endif

        historic_before = tx_amiga_zombie_tasks();
        live_before = tx_amiga_zombie_tasks_live();

        while ((held_count < 32U) &&
               ((sig = AllocSignal(-1L)) >= (BYTE)0))
        {
            held[held_count++] = sig;
        }
        t_check(sig < (BYTE)0, "exhausted every spare signal bit",
                (LONG)held_count);

        status = tx_thread_terminate(&bt_reap_target);
        t_check(status == TX_SUCCESS, "terminated the dormant target",
                (LONG)status);
        if (status == TX_SUCCESS)
        {
            /* Raising this Task's priority is not sufficient:
               tx_thread_delete() may reschedule internally before it returns. */
            Forbid();
            status = tx_thread_delete(&bt_reap_target);
            if (status == TX_SUCCESS)
            {
                deleted = TX_TRUE;
                historic_after = tx_amiga_zombie_tasks();
                live_after = tx_amiga_zombie_tasks_live();
            }
            Permit();

            t_check(status == TX_SUCCESS,
                    "deleted without a reaper handshake signal",
                    (LONG)status);
        }

        if (deleted != TX_FALSE)
        {
            t_check(historic_after == historic_before + 1UL,
                    "the unconfirmed task was recorded as a zombie",
                    (LONG)historic_after);
            t_check(live_after == live_before + 1UL,
                    "the native task remains live until it destroys itself",
                    (LONG)live_after);
            t_check(bt_reap_target.tx_thread_amiga_task == NULL,
                    "the deleted control block no longer owns the task", 0);
            t_check(tx_amiga_stack_in_use(arena, arena_size) == TX_FALSE,
                    "delete removed the target from the created list", 0);
        }

        while (held_count != 0U)
            FreeSignal((LONG)held[--held_count]);
    }

    status = tx_amiga_orphan_thread(bt_reap_owner, bt_reap_owner_gen);
    t_check(status == TX_SUCCESS, "orphaned the reaper test owner",
            (LONG)status);

    if (deleted != TX_FALSE)
    {
        waited = 0UL;
        while ((tx_amiga_zombie_tasks_live() != live_before) &&
               (waited < (ULONG)(30 * 50)))
        {
            Delay(1);
            waited++;
        }
        t_check(tx_amiga_zombie_tasks_live() == live_before,
                "the detached native task eventually destroyed itself",
                (LONG)tx_amiga_zombie_tasks_live());
        t_check(bt_reap_entry_calls == 0UL,
                "a terminated dormant thread never entered user code",
                (LONG)bt_reap_entry_calls);
    }

    if ((created == TX_FALSE) ||
        ((deleted != TX_FALSE) &&
         (tx_amiga_zombie_tasks_live() == live_before)))
    {
        FreeMem(arena, arena_size);
    }
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    UINT         status;
    UWORD        i;
    UWORD        spawned = 0;
    ULONG        want    = 0UL;
    LONG         rounds  = 0;

    (VOID)argc; (VOID)argv;

    t_log("bracket: the ThreadX/Exec adoption layer, %ld workers, %ld rounds\n",
          (LONG)BT_WORKERS, (LONG)BT_ROUNDS);

    status = tx_amiga_kernel_start();
    t_check(status == TX_SUCCESS, "ThreadX kernel started", (LONG)status);
    if (status != TX_SUCCESS)
    {
        t_log("%ld checks, %ld failures, FAIL\n", (LONG)t_checks,
              (LONG)t_failures);
        return 20;
    }

    t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
            "an unadopted Task is not the baton holder", 0);

    bt_test_interrupt_control_idempotent();

    /* Green threads have no native Exec task to become a reaper zombie; this
       case specifically exhausts the native thread's handshake signals. */
    bt_test_no_signal_reap();

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

        t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
                "another Task holding the baton does not make us a thread", 0);

        t_check(tx_amiga_adopted_thread() != bt_holder.bt_Thread,
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

    t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
            "unadopted after the churn, as before it", 0);

    /* ---- a Task that exits without unwinding --------------------------- */

    t_log("bracket: cleaning registrations left by dead Exec Tasks\n", 0, 0);

    bt_dead_holding.bt_Parent = me;
    t_check(bt_spawn(&bt_dead_holding, bt_die_holding_entry,
                     "dead-holding", BT_PRI) != NULL,
            "spawned baton-holding death", 0);
    if (bt_dead_holding.bt_Task != NULL)
    {
        Signal(bt_dead_holding.bt_Task, BT_SIG_GO);
        if (bt_wait_dead(&bt_dead_holding, "baton-holding Task to exit"))
        {
            bt_discard_dead(&bt_dead_holding, FALSE,
                            "holding death has no released bracket");
        }
    }

    bt_dead_dormant.bt_Parent = me;
    t_check(bt_spawn(&bt_dead_dormant, bt_die_dormant_entry,
                     "dead-dormant", BT_PRI) != NULL,
            "spawned dormant cached death", 0);
    if (bt_dead_dormant.bt_Task != NULL)
    {
        Signal(bt_dead_dormant.bt_Task, BT_SIG_GO);
        if (bt_wait_dead(&bt_dead_dormant, "dormant Task to exit"))
        {
            bt_discard_dead(&bt_dead_dormant, FALSE,
                            "dormant death has no released bracket");
        }
    }

    bt_dead_released.bt_Parent = me;
    t_check(bt_spawn(&bt_dead_released, bt_die_released_entry,
                     "dead-released", BT_PRI) != NULL,
            "spawned released-baton death", 0);
    if (bt_dead_released.bt_Task != NULL)
    {
        ULONG live_before = ami_baton_stats.bs_Live;

        Signal(bt_dead_released.bt_Task, BT_SIG_GO);
        if (bt_wait_dead(&bt_dead_released,
                         "released-baton Task to exit"))
        {
            t_check(ami_baton_stats.bs_Live == live_before + 1UL,
                    "released death left one live baton bracket",
                    (LONG)ami_baton_stats.bs_Live);
            t_check(ami_netstack_baton_abandon(&bt_decoy_thread)
                    == FALSE,
                    "another TX_THREAD cannot inherit the dead Task's bracket",
                    0);
            t_check(ami_baton_stats.bs_Live == live_before + 1UL,
                    "a wrong-identity abandon leaves the live count alone",
                    (LONG)ami_baton_stats.bs_Live);
            bt_discard_dead(&bt_dead_released, TRUE,
                            "released death owned one baton bracket");
            t_check(ami_baton_stats.bs_Live == live_before,
                    "abandon returned the baton live count",
                    (LONG)ami_baton_stats.bs_Live);
            t_check(ami_netstack_baton_abandon(bt_dead_released.bt_Thread)
                    == FALSE,
                    "abandon is idempotent after the slot is gone", 0);
        }
    }

    t_check(bt_dead_flags_status == TX_SUCCESS,
            "created the forced-death event group", (LONG)bt_dead_flags_status);
    if (bt_dead_flags_status == TX_SUCCESS)
    {
        ULONG waited = 0UL;

        bt_dead_event.bt_Parent = me;
        t_check(bt_spawn(&bt_dead_event, bt_die_event_entry,
                         "dead-event", BT_PRI) != NULL,
                "spawned ThreadX-suspended death", 0);
        if (bt_dead_event.bt_Task != NULL)
        {
            Signal(bt_dead_event.bt_Task, BT_SIG_GO);

            while ((bt_dead_event.bt_Thread->tx_thread_state != TX_EVENT_FLAG ||
                    bt_dead_flags.tx_event_flags_group_suspended_count != 1UL) &&
                   waited < (ULONG)(30 * 50))
            {
                Delay(1);
                waited++;
            }

            t_check(bt_dead_event.bt_Thread->tx_thread_state == TX_EVENT_FLAG,
                    "victim suspended inside ThreadX",
                    (LONG)bt_dead_event.bt_Thread->tx_thread_state);
            t_check(bt_dead_flags.tx_event_flags_group_suspended_count == 1UL,
                    "event group contains the victim",
                    (LONG)bt_dead_flags.tx_event_flags_group_suspended_count);

            if (bt_dead_event.bt_Thread->tx_thread_state == TX_EVENT_FLAG &&
                bt_dead_flags.tx_event_flags_group_suspended_count == 1UL)
            {
                RemTask(bt_dead_event.bt_Task);
                bt_dead_event.bt_Task = NULL;
                bt_discard_dead(&bt_dead_event, FALSE,
                                "ThreadX wait has no released bracket");
                t_check(bt_dead_flags.tx_event_flags_group_suspended_count == 0UL,
                        "discard unlinked the event suspension",
                        (LONG)bt_dead_flags.tx_event_flags_group_suspended_count);
            }
            else
            {
                (VOID)tx_event_flags_set(&bt_dead_flags, 1UL, TX_OR);
                bt_wait_for(&bt_dead_event.bt_Done,
                            "failed event victim to leave after wakeup");
                if (bt_dead_event.bt_Done != 0U)
                {
                    bt_dead_event.bt_Task = NULL;
                    bt_discard_dead(&bt_dead_event, FALSE,
                                    "woken event victim has no released bracket");
                }
            }
        }


        t_check(tx_event_flags_delete(&bt_dead_flags) == TX_SUCCESS,
                "deleted the forced-death event group", 0);
    }

    /* ---- a Task removed by Exec while it holds the baton ---------------- */

    bt_test_reclaim_dead_holder();
    bt_test_live_waiter_not_dead();
    bt_test_onstack_caller_and_slot_reuse();
    bt_test_cached_handle();
    bt_test_corpse_leftovers();
    bt_test_unpublished_slot_sweep();
    bt_test_pool_exhaustion();

    /* ---- the bracket, watched from inside its own window ---------------- */

    SetSignal(0, BT_SIG_GO);
    bt_phase_stop = 0U;
    bt_baton_released = 0U;
    spawned       = 0;

    bt_probe.bt_Parent = me;
    t_check(bt_spawn(&bt_probe, bt_probe_entry, "bracket-probe",
                     BT_PROBE_PRI) != NULL, "spawned the probe Task", 0);

    /* Every one of these holds its adoption for the whole phase, and main
       waits for all of them at the barrier below, so asking for more than the
       port has slots would wedge the barrier rather than prove anything.  The
       pool's own ceiling is bt_test_pool_exhaustion()'s subject. */
    want = tx_amiga_adopt_slots_free() - tx_amiga_adopt_reserve();
    if (want > (ULONG)BT_BATON_TASKS)
        want = (ULONG)BT_BATON_TASKS;
    t_log("  baton_tasks_wanted=%ld baton_tasks_run=%lu\n",
          (LONG)BT_BATON_TASKS, (LONG)want);

    for (i = 0; i < (UWORD)want; i++)
    {
        bt_baton[i].bt_Parent = me;
        if (bt_spawn(&bt_baton[i], bt_baton_entry, "bracket-baton", BT_PRI) != NULL)
            spawned++;
    }
    t_check(spawned == (UWORD)want, "spawned every baton Task", (LONG)spawned);

    for (i = 0; i < BT_BATON_TASKS; i++)
        if (bt_baton[i].bt_Task != NULL)
            Signal(bt_baton[i].bt_Task, BT_SIG_GO);

    {
        ULONG waited = 0UL;

        while (bt_baton_released < spawned && waited < (ULONG)(30 * 50))
        {
            Delay(1);
            waited++;
        }

        t_check(bt_baton_released == spawned,
                "every baton Task reached the released barrier",
                (LONG)bt_baton_released);
    }

    for (i = 0; i < BT_BATON_TASKS; i++)
        if (bt_baton[i].bt_Task != NULL)
            Signal(bt_baton[i].bt_Task, BT_SIG_ACQUIRE);

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

    t_check(bt_probe_samples >= 100UL, "the probe sampled often enough",
            (LONG)bt_probe_samples);

    t_check(bt_probe_shared == 0UL,
            "_tx_thread_system_state is never raised with switching enabled",
            (LONG)bt_probe_shared);

    t_check(ami_baton_stats.bs_StateShared == 0UL,
            "no bracket found the interrupt state already raised",
            (LONG)ami_baton_stats.bs_StateShared);

    /* The release/acquire bracket lives in the TX_THREAD, so EVERY adopted
       Task can be in one at the same time; there is no separate baton table.
       What bounds it now is how many Tasks can be adopted at once, which is
       the port's slot pool, and that ceiling is bt_test_pool_exhaustion()'s
       subject.  The old spelling of this check was "more than sixteen", which
       was about a sixteen-entry baton table that no longer exists. */
    t_check(ami_baton_stats.bs_LiveMax == (ULONG)spawned,
            "every adopted Task was in a released bracket at once",
            (LONG)ami_baton_stats.bs_LiveMax);
    t_check((ULONG)spawned ==
            tx_amiga_adopt_slots() - tx_amiga_adopt_reserve(),
            "and that was every slot an ordinary caller may have",
            (LONG)spawned);
    t_check(ami_baton_stats.bs_Full == 0UL, "no fixed baton table remains",
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

    /* This case OWNS tx_amiga_kernel_stop(), which has to happen before main()
       returns: the kernel's VERTB interrupt server lives in this program's
       hunk, which AmigaDOS frees on exit. */
    bt_test_shutdown_wakes_mismatched_waiter();

    /* P0-2.  A claim reads the kernel flags under the same Forbid() the stop
       commits under, so an adoption that arrives after the commit is refused
       rather than landing a fresh thread on a kernel that has already counted
       them.  A refused claim must also leave the pool exactly as it found it. */
    {
        TX_THREAD *late_thread = TX_NULL;
        ULONG      late_gen    = 0UL;
        UINT       late_status;

        late_status = tx_amiga_adopt_thread(&late_thread, &late_gen,
                                            (CHAR *)"after the stop", 20,
                                            (UINT)TX_FALSE);
        t_check(late_status == TX_NOT_DONE,
                "adoption after the kernel stopped is refused",
                (LONG)late_status);
        t_check(late_thread == TX_NULL && late_gen == 0UL,
                "and hands back no handle", (LONG)late_gen);
        t_check(tx_amiga_adopt_slots_free() == tx_amiga_adopt_slots(),
                "the refused claim left every slot free",
                (LONG)tx_amiga_adopt_slots_free());
        t_check(_tx_amiga_adopt_waiting == 0UL,
                "and nobody is parked waiting for one",
                (LONG)_tx_amiga_adopt_waiting);
    }

    t_log("  pool_parks=%lu pool_waiter_full=%lu\n",
          (LONG)_tx_amiga_adopt_parks, (LONG)_tx_amiga_adopt_waiter_full);
    t_log("  pool_unpublished_freed=%lu pool_peak=%lu\n",
          (LONG)_tx_amiga_adopt_unpublished_freed, (LONG)_tx_amiga_adopt_peak);

    t_log("%ld checks, %ld failures, ", (LONG)t_checks, (LONG)t_failures);
    t_log("%s\n", (LONG)((t_failures == 0UL) ? "PASS" : "FAIL"), 0);

    return (t_failures == 0UL) ? 0 : 20;
}
