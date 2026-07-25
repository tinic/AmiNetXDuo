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
/*    entry does immediately before calling _tx_thread_schedule().         */
/*                                                                        */
/**************************************************************************/

#define TX_SOURCE_CODE

#include "tx_amiga_internal.h"

#include <devices/timer.h>


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
 */
static VOID _tx_amiga_timer_task_entry(VOID)
{

struct MsgPort      *port;
struct TimeRequest  *tr;
ULONG                port_sig;
ULONG                interval_secs;
ULONG                interval_micro;


    /* Wait for _tx_amiga_start_interrupts().  */
    Wait(SIGF_SINGLE);

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

    if (OpenDevice((CONST_STRPTR) "timer.device", (ULONG) TX_AMIGA_TIMER_UNIT,
                   (struct IORequest *) tr, 0UL) != 0)
    {
        DeleteIORequest((APTR) tr);
        DeleteMsgPort(port);
        Wait(0UL);
    }

    port_sig =  1UL << ((ULONG) port -> mp_SigBit);

    interval_secs  =  1UL / (ULONG) TX_TIMER_TICKS_PER_SECOND;
    interval_micro =  1000000UL / (ULONG) TX_TIMER_TICKS_PER_SECOND;
    if (interval_micro >= 1000000UL)
    {
        interval_secs  =  interval_micro / 1000000UL;
        interval_micro =  interval_micro % 1000000UL;
    }

    while (_tx_amiga_timer_stop == TX_FALSE)
    {

        tr -> tr_node.io_Command =  TR_ADDREQUEST;
        tr -> tr_time.tv_secs    =  interval_secs;
        tr -> tr_time.tv_micro   =  interval_micro;
        SendIO((struct IORequest *) tr);

        Wait(port_sig | SIGF_SINGLE);

        if (CheckIO((struct IORequest *) tr) == (struct IORequest *) 0)
        {
            AbortIO((struct IORequest *) tr);
        }
        WaitIO((struct IORequest *) tr);

        if (_tx_amiga_timer_stop != TX_FALSE)
        {
            break;
        }

        /* Enter "interrupt" context, run the tick, leave it.  */
        _tx_thread_context_save();
        _tx_timer_interrupt();
        _tx_thread_context_restore();

        /* Unconditionally poke the scheduler.  Only the idle case strictly
           needs it, but one Signal() per tick is a cheap insurance policy
           against a lost wake-up hanging the whole stack.  */
        _tx_amiga_wake_scheduler();
    }

    CloseDevice((struct IORequest *) tr);
    DeleteIORequest((APTR) tr);
    DeleteMsgPort(port);

    Forbid();
    _tx_amiga_timer_task =  (VOID *) 0;
    RemTask((struct Task *) 0);
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
