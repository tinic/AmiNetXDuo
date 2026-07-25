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
/*    task, allocates the signal that yielding threads use to hand the     */
/*    baton back, reserves the memory block handed to                      */
/*    tx_application_define(), and creates the periodic tick Task.  The    */
/*    tick Task is held at its start signal until                          */
/*    _tx_amiga_start_interrupts() releases it, which the generic kernel   */
/*    entry does immediately before calling _tx_thread_schedule().  It     */
/*    then validates its wakeup source, which takes about 250 ms, before   */
/*    the first tick is delivered.                                         */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#include <devices/timer.h>

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
volatile UINT   _tx_amiga_timer_stop          = TX_FALSE;
volatile ULONG  _tx_amiga_zombies             = 0UL;

/* Set when the port allocated the kernel memory block itself.  */
static UINT     _tx_amiga_memory_owned        = TX_FALSE;

/* Stack of the tick task; owned by the port.  */
static APTR     _tx_amiga_timer_stack         = (APTR) 0;
static ULONG    _tx_amiga_timer_stack_size    = 4096UL;

/* Handshake for tx_amiga_kernel_start().  */
static struct Task *_tx_amiga_starter_task    = (struct Task *) 0;
static ULONG        _tx_amiga_starter_signal  = 0UL;
static UINT         _tx_amiga_start_status    = TX_NOT_DONE;

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

    /* TWO allocations, deliberately.  RemTask() walks tc_MemEntry and hands
       each MemList to FreeEntry(), which is the exact inverse of AllocEntry():
       it frees every me_Addr/me_Length the list describes AND THEN THE MemList
       ITSELF.  Putting the MemList inside the block it describes therefore
       frees that address twice -- FreeMem(block, block_size) followed by
       FreeMem(block, sizeof(struct MemList)) -- which is Guru 01000009,
       AN_FreeTwice, on every task that exits.  amiga.lib's CreateTask() keeps
       them apart for the same reason.  */

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

    if (_tx_amiga_scheduler_task != (VOID *) 0)
    {
        Signal((struct Task *) _tx_amiga_scheduler_task, _tx_amiga_scheduler_signal);
    }
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

        /* Without a signal there is no way to hand the baton back; refuse to
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
        _tx_amiga_memory_owned =  TX_TRUE;
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
 * _tx_thread_context_save() takes the core lock with Forbid(), which is what
 * gives the tick genuine ISR semantics: while it is held, no other task in the
 * machine runs, so the ThreadX thread that currently holds the baton really is
 * frozen -- exactly the property the Linux port buys with pthread_kill().
 *
 * THE WAKEUP SOURCE IS NOT THE TIME BASE.  The task parks on timer.device
 * UNIT_VBLANK, which costs a list insertion on an interrupt the machine takes
 * anyway; but it never counts those wakeups.  On each one it reads the E-Clock
 * and asks how many whole TX_TIMER_TICKS_PER_SECOND periods have really
 * elapsed, then calls _tx_timer_interrupt() exactly that many times.  A late
 * or coalesced wakeup pays its arrears, an early one delivers nothing, and the
 * ThreadX clock tracks real time regardless of what the display is doing.
 *
 * That matters more than it looks.  VBlank is 50 Hz PAL and 60 Hz NTSC, so a
 * stack that counts frames (as the AmiTCP lineage does, with a hardcoded 50)
 * runs 20% fast on an NTSC machine.  Under RTG the chipset VERTB is no longer
 * driving the monitor, and on PiStorm/Emu68-class systems and under emulation
 * its rate and regularity are outside our control.  The E-Clock is CIA-derived
 * and reports its own frequency, so it is right on all of them.
 *
 * The previous design -- 100 Hz on UNIT_MICROHZ, one fresh IORequest round
 * trip per tick, ticks counted rather than measured -- lost 4-5% of the clock
 * under soak load, because every re-arm paid its own scheduling latency and
 * nothing ever noticed the shortfall.
 */

