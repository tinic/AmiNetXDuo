/***************************************************************************
 * Eclipse ThreadX, AmigaOS/m68k port.
 *
 * Derived in structure from ports/linux/gnu/src/tx_initialize_low_level.c
 *   Copyright (c) 2024 Microsoft Corporation
 *   Copyright (c) 2026-present Eclipse ThreadX contributors
 *
 * SPDX-License-Identifier: MIT
 **************************************************************************/

/* _tx_initialize_low_level, AmigaOS/m68k: adopts the calling Exec Task as the
   ThreadX scheduler ("master"), reserves the block tx_application_define() is
   given, and creates the tick Task.  Also holds kernel start and stop.  */

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#include <devices/timer.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>

/* `struct timerequest`, not `struct TimeRequest`: the lowercase spelling compiles
   against NDK 3.2 (which renamed the types and kept the old names as aliases) and
   against every earlier NDK; the CamelCase one does not.  */

/* ReadEClock() resolves timer.device through the symbol named by TIMER_BASE_NAME.
   Point it at a base of our own: the port must not depend on which of it and
   src/common/compat.c opened the device first, or on compat.c being linked.  */

#define __NOLIBBASE__
#define TIMER_BASE_NAME     _tx_amiga_timer_base
#include <proto/timer.h>

#include "aminetxduo/compat.h"      /* AMI_ERROR/AMI_WARN/AMI_INFO */

/* Freed with FreeMem and the same size, so nothing downstream cares.  */
static APTR _tx_amiga_alloc(ULONG size, ULONG memf)
{
    return(AllocMem(size, memf));
}



/* ---------------------------------------------------------------- state, */

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

