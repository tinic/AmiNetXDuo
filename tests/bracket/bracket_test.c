/*
 * AmiNetXDuo, the ThreadX/Exec adoption layer, on its own.
 *
 * WHY THIS EXISTS
 *
 * The defect of docs/RESEARCH.md 77.6 was one pointer read: ami_netstack_enter()
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
 * deliberate adopt/orphan churn, and never asks the question this file asks.
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
 * to that one, and t_baton_is_not_shared() can fail against the old code,
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
BOOL ami_netstack_baton_abandon(TX_THREAD *thread);
extern volatile ULONG _tx_thread_system_state;
extern TX_THREAD *_tx_thread_current_ptr;

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
 * and wedging is one of the things this file is here to catch. A sibling
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
 * the full account of what that looked like, AN_FreeTwice when Exec noticed,
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
static BtTask bt_dead_holding;
static BtTask bt_dead_dormant;
static BtTask bt_dead_released;
static BtTask bt_dead_event;
static TX_EVENT_FLAGS_GROUP bt_dead_flags;
static volatile UINT bt_dead_flags_status = TX_NOT_DONE;
static TX_THREAD bt_reap_owner;
static TX_THREAD bt_reap_target;
static TX_THREAD bt_overlap_probe;
static volatile ULONG bt_reap_entry_calls;

static VOID bt_wait_for(volatile UWORD *flag, const char *what);
static VOID bt_reap(BtTask *bt);

static VOID bt_reap_target_entry(ULONG input)
{
    (VOID)input;
    bt_reap_entry_calls++;
}

/*
 * How far the holder got. It runs as a plain Task, so it cannot print, and a
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

/*
 * A spawned Task's last act.  RemTask() rather than falling off the end of the
 * entry point, and the flag and the Signal go inside the Forbid() that ends in
 * it.
 *
 * Returning instead lets Exec's default finaliser remove the task some
 * instructions later, and in those instructions main() has already seen
 * bt_Done, run bt_reap() and FreeMem()ed the stack the dying task is still
 * standing on, and then handed that same block to the next Task it spawns.
 * The machine resets: the entry point returns through a stack that now belongs
 * to somebody else, and the PC lands in the middle of a struct Task.  100%
 * reproducible on Kickstart 3.1 under both Amiberry and FS-UAE, and it is what
 * "17/17 checks passed" was hiding, the checks that print are the ones before
 * the first reap.
 *
 * Exec discards the forbid nesting of a task it removes, so the flag, the
 * Signal and the removal are one indivisible step: main cannot see bt_Done set
 * and still find this Task alive.  port/threadx-amiga's _tx_amiga_task_destroy()
 * has the same shape for the same reason.
 */
static VOID bt_finish(BtTask *bt)
{
    Forbid();
    bt->bt_Done = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);
    RemTask(NULL);

    /* Unreachable.  */
    for (;;)
        Wait(0UL);
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
        bt_finish(bt);
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
    bt_finish(bt);
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

    bt_finish(bt);
}

/* ---------------------------------------------------- forced task death -- */

/*
 * Four points at which an Exec Task may be removed without unwinding:
 *
 *   holding   the adopted thread is _tx_thread_current_ptr;
 *   dormant   its cached TX_THREAD is suspended between calls;
 *   released  netstack_baton.c owns a slot while the Task is in Exec;
 *   event     ThreadX has linked it into an object's suspension list.
 *
 * The first three remove themselves, exactly like a command killed while it
 * is running.  The event waiter cannot execute its own RemTask(), so main
 * removes it after observing the actual TX_EVENT_FLAG state.
 */