/* Arm one wakeup.  */
static VOID _tx_amiga_timer_arm(struct TimeRequest *tr, ULONG secs, ULONG micro)
{

    tr -> tr_node.io_Command =  TR_ADDREQUEST;
    tr -> tr_time.tv_secs    =  secs;
    tr -> tr_time.tv_micro   =  micro;
    SendIO((struct IORequest *) tr);
}


/*
 * Measure what a wakeup source actually does, in Hz * 100.  Returns 0 for a
 * source that produced nothing at all in the window.
 *
 * `guard` is a request on a DIFFERENT unit and port, and it is what bounds the
 * measurement: the window ends when the guard completes, so a source with no
 * VERTB interrupt behind it (exactly the configuration this check exists to
 * survive) yields 0 rather than parking the tick task in Wait() forever.
 *
 * NOTHING HERE IS EVER AbortIO()ed.  The guard always completes on its own,
 * and the source's outstanding request is left pending for the caller: if the
 * source is accepted that request becomes the first tick wakeup, and if it is
 * rejected the whole request is thrown away.  That is not fastidiousness --
 * a timer.device request that has been aborted and then re-armed does not
 * complete again (measured, twice, under FS-UAE with Kickstart 3.1), and both
 * times the symptom was a ThreadX clock stuck at zero with every thread on it.
 *
 * On return `tr` HAS A REQUEST OUTSTANDING.
 */
