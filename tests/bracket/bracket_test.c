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

    TX_THREAD       bt_Thread;
} BtTask;

static BtTask bt_worker[BT_WORKERS];
static BtTask bt_holder;
static BtTask bt_dead_holding;
static BtTask bt_dead_dormant;
static BtTask bt_dead_released;
static BtTask bt_dead_event;
#ifdef AMINETXDUO_GREEN_REALM
static BtTask bt_dead_gate_owner;
#endif
static TX_EVENT_FLAGS_GROUP bt_dead_flags;
#ifdef AMINETXDUO_GREEN_REALM
static TX_AMIGA_GATE bt_dead_gate;
static TX_TIMER bt_gate_port_timer;
static struct MsgPort *bt_gate_port;
static struct Message bt_gate_message;

BYTE ami_green_checked_waitio(struct IORequest *request);
struct Message *ami_green_checked_waitport(struct MsgPort *port);
#endif
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

    if (tx_amiga_adopt_thread(&bt->bt_Thread, (CHAR *)"bracket holder", 20)
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
    (VOID)tx_amiga_orphan_thread(&bt->bt_Thread);
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

/* Enter through the production request gate, then stop in the same kind of
   ThreadX suspension a blocking socket receive uses.  main removes the parked
   owner Task while this continuation is still active. */
#ifdef AMINETXDUO_GREEN_REALM
static VOID bt_gate_post_message(ULONG input)
{
    (VOID)input;
    PutMsg(bt_gate_port, &bt_gate_message);
}

static VOID bt_gate_test_exec_waits(BtTask *bt)
{
    struct MsgPort     *io_port;
    struct timerequest *request;
    struct Message     *message;
    TX_AMIGA_GREEN_STATS before;
    TX_AMIGA_GREEN_STATS after;
    UINT status;

    /* An incomplete timer request makes WaitIO's blocking arm deterministic. */
    io_port = CreateMsgPort();
    request = (io_port != NULL)
            ? (struct timerequest *)CreateIORequest(
                  io_port, (ULONG)sizeof(struct timerequest))
            : NULL;
    if (request == NULL ||
        OpenDevice((CONST_STRPTR)"timer.device", (ULONG)UNIT_VBLANK,
                   (struct IORequest *)request, 0UL) != 0)
    {
        bt->bt_Failures++;
        if (request != NULL)
            DeleteIORequest((struct IORequest *)request);
        if (io_port != NULL)
            DeleteMsgPort(io_port);
        return;
    }

    request->tr_node.io_Command = TR_ADDREQUEST;
    request->tr_time.tv_secs    = 0UL;
    request->tr_time.tv_micro   = 100000UL;
    SendIO((struct IORequest *)request);

    tx_amiga_green_stats(&before);
    (VOID)ami_green_checked_waitio((struct IORequest *)request);
    tx_amiga_green_stats(&after);
    bt->bt_Rounds = (LONG)(after.gs_stray_wait - before.gs_stray_wait);
    if (bt->bt_Rounds != 1)
        bt->bt_Failures++;

    CloseDevice((struct IORequest *)request);
    DeleteIORequest((struct IORequest *)request);
    DeleteMsgPort(io_port);

    /* WaitPort must return the first message without removing it.  A ThreadX
       timer posts after this proxy has suspended in the green-wait wrapper. */
    bt_gate_port = CreateMsgPort();
    if (bt_gate_port == NULL)
    {
        bt->bt_Failures++;
        return;
    }

    bt_gate_message.mn_Node.ln_Type = NT_MESSAGE;
    bt_gate_message.mn_ReplyPort    = NULL;
    bt_gate_message.mn_Length       = (UWORD)sizeof(bt_gate_message);

    status = tx_timer_create(&bt_gate_port_timer, (CHAR *)"gate port post",
                             bt_gate_post_message, 0UL, 2UL, 0UL,
                             TX_AUTO_ACTIVATE);
    if (status != TX_SUCCESS)
    {
        bt->bt_Failures++;
        DeleteMsgPort(bt_gate_port);
        bt_gate_port = NULL;
        return;
    }

    tx_amiga_green_stats(&before);
    message = ami_green_checked_waitport(bt_gate_port);
    tx_amiga_green_stats(&after);
    bt->bt_Saw = (LONG)(after.gs_stray_wait - before.gs_stray_wait);
    if (bt->bt_Saw != 1 || message != &bt_gate_message ||
        GetMsg(bt_gate_port) != &bt_gate_message)
    {
        bt->bt_Failures++;
    }

    (VOID)tx_timer_delete(&bt_gate_port_timer);
    DeleteMsgPort(bt_gate_port);
    bt_gate_port = NULL;
}

static VOID bt_die_gate_entry(VOID)
{
    struct Task *me = FindTask(NULL);
    BtTask      *bt = (BtTask *)me->tc_UserData;
    ULONG        actual = 0UL;
    UINT         status;

    Wait(BT_SIG_GO);

    status = tx_amiga_gate_bind(&bt_dead_gate, (CHAR *)"dead request gate", 20);
    if (status != TX_SUCCESS)
    {
        bt->bt_Failures = (LONG)status;
        bt_finish(bt);
    }

    status = tx_amiga_gate_call(&bt_dead_gate, 0UL);
    if (status != TX_SUCCESS)
    {
        bt->bt_Failures = (LONG)status;
        tx_amiga_gate_release(&bt_dead_gate);
        bt_finish(bt);
    }

    /* From here onward this continuation is the gate's green proxy, not the
       Exec owner.  It must never execute bt_finish(), which would remove the
       realm Task rather than the parked owner. */
    bt_gate_test_exec_waits(bt);

    bt->bt_Ready = 1U;
    Signal(bt->bt_Parent, BT_SIG_GO);

    (VOID)tx_event_flags_get(&bt_dead_flags, 2UL, TX_OR_CLEAR, &actual,
                             TX_WAIT_FOREVER);

    bt->bt_Failures++;
    for (;;)
        (VOID)tx_thread_sleep(0x7FFFFFFFUL);
}
#endif

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

/* Port rule under test: _tx_thread_system_state may only be raised under an
   unbroken Forbid().  The probe is the highest-priority Task in the phase and
   wakes on a timer interrupt, so it lands inside any Permit() window. */

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

/* The target's stack is only safe to release once the live-zombie count is back
   at its baseline. */
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

    t_check(tx_amiga_caller_is_thread() == (UINT)TX_FALSE,
            "an unadopted Task is not the baton holder", 0);

#ifndef AMINETXDUO_GREEN_REALM
    /* Green threads have no native Exec task to become a reaper zombie; this
       case specifically exhausts the native thread's handshake signals. */
    bt_test_no_signal_reap();
#endif

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

#ifdef AMINETXDUO_GREEN_REALM
        /* A request owner can disappear while its continuation is suspended
           in NetX.  This is the production gate path, including the owner-side
           stack switch and Wait(), rather than another adopted-thread test. */
        {
            ULONG waited = 0UL;
            UINT  reaped;

            bt_dead_gate_owner.bt_Parent = me;
            t_check(bt_spawn(&bt_dead_gate_owner, bt_die_gate_entry,
                             "dead-gate-owner", BT_PRI) != NULL,
                    "spawned request-gate owner death", 0);
            if (bt_dead_gate_owner.bt_Task != NULL)
            {
                Signal(bt_dead_gate_owner.bt_Task, BT_SIG_GO);

                while ((bt_dead_gate_owner.bt_Ready == 0U ||
                        bt_dead_gate.ag_Thread.tx_thread_state != TX_EVENT_FLAG ||
                        bt_dead_flags.tx_event_flags_group_suspended_count != 1UL) &&
                       waited < (ULONG)(30 * 50))
                {
                    Delay(1);
                    waited++;
                }

                t_check(bt_dead_gate_owner.bt_Failures == 0,
                        "request gate entered without an error",
                        bt_dead_gate_owner.bt_Failures);
                t_check(bt_dead_gate_owner.bt_Rounds == 1,
                        "blocking WaitIO suspended only its green proxy",
                        bt_dead_gate_owner.bt_Rounds);
                t_check(bt_dead_gate_owner.bt_Saw == 1,
                        "blocking WaitPort preserved its message contract",
                        bt_dead_gate_owner.bt_Saw);
                t_check(bt_dead_gate.ag_Active != 0U,
                        "request was active when its owner died", 0);
                t_check(bt_dead_gate.ag_Thread.tx_thread_state == TX_EVENT_FLAG,
                        "request proxy suspended inside ThreadX",
                        (LONG)bt_dead_gate.ag_Thread.tx_thread_state);
                t_check(bt_dead_flags.tx_event_flags_group_suspended_count == 1UL,
                        "event group contains the request proxy",
                        (LONG)bt_dead_flags.tx_event_flags_group_suspended_count);

                if (bt_dead_gate.ag_Thread.tx_thread_state == TX_EVENT_FLAG &&
                    bt_dead_flags.tx_event_flags_group_suspended_count == 1UL)
                {
                    RemTask(bt_dead_gate_owner.bt_Task);
                    bt_dead_gate_owner.bt_Task = NULL;

                    reaped = tx_amiga_gate_orphan(&bt_dead_gate);
                    t_check(reaped == TX_TRUE,
                            "dead request owner was reaped immediately",
                            (LONG)reaped);
                    t_check(bt_dead_gate.ag_OwnerDead != 0U,
                            "request gate records its dead owner", 0);
                    t_check(bt_dead_gate.ag_Live == 0U,
                            "request proxy was deleted", 0);
                    t_check(bt_dead_gate.ag_Thread.tx_thread_id == 0UL,
                            "request proxy control block was cleared", 0);
                    t_check(bt_dead_gate.ag_Side == NULL,
                            "dead owner's side stack was released", 0);
                    t_check(bt_dead_gate.ag_Task == NULL &&
                            bt_dead_gate.ag_DoneMask == 0UL,
                            "dead owner's signal bookkeeping was dropped", 0);
                    t_check(bt_dead_flags.tx_event_flags_group_suspended_count == 0UL,
                            "reap unlinked the proxy's ThreadX wait",
                            (LONG)bt_dead_flags.tx_event_flags_group_suspended_count);
                    t_check(tx_amiga_stack_in_use(bt_dead_gate_owner.bt_Stack,
                                                  bt_dead_gate_owner.bt_StackSize)
                            == TX_FALSE,
                            "reap released the dead owner's shared stack", 0);

                    bt_reap(&bt_dead_gate_owner);
                }
                else
                {
                    /* Keep a failed assertion from leaving either the owner
                       or the realm behind and obscuring the remaining suite. */
                    if (bt_dead_gate_owner.bt_Done != 0U)
                    {
                        bt_reap(&bt_dead_gate_owner);
                    }
                    else
                    {
                        ULONG reap_waited = 0UL;

                        RemTask(bt_dead_gate_owner.bt_Task);
                        bt_dead_gate_owner.bt_Task = NULL;
                        while (bt_dead_gate.ag_Live != 0U &&
                               tx_amiga_gate_orphan(&bt_dead_gate) == TX_FALSE &&
                               reap_waited < (ULONG)(30 * 50))
                        {
                            Delay(1);
                            reap_waited++;
                        }
                        t_check(bt_dead_gate.ag_Live == 0U,
                                "failed gate case was still made quiescent",
                                (LONG)bt_dead_gate.ag_Live);
                        if (bt_dead_gate.ag_Live == 0U)
                            bt_reap(&bt_dead_gate_owner);
                    }
                }
            }
        }
#endif

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

    t_check(bt_probe_samples >= 100UL, "the probe sampled often enough",
            (LONG)bt_probe_samples);

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

    /* The kernel must come down before main() returns: its VERTB interrupt
       server lives in this program's hunk, which AmigaDOS frees on exit. */
    t_check(tx_amiga_kernel_stop() == TX_SUCCESS, "ThreadX kernel stopped", 0);

    t_log("%ld checks, %ld failures, ", (LONG)t_checks, (LONG)t_failures);
    t_log("%s\n", (LONG)((t_failures == 0UL) ? "PASS" : "FAIL"), 0);

    return (t_failures == 0UL) ? 0 : 20;
}
