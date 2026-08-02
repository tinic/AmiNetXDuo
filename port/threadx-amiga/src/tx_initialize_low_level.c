/***************************************************************************
 * Eclipse ThreadX -- AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_initialize_low_level.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/**************************************************************************/
/*                                                                        */
/*    _tx_initialize_low_level                          AmigaOS/m68k      */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    Adopts the calling Exec Task as the ThreadX scheduler ("master")     */
/*    task, allocates the signal that yielding threads use to release the  */
/*    baton, reserves the memory block passed to tx_application_define(),  */
/*    and creates the periodic tick Task.  The tick Task is held at its    */
/*    start signal until _tx_amiga_start_interrupts() releases it, which   */
/*    the generic kernel entry does immediately before calling             */
/*    _tx_thread_schedule().  It then validates its wakeup source, which   */
/*    takes about 250 ms, before the first tick is delivered.              */
/*                                                                        */
/*    This file also holds the two ends of the kernel's lifetime:          */
/*    tx_amiga_kernel_start(), which puts the above on a private Task and  */
/*    returns once the scheduler is live, and tx_amiga_kernel_stop(),      */
/*    which takes it all back down and returns only when nothing the port  */
/*    created will execute again.  What stop requires of the caller, and   */
/*    why it refuses rather than reaps outstanding threads, is in          */
/*    inc/tx_amiga.h; the mechanism is here.                               */
/*                                                                        */
/*    The three Tasks that have to die, and how each is reached:           */
/*                                                                        */
/*      tick      _tx_amiga_timer_stop plus SIGF_SINGLE.  Every place the  */
/*                tick task can park now waits on that signal; it used to  */
/*                Wait(0) forever when it had no usable timer, which is    */
/*                fine for a kernel that never stops and unreapable for    */
/*                one that does.                                           */
/*      system    ThreadX's own timer thread, on a stack in ThreadX's BSS. */
/*      timer     Ordinary tx_thread_terminate() + tx_thread_delete(),     */
/*                after the tick is quiet.                                 */
/*      master    _tx_amiga_kernel_stopping makes _tx_thread_schedule()    */
/*                return, which unwinds through tx_kernel_enter() back     */
/*                into _tx_amiga_kernel_task_entry().                      */
/*                                                                        */
/*    Each publishes its "gone" flag and pokes the stopper inside the same */
/*    Forbid() as its RemTask().  Exec discards the forbid nesting of a    */
/*    Task it removes, so the pair is atomic and seeing the flag means the */
/*    Task is off the ready list and finished with its stack, which is     */
/*    what makes freeing that stack safe.  Same shape as                   */
/*    _tx_amiga_task_destroy().                                            */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#include <devices/timer.h>

/* `struct timerequest`, not `struct TimeRequest`.  NDK 3.2 renamed the timer
   types to TimeVal/TimeRequest to stop `struct timeval` colliding with the
   POSIX one, and kept the old lowercase names as aliases; NDK 3.9 and earlier
   only ever had the lowercase ones.  The lowercase spelling therefore compiles
   against both NDKs and the CamelCase one does not, and the member names
   (tr_node, tr_time, tv_secs, tv_micro) are identical either way.  The rest of
   this tree already spells it lowercase, including _tx_amiga_stop_wait()
   further down this file.  */

/* ReadEClock() is an inline that resolves the timer.device base through the
   symbol named by TIMER_BASE_NAME.  Point it at a base of our own rather than
   the global TimerBase: src/common/compat.c defines that one for ami_millis(),
   and the port must not depend on which of the two opened the device first --
   or on compat.c being linked in at all.  __NOLIBBASE__ suppresses proto's
   declaration of the global we are not using.  */

#define __NOLIBBASE__
#define TIMER_BASE_NAME     _tx_amiga_timer_base
#include <proto/timer.h>

#include "aminetxduo/compat.h"      /* ami_log() + AMI_LOG_* only */


/* ---------------------------------------------------------------- state -- */

VOID           *_tx_amiga_scheduler_task      = (VOID *) 0;
ULONG           _tx_amiga_scheduler_signal    = 0UL;
VOID           *_tx_amiga_timer_task          = (VOID *) 0;
VOID           *_tx_amiga_adopt_task          = (VOID *) 0;
ULONG           _tx_amiga_adopt_signal        = 0UL;
VOID           *_tx_amiga_kernel_memory       = (VOID *) 0;
ULONG           _tx_amiga_kernel_memory_size  = 0UL;

volatile UINT   _tx_amiga_kernel_up           = TX_FALSE;
volatile UINT   _tx_amiga_kernel_stopping     = TX_FALSE;
volatile UINT   _tx_amiga_timer_stop          = TX_FALSE;
volatile ULONG  _tx_amiga_zombies             = 0UL;
volatile ULONG  _tx_amiga_zombies_live        = 0UL;

/* Set when the port allocated the kernel memory block itself.  */
static UINT     _tx_amiga_memory_owned        = TX_FALSE;

/* Stack of the tick task; allocated and freed by the port.  */
static APTR     _tx_amiga_timer_stack         = (APTR) 0;
static ULONG    _tx_amiga_timer_stack_size    = 4096UL;

/* Stack of the master (scheduler) task; allocated and freed by the port.  */
static APTR     _tx_amiga_master_stack        = (APTR) 0;
static ULONG    _tx_amiga_master_stack_size   = 0UL;

/* Handshake for tx_amiga_kernel_start().  */
static struct Task *_tx_amiga_starter_task    = (struct Task *) 0;
static ULONG        _tx_amiga_starter_signal  = 0UL;
static UINT         _tx_amiga_start_status    = TX_NOT_DONE;

/*
 * Handshake for tx_amiga_kernel_stop().
 *
 * Each of the two Tasks the port created sets its flag and pokes the stopper in
 * the same Forbid() as its RemTask(), whose forbid nesting Exec discards.  The
 * pair is therefore atomic: seeing the flag means the Task is off the ready
 * list and done with its stack, which is what makes the stack safe to free.
 * Same shape as _tx_amiga_task_destroy().
 */
static struct Task    *_tx_amiga_stop_task    = (struct Task *) 0;
static ULONG           _tx_amiga_stop_signal  = 0UL;
static volatile ULONG  _tx_amiga_timer_gone   = 0UL;
static volatile ULONG  _tx_amiga_master_gone  = 0UL;

/* timer.device base for ReadEClock(); see the TIMER_BASE_NAME note above.  */
struct Device      *_tx_amiga_timer_base      = (struct Device *) 0;

/* Live tick accounting, published through tx_amiga_tick_stats().  */
static TX_AMIGA_TICK_STATS  _tx_amiga_tick;


/* ThreadX externals.  */

VOID    _tx_initialize_low_level(VOID);
VOID    _tx_timer_interrupt(VOID);
VOID    _tx_thread_context_save(VOID);
VOID    _tx_thread_context_restore(VOID);

extern VOID    *_tx_initialize_unused_memory;


static VOID _tx_amiga_timer_task_entry(VOID);
static VOID _tx_amiga_kernel_task_entry(VOID);


/* ------------------------------------------------------- task creation --- */