static VOID bt_die_holding_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;

    Wait(BT_SIG_GO);
    if (tx_amiga_adopt_thread(&bt->bt_Thread, (CHAR *)"dead holding", 20)
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
    if (tx_amiga_adopt_thread(&bt->bt_Thread, (CHAR *)"dead dormant", 20)
        != TX_SUCCESS ||
        tx_amiga_adopt_suspend(&bt->bt_Thread) != TX_SUCCESS)
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
    if (tx_amiga_adopt_thread(&bt->bt_Thread, (CHAR *)"dead released", 20)
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
    if (tx_amiga_adopt_thread(&bt->bt_Thread, (CHAR *)"dead event", 20)
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

    had_slot = ami_netstack_baton_abandon(&bt->bt_Thread);
    t_check(had_slot == expect_baton_slot, what, (LONG)had_slot);

    status = tx_amiga_discard_thread(&bt->bt_Thread);
    t_check(status == TX_SUCCESS, "foreign dead TX_THREAD was discarded",
            (LONG)status);
    t_check(bt->bt_Thread.tx_thread_id == 0UL,
            "discard deleted the dead TX_THREAD", 0);
    t_check(tx_amiga_stack_in_use(bt->bt_Stack, bt->bt_StackSize) == TX_FALSE,
            "discard released the dead Task's stack range", 0);

    Forbid();
    still_baton = (_tx_thread_current_ptr == &bt->bt_Thread);
    Permit();
    t_check(still_baton == FALSE,
            "dead TX_THREAD is no longer the global baton holder", 0);

    bt_reap(bt);
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
 * address zero, the Enforcer hits this phase exists for.
 *
 * The port's rule is therefore that the counter may only be raised under an
 * unbroken Forbid(): tx_thread_context_save.c states it, tx_amiga_adopt.c
 * repeats it. netstack_baton.c raised it and then Permit()ed, which is the
 * defect.
 *
 * Catching that needs an observer that runs INSIDE the window, and no Task can
 * poll its way in, Exec will not switch between two Tasks of the same
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
        bt_finish(bt);
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

    bt_finish(bt);
}

/*
 * ThreadX calls this from tx_kernel_enter() and the link fails without it. This
 * layer is what is under test, so there is nothing to define: every thread here
 * arrives by adoption from an Exec Task, which is the whole point.
 */
VOID tx_application_define(VOID *first_unused_memory)
{
    (VOID)first_unused_memory;
    bt_dead_flags_status = tx_event_flags_create(&bt_dead_flags,
                                                  (CHAR *)"dead task wait");
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

/*
 * Exercise the branch where _tx_amiga_reap() cannot allocate a handshake
 * signal.  Keeping this Task above the native target's Exec priority makes the
 * ordering deterministic: delete has to detach and record the still-live task
 * before it can run.  Only after the live-zombie count returns to its baseline
 * is the target's stack safe to release.
 *
 * The target lives in the middle of its allocation so the same fixture can
 * ask both overlap checkers about a new range that wholly contains it.  That
 * is the shape endpoint-only comparisons used to miss.
 */
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
    LONG  old_priority;
    UINT  status;
    UINT  created = TX_FALSE;
    UINT  deleted = TX_FALSE;

    t_log("bracket: no-signal native-thread reaper fallback\n", 0, 0);

    arena = AllocMem(arena_size, MEMF_PUBLIC | MEMF_CLEAR);
    t_check(arena != NULL, "allocated the contained-stack arena", 0);
    if (arena == NULL)
        return;

    status = tx_amiga_adopt_thread(&bt_reap_owner,
                                   (CHAR *)"reaper test owner", 20);
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

        status = tx_thread_create(&bt_overlap_probe,
                                  (CHAR *)"overlap probe",
                                  bt_reap_target_entry, 0UL,
                                  arena, arena_size,
                                  20U, 20U, TX_NO_TIME_SLICE,
                                  TX_DONT_START);
        t_check(status == TX_PTR_ERROR,
                "ThreadX rejects a stack containing an existing stack",
                (LONG)status);

        historic_before = tx_amiga_zombie_tasks();
        live_before = tx_amiga_zombie_tasks_live();

        old_priority = (LONG)SetTaskPri(FindTask(NULL), 10);
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
            status = tx_thread_delete(&bt_reap_target);
            t_check(status == TX_SUCCESS,
                    "deleted without a reaper handshake signal",
                    (LONG)status);
            if (status == TX_SUCCESS)
                deleted = TX_TRUE;
        }

        if (deleted != TX_FALSE)
        {
            /* Snapshot both counters before reporting either assertion.
               t_check() flushes console output and may block there, which
               gives the dying task time to remove itself from the live count
               between two otherwise adjacent checks. */
            historic_after = tx_amiga_zombie_tasks();
            live_after = tx_amiga_zombie_tasks_live();

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
        (VOID)SetTaskPri(FindTask(NULL), old_priority);
    }

    status = tx_amiga_orphan_thread(&bt_reap_owner);
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

    /* main() has adopted nothing, and nothing else is running yet. */
    t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
            "an unadopted Task is not the baton holder", 0);

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

        /*
         * THE ONE THAT MATTERS. Another Task holds the baton right now. main()
         * has adopted nothing, so the layer must say no. The released defect
         * said yes here, and everything downstream, a caller skipping
         * adoption and entering NetX Duo unbracketed, followed from it.
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
                            "holding death has no release slot");
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
                            "dormant death has no release slot");
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
                    "released death left one live baton slot",
                    (LONG)ami_baton_stats.bs_Live);
            t_check(ami_netstack_baton_abandon(&bt_dead_dormant.bt_Thread)
                    == FALSE,
                    "another TX_THREAD cannot inherit the dead Task's slot",
                    0);
            t_check(ami_baton_stats.bs_Live == live_before + 1UL,
                    "a wrong-identity abandon leaves the live count alone",
                    (LONG)ami_baton_stats.bs_Live);
            bt_discard_dead(&bt_dead_released, TRUE,
                            "released death owned one baton slot");
            t_check(ami_baton_stats.bs_Live == live_before,
                    "abandon returned the baton live count",
                    (LONG)ami_baton_stats.bs_Live);
            t_check(ami_netstack_baton_abandon(&bt_dead_released.bt_Thread)
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

            while ((bt_dead_event.bt_Thread.tx_thread_state != TX_EVENT_FLAG ||
                    bt_dead_flags.tx_event_flags_group_suspended_count != 1UL) &&
                   waited < (ULONG)(30 * 50))
            {
                Delay(1);
                waited++;
            }

            t_check(bt_dead_event.bt_Thread.tx_thread_state == TX_EVENT_FLAG,
                    "victim suspended inside ThreadX",
                    (LONG)bt_dead_event.bt_Thread.tx_thread_state);
            t_check(bt_dead_flags.tx_event_flags_group_suspended_count == 1UL,
                    "event group contains the victim",
                    (LONG)bt_dead_flags.tx_event_flags_group_suspended_count);

            if (bt_dead_event.bt_Thread.tx_thread_state == TX_EVENT_FLAG &&
                bt_dead_flags.tx_event_flags_group_suspended_count == 1UL)
            {
                RemTask(bt_dead_event.bt_Task);
                bt_dead_event.bt_Task = NULL;
                bt_discard_dead(&bt_dead_event, FALSE,
                                "ThreadX wait has no release slot");
                t_check(bt_dead_flags.tx_event_flags_group_suspended_count == 0UL,
                        "discard unlinked the event suspension",
                        (LONG)bt_dead_flags.tx_event_flags_group_suspended_count);
            }
            else
            {
                /* Do not leave a live waiter behind after a diagnostic
                   failure: that would turn shutdown into a second,
                   misleading failure. Once woken, bt_finish() removes the
                   Exec Task atomically; its stale TX_THREAD can be discarded
                   through the same cleanup path. */
                (VOID)tx_event_flags_set(&bt_dead_flags, 1UL, TX_OR);
                bt_wait_for(&bt_dead_event.bt_Done,
                            "failed event victim to leave after wakeup");
                if (bt_dead_event.bt_Done != 0U)
                {
                    bt_dead_event.bt_Task = NULL;
                    bt_discard_dead(&bt_dead_event, FALSE,
                                    "woken event victim has no release slot");
                }
            }
        }

        t_check(tx_event_flags_delete(&bt_dead_flags) == TX_SUCCESS,
                "deleted the forced-death event group", 0);
    }

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
     * Task in the machine, and the Task it dispatches is holding the baton,
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

    /*
     * The kernel comes down before the program does. tx_amiga_kernel_start()
     * leaves a VERTB interrupt server whose struct Interrupt, and whose
     * is_Code, are in this program's hunk, and AmigaDOS frees that hunk the
     * instant main() returns; the next VBlank then calls into it. Nothing
     * above needs deleting first: every TX_THREAD here belongs to an Exec Task
     * that adopted itself and orphaned itself again, which is the whole point
     * of this test, so the kernel owns no application thread by now.
     */
    t_check(tx_amiga_kernel_stop() == TX_SUCCESS, "ThreadX kernel stopped", 0);

    t_log("%ld checks, %ld failures, ", (LONG)t_checks, (LONG)t_failures);
    t_log("%s\n", (LONG)((t_failures == 0UL) ? "PASS" : "FAIL"), 0);

    return (t_failures == 0UL) ? 0 : 20;
}