static ULONG _tx_amiga_timer_probe(struct TimeRequest *tr, ULONG sig,
                                   struct TimeRequest *guard, ULONG guard_sig,
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
static VOID _tx_amiga_timer_discard(struct TimeRequest *tr, struct MsgPort *port)
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
struct TimeRequest  *tr;
struct TimeRequest  *guard;
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
ULONG                last_lo;
ULONG                start_lo;
ULONG                delta;
ULONG                service;
ULONG                rate_chz;
ULONG                unit;
ULONG                ticks;
ULONG                i;
UINT                 armed;


    /*
     * Wait for _tx_amiga_start_interrupts().
     *
     * Opening and validating the wakeup source AHEAD of this point was tried,
     * to overlap the 250 ms validation with the rest of kernel start-up rather
     * than delay the first tick by it.  It made the "preferred unit will not
     * open" fallback leave the clock dead (_tx_timer_system_clock stuck at 0)
     * and is reverted: the ThreadX clock starting a quarter of a second late
     * is a startup transient nobody can observe, a clock that never starts is
     * the whole machine.
     */
    Wait(SIGF_SINGLE);

    armed    =  TX_FALSE;
    rate_chz =  0UL;

    port =  CreateMsgPort();
    if (port == (struct MsgPort *) 0)
    {
        Wait(0UL);                       /* park forever; kernel has no tick */
    }

    tr =  (struct TimeRequest *) CreateIORequest(port, (ULONG) sizeof(struct TimeRequest));
    if (tr == (struct TimeRequest *) 0)
    {
        DeleteMsgPort(port);
        Wait(0UL);
    }

    port_sig =  1UL << ((ULONG) port -> mp_SigBit);

    /* MICROHZ interval for the fallback, and for the startup guard.  */
    interval_micro =  1000000UL / (ULONG) TX_TIMER_TICKS_PER_SECOND;
    interval_secs  =  interval_micro / 1000000UL;
    interval_micro =  interval_micro % 1000000UL;

    /*
     * The startup guard: a UNIT_MICROHZ request on its OWN port, so that its
     * completion is distinguishable from the source under test by signal
     * alone.  It is what stops a machine whose VBlank never fires from parking
     * the tick task forever in Wait() instead of falling back -- and if the
     * preferred unit is rejected, the guard BECOMES the tick rather than being
     * thrown away.  A machine where even this will not open has no usable
     * timer at all, and the probe simply runs unguarded.
     */
    guard      =  (struct TimeRequest *) 0;
    guard_port =  (struct MsgPort *) 0;
    guard_sig  =  0UL;

    guard_port =  CreateMsgPort();
    if (guard_port != (struct MsgPort *) 0)
    {
        guard =  (struct TimeRequest *) CreateIORequest(guard_port,
                                                        (ULONG) sizeof(struct TimeRequest));
        if (guard != (struct TimeRequest *) 0)
        {
            if (OpenDevice((CONST_STRPTR) "timer.device", (ULONG) UNIT_MICROHZ,
                           (struct IORequest *) guard, 0UL) != 0)
            {
                DeleteIORequest((APTR) guard);
                guard =  (struct TimeRequest *) 0;
            }
            else
            {
                guard_sig =  1UL << ((ULONG) guard_port -> mp_SigBit);
            }
        }
        if (guard == (struct TimeRequest *) 0)
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

        if (guard != (struct TimeRequest *) 0)
        {
            /* Hand the tick over to the guard's request, which is already
               open on UNIT_MICROHZ; see the fallback below for why the
               rejected request is thrown away rather than reopened.  */
            DeleteIORequest((APTR) tr);
            DeleteMsgPort(port);
            tr         =  guard;
            port       =  guard_port;
            port_sig   =  guard_sig;
            guard      =  (struct TimeRequest *) 0;
            guard_port =  (struct MsgPort *) 0;
            guard_sig  =  0UL;
        }
        else if (OpenDevice((CONST_STRPTR) "timer.device", (ULONG) UNIT_MICROHZ,
                            (struct IORequest *) tr, 0UL) != 0)
        {
            ami_log(AMI_LOG_ERROR, "tick: no timer.device at all; ThreadX has no clock");
            DeleteIORequest((APTR) tr);
            DeleteMsgPort(port);
            Wait(0UL);
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

    if ((unit != (ULONG) UNIT_MICROHZ) && (guard != (struct TimeRequest *) 0))
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
            guard      =  (struct TimeRequest *) 0;
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
            guard      =  (struct TimeRequest *) 0;
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
            (ULONG) TX_TIMER_TICKS_PER_SECOND * 100UL;
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

    frac =  0UL;
    ReadEClock(&now);
    last_lo  =  now.ev_lo;
    start_lo =  now.ev_lo;

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
            /* Woken by something that was not our request.  Do NOT abort it --
               an aborted timer request cannot be re-armed (see
               _tx_amiga_timer_probe) -- just go back to sleep.  */
            continue;
        }
        WaitIO((struct IORequest *) tr);
        armed =  TX_FALSE;

        /* Re-arm before doing the tick work.  On UNIT_VBLANK that reserves the
           next frame up front, so a tick that overruns costs a late delivery
           rather than a skipped frame -- and the catch-up below then makes even
           that invisible to the clock.  */
        _tx_amiga_timer_arm(tr, interval_secs, interval_micro);
        armed =  TX_TRUE;

        _tx_amiga_tick.tx_amiga_tick_wakeups++;
        _tx_amiga_tick.tx_amiga_tick_uptime_ms =
            ((ULONG) (now.ev_lo - start_lo)) / eclock_per_ms;

        delta =  (ULONG) (now.ev_lo - last_lo);   /* correct across one wrap */
        ticks =  delta / eclock_per_tick;

        if (ticks == 0UL)
        {

            /* Woke early.  A 60 Hz NTSC VBlank against a 50 Hz tick does this
               ten times a second; deliver nothing and keep the phase.  */
            _tx_amiga_tick.tx_amiga_tick_empty++;
        }
        else
        {

            if (ticks > (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP)
            {

                /* Something held the machine for longer than the cap allows.
                   Resync rather than pay the arrears off over the following
                   seconds: a burst of thousands of timer callbacks under the
                   core lock is far worse for the stack than the lost time.  */
                _tx_amiga_tick.tx_amiga_tick_lost +=
                    ticks - (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP;
                _tx_amiga_tick.tx_amiga_tick_clipped++;

                if (_tx_amiga_tick.tx_amiga_tick_clipped <= 3UL)
                {
                    ami_log(AMI_LOG_WARN,
                            "tick: stalled %ld ms, dropping %ld of %ld ticks (cap %ld)",
                            (LONG) (delta / eclock_per_ms),
                            (LONG) (ticks - (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP),
                            (LONG) ticks, (LONG) TX_AMIGA_TIMER_MAX_CATCHUP);
                }

                ticks   =  (ULONG) TX_AMIGA_TIMER_MAX_CATCHUP;
                last_lo =  now.ev_lo;
                frac    =  0UL;
            }
            else
            {

                /* Advance the phase by exactly `ticks` periods with the
                   remainder carried, so the long-run delivered rate is exactly
                   TX_TIMER_TICKS_PER_SECOND whatever the E-Clock frequency.  */
                frac    +=  ticks * eclock_rem;
                last_lo +=  (ticks * eclock_per_tick) +
                            (frac / (ULONG) TX_TIMER_TICKS_PER_SECOND);
                frac    %=  (ULONG) TX_TIMER_TICKS_PER_SECOND;

                if (ticks > 1UL)
                {
                    _tx_amiga_tick.tx_amiga_tick_catchups++;
                }
            }

            for (i = 0UL; i < ticks; i++)
            {

                /* Enter "interrupt" context, run the tick, leave it.  */
                _tx_thread_context_save();
                _tx_timer_interrupt();
                _tx_thread_context_restore();
            }
            _tx_amiga_tick.tx_amiga_tick_delivered +=  ticks;

            /* Unconditionally poke the scheduler.  Only the idle case strictly
               needs it, but one Signal() per tick is a cheap insurance policy
               against a lost wake-up hanging the whole stack.  */
            _tx_amiga_wake_scheduler();
        }

        /* Service cost of this wakeup: everything the tick task did between
           waking and going back to sleep.  Summed over tx_amiga_tick_wakeups
           this is the CPU the tick takes off the machine.  */
        {
            struct EClockVal end;

            (VOID) ReadEClock(&end);
            service =  (ULONG) (end.ev_lo - now.ev_lo);
            if (service < 4000000UL)
            {
                _tx_amiga_tick.tx_amiga_tick_service_us +=
                    (service * 1000UL) / eclock_per_ms;
            }
        }
    }

    if (_tx_amiga_tick.tx_amiga_tick_clipped != 0UL)
    {
        ami_log(AMI_LOG_WARN, "tick: %ld stalls clipped, %ld ticks dropped in total",
                (LONG) _tx_amiga_tick.tx_amiga_tick_clipped,
                (LONG) _tx_amiga_tick.tx_amiga_tick_lost);
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

    if (guard != (struct TimeRequest *) 0)
    {
        CloseDevice((struct IORequest *) guard);
        DeleteIORequest((APTR) guard);
        DeleteMsgPort(guard_port);
    }

    Forbid();
    _tx_amiga_timer_task =  (VOID *) 0;
    RemTask((struct Task *) 0);
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


/* -------------------------------------------------- library-style start -- */

static VOID _tx_amiga_kernel_task_entry(VOID)
{

    _tx_initialize_kernel_enter();       /* tx_kernel_enter(); never returns */
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

    task =  _tx_amiga_task_create("ThreadX", (BYTE) TX_AMIGA_TASK_PRIORITY,
                                  _tx_amiga_kernel_task_entry, stack, stack_size, (APTR) 0);
    if (task == (struct Task *) 0)
    {
        _tx_amiga_starter_task =  (struct Task *) 0;
        FreeMem(stack, stack_size);
        FreeSignal(sig);
        return(TX_NO_MEMORY);
    }

    Wait(_tx_amiga_starter_signal);

    _tx_amiga_starter_task =  (struct Task *) 0;
    FreeSignal(sig);

    return(_tx_amiga_start_status);
}