struct Task *_tx_amiga_task_create(CHAR *name, BYTE priority, VOID (*entry)(VOID),
                                   APTR stack, ULONG stack_size, APTR user_data)
{

struct MemList          *memlist;
struct _tx_amiga_ctrl   *ctrl;
struct Task             *task;
ULONG                    base;
ULONG                    top;


    if ((stack == (APTR) 0) || (stack_size < 256UL))
    {
        return((struct Task *) 0);
    }

    /* Two allocations.  RemTask() walks tc_MemEntry and hands each MemList to
       FreeEntry(), the exact inverse of AllocEntry(): it frees every
       me_Addr/me_Length the list describes and then the MemList itself.
       Putting the MemList inside the block it describes therefore frees that
       address twice -- FreeMem(block, block_size) followed by FreeMem(block,
       sizeof(struct MemList)) -- which is Guru 01000009, AN_FreeTwice, on every
       task that exits.  amiga.lib's CreateTask() keeps them apart for the same
       reason.  */

    memlist =  (struct MemList *) AllocMem((ULONG) sizeof(struct MemList),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (memlist == (struct MemList *) 0)
    {
        return((struct Task *) 0);
    }

    ctrl =  (struct _tx_amiga_ctrl *) AllocMem((ULONG) sizeof(struct _tx_amiga_ctrl),
                                               MEMF_PUBLIC | MEMF_CLEAR);
    if (ctrl == (struct _tx_amiga_ctrl *) 0)
    {
        FreeMem((APTR) memlist, (ULONG) sizeof(struct MemList));
        return((struct Task *) 0);
    }

    task =  &ctrl -> ctrl_task;

    memlist -> ml_NumEntries      =  1;
    memlist -> ml_ME[0].me_Addr   =  (APTR) ctrl;
    memlist -> ml_ME[0].me_Length =  (ULONG) sizeof(struct _tx_amiga_ctrl);

    /* Longword-align the stack window.  */
    base =  (((ULONG) stack) + 3UL) & ~3UL;
    top  =  (((ULONG) stack) + stack_size) & ~3UL;
    if (top <= base)
    {
        FreeMem((APTR) ctrl, (ULONG) sizeof(struct _tx_amiga_ctrl));
        FreeMem((APTR) memlist, (ULONG) sizeof(struct MemList));
        return((struct Task *) 0);
    }

    ctrl -> ctrl_magic  =  TX_AMIGA_CTRL_MAGIC;
    ctrl -> ctrl_thread =  (TX_THREAD *) user_data;

    task -> tc_Node.ln_Type =  NT_TASK;
    task -> tc_Node.ln_Pri  =  priority;
    task -> tc_Node.ln_Name =  name;
    task -> tc_SPLower      =  (APTR) base;
    task -> tc_SPUpper      =  (APTR) top;
    task -> tc_SPReg        =  (APTR) top;
    /* tc_UserData points back at the Task, which is also the control block.
       That is what makes _tx_amiga_ctrl_of() safe to call on a task the port
       did not create: no other task has tc_UserData == itself.  The TX_THREAD
       lives in ctrl_thread instead.  */
    task -> tc_UserData     =  (APTR) task;

    _tx_amiga_newlist(&task -> tc_MemEntry);
    AddTail(&task -> tc_MemEntry, (struct Node *) memlist);

    if (AddTask(task, (APTR) entry, (APTR) 0) == (APTR) 0)
    {
        FreeMem((APTR) ctrl, (ULONG) sizeof(struct _tx_amiga_ctrl));
        FreeMem((APTR) memlist, (ULONG) sizeof(struct MemList));
        return((struct Task *) 0);
    }

    TXTRACE("TXT create task=%08lx ml=%08lx stack=%08lx..%08lx",
            (LONG) task, (LONG) memlist, (LONG) base, (LONG) top);

    return(task);
}


/* ---------------------------------------------------------- scheduling --- */

VOID _tx_amiga_wake_scheduler(VOID)
{

    /* Forbid() because the master Task NULLs the pointer inside its own final
       Forbid() and then RemTask()s itself, which frees the struct Task.  Read
       and Signal have to be one atom, or a poke that arrives in that window
       writes signal bits into freed memory.  */
    Forbid();
    if (_tx_amiga_scheduler_task != (VOID *) 0)
    {
        Signal((struct Task *) _tx_amiga_scheduler_task, _tx_amiga_scheduler_signal);
    }
    Permit();
}


VOID _tx_amiga_signal_task(VOID *task, ULONG sigmask)
{

    _tx_amiga_signal((APTR) task, sigmask);
}


/* ------------------------------------------------------- initialisation -- */

VOID tx_amiga_set_kernel_memory(VOID *memory, ULONG size)
{

    _tx_amiga_kernel_memory       =  memory;
    _tx_amiga_kernel_memory_size  =  size;
    _tx_amiga_memory_owned        =  TX_FALSE;
}


UINT tx_amiga_kernel_running(VOID)
{

    return(_tx_amiga_kernel_up);
}


ULONG tx_amiga_zombie_tasks(VOID)
{

    return(_tx_amiga_zombies);
}


ULONG tx_amiga_zombie_tasks_live(VOID)
{

    return(_tx_amiga_zombies_live);
}


VOID _tx_initialize_low_level(VOID)
{

struct Task     *me;
BYTE             sig;


    /* The task that called tx_kernel_enter() becomes the ThreadX scheduler.
       It never returns from _tx_thread_schedule().  */
    me =  FindTask((STRPTR) 0);

    sig =  AllocSignal(-1);
    if (sig < 0)
    {

        /* Without a signal there is no way to release the baton; refuse to
           come up rather than fail obscurely later.  */
        _tx_amiga_scheduler_task    =  (VOID *) 0;
        _tx_amiga_scheduler_signal  =  0UL;
        return;
    }

    _tx_amiga_scheduler_task    =  (VOID *) me;
    _tx_amiga_scheduler_signal  =  1UL << ((ULONG) sig);

    /* Reserve the region handed to tx_application_define().  */
    if (_tx_amiga_kernel_memory == (VOID *) 0)
    {
        _tx_amiga_kernel_memory_size =  (ULONG) TX_AMIGA_MEMORY_SIZE;
        _tx_amiga_kernel_memory =  (VOID *) AllocMem(_tx_amiga_kernel_memory_size,
                                                     MEMF_PUBLIC | MEMF_CLEAR);
        if (_tx_amiga_kernel_memory == (VOID *) 0)
        {

            /* Owning nothing.  The old code set the flag regardless, and the
               teardown then called FreeMem(NULL, TX_AMIGA_MEMORY_SIZE).  */
            _tx_amiga_kernel_memory_size =  0UL;
        }
        else
        {
            _tx_amiga_memory_owned =  TX_TRUE;
        }
    }

    _tx_initialize_unused_memory =  _tx_amiga_kernel_memory;

    /* Create the periodic tick task.  It parks on SIGF_SINGLE until
       _tx_amiga_start_interrupts() lets it go.  */
    _tx_amiga_timer_stop  =  TX_FALSE;
    _tx_amiga_timer_stack =  AllocMem(_tx_amiga_timer_stack_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (_tx_amiga_timer_stack != (APTR) 0)
    {
        _tx_amiga_timer_task =  (VOID *) _tx_amiga_task_create("ThreadX tick",
                                                               (BYTE) TX_AMIGA_TIMER_PRIORITY,
                                                               _tx_amiga_timer_task_entry,
                                                               _tx_amiga_timer_stack,
                                                               _tx_amiga_timer_stack_size,
                                                               (APTR) 0);
    }
}


VOID _tx_amiga_start_interrupts(void)
{

    /* Release the tick task.  SIGF_SINGLE latches, so there is no race with
       the task reaching its Wait().  */
    if (_tx_amiga_timer_task != (VOID *) 0)
    {
        Signal((struct Task *) _tx_amiga_timer_task, SIGF_SINGLE);
    }

    _tx_amiga_kernel_up =  TX_TRUE;

    /* Wake whoever called tx_amiga_kernel_start().  */
    if (_tx_amiga_starter_task != (struct Task *) 0)
    {
        _tx_amiga_start_status =  TX_SUCCESS;
        Signal(_tx_amiga_starter_task, _tx_amiga_starter_signal);
    }
}


/* ---------------------------------------------------------- tick task ---- */

/*
 * The ThreadX periodic interrupt.
 *
 * This runs as an ordinary (high priority) Exec Task, not an interrupt server.
 * _tx_thread_context_save() takes the core lock with Forbid(), which gives the
 * tick ISR semantics: while it is held, no other task in the machine runs, so
 * the ThreadX thread that holds the baton is frozen -- the property the Linux
 * port buys with pthread_kill().
 *
 * The wakeup source is not the time base.  The task parks on timer.device
 * UNIT_VBLANK, which costs a list insertion on an interrupt the machine takes
 * anyway, but it never counts those wakeups.  On each one it reads the E-Clock
 * and works out how many whole TX_TIMER_TICKS_PER_SECOND periods have elapsed.
 * That number is the clock.  It is also what it would like to deliver, but the
 * two are separate: TX_AMIGA_TIMER_MAX_CATCHUP and TX_AMIGA_TIMER_BUDGET_MS bound
 * how many _tx_timer_interrupt() calls one wakeup may make, and whatever they
 * refuse is added to _tx_timer_system_clock directly instead.  So tx_time_get()
 * is the E-Clock's answer whatever the display or the machine is doing, and only
 * the timer wheel -- which must be walked a slot at a time or timers in the
 * skipped slots go unseen for a whole revolution -- falls behind.  How far
 * behind is tx_amiga_tick_skew.
 *
 * VBlank is 50 Hz PAL and 60 Hz NTSC, so a stack that counts frames (as the
 * AmiTCP lineage does, with a hardcoded 50) runs 20% fast on an NTSC machine.
 * Under RTG the chipset VERTB is no longer driving the monitor, and on
 * PiStorm/Emu68-class systems and under emulation its rate and regularity are
 * outside our control.  The E-Clock is CIA-derived and reports its own
 * frequency, so it is right on all of them.
 *
 * The previous design -- 100 Hz on UNIT_MICROHZ, one fresh IORequest round trip
 * per tick, ticks counted rather than measured -- lost 4-5% of the clock under
 * soak load, because every re-arm paid its own scheduling latency and nothing
 * noticed the shortfall.
 */

/*
 * Tell a waiting tx_amiga_kernel_stop() that this Task has finished with its
 * stack.  Call inside the Forbid() that ends in RemTask(), never outside it.
 */
static VOID _tx_amiga_stop_notify(volatile ULONG *flag)
{

    *flag =  1UL;

    if (_tx_amiga_stop_task != (struct Task *) 0)
    {
        Signal(_tx_amiga_stop_task, _tx_amiga_stop_signal);
    }
}


/*
 * Park a tick task that has no usable wakeup source, so that it is still
 * reapable.
 *
 * This used to be Wait(0UL) -- park forever -- which is correct for a kernel
 * that never comes down and fatal for one that does: the Task would keep its
 * entry point inside a code hunk that AmigaDOS is about to unload, and nothing
 * could wake it to say so.  Waiting on SIGF_SINGLE instead costs nothing and
 * leaves tx_amiga_kernel_stop() a way in.
 */
static VOID _tx_amiga_timer_park(VOID)
{

    while (_tx_amiga_timer_stop == TX_FALSE)
    {
        (VOID) Wait(SIGF_SINGLE);
    }
}


/* The tick task's last act.  Never returns.  */
static VOID _tx_amiga_timer_exit(VOID)
{

    Forbid();
    _tx_amiga_timer_task =  (VOID *) 0;
    _tx_amiga_stop_notify(&_tx_amiga_timer_gone);
    RemTask((struct Task *) 0);

    /* Unreachable.  */
    for (;;)
    {
        Wait(0UL);
    }
}


/* Arm one wakeup.  */
static VOID _tx_amiga_timer_arm(struct timerequest *tr, ULONG secs, ULONG micro)
{

    tr -> tr_node.io_Command =  TR_ADDREQUEST;
    tr -> tr_time.tv_secs    =  secs;
    tr -> tr_time.tv_micro   =  micro;
    SendIO((struct IORequest *) tr);
}


/*
 * Measure what a wakeup source does, in Hz * 100.  Returns 0 for a source that
 * produced nothing in the window.
 *
 * `guard` is a request on a different unit and port, and it bounds the
 * measurement: the window ends when the guard completes, so a source with no
 * VERTB interrupt behind it -- the configuration this check exists to survive
 * -- yields 0 rather than parking the tick task in Wait() forever.
 *
 * Nothing here is ever AbortIO()ed.  The guard always completes on its own, and
 * the source's outstanding request is left pending for the caller: if the
 * source is accepted that request becomes the first tick wakeup, and if it is
 * rejected the whole request is thrown away.  A timer.device request that has
 * been aborted and then re-armed does not complete again (measured twice under
 * FS-UAE with Kickstart 3.1), and both times the symptom was a ThreadX clock
 * stuck at zero with every thread on it.
 *
 * On return `tr` has a request outstanding.
 */
static ULONG _tx_amiga_timer_probe(struct timerequest *tr, ULONG sig,
                                   struct timerequest *guard, ULONG guard_sig,
                                   ULONG eclock_per_ms)
{

struct EClockVal    t0;
struct EClockVal    t1;
ULONG               got;
ULONG               wakeups;
ULONG               elapsed_ms;


    wakeups =  0UL;

    _tx_amiga_timer_arm(guard,
                        ((ULONG) TX_AMIGA_TIMER_PROBE_MS) / 1000UL,
                        (((ULONG) TX_AMIGA_TIMER_PROBE_MS) % 1000UL) * 1000UL);

    ReadEClock(&t0);
    _tx_amiga_timer_arm(tr, 0UL, 1UL);      /* "next vertical blank"  */

    for (;;)
    {

        got =  Wait(sig | guard_sig);

        if ((got & sig) != 0UL)
        {
            if (CheckIO((struct IORequest *) tr) != (struct IORequest *) 0)
            {
                WaitIO((struct IORequest *) tr);

                /* Bounded so the Hz * 100 scaling below cannot overflow even
                   if the source answers instantly and spins.  */
                if (wakeups < 20000UL)
                {
                    wakeups++;
                }
                _tx_amiga_timer_arm(tr, 0UL, 1UL);
            }
        }

        if ((got & guard_sig) != 0UL)
        {
            WaitIO((struct IORequest *) guard);
            break;
        }
    }

    ReadEClock(&t1);
    elapsed_ms =  ((ULONG) (t1.ev_lo - t0.ev_lo)) / eclock_per_ms;

    if ((wakeups == 0UL) || (elapsed_ms == 0UL))
    {
        return(0UL);
    }

    return((wakeups * 100000UL) / elapsed_ms);
}


/* Close and destroy a request/port pair the port is done with.  The request
   may still be outstanding, which is the only place AbortIO() is used: the
   pair is destroyed immediately afterwards and never re-armed.  */
static VOID _tx_amiga_timer_discard(struct timerequest *tr, struct MsgPort *port)
{

    if (CheckIO((struct IORequest *) tr) == (struct IORequest *) 0)
    {
        AbortIO((struct IORequest *) tr);
    }
    WaitIO((struct IORequest *) tr);

    CloseDevice((struct IORequest *) tr);
    DeleteIORequest((APTR) tr);
    DeleteMsgPort(port);
}


static VOID _tx_amiga_timer_task_entry(VOID)
{

struct MsgPort      *port;
struct MsgPort      *guard_port;
struct timerequest  *tr;
struct timerequest  *guard;
struct EClockVal     now;
ULONG                port_sig;
ULONG                guard_sig;
ULONG                interval_secs;
ULONG                interval_micro;
ULONG                eclock_hz;
ULONG                eclock_per_ms;
ULONG                eclock_per_tick;
ULONG                eclock_rem;
ULONG                frac;
ULONG                carry;
ULONG                advance;
ULONG                last_lo;
ULONG                up_lo;      /* last reading the uptime was advanced from */
ULONG                up_rem;     /* E-Clock ticks not yet worth a millisecond */
ULONG                up_gain;
ULONG                up_carry;   /* thousandths of a tick */
ULONG                up_num;
ULONG                backlog;
ULONG                measured;
ULONG                delta;
ULONG                service;
ULONG                last_service;
ULONG                rate_chz;
ULONG                unit;
ULONG                i;
UINT                 armed;


    /*
     * Wait for _tx_amiga_start_interrupts().
     *
     * Opening and validating the wakeup source ahead of this point was tried,
     * to overlap the 250 ms validation with the rest of kernel start-up rather
     * than delay the first tick by it.  It made the "preferred unit will not
     * open" fallback leave the clock dead (_tx_timer_system_clock stuck at 0)
     * and was reverted: a ThreadX clock starting a quarter of a second late is
     * an unobservable startup transient, a clock that never starts is not.
     */
    Wait(SIGF_SINGLE);

    armed    =  TX_FALSE;
    rate_chz =  0UL;

    if (_tx_amiga_timer_stop != TX_FALSE)
    {
        /* Stopped before we ever ticked.  Nothing to tear down.  */
        _tx_amiga_timer_exit();
    }

    port =  CreateMsgPort();
    if (port == (struct MsgPort *) 0)
    {
        _tx_amiga_timer_park();          /* kernel has no tick */
        _tx_amiga_timer_exit();
    }

    tr =  (struct timerequest *) CreateIORequest(port, (ULONG) sizeof(struct timerequest));
    if (tr == (struct timerequest *) 0)
    {
        DeleteMsgPort(port);
        _tx_amiga_timer_park();
        _tx_amiga_timer_exit();
    }

    port_sig =  1UL << ((ULONG) port -> mp_SigBit);

    /* MICROHZ interval for the fallback, and for the startup guard.  */
    interval_micro =  1000000UL / (ULONG) TX_AMIGA_TIMER_WAKEUP_HZ;
    interval_secs  =  interval_micro / 1000000UL;
    interval_micro =  interval_micro % 1000000UL;

    /*
     * The startup guard: a UNIT_MICROHZ request on its own port, so its
     * completion is distinguishable from the source under test by signal alone.
     * It stops a machine whose VBlank never fires from parking the tick task
     * forever in Wait() instead of falling back, and if the preferred unit is
     * rejected the guard becomes the tick rather than being thrown away.  A
     * machine where even this will not open has no usable timer at all, and the
     * probe runs unguarded.
     */
    guard      =  (struct timerequest *) 0;
    guard_port =  (struct MsgPort *) 0;
    guard_sig  =  0UL;

    guard_port =  CreateMsgPort();
    if (guard_port != (struct MsgPort *) 0)
    {
        guard =  (struct timerequest *) CreateIORequest(guard_port,
                                                        (ULONG) sizeof(struct timerequest));
        if (guard != (struct timerequest *) 0)
        {
            if (OpenDevice((CONST_STRPTR) "timer.device", (ULONG) UNIT_MICROHZ,
                           (struct IORequest *) guard, 0UL) != 0)
            {
                DeleteIORequest((APTR) guard);
                guard =  (struct timerequest *) 0;
            }
            else
            {
                guard_sig =  1UL << ((ULONG) guard_port -> mp_SigBit);
            }
        }
        if (guard == (struct timerequest *) 0)
        {
            DeleteMsgPort(guard_port);
            guard_port =  (struct MsgPort *) 0;
        }
    }

    unit =  (ULONG) TX_AMIGA_TIMER_UNIT;
    if (OpenDevice((CONST_STRPTR) "timer.device", unit,
                   (struct IORequest *) tr, 0UL) != 0)
    {

        /* The preferred unit is not there at all.  */
        ami_log(AMI_LOG_WARN, "tick: timer.device unit %ld would not open; using UNIT_MICROHZ",
                (LONG) unit);

        if (guard != (struct timerequest *) 0)
        {
            /* Move the tick to the guard's request, which is already open on
               UNIT_MICROHZ; see the fallback below for why the rejected
               request is thrown away rather than reopened.  */
            DeleteIORequest((APTR) tr);
            DeleteMsgPort(port);
            tr         =  guard;
            port       =  guard_port;
            port_sig   =  guard_sig;
            guard      =  (struct timerequest *) 0;
            guard_port =  (struct MsgPort *) 0;
            guard_sig  =  0UL;
        }
        else if (OpenDevice((CONST_STRPTR) "timer.device", (ULONG) UNIT_MICROHZ,
                            (struct IORequest *) tr, 0UL) != 0)
        {
            ami_log(AMI_LOG_ERROR, "tick: no timer.device at all; ThreadX has no clock");
            DeleteIORequest((APTR) tr);
            DeleteMsgPort(port);
            _tx_amiga_timer_park();
            _tx_amiga_timer_exit();
        }

        unit =  (ULONG) UNIT_MICROHZ;
        _tx_amiga_tick.tx_amiga_tick_fallback =  (ULONG) TX_TRUE;
    }

    /* Any open unit gives us the device base, and therefore ReadEClock().  */
    _tx_amiga_timer_base =  tr -> tr_node.io_Device;

    eclock_hz =  ReadEClock(&now);
    if (eclock_hz == 0UL)
    {
        /* Cannot happen on real hardware; refuse to divide by it if it does. */
        eclock_hz =  709379UL;
        ami_log(AMI_LOG_WARN, "tick: ReadEClock reported 0 Hz; assuming %ld",
                (LONG) eclock_hz);
    }
    eclock_per_ms =  eclock_hz / 1000UL;
    if (eclock_per_ms == 0UL)
    {
        eclock_per_ms =  1UL;
    }

    /*
     * Floor the per-tick period and carry the remainder, so that the long-run
     * rate is exactly TX_TIMER_TICKS_PER_SECOND however awkward the E-Clock
     * frequency is.  709379 / 50 truncates to 14187, which on its own would
     * gain a couple of seconds a day.
     */
    eclock_per_tick =  eclock_hz / (ULONG) TX_TIMER_TICKS_PER_SECOND;
    eclock_rem      =  eclock_hz % (ULONG) TX_TIMER_TICKS_PER_SECOND;
    if (eclock_per_tick == 0UL)
    {
        eclock_per_tick =  1UL;
        eclock_rem      =  0UL;
    }

    /* ---- validate the wakeup source ------------------------------------- */

    if ((unit != (ULONG) UNIT_MICROHZ) && (guard != (struct timerequest *) 0))
    {

        /*
         * UNIT_VBLANK rounds any request up to the next vertical blank, so the
         * smallest request asks for one frame -- 50 Hz PAL, 60 Hz NTSC, and on
         * either the tick's own 50 Hz is delivered from the E-Clock underneath.
         */
        rate_chz =  _tx_amiga_timer_probe(tr, port_sig, guard, guard_sig, eclock_per_ms);

        _tx_amiga_tick.tx_amiga_tick_source_chz =  rate_chz;

        if ((rate_chz < ((ULONG) TX_AMIGA_TIMER_PROBE_MIN_HZ * 100UL)) ||
            (rate_chz > ((ULONG) TX_AMIGA_TIMER_PROBE_MAX_HZ * 100UL)))
        {

            ami_log(AMI_LOG_WARN,
                    "tick: timer.device unit %ld woke at %ld.%02ld Hz, outside %ld..%ld Hz -- "
                    "falling back to UNIT_MICROHZ",
                    (LONG) unit, (LONG) (rate_chz / 100UL), (LONG) (rate_chz % 100UL),
                    (LONG) TX_AMIGA_TIMER_PROBE_MIN_HZ, (LONG) TX_AMIGA_TIMER_PROBE_MAX_HZ);

            /* Throw the rejected source away whole and promote the guard.  Its
               outstanding request may be one that will never complete, so it is
               aborted -- and then destroyed rather than re-armed, for the reason
               in _tx_amiga_timer_probe().  */
            _tx_amiga_timer_discard(tr, port);

            tr         =  guard;
            port       =  guard_port;
            port_sig   =  guard_sig;
            guard      =  (struct timerequest *) 0;
            guard_port =  (struct MsgPort *) 0;
            guard_sig  =  0UL;

            unit =  (ULONG) UNIT_MICROHZ;
            _tx_amiga_timer_base =  tr -> tr_node.io_Device;
            _tx_amiga_tick.tx_amiga_tick_fallback =  (ULONG) TX_TRUE;
        }
        else
        {

            /* Accepted.  The probe left a request outstanding on it, which is
               the first tick wakeup; the guard has done its job.  */
            armed =  TX_TRUE;

            CloseDevice((struct IORequest *) guard);
            DeleteIORequest((APTR) guard);
            DeleteMsgPort(guard_port);
            guard      =  (struct timerequest *) 0;
            guard_port =  (struct MsgPort *) 0;
            guard_sig  =  0UL;
        }
    }
    else if (unit != (ULONG) UNIT_MICROHZ)
    {

        /* No UNIT_MICROHZ to fall back to, so there is nothing a failed
           validation could do except hang; skip it and hope.  */
        ami_log(AMI_LOG_WARN,
                "tick: no UNIT_MICROHZ available to validate against; "
                "using unit %ld unchecked", (LONG) unit);
    }

    if (unit == (ULONG) UNIT_MICROHZ)
    {
        /* One request per tick period; the E-Clock still decides how many
           ticks that wakeup is worth.  */
        _tx_amiga_tick.tx_amiga_tick_source_chz =
            (ULONG) TX_AMIGA_TIMER_WAKEUP_HZ * 100UL;
    }
    else
    {
        interval_secs  =  0UL;
        interval_micro =  1UL;           /* "next vertical blank"  */
    }

    _tx_amiga_tick.tx_amiga_tick_unit       =  unit;
    _tx_amiga_tick.tx_amiga_tick_eclock_hz  =  eclock_hz;

    ami_log(AMI_LOG_INFO,
            "tick: %ld Hz from timer.device unit %ld (%ld.%02ld Hz wakeups), E-Clock %ld Hz",
            (LONG) TX_TIMER_TICKS_PER_SECOND, (LONG) unit,
            (LONG) (_tx_amiga_tick.tx_amiga_tick_source_chz / 100UL),
            (LONG) (_tx_amiga_tick.tx_amiga_tick_source_chz % 100UL),
            (LONG) eclock_hz);

    /* ---- the tick ------------------------------------------------------- */

    frac         =  0UL;
    backlog      =  0UL;
    last_service =  0UL;
    ReadEClock(&now);
    last_lo =  now.ev_lo;
    up_lo    =  now.ev_lo;
    up_rem   =  0UL;
    up_carry =  0UL;

    if (armed == TX_FALSE)
    {
        /* A source that passed validation is already armed: the probe's last
           request is this tick's first wakeup.  */
        _tx_amiga_timer_arm(tr, interval_secs, interval_micro);
        armed =  TX_TRUE;
    }

    while (_tx_amiga_timer_stop == TX_FALSE)
    {

        Wait(port_sig | SIGF_SINGLE);

        /* Timestamp the wakeup before anything else: this is both the tick's
           time reference and the start of the service-cost window.  */
        (VOID) ReadEClock(&now);

        if (_tx_amiga_timer_stop != TX_FALSE)
        {
            /* Leave the request armed; the teardown below retires it.  */
            break;
        }

        if (CheckIO((struct IORequest *) tr) == (struct IORequest *) 0)
        {
            /* Woken by something that was not our request.  Do not abort it --
               an aborted timer request cannot be re-armed (see
               _tx_amiga_timer_probe) -- just go back to sleep.  */
            continue;
        }
        WaitIO((struct IORequest *) tr);
        armed =  TX_FALSE;

        /* Re-arm before doing the tick work.  On UNIT_VBLANK that reserves the
           next frame up front, so a tick that overruns costs a late delivery
           rather than a skipped frame, and the catch-up below then makes even
           that invisible to the clock.  */
        _tx_amiga_timer_arm(tr, interval_secs, interval_micro);
        armed =  TX_TRUE;

        _tx_amiga_tick.tx_amiga_tick_wakeups++;

        /* Accumulated, because ev_lo wraps every ~100 minutes and a machine up
           longer than that would otherwise report a few minutes.  The tick runs
           50 times a second, so it never misses a wrap.  Divided by the rate,
           not by a ticks-per-millisecond: 709379/1000 truncates to 709 and runs
           0.05% fast, which reads as drift against an honest clock.  */
        up_rem  +=  (ULONG) (now.ev_lo - up_lo);
        up_lo    =  now.ev_lo;
        if (up_rem >= eclock_hz)
        {
            up_rem -=  eclock_hz;
            _tx_amiga_tick.tx_amiga_tick_uptime_ms +=  1000UL;
        }
        up_gain  =  (up_rem * 1000UL) / eclock_hz;
        _tx_amiga_tick.tx_amiga_tick_uptime_ms +=  up_gain;
        up_num   =  up_gain * eclock_hz + up_carry;
        up_rem  -=  up_num / 1000UL;
        up_carry =  up_num % 1000UL;

        delta    =  (ULONG) (now.ev_lo - last_lo); /* correct across one wrap */
        measured =  delta / eclock_per_tick;

        if (measured == 0UL)
        {

            /* Woke early.  A 60 Hz NTSC VBlank against a 50 Hz tick does this
               ten times a second; deliver nothing and keep the phase.  */
            _tx_amiga_tick.tx_amiga_tick_empty++;
        }
        else
        {

            /*
             * Advance the phase by exactly `measured` periods with the
             * remainder carried, so the long-run rate is exactly
             * TX_TIMER_TICKS_PER_SECOND whatever the E-Clock frequency.  Always
             * by the full `measured`, whatever is delivered below: the anchor is
             * the clock's, and what the wheel still owes is `backlog`.  Holding
             * the anchor back to re-measure undelivered periods would be a
             * second way of saying the same thing, and the two would drift.
             *
             * The carry is held over rather than applied when it would put the
             * anchor past `now`, which the next wakeup would read as a wrap and
             * a 100 minute stall.  measured * eclock_per_tick <= delta by the
             * division, so only the carry can overshoot, and only when the
             * division came out exact.
             */
            advance  =  measured * eclock_per_tick;
            frac    +=  measured * eclock_rem;
            carry    =  frac / (ULONG) TX_TIMER_TICKS_PER_SECOND;
            if ((advance + carry) > delta)
            {
                carry =  0UL;
            }
            else
            {
                frac -=  carry * (ULONG) TX_TIMER_TICKS_PER_SECOND;
            }
            last_lo +=  advance + carry;

            /* The clock, and the only thing that sets it.  Real elapsed time,
               whatever the wheel below is or is not given.  */
            _tx_amiga_timer_clock_advance(measured);

            /* What the wheel owes: this wakeup's periods on top of anything a
               previous wakeup ran out of budget for.  */
            backlog +=  measured;

            /* Sampled here because this is the moment the wheel is furthest
               behind the clock -- the whole backlog, plus everything dropped
               for good earlier.  That is the worst lateness a timer sitting on
               the wheel can have seen.  */
            if ((backlog + _tx_amiga_tick.tx_amiga_tick_lost) >
                _tx_amiga_tick.tx_amiga_tick_skew_peak)
            {
                _tx_amiga_tick.tx_amiga_tick_skew_peak =
                    backlog + _tx_amiga_tick.tx_amiga_tick_lost;
            }

            if (backlog > (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP)
            {

                /* The ceiling on the backlog, and the only place a tick is
                   thrown away.  The budget below defers rather than drops, so
                   without this a machine that never catches up would grow an
                   unbounded backlog and the wheel would fall further behind
                   forever.  This is also the only path that skips a wheel slot,
                   which hides the timers in it for a revolution -- so it is
                   deliberately the pathological case and not the ordinary one. */
                _tx_amiga_tick.tx_amiga_tick_lost +=
                    backlog - (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP;
                _tx_amiga_tick.tx_amiga_tick_clipped++;

                /* Kept for every stall, not just the three that get logged. */
                if ((ULONG) (delta / eclock_per_ms) >
                    _tx_amiga_tick.tx_amiga_tick_worst_stall_ms)
                {
                    _tx_amiga_tick.tx_amiga_tick_worst_stall_ms =
                        (ULONG) (delta / eclock_per_ms);
                    _tx_amiga_tick.tx_amiga_tick_worst_service_us =
                        (last_service * 1000UL) / eclock_per_ms;
                }

                if (_tx_amiga_tick.tx_amiga_tick_clipped <= 3UL)
                {
                    /* The previous wakeup's service cost is in the message
                       because it is the only way to tell the two causes of a
                       stall apart, and they need different repairs: a service
                       figure close to the stall means the tick task overran its
                       own period (too much work under the core lock), and one
                       in the ordinary hundreds of microseconds means somebody
                       else held the machine and the tick was not dispatched. */
                    ami_log(AMI_LOG_WARN,
                            "tick: %ld ms since the last wakeup, wheel skips %ld "
                            "of %ld owed (cap %ld, previous service %ld us)",
                            (LONG) (delta / eclock_per_ms),
                            (LONG) (backlog - (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP),
                            (LONG) backlog, (LONG) TX_AMIGA_TIMER_MAX_CATCHUP,
                            (LONG) ((last_service * 1000UL) / eclock_per_ms));
                }

                backlog =  (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP;
            }
            else if (backlog > 1UL)
            {
                _tx_amiga_tick.tx_amiga_tick_catchups++;
            }

            for (i = 0UL; i < backlog; i++)
            {

                /* Half the period is the tick's, at most.  Every tick delivered
                   here runs under the Forbid() _tx_thread_context_save() takes,
                   so it is time no other task in the machine runs; a long
                   catch-up would starve everything else for its whole length.
                   Checked between ticks rather than inside one, because a tick
                   is not interruptible: this bounds the burst, not the tick.

                   The rest stays in `backlog` and is delivered at a later
                   wakeup.  Dropping it would skip wheel slots, and a slot not
                   walked hides its timers for a revolution.  Breaking out is the
                   yield: the request was re-armed before any of this ran, so the
                   Wait() at the top of the loop parks until the next wakeup
                   without consuming a signal, and the Forbid() is
                   _tx_thread_context_restore()'s to release and it already has. */
                if (i > 0UL)
                {
                    struct EClockVal budget_now;

                    (VOID) ReadEClock(&budget_now);
                    if ((ULONG) (budget_now.ev_lo - now.ev_lo) >
                        (eclock_per_ms * (ULONG) TX_AMIGA_TIMER_BUDGET_MS))
                    {
                        _tx_amiga_tick.tx_amiga_tick_over_budget++;
                        _tx_amiga_tick.tx_amiga_tick_deferred +=  backlog - i;
                        break;
                    }
                }

                /* Enter "interrupt" context, run the tick, leave it.  */
                _tx_thread_context_save();
                _tx_timer_interrupt();
                _tx_thread_context_restore();
            }

            backlog -=  i;
            _tx_amiga_tick.tx_amiga_tick_delivered +=  i;
            _tx_amiga_tick.tx_amiga_tick_skew =
                backlog + _tx_amiga_tick.tx_amiga_tick_lost;

            /* Unconditionally poke the scheduler.  Only the idle case needs it,
               but one Signal() per tick is cheap insurance against a lost
               wake-up hanging the whole stack.  */
            _tx_amiga_wake_scheduler();
        }

        /* Service cost of this wakeup: everything the tick task did between
           waking and going back to sleep.  Summed over tx_amiga_tick_wakeups
           this is the CPU the tick takes off the machine.  */
        {
            struct EClockVal end;

            (VOID) ReadEClock(&end);
            service      =  (ULONG) (end.ev_lo - now.ev_lo);
            last_service =  service;
            if (service < 4000000UL)
            {
                _tx_amiga_tick.tx_amiga_tick_service_us +=
                    (service * 1000UL) / eclock_per_ms;
            }
        }
    }

    if (_tx_amiga_tick.tx_amiga_tick_clipped != 0UL)
    {
        ami_log(AMI_LOG_WARN,
                "tick: %ld stalls clipped, wheel %ld ticks behind the clock "
                "(worst %ld)",
                (LONG) _tx_amiga_tick.tx_amiga_tick_clipped,
                (LONG) _tx_amiga_tick.tx_amiga_tick_skew,
                (LONG) _tx_amiga_tick.tx_amiga_tick_skew_peak);
    }
    ami_log(AMI_LOG_INFO,
            "tick: %ld wakeups -> %ld ticks in %ld ms (%ld empty, %ld caught up)",
            (LONG) _tx_amiga_tick.tx_amiga_tick_wakeups,
            (LONG) _tx_amiga_tick.tx_amiga_tick_delivered,
            (LONG) _tx_amiga_tick.tx_amiga_tick_uptime_ms,
            (LONG) _tx_amiga_tick.tx_amiga_tick_empty,
            (LONG) _tx_amiga_tick.tx_amiga_tick_catchups);
    ami_log(AMI_LOG_INFO, "tick: %ld us in-task over %ld wakeups",
            (LONG) _tx_amiga_tick.tx_amiga_tick_service_us,
            (LONG) _tx_amiga_tick.tx_amiga_tick_wakeups);

    _tx_amiga_timer_base =  (struct Device *) 0;

    if (armed != TX_FALSE)
    {
        _tx_amiga_timer_discard(tr, port);
    }
    else
    {
        CloseDevice((struct IORequest *) tr);
        DeleteIORequest((APTR) tr);
        DeleteMsgPort(port);
    }

    if (guard != (struct timerequest *) 0)
    {
        CloseDevice((struct IORequest *) guard);
        DeleteIORequest((APTR) guard);
        DeleteMsgPort(guard_port);
    }

    _tx_amiga_timer_exit();
}


VOID tx_amiga_tick_stats(TX_AMIGA_TICK_STATS *stats)
{

    if (stats == (TX_AMIGA_TICK_STATS *) 0)
    {
        return;
    }

    /* One consistent snapshot; the tick task is the only writer.  */
    Forbid();
    *stats =  _tx_amiga_tick;
    Permit();
}


TX_AMIGA_TICK_STATS *tx_amiga_tick_stats_live(VOID)
{

    return &_tx_amiga_tick;
}


/* -------------------------------------------------- library-style start -- */

static VOID _tx_amiga_kernel_task_entry(VOID)
{

    /* _tx_initialize_kernel_enter() is tx_kernel_enter(); it ends in
       _tx_thread_schedule(), which returns only when tx_amiga_kernel_stop()
       has asked it to.  */
    _tx_initialize_kernel_enter();

    TXTRACE("TXT master exiting task=%08lx", (LONG) FindTask((STRPTR) 0));

    Forbid();
    _tx_amiga_scheduler_task   =  (VOID *) 0;
    _tx_amiga_scheduler_signal =  0UL;
    _tx_amiga_stop_notify(&_tx_amiga_master_gone);
    RemTask((struct Task *) 0);

    /* Unreachable.  */
    for (;;)
    {
        Wait(0UL);
    }
}


UINT tx_amiga_kernel_start(VOID)
{

struct Task *task;
BYTE         sig;
APTR         stack;
ULONG        stack_size =  8192UL;


    if (_tx_amiga_kernel_up != TX_FALSE)
    {
        return(TX_SUCCESS);
    }
    if (_tx_amiga_kernel_stopping != TX_FALSE)
    {

        /* A stop is in flight, or one failed and left the kernel wedged.
           Starting a second one on top would race the first for every global
           in this file.  */
        return(TX_NOT_DONE);
    }

    sig =  AllocSignal(-1);
    if (sig < 0)
    {
        return(TX_NO_MEMORY);
    }

    stack =  AllocMem(stack_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (stack == (APTR) 0)
    {
        FreeSignal(sig);
        return(TX_NO_MEMORY);
    }

    _tx_amiga_starter_task    =  FindTask((STRPTR) 0);
    _tx_amiga_starter_signal  =  1UL << ((ULONG) sig);
    _tx_amiga_start_status    =  TX_NOT_DONE;

    _tx_amiga_timer_gone      =  0UL;
    _tx_amiga_master_gone     =  0UL;

    task =  _tx_amiga_task_create("ThreadX", (BYTE) TX_AMIGA_TASK_PRIORITY,
                                  _tx_amiga_kernel_task_entry, stack, stack_size, (APTR) 0);
    if (task == (struct Task *) 0)
    {
        _tx_amiga_starter_task =  (struct Task *) 0;
        FreeMem(stack, stack_size);
        FreeSignal(sig);
        return(TX_NO_MEMORY);
    }

    /* Remembered so that tx_amiga_kernel_stop() can free it; the master Task
       cannot free the stack it is standing on.  */
    _tx_amiga_master_stack      =  stack;
    _tx_amiga_master_stack_size =  stack_size;

    Wait(_tx_amiga_starter_signal);

    _tx_amiga_starter_task =  (struct Task *) 0;
    FreeSignal(sig);

    return(_tx_amiga_start_status);
}


/* --------------------------------------------------- library-style stop --- */

/*
 * Wait, with a timeout, for one of the teardown flags above.
 *
 * A fresh timer.device request per call: an AbortIO()d timer.device request
 * does not complete again when it is re-armed (see _tx_amiga_timer_probe), so a
 * request that may have to be aborted can never be reused.  Two waits therefore
 * cost two OpenDevice()s, which on a shutdown path is acceptable.  Mirrors
 * _tx_amiga_reap()'s structure.
 *
 * Returns TX_TRUE if the flag came up, TX_FALSE on timeout.
 */
static UINT _tx_amiga_stop_wait(volatile ULONG *flag, ULONG sigmask, ULONG secs)
{

struct MsgPort      *port;
struct timerequest  *tr;
ULONG                portsig;
ULONG                signals;
UINT                 got;


    if (*flag != 0UL)
    {
        return(TX_TRUE);
    }

    portsig =  0UL;
    tr      =  (struct timerequest *) 0;

    port =  CreateMsgPort();
    if (port != (struct MsgPort *) 0)
    {
        tr =  (struct timerequest *) CreateIORequest(port, (ULONG) sizeof(struct timerequest));
        if (tr != (struct timerequest *) 0)
        {
            if (OpenDevice((CONST_STRPTR) "timer.device", (ULONG) UNIT_VBLANK,
                           (struct IORequest *) tr, 0UL) != 0)
            {
                DeleteIORequest((APTR) tr);
                tr =  (struct timerequest *) 0;
            }
            else
            {
                portsig =  1UL << ((ULONG) port -> mp_SigBit);
            }
        }
        if (tr == (struct timerequest *) 0)
        {
            DeleteMsgPort(port);
            port =  (struct MsgPort *) 0;
        }
    }

    if (tr != (struct timerequest *) 0)
    {
        tr -> tr_node.io_Command =  TR_ADDREQUEST;
        tr -> tr_time.tv_secs    =  secs;
        tr -> tr_time.tv_micro   =  0UL;
        SendIO((struct IORequest *) tr);
    }

    for (;;)
    {

        signals =  Wait(sigmask | portsig);

        if (*flag != 0UL)
        {
            break;
        }
        if ((portsig != 0UL) && ((signals & portsig) != 0UL))
        {
            break;                              /* timed out */
        }
        if (portsig == 0UL)
        {

            /* No timer could be opened, so there is no timeout to be had.  One
               wait is all we can offer; going round again would block for ever,
               which on the path whose job is making exit safe is worse than
               reporting failure.  */
            break;
        }
    }

    got =  (*flag != 0UL) ? ((UINT) TX_TRUE) : ((UINT) TX_FALSE);

    if (tr != (struct timerequest *) 0)
    {
        if (CheckIO((struct IORequest *) tr) == (struct IORequest *) 0)
        {
            AbortIO((struct IORequest *) tr);
        }
        WaitIO((struct IORequest *) tr);
        CloseDevice((struct IORequest *) tr);
        DeleteIORequest((APTR) tr);
    }
    if (port != (struct MsgPort *) 0)
    {
        DeleteMsgPort(port);
    }

    return(got);
}


/*
 * Application-owned ThreadX threads that would still be there after the stop.
 *
 * Two are discounted: the system timer thread, which ThreadX creates for itself
 * and stop reaps, and `mine`, the caller's own adopted thread, which stop
 * orphans.  Discounting `mine` here rather than orphaning first is what lets a
 * refusal leave the caller as it found it.
 */
static ULONG _tx_amiga_application_threads(TX_THREAD *mine)
{

TX_THREAD   *thread_ptr;
ULONG        count;
ULONG        i;


    count =  0UL;

    Forbid();

    thread_ptr =  _tx_thread_created_ptr;
    for (i = 0UL; (i < _tx_thread_created_count) && (thread_ptr != TX_NULL); i++)
    {

        if (thread_ptr != mine)
        {
#if !defined(TX_NO_TIMER) && !defined(TX_TIMER_PROCESS_IN_ISR)
            if (thread_ptr != &_tx_timer_thread)
#endif
            {
                count++;
            }
        }
        thread_ptr =  thread_ptr -> tx_thread_created_next;
    }

    Permit();

    return(count);
}


UINT tx_amiga_kernel_stop(VOID)
{

struct Task *me;
TX_THREAD   *adopted;
BYTE         sig;
ULONG        sigmask;
ULONG        remaining;
UINT         status;


    me =  FindTask((STRPTR) 0);

    /* ---- who may call this ----------------------------------------------- */

    if (_tx_amiga_ctrl_of(me) != (struct _tx_amiga_ctrl *) 0)
    {

        /* A Task the port created: the master, the tick, or a ThreadX thread.
           This function reaps every one of them, so it would be waiting for
           itself.  */
        ami_log(AMI_LOG_ERROR, "kernel stop: called from a Task the port owns");
        return(TX_CALLER_ERROR);
    }

    if ((_tx_amiga_kernel_up == TX_FALSE) && (_tx_amiga_kernel_stopping == TX_FALSE))
    {
        /* Never started, or already stopped.  Idempotent: a shutdown path may
           run twice and must not turn the second run into an error.  */
        return(TX_SUCCESS);
    }

    /* ---- preconditions --------------------------------------------------- */

    /*
     * The caller may be adopted -- netstack_shutdown() calls from that position
     * -- so its own TX_THREAD does not count against it.  Nothing is orphaned
     * yet: every refusal below has to leave the caller as it found it.
     */
    adopted =  tx_amiga_adopted_thread();

    /* Taken before the Forbid() below, because a refusal has to free it again
       and AllocSignal() inside a critical section buys nothing.  */
    sig =  AllocSignal(-1);
    if (sig < 0)
    {
        ami_log(AMI_LOG_ERROR, "kernel stop: no free Exec signal for the handshake");
        return(TX_NO_MEMORY);
    }
    sigmask =  1UL << ((ULONG) sig);

    /*
     * Check and commit as one atom.  tx_amiga_adopt_thread() runs on some other
     * application Task and only refuses once _tx_amiga_kernel_up is clear.  Do
     * the counting and the clearing in separate critical sections and there is
     * a window in which a Task counts as absent, then adopts, then holds the
     * baton of a kernel that is being dismantled underneath it.
     */
    status =  TX_SUCCESS;

    Forbid();

    remaining =  _tx_amiga_application_threads(adopted);

    if ((_tx_amiga_stop_task != (struct Task *) 0) && (_tx_amiga_stop_task != me))
    {

        /* Another Task is already inside this function.  */
        status =  TX_NOT_DONE;
    }
    else if (remaining != 0UL)
    {

        /*
         * Refuse.  The port cannot terminate these safely on the caller's
         * behalf: tx_thread_delete() of a thread that is blocked in Exec
         * produces a zombie (see _tx_amiga_reap), and a zombie is a Task still
         * running on memory somebody else allocated.  Turning "you still have
         * threads" into "here are some zombies" would convert a clear refusal
         * into an unreportable hazard.
         */
        status =  TX_THREAD_ERROR;
    }
    else if (_tx_amiga_zombies_live != 0UL)
    {

        /*
         * Refuse.  A zombie cannot be reclaimed on demand, and it will run port
         * and application code again when it finally unblocks.  Only the caller
         * knows how to unstick it.
         */
        status =  TX_THREAD_ERROR;
    }
    else if ((_tx_amiga_master_gone == 0UL) &&
             ((_tx_amiga_scheduler_task == (VOID *) 0) ||
              (_tx_amiga_scheduler_signal == 0UL)))
    {

        /* No master Task to bring down: _tx_initialize_low_level() could not
           allocate its signal, so the scheduler is parked in Wait(0) and will
           never leave.  */
        status =  TX_NOT_DONE;
    }
    else
    {

        /* Committed.  Nothing may adopt from here on.  */
        _tx_amiga_kernel_up       =  TX_FALSE;
        _tx_amiga_kernel_stopping =  TX_TRUE;
        _tx_amiga_stop_task       =  me;
        _tx_amiga_stop_signal     =  sigmask;
    }

    Permit();

    if (status != TX_SUCCESS)
    {

        FreeSignal(sig);

        if (status == TX_THREAD_ERROR)
        {
            ami_log(AMI_LOG_ERROR,
                    "kernel stop: refused, %ld application thread(s) and %ld live zombie(s)",
                    (LONG) remaining, (LONG) _tx_amiga_zombies_live);
        }
        else
        {
            ami_log(AMI_LOG_ERROR,
                    "kernel stop: cannot proceed -- a stop is already running, or "
                    "the scheduler Task cannot be reached");
        }
        return(status);
    }

    /* ---- past this point the kernel is coming down ------------------------ */

    /* The caller stops being a ThreadX thread here, and not before: everything
       above this line is allowed to refuse and leave it adopted.  */
    if (adopted != TX_NULL)
    {
        (VOID) tx_amiga_orphan_thread(adopted);
    }

    /* ---- 1. the tick ----------------------------------------------------- */

    /*
     * First, because everything after this is quieter without it, and because
     * the tick Task's entry point is inside the caller's code hunk.  It waits
     * on its timer request or SIGF_SINGLE, so the Signal() makes it notice now
     * rather than one tick from now; the tick's own teardown retires the
     * outstanding timer request (it aborts and destroys it, never re-arms it).
     */
    if (_tx_amiga_timer_task != (VOID *) 0)
    {

        _tx_amiga_timer_stop =  TX_TRUE;
        Signal((struct Task *) _tx_amiga_timer_task, SIGF_SINGLE);

        if (_tx_amiga_stop_wait(&_tx_amiga_timer_gone, sigmask,
                                (ULONG) TX_AMIGA_STOP_TIMEOUT_SECS) == TX_FALSE)
        {
            ami_log(AMI_LOG_ERROR, "kernel stop: the tick Task did not exit");
            status =  TX_NOT_DONE;
        }
    }
    else
    {
        _tx_amiga_timer_stop =  TX_TRUE;
        _tx_amiga_timer_gone =  1UL;
    }

    /* ---- 2. ThreadX's own system timer thread ----------------------------- */

#if !defined(TX_NO_TIMER) && !defined(TX_TIMER_PROCESS_IN_ISR)
    if (status == TX_SUCCESS)
    {

        /*
         * ThreadX creates this one itself, on a stack that lives in ThreadX's
         * BSS, i.e. in the caller's data hunk.  It is not the application's to
         * delete, and leaving it behind means something is still running in the
         * program's BSS after the program exits.
         *
         * The tick is already stopped, so it is parked on its run signal and
         * the ordinary reaper wakes it.
         */
        ULONG zombies_before =  _tx_amiga_zombies_live;

        (VOID) _tx_thread_terminate(&_tx_timer_thread);
        (VOID) _tx_thread_delete(&_tx_timer_thread);

        if (_tx_amiga_zombies_live != zombies_before)
        {
            ami_log(AMI_LOG_ERROR,
                    "kernel stop: the system timer thread became a zombie");
            status =  TX_NOT_DONE;
        }
    }
#endif

    /* ---- 3. the master (scheduler) Task ----------------------------------- */

    if (status == TX_SUCCESS)
    {

        _tx_amiga_wake_scheduler();

        if (_tx_amiga_stop_wait(&_tx_amiga_master_gone, sigmask,
                                (ULONG) TX_AMIGA_STOP_TIMEOUT_SECS) == TX_FALSE)
        {
            ami_log(AMI_LOG_ERROR, "kernel stop: the scheduler Task did not exit");
            status =  TX_NOT_DONE;
        }
    }

    /* ---- 4. give the memory back ----------------------------------------- */

    Forbid();
    _tx_amiga_stop_task   =  (struct Task *) 0;
    _tx_amiga_stop_signal =  0UL;
    Permit();

    SetSignal(0UL, sigmask);
    FreeSignal(sig);

    if (status != TX_SUCCESS)
    {

        /*
         * Something the port created is still alive and cannot be reached.
         * Leave every allocation where it is -- the survivor is standing on
         * some of it -- and tell the caller that exiting is not safe.  The
         * stopping flag stays set, so nothing will start a second kernel on top
         * of it.
         */
        ami_log(AMI_LOG_ERROR,
                "kernel stop: FAILED -- it is NOT safe to unload this program");
        return(status);
    }

    /* The two Tasks published their flags inside the same Forbid() as their
       RemTask(), so they are off the ready list and done with these stacks. */
    if (_tx_amiga_master_stack != (APTR) 0)
    {
        FreeMem(_tx_amiga_master_stack, _tx_amiga_master_stack_size);
        _tx_amiga_master_stack      =  (APTR) 0;
        _tx_amiga_master_stack_size =  0UL;
    }

    if (_tx_amiga_timer_stack != (APTR) 0)
    {
        FreeMem(_tx_amiga_timer_stack, _tx_amiga_timer_stack_size);
        _tx_amiga_timer_stack =  (APTR) 0;
    }

    if ((_tx_amiga_memory_owned != TX_FALSE) &&
        (_tx_amiga_kernel_memory != (VOID *) 0))
    {
        FreeMem(_tx_amiga_kernel_memory, _tx_amiga_kernel_memory_size);
        _tx_amiga_kernel_memory      =  (VOID *) 0;
        _tx_amiga_kernel_memory_size =  0UL;
        _tx_amiga_memory_owned       =  TX_FALSE;
    }

    /* ---- 5. back to the state tx_amiga_kernel_start() expects ------------- */

    _tx_amiga_timer_base =  (struct Device *) 0;
    _tx_amiga_timer_stop =  TX_FALSE;
    _tx_amiga_adopt_task   =  (VOID *) 0;
    _tx_amiga_adopt_signal =  0UL;

    {
        UBYTE  *p =  (UBYTE *) &_tx_amiga_tick;
        ULONG   n;

        for (n = 0UL; n < (ULONG) sizeof(_tx_amiga_tick); n++)
        {
            p[n] =  0;
        }
    }

    _tx_amiga_kernel_stopping =  TX_FALSE;

    ami_log(AMI_LOG_INFO, "kernel stop: ThreadX is down; nothing of the port is running");

    return(TX_SUCCESS);
}