/* Handshake for tx_amiga_kernel_stop().  Each Task the port created sets its flag
   and pokes the stopper inside the same Forbid() as its RemTask(), whose nesting
   Exec discards, so seeing the flag means its stack is safe to free.  */
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

    /* Two allocations.  RemTask() hands each MemList to FreeEntry(), which frees
       both the entries it describes and the MemList itself, so a MemList inside the
       block it describes is freed twice (Guru 01000009) on every task that exits. */

    memlist =  (struct MemList *) _tx_amiga_alloc((ULONG) sizeof(struct MemList),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (memlist == (struct MemList *) 0)
    {
        return((struct Task *) 0);
    }

    ctrl =  (struct _tx_amiga_ctrl *) _tx_amiga_alloc((ULONG) sizeof(struct _tx_amiga_ctrl),
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
    /* tc_UserData points back at the Task, which is also the control block: that
       is what makes _tx_amiga_ctrl_of() safe on a task the port did not create. */
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
       Forbid() and then frees the struct Task.  Read and Signal have to be one
       atom, or a poke that arrives in that window writes into freed memory.  */
    Forbid();
    TX_AMIGA_COUNT(TX_AMIGA_SC_WAKE);
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


/* ------------------------------------------------------- initialisation, */

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


#ifdef AMINETXDUO_SCHEDCOUNT

ULONG   _tx_amiga_sched_count[TX_AMIGA_SC_MAX];

VOID tx_amiga_sched_stats(TX_AMIGA_SCHED_STATS *stats)
{

    if (stats == (TX_AMIGA_SCHED_STATS *) 0)
    {
        return;
    }

    Forbid();

    stats -> sc_disable        =  _tx_amiga_sched_count[TX_AMIGA_SC_DISABLE];
    stats -> sc_restore        =  _tx_amiga_sched_count[TX_AMIGA_SC_RESTORE];
    stats -> sc_permit_slow    =  _tx_amiga_sched_count[TX_AMIGA_SC_PERMIT_SLOW];
    stats -> sc_mutex_get      =  _tx_amiga_sched_count[TX_AMIGA_SC_MUTEX_GET];
    stats -> sc_mutex_put      =  _tx_amiga_sched_count[TX_AMIGA_SC_MUTEX_PUT];
    stats -> sc_sys_return     =  _tx_amiga_sched_count[TX_AMIGA_SC_SYS_RETURN];
    stats -> sc_wake           =  _tx_amiga_sched_count[TX_AMIGA_SC_WAKE];
    stats -> sc_sched_dispatch =  _tx_amiga_sched_count[TX_AMIGA_SC_SCHED_DISPATCH];
    stats -> sc_sched_wait     =  _tx_amiga_sched_count[TX_AMIGA_SC_SCHED_WAIT];
    stats -> sc_park_wait      =  _tx_amiga_sched_count[TX_AMIGA_SC_PARK_WAIT];
    stats -> sc_park_spurious  =  _tx_amiga_sched_count[TX_AMIGA_SC_PARK_SPURIOUS];
    stats -> sc_direct         =  _tx_amiga_sched_count[TX_AMIGA_SC_DIRECT];

    /* Exec's own.  DispCount is what Reschedule/Switch/Dispatch time is
       charged against, and nothing we could instrument would produce it.  */
    stats -> sc_exec_dispatch  =  SysBase -> DispCount;
    stats -> sc_exec_idle      =  SysBase -> IdleCount;

    Permit();
}

#endif /* AMINETXDUO_SCHEDCOUNT */


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
        _tx_amiga_kernel_memory =  (VOID *) _tx_amiga_alloc(_tx_amiga_kernel_memory_size,
                                                     MEMF_PUBLIC | MEMF_CLEAR);
        if (_tx_amiga_kernel_memory == (VOID *) 0)
        {

            /* Owning nothing: the flag must stay clear, or the teardown
               FreeMem()s a NULL block.  */
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
    _tx_amiga_timer_stack =  _tx_amiga_alloc(_tx_amiga_timer_stack_size, MEMF_PUBLIC | MEMF_CLEAR);
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

/* The ThreadX periodic interrupt, an ordinary high-priority Exec Task; the
   Forbid() in _tx_thread_context_save() is what gives it ISR semantics.  The
   wakeup source is not the time base: the E-Clock decides what a wakeup is worth. */

/* Tell a waiting tx_amiga_kernel_stop() that this Task has finished with its
   stack.  Call inside the Forbid() that ends in RemTask(), never outside it.  */
static VOID _tx_amiga_stop_notify(volatile ULONG *flag)
{

    *flag =  1UL;

    if (_tx_amiga_stop_task != (struct Task *) 0)
    {
        Signal(_tx_amiga_stop_task, _tx_amiga_stop_signal);
    }
}


/* Park a tick task that has no usable wakeup source so that it stays reapable:
   SIGF_SINGLE, never Wait(0), or tx_amiga_kernel_stop() has no way to wake it.  */
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
/* VBlank wakeup source: a VERTB server rides an interrupt the machine already
   takes and does one Signal(), where a timer.device request costs a full IORequest
   round trip.  The rate is still not the time base; the E-Clock decides.  */
static ULONG _tx_amiga_vblank_sigmask =  0UL;
static ULONG _tx_amiga_vblank_frames  =  0UL;

/* Whom the server signals.  The tick Task by default; in a green build the task
   hands the wakeup to the REALM once the source is validated (the tick merge
   below), and a frame then costs one Signal() and no dedicated-Task switch.  */
static struct Task * volatile _tx_amiga_vblank_task =  (struct Task *) 0;

/* Frames per wakeup.  The E-Clock is the time base, not this, so dividing the
   wakeup rate does not divide the clock.  It coarsens delivery to the divider
   times the frame, and does NOT change the tick count.  */
#ifndef TX_AMIGA_VBLANK_DIVIDER
#define TX_AMIGA_VBLANK_DIVIDER                 2UL
#endif

/* The chain convention is carried in the Z FLAG, not in d0: a server returning Z
   clear makes Exec skip the rest of the chain, and a C `return 0` cannot promise
   the flag.  Hence an asm entry whose last flag-affecting instruction is a moveq. */
__asm__(
"       .text\n"
"       .align  2\n"
"       .globl  __tx_amiga_vblank_entry\n"
"__tx_amiga_vblank_entry:\n"
"       jsr     __tx_amiga_vblank_server\n"
"       moveq   #0,%d0\n"
"       rts\n");

extern VOID _tx_amiga_vblank_entry(VOID);

/* Not static: the assembly entry above refers to it by name, and that alone is not
   enough -- under -flto nothing parses the asm() string, so `used' is what holds
   the symbol.  */
ULONG _tx_amiga_vblank_server(VOID) __attribute__((used));
ULONG _tx_amiga_vblank_server(VOID)
{

struct Task *task =  _tx_amiga_vblank_task;


#ifdef TX_AMIGA_TICK_TEST_DEAD_VERTB
    /* A machine whose VERTB never fires, on demand: the watchdog request kept
       beside this server is otherwise unreachable.  Test builds only.  */
    return(0UL);
#endif

    /* Interrupt context: Signal() is the only thing this may do.  Exec saves
       d0/d1/a0/a1/a5/a6 across a server, and the C ABI preserves the rest. */
    if ((task != (struct Task *) 0) && (_tx_amiga_vblank_sigmask != 0UL))
    {
        _tx_amiga_vblank_frames++;

        if (_tx_amiga_vblank_frames >= (ULONG) TX_AMIGA_VBLANK_DIVIDER)
        {
            _tx_amiga_vblank_frames =  0UL;
            Signal(task, _tx_amiga_vblank_sigmask);
        }
    }

    return(0UL);        /* 0: not exclusive, let the rest of the chain run */
}

static struct Interrupt _tx_amiga_vblank_int;

static VOID _tx_amiga_timer_arm(struct timerequest *tr, ULONG secs, ULONG micro)
{

    tr -> tr_node.io_Command =  TR_ADDREQUEST;
    tr -> tr_time.tv_secs    =  secs;
    tr -> tr_time.tv_micro   =  micro;
    SendIO((struct IORequest *) tr);
}


/* Measure a wakeup source, in Hz * 100; 0 if it produced nothing.  `guard`, on a
   different unit and port, bounds the window.  Nothing is ever AbortIO()ed here:
   an aborted request never completes again.  `tr` is left outstanding.  */
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


/* Close and destroy a request/port pair the port is done with.  The only place
   AbortIO() is used: the pair is destroyed immediately after and never re-armed. */
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


/* ---------------------------------------------- the shared tick service ----
 *
 * Everything one wakeup does, with two callers: the tick task (every build) and,
 * in a green build, the realm's scheduler loop.  The E-Clock is the time base for
 * both, and the whole service runs under one Forbid().
 */

struct _tx_amiga_tick_run   _tx_amiga_tick_run;   /* struct: tx_amiga_internal.h */


VOID _tx_amiga_tick_deliver(UINT from_realm)
{

struct _tx_amiga_tick_run  *r =  &_tx_amiga_tick_run;
struct EClockVal            svc_now;
ULONG                       svc_delta;
ULONG                       svc_measured;
ULONG                       svc_advance;
ULONG                       svc_carry;
ULONG                       svc_cost;
ULONG                       svc_i;


    Forbid();

    if (r -> tr_live == ((UINT) TX_FALSE))
    {
        Permit();
        return;
    }

    (VOID) ReadEClock(&svc_now);

    /* Accumulated, because ev_lo wraps every ~100 minutes; the service runs
       many times a second, so it never misses a wrap.  WHILE, not IF: a
       wakeup seconds late must not leave whole seconds in the remainder. */
    r -> tr_up_rem +=  (ULONG) (svc_now.ev_lo - r -> tr_up_lo);
    r -> tr_up_lo   =  svc_now.ev_lo;
    while (r -> tr_up_rem >= r -> tr_eclock_hz)
    {
        r -> tr_up_rem -=  r -> tr_eclock_hz;
        _tx_amiga_tick.tx_amiga_tick_uptime_ms +=  1000UL;
    }
    _tx_amiga_tick.tx_amiga_tick_uptime_rem =  r -> tr_up_rem;

    svc_delta    =  (ULONG) (svc_now.ev_lo - r -> tr_last_lo);
    svc_measured =  svc_delta / r -> tr_eclock_per_tick;

    if (svc_measured == 0UL)
    {

        /* Nothing owed.  The tick task's early wake is counted as it always was;
           a realm pass that merely came by is neither a wakeup nor an empty one. */
        if (from_realm == ((UINT) TX_FALSE))
        {
            _tx_amiga_tick.tx_amiga_tick_wakeups++;
            _tx_amiga_tick.tx_amiga_tick_empty++;
        }
        Permit();
        return;
    }

    _tx_amiga_tick.tx_amiga_tick_wakeups++;

    /* Advance by exactly `svc_measured` periods with the remainder carried, so the
       long-run rate is exactly TX_TIMER_TICKS_PER_SECOND.  A carry that would put
       the anchor past `svc_now` is held over: the next service reads that as a wrap. */
    svc_advance   =  svc_measured * r -> tr_eclock_per_tick;
    r -> tr_frac +=  svc_measured * r -> tr_eclock_rem;
    svc_carry     =  r -> tr_frac / (ULONG) TX_TIMER_TICKS_PER_SECOND;
    if ((svc_advance + svc_carry) > svc_delta)
    {
        svc_carry =  0UL;
    }
    else
    {
        r -> tr_frac -=  svc_carry * (ULONG) TX_TIMER_TICKS_PER_SECOND;
    }
    r -> tr_last_lo +=  svc_advance + svc_carry;

    /* The clock, and the only thing that sets it.  */
    _tx_amiga_timer_clock_advance(svc_measured);

    r -> tr_backlog +=  svc_measured;

    /* Sampled here because this is the moment the wheel is furthest behind
       the clock.  */
    if ((r -> tr_backlog + _tx_amiga_tick.tx_amiga_tick_lost) >
        _tx_amiga_tick.tx_amiga_tick_skew_peak)
    {
        _tx_amiga_tick.tx_amiga_tick_skew_peak =
            r -> tr_backlog + _tx_amiga_tick.tx_amiga_tick_lost;
    }

    if (svc_delta > r -> tr_worst_delta)
    {
        r -> tr_worst_delta =  svc_delta;
        _tx_amiga_tick.tx_amiga_tick_worst_stall_ms =
            tx_amiga_eclock_ms(svc_delta, r -> tr_eclock_per_ms);
        _tx_amiga_tick.tx_amiga_tick_worst_service_us =
            tx_amiga_eclock_us(r -> tr_last_service, r -> tr_eclock_per_ms);
    }

    if (r -> tr_backlog > (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP)
    {

        /* The ceiling on the backlog, and the only place a tick is thrown
           away; also the only path that skips a wheel slot.  */
        _tx_amiga_tick.tx_amiga_tick_lost +=
            r -> tr_backlog - (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP;
        _tx_amiga_tick.tx_amiga_tick_clipped++;

        if (_tx_amiga_tick.tx_amiga_tick_clipped <= 3UL)
        {
            AMI_WARN("tick: %ld ms since the last wakeup, wheel skips %ld "
                    "of %ld owed (cap %ld, previous service %ld us)",
                    (LONG) tx_amiga_eclock_ms(svc_delta, r -> tr_eclock_per_ms),
                    (LONG) (r -> tr_backlog - (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP),
                    (LONG) r -> tr_backlog, (LONG) TX_AMIGA_TIMER_MAX_CATCHUP,
                    (LONG) tx_amiga_eclock_us(r -> tr_last_service,
                                              r -> tr_eclock_per_ms));
        }

        r -> tr_backlog =  (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP;
    }
    else if (r -> tr_backlog > 1UL)
    {
        _tx_amiga_tick.tx_amiga_tick_catchups++;
    }

    for (svc_i = 0UL; svc_i < r -> tr_backlog; svc_i++)
    {

        /* Half the period is the tick's, at most; the budget bounds the
           burst, and the rest of the backlog waits for a later service.  */
        if (svc_i > 0UL)
        {
            struct EClockVal budget_now;

            (VOID) ReadEClock(&budget_now);
            if ((ULONG) (budget_now.ev_lo - svc_now.ev_lo) >
                (r -> tr_eclock_per_ms * (ULONG) TX_AMIGA_TIMER_BUDGET_MS))
            {
                _tx_amiga_tick.tx_amiga_tick_over_budget++;
                _tx_amiga_tick.tx_amiga_tick_deferred +=
                    r -> tr_backlog - svc_i;
                break;
            }
        }

        /* Enter "interrupt" context, run the tick, leave it.  */
        _tx_thread_context_save();
        _tx_timer_interrupt();
        _tx_thread_context_restore();
    }

    r -> tr_backlog -=  svc_i;
    _tx_amiga_tick.tx_amiga_tick_delivered +=  svc_i;
    _tx_amiga_tick.tx_amiga_tick_skew =
        r -> tr_backlog + _tx_amiga_tick.tx_amiga_tick_lost;

    /* Poke the scheduler when it could actually dispatch.  Pointless from
       the realm: it IS the scheduler, mid-pass.  */
    if (from_realm == ((UINT) TX_FALSE))
    {
        if ((_tx_thread_current_ptr == TX_NULL) &&
            (_tx_thread_execute_ptr != TX_NULL))
        {
            _tx_amiga_wake_scheduler();
        }
    }

    /* Service cost of this delivery, summed over tx_amiga_tick_wakeups.  */
    {
        struct EClockVal end;

        (VOID) ReadEClock(&end);
        svc_cost             =  (ULONG) (end.ev_lo - svc_now.ev_lo);
        r -> tr_last_service =  svc_cost;
        if (svc_cost < 4000000UL)
        {
            _tx_amiga_tick.tx_amiga_tick_service_us +=
                (svc_cost * 1000UL) / r -> tr_eclock_per_ms;
        }
    }

    Permit();
}


static VOID _tx_amiga_timer_task_entry(VOID)
{

struct MsgPort      *port;
struct MsgPort      *guard_port;
struct timerequest  *tr;
struct timerequest  *guard;
struct EClockVal     now;
ULONG                port_sig;
ULONG                wake_sig;
ULONG                arm_secs;
ULONG                arm_micro;
BYTE                 vb_bit;
UINT                 vb_mode;
ULONG                guard_sig;
ULONG                interval_secs;
ULONG                interval_micro;
ULONG                eclock_hz;
ULONG                eclock_per_ms;
ULONG                eclock_per_tick;
ULONG                eclock_rem;
ULONG                rate_chz;
ULONG                unit;
UINT                 armed;


    /* Wait for _tx_amiga_start_interrupts().  The wakeup source must not be opened
       and validated before this point: the "preferred unit will not open" fallback
       then leaves the clock dead at zero.  */
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
    interval_micro =  1000000UL / (ULONG) TX_TIMER_TICKS_PER_SECOND;
    interval_secs  =  interval_micro / 1000000UL;
    interval_micro =  interval_micro % 1000000UL;

    /* The startup guard: a UNIT_MICROHZ request on its own port, distinguishable by
       signal alone.  It stops a machine whose VBlank never fires from parking the
       tick task forever, and becomes the tick if the source is rejected.  */
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
        AMI_WARN("tick: timer.device unit %ld would not open; using UNIT_MICROHZ",
                (LONG) unit);

        if (guard != (struct timerequest *) 0)
        {
            /* Move the tick to the guard's request, already open on UNIT_MICROHZ;
               the rejected request is thrown away rather than reopened.  */
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
            AMI_ERROR("tick: no timer.device at all; ThreadX has no clock");
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
        AMI_WARN("tick: ReadEClock reported 0 Hz; assuming %ld",
                (LONG) eclock_hz);
    }
    eclock_per_ms =  eclock_hz / 1000UL;
    if (eclock_per_ms == 0UL)
    {
        eclock_per_ms =  1UL;
    }

    /* Floor the per-tick period and carry the remainder, so the long-run rate is
       exactly TX_TIMER_TICKS_PER_SECOND however awkward the E-Clock frequency is:
       709379 / 50 truncates to 14187, which alone would gain seconds a day.  */
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

        /* UNIT_VBLANK rounds any request up to the next vertical blank, so the
           smallest request asks for one frame: 50 Hz PAL, 60 Hz NTSC.  */
        rate_chz =  _tx_amiga_timer_probe(tr, port_sig, guard, guard_sig, eclock_per_ms);

        _tx_amiga_tick.tx_amiga_tick_source_chz =  rate_chz;

        if ((rate_chz < ((ULONG) TX_AMIGA_TIMER_PROBE_MIN_HZ * 100UL)) ||
            (rate_chz > ((ULONG) TX_AMIGA_TIMER_PROBE_MAX_HZ * 100UL)))
        {

            AMI_WARN("tick: timer.device unit %ld woke at %ld.%02ld Hz, outside %ld..%ld Hz, "
                    "falling back to UNIT_MICROHZ",
                    (LONG) unit, (LONG) (rate_chz / 100UL), (LONG) (rate_chz % 100UL),
                    (LONG) TX_AMIGA_TIMER_PROBE_MIN_HZ, (LONG) TX_AMIGA_TIMER_PROBE_MAX_HZ);

            /* Throw the rejected source away whole and promote the guard.  Its
               outstanding request may never complete, so it is aborted and then
               destroyed rather than re-armed (see _tx_amiga_timer_probe).  */
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
        AMI_WARN("tick: no UNIT_MICROHZ available to validate against; "
                "using unit %ld unchecked", (LONG) unit);
    }

    if (unit == (ULONG) UNIT_MICROHZ)
    {
        /* One request per tick period; the E-Clock still decides how many
           ticks that wakeup is worth.  */
        _tx_amiga_tick.tx_amiga_tick_source_chz =
            (ULONG) TX_TIMER_TICKS_PER_SECOND * 100UL;
    }
    else
    {
        interval_secs  =  0UL;
        interval_micro =  1UL;           /* "next vertical blank"  */
    }

    _tx_amiga_tick.tx_amiga_tick_unit       =  unit;
    _tx_amiga_tick.tx_amiga_tick_eclock_hz  =  eclock_hz;

    AMI_INFO("tick: %ld Hz from timer.device unit %ld (%ld.%02ld Hz wakeups), E-Clock %ld Hz",
            (LONG) TX_TIMER_TICKS_PER_SECOND, (LONG) unit,
            (LONG) (_tx_amiga_tick.tx_amiga_tick_source_chz / 100UL),
            (LONG) (_tx_amiga_tick.tx_amiga_tick_source_chz % 100UL),
            (LONG) eclock_hz);

    /* ---- the tick ------------------------------------------------------- */

    ReadEClock(&now);

    Forbid();
    _tx_amiga_tick_run.tr_eclock_hz       =  eclock_hz;
    _tx_amiga_tick_run.tr_eclock_per_ms   =  eclock_per_ms;
    _tx_amiga_tick_run.tr_eclock_per_tick =  eclock_per_tick;
    _tx_amiga_tick_run.tr_eclock_rem      =  eclock_rem;
    _tx_amiga_tick_run.tr_frac            =  0UL;
    _tx_amiga_tick_run.tr_backlog         =  0UL;
    _tx_amiga_tick_run.tr_last_lo         =  now.ev_lo;
    _tx_amiga_tick_run.tr_up_lo           =  now.ev_lo;
    _tx_amiga_tick_run.tr_up_rem          =  0UL;
    _tx_amiga_tick_run.tr_last_service    =  0UL;
    _tx_amiga_tick_run.tr_worst_delta     =  0UL;
    _tx_amiga_tick_run.tr_realm           =  (UINT) TX_FALSE;
    _tx_amiga_tick_run.tr_live            =  (UINT) TX_TRUE;
    Permit();

    if (armed == TX_FALSE)
    {
        /* A source that passed validation is already armed: the probe's last
           request is this tick's first wakeup.  */
        _tx_amiga_timer_arm(tr, interval_secs, interval_micro);
        armed =  TX_TRUE;
    }

    /* Wakeup source.  The request stays armed either way: under the server it is
       simply never waited on, which keeps teardown identical.  */
    wake_sig  =  port_sig;
    vb_mode   =  TX_FALSE;
    vb_bit    =  -1;
    arm_secs  =  interval_secs;
    arm_micro =  interval_micro;
#ifndef TX_AMIGA_TICK_NO_VBLANK_SERVER
    vb_bit =  AllocSignal(-1L);
    if (vb_bit != -1)
    {
        _tx_amiga_vblank_sigmask =  1UL << ((ULONG) vb_bit);

        _tx_amiga_vblank_int.is_Node.ln_Type =  NT_INTERRUPT;
        _tx_amiga_vblank_int.is_Node.ln_Pri  =  -60;
        _tx_amiga_vblank_int.is_Node.ln_Name =  (char *) "ThreadX tick";
        _tx_amiga_vblank_int.is_Data         =  (APTR) 0;
        _tx_amiga_vblank_int.is_Code         =  (VOID (*)()) _tx_amiga_vblank_entry;

        AddIntServer((ULONG) INTB_VERTB, &_tx_amiga_vblank_int);
#ifdef AMINETXDUO_GREEN_REALM
        /* The tick merge: the server signals the REALM -- its own scheduler signal,
           so no bit of the realm's 32 is spent -- and the realm services the tick
           in passing.  This task keeps only the once-a-second watchdog.  */
        Forbid();
        _tx_amiga_vblank_task       =  (struct Task *) _tx_amiga_scheduler_task;
        _tx_amiga_vblank_sigmask    =  _tx_amiga_scheduler_signal;
        _tx_amiga_tick_run.tr_realm =  (UINT) TX_TRUE;
        Permit();
        wake_sig =  port_sig;           /* the watchdog is all we wake for  */
#else
        _tx_amiga_vblank_task =  (struct Task *) _tx_amiga_timer_task;
        wake_sig =  _tx_amiga_vblank_sigmask;
#endif
        vb_mode  =  TX_TRUE;
        arm_secs =  1UL;        /* watchdog only; VERTB is the tick */
        arm_micro =  0UL;
    }
    else
    {
        /* The one thing here that can fail: with no free signal the server has no
           way to reach the tick task, so the kernel falls back to waiting on the
           timer.device request at the tick period -- correct, and slower.  */
        AMI_WARN("tick: no free signal for the VBlank server; falling back to "
                "timer.device requests");
    }
#endif

    while (_tx_amiga_timer_stop == TX_FALSE)
    {

        Wait(wake_sig | port_sig | SIGF_SINGLE);

        if (_tx_amiga_timer_stop != TX_FALSE)
        {
            /* Leave the request armed; the teardown below retires it.  */
            break;
        }

        if (CheckIO((struct IORequest *) tr) != (struct IORequest *) 0)
        {
            /* Re-arm before doing the tick work: on UNIT_VBLANK that reserves the
               next frame up front, so an overrun costs a late delivery rather than
               a skipped frame.  Under the server this request is the watchdog.  */
            WaitIO((struct IORequest *) tr);
            armed =  TX_FALSE;
            _tx_amiga_timer_arm(tr, arm_secs, arm_micro);
            armed =  TX_TRUE;
        }
        else if (vb_mode == TX_FALSE)
        {
            /* Woken by something that was not our request.  Do not abort it -- an
               aborted timer request cannot be re-armed -- just go back to sleep. */
            continue;
        }

        /* Everything one wakeup does lives in the shared service now; in a
           green build the realm is its other caller.  */
        _tx_amiga_tick_deliver((UINT) TX_FALSE);
    }

    if (_tx_amiga_tick.tx_amiga_tick_clipped != 0UL)
    {
        AMI_WARN("tick: %ld stalls clipped, wheel %ld ticks behind the clock "
                "(worst %ld)",
                (LONG) _tx_amiga_tick.tx_amiga_tick_clipped,
                (LONG) _tx_amiga_tick.tx_amiga_tick_skew,
                (LONG) _tx_amiga_tick.tx_amiga_tick_skew_peak);
    }
    AMI_INFO("tick: %ld wakeups -> %ld ticks in %ld ms (%ld empty, %ld caught up)",
            (LONG) _tx_amiga_tick.tx_amiga_tick_wakeups,
            (LONG) _tx_amiga_tick.tx_amiga_tick_delivered,
            (LONG) _tx_amiga_tick.tx_amiga_tick_uptime_ms,
            (LONG) _tx_amiga_tick.tx_amiga_tick_empty,
            (LONG) _tx_amiga_tick.tx_amiga_tick_catchups);
    AMI_INFO("tick: %ld us in-task over %ld wakeups",
            (LONG) _tx_amiga_tick.tx_amiga_tick_service_us,
            (LONG) _tx_amiga_tick.tx_amiga_tick_wakeups);

    /* Retire the shared service before its timer base goes away: the realm
       tests tr_live under the same Forbid() the service holds, so after
       this no caller can reach ReadEClock() on a dead base.  */
    Forbid();
    _tx_amiga_tick_run.tr_live  =  (UINT) TX_FALSE;
    _tx_amiga_tick_run.tr_realm =  (UINT) TX_FALSE;
    Permit();

    _tx_amiga_timer_base =  (struct Device *) 0;

#ifndef TX_AMIGA_TICK_NO_VBLANK_SERVER
    /* Before the signal it pokes is freed, or a frame landing in the gap
       signals a bit this Task no longer owns. */
    if (vb_mode != TX_FALSE)
    {
        RemIntServer((ULONG) INTB_VERTB, &_tx_amiga_vblank_int);
        _tx_amiga_vblank_sigmask =  0UL;
        _tx_amiga_vblank_task    =  (struct Task *) 0;
    }
    if (vb_bit != -1)
    {
        FreeSignal((LONG) vb_bit);
    }
#endif

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


/* -------------------------------------------------- library-style start, */

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

    stack =  _tx_amiga_alloc(stack_size, MEMF_PUBLIC | MEMF_CLEAR);
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

/* Wait, with a timeout, for one of the teardown flags above; TX_TRUE if it came up,
   TX_FALSE on timeout.  A fresh timer.device request per call: an AbortIO()d one
   never completes again, so a request that may be aborted cannot be reused.  */
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
               wait is all we can offer; going round again would block forever.  */
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


/* Application-owned ThreadX threads that would survive the stop.  ThreadX's system
   timer thread and `mine` are discounted; `mine` is discounted here rather than
   orphaned first, so that a refusal leaves the caller as it found it.  */
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

        /* A Task the port created: this function reaps every one of them, so it
           would be waiting for itself.  */
        AMI_ERROR("kernel stop: called from a Task the port owns");
        return(TX_CALLER_ERROR);
    }

    if ((_tx_amiga_kernel_up == TX_FALSE) && (_tx_amiga_kernel_stopping == TX_FALSE))
    {
        /* Never started, or already stopped.  Idempotent: a shutdown path may
           run twice and must not turn the second run into an error.  */
        return(TX_SUCCESS);
    }

    /* ---- preconditions --------------------------------------------------- */

    /* The caller may be adopted -- netstack_shutdown() calls from that position --
       so its own TX_THREAD does not count against it.  Nothing is orphaned yet:
       every refusal below has to leave the caller as it found it.  */
    adopted =  tx_amiga_adopted_thread();

    /* Taken before the Forbid() below, because a refusal has to free it again
       and AllocSignal() inside a critical section buys nothing.  */
    sig =  AllocSignal(-1);
    if (sig < 0)
    {
        AMI_ERROR("kernel stop: no free Exec signal for the handshake");
        return(TX_NO_MEMORY);
    }
    sigmask =  1UL << ((ULONG) sig);

    /* Check and commit as one atom.  tx_amiga_adopt_thread() only refuses once
       _tx_amiga_kernel_up is clear, so counting and clearing in separate critical
       sections leaves a window in which a Task adopts a kernel being dismantled. */
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

        /* Refuse.  tx_thread_delete() of a thread blocked in Exec produces a zombie
           (see _tx_amiga_reap), so tearing these down on the caller's behalf would
           turn a clear refusal into an unreportable hazard.  */
        status =  TX_THREAD_ERROR;
    }
    else if (_tx_amiga_zombies_live != 0UL)
    {

        /* Refuse.  A zombie cannot be reclaimed on demand and will run port and
           application code again when it unblocks; only the caller can unstick it. */
        status =  TX_THREAD_ERROR;
    }
    else if ((_tx_amiga_master_gone == 0UL) &&
             ((_tx_amiga_scheduler_task == (VOID *) 0) ||
              (_tx_amiga_scheduler_signal == 0UL)))
    {

        /* No master Task to bring down: _tx_initialize_low_level() could not
           allocate its signal, so the scheduler is parked in Wait(0) for good.  */
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
            AMI_ERROR("kernel stop: refused, %ld application thread(s) and %ld live zombie(s)",
                    (LONG) remaining, (LONG) _tx_amiga_zombies_live);
        }
        else
        {
            AMI_ERROR("kernel stop: cannot proceed, a stop is already running, or "
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

    /* First, because everything after is quieter without it and because the tick
       Task's entry point is inside the caller's code hunk.  The Signal() makes it
       notice now rather than one tick from now.  */
    if (_tx_amiga_timer_task != (VOID *) 0)
    {

        _tx_amiga_timer_stop =  TX_TRUE;
        Signal((struct Task *) _tx_amiga_timer_task, SIGF_SINGLE);

        if (_tx_amiga_stop_wait(&_tx_amiga_timer_gone, sigmask,
                                (ULONG) TX_AMIGA_STOP_TIMEOUT_SECS) == TX_FALSE)
        {
            AMI_ERROR("kernel stop: the tick Task did not exit");
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

        /* ThreadX creates this one itself, on a stack in ThreadX's BSS -- the
           caller's data hunk -- so it is not the application's to delete and not
           safe to leave behind.  The ordinary reaper wakes it.  */
        ULONG zombies_before =  _tx_amiga_zombies_live;

        (VOID) _tx_thread_terminate(&_tx_timer_thread);
        (VOID) _tx_thread_delete(&_tx_timer_thread);

        if (_tx_amiga_zombies_live != zombies_before)
        {
            AMI_ERROR("kernel stop: the system timer thread became a zombie");
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
            AMI_ERROR("kernel stop: the scheduler Task did not exit");
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

        /* Something the port created is still alive and cannot be reached.  Leave
           every allocation where it is, the survivor is standing on some of it, and
           tell the caller that exiting is not safe.  */
        AMI_ERROR("kernel stop: FAILED, it is NOT safe to unload this program");
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

    AMI_INFO("kernel stop: ThreadX is down; nothing of the port is running");

    return(TX_SUCCESS);
}
