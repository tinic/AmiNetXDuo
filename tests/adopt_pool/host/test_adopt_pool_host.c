/*
 * AmiNetXDuo, the adoption slot pool's identity checks, without an Amiga.
 * SPDX-License-Identifier: MIT
 *
 * port/threadx-amiga/src/tx_amiga_pool.c is compiled in whole, against the real
 * tx_amiga_internal.h.  Exec is faked below: a Task is "alive" while a flag says
 * so, and its stamp is the port's own formula over the fields the fake sets, so
 * a recycled address is the same struct Task rewritten with another stack and
 * name while the flag stays set.
 *
 *   wake   _tx_amiga_adopt_wake_scan(): the final pass signals a stamp match
 *          and clears a stamp mismatch WITHOUT signalling it
 */

#include "tx_amiga_pool.c"

#include <stdio.h>
#include <string.h>


/* ---------------------------------------------------------- fake Exec --- */

#define FAKE_TASKS      4

static struct Task      fake_task[FAKE_TASKS];
static int              fake_alive[FAKE_TASKS];
static struct Task     *fake_current;

static unsigned         signal_calls;
static struct Task     *signal_task;
static ULONG            signal_mask;

struct ExecBase        *SysBase;
volatile UINT           _tx_amiga_kernel_up;
volatile UINT           _tx_amiga_kernel_stopping;

static int fake_index(struct Task *task)
{
    int i;

    for (i = 0; i < FAKE_TASKS; i++)
    {
        if (task == &fake_task[i])
            return i;
    }
    return -1;
}

VOID  Forbid(VOID)  { }
VOID  Permit(VOID)  { }
VOID  Disable(VOID) { }
VOID  Enable(VOID)  { }

struct Task *FindTask(const char *name)
{
    (void) name;
    return fake_current;
}

VOID Signal(struct Task *task, ULONG signalSet)
{
    signal_calls++;
    signal_task = task;
    signal_mask = signalSet;
}

/* Reached only by the park path, which these tests do not take. */
ULONG Wait(ULONG signalSet)             { return signalSet; }
ULONG SetSignal(ULONG n, ULONG s)       { (void) n; (void) s; return 0; }
BYTE  AllocSignal(LONG n)               { (void) n; return -1; }
VOID  FreeSignal(LONG n)                { (void) n; }

UINT tx_amiga_task_alive_locked(struct Task *task)
{
    int i = fake_index(task);

    return (i >= 0 && fake_alive[i]) ? (UINT) TX_TRUE : (UINT) TX_FALSE;
}

/* tx_thread_interrupt_control.c's formula, less et_UniqueID. */
ULONG _tx_amiga_task_stamp(struct Task *task)
{
    ULONG stamp;

    if (task == (struct Task *) 0)
        return 0UL;

    stamp = ((ULONG) (unsigned long) task->tc_SPLower) ^
            (((ULONG) (unsigned long) task->tc_SPUpper) << 1) ^
            (((ULONG) (unsigned long) task->tc_Node.ln_Name) << 2);

    return (stamp != 0UL) ? stamp : 1UL;
}

static char fake_stack[FAKE_TASKS][2][64];
static char fake_name_a[] = "original";
static char fake_name_b[] = "recycled";

/* A Task at slot i, alive, with stack set `gen` and a name. */
static struct Task *fake_spawn(int i, int gen, char *name)
{
    struct Task *t = &fake_task[i];

    memset(t, 0, sizeof(*t));
    t->tc_Node.ln_Name = name;
    t->tc_SPLower      = &fake_stack[i][gen][0];
    t->tc_SPUpper      = &fake_stack[i][gen][63];
    fake_alive[i]      = 1;
    return t;
}

/* The Task at slot i is removed and another is created at the same address. */
static struct Task *fake_recycle(int i, ULONG sigalloc)
{
    struct Task *t = fake_spawn(i, 1, fake_name_b);

    t->tc_SigAlloc = sigalloc;
    return t;
}


/* ------------------------------------------------------------ harness --- */

static int failures;

static void expect(const char *what, unsigned long got, unsigned long want)
{
    if (got != want)
    {
        printf("FAIL %s: got %lu, want %lu\n", what, got, want);
        failures++;
    }
    else
    {
        printf("ok   %s = %lu\n", what, got);
    }
}

static void reset(void)
{
    memset(_tx_amiga_adopt_pool, 0, sizeof(_tx_amiga_adopt_pool));
    memset(_tx_amiga_adopt_waiters, 0, sizeof(_tx_amiga_adopt_waiters));
    memset(fake_task, 0, sizeof(fake_task));
    memset(fake_alive, 0, sizeof(fake_alive));
    _tx_amiga_adopt_waiting           = 0UL;
    _tx_amiga_adopt_unpublished_freed = 0UL;
    _tx_amiga_kernel_up               = TX_TRUE;
    _tx_amiga_kernel_stopping         = TX_FALSE;
    fake_current                      = (struct Task *) 0;
    signal_calls                      = 0U;
    signal_task                       = (struct Task *) 0;
    signal_mask                       = 0UL;
}

/* Register `task` as parked the way _tx_amiga_slot_claim_or_park() does. */
static void park(ULONG entry, struct Task *task, ULONG sigmask)
{
    _tx_amiga_adopt_waiters[entry].aw_task   = task;
    _tx_amiga_adopt_waiters[entry].aw_signal = sigmask;
    _tx_amiga_adopt_waiters[entry].aw_stamp  = _tx_amiga_task_stamp(task);
    _tx_amiga_adopt_waiting++;
}


/* ----------------------------------------------------------- wake scan --- */

#define WAITER_SIG  0x00010000UL

static void test_wake(void)
{
    struct Task *t;

    /* Final pass, the waiter still parked: signalled and cleared. */
    reset();
    t = fake_spawn(0, 0, fake_name_a);
    t->tc_SigAlloc = WAITER_SIG;
    park(0UL, t, WAITER_SIG);
    _tx_amiga_adopt_wake_scan((UINT) TX_TRUE);
    expect("final, stamp matches: Signal() calls", signal_calls, 1UL);
    expect("final, stamp matches: signalled the waiter",
           signal_task == t, 1UL);
    expect("final, stamp matches: on its bit", signal_mask, WAITER_SIG);
    expect("final, stamp matches: entry cleared",
           _tx_amiga_adopt_waiters[0].aw_task == (struct Task *) 0, 1UL);
    expect("final, stamp matches: waiting", _tx_amiga_adopt_waiting, 0UL);

    /* Final pass, the waiter gone and its address recycled by a live Task
       that happens to own the same bit: not signalled, and cleared. */
    reset();
    t = fake_spawn(0, 0, fake_name_a);
    t->tc_SigAlloc = WAITER_SIG;
    park(0UL, t, WAITER_SIG);
    t = fake_recycle(0, WAITER_SIG);
    _tx_amiga_adopt_wake_scan((UINT) TX_TRUE);
    expect("final, recycled address: Signal() calls", signal_calls, 0UL);
    expect("final, recycled address: entry cleared",
           _tx_amiga_adopt_waiters[0].aw_task == (struct Task *) 0, 1UL);
    expect("final, recycled address: waiting", _tx_amiga_adopt_waiting, 0UL);

    /* Both at once: only the match is signalled, and the table ends empty. */
    reset();
    t = fake_spawn(0, 0, fake_name_a);
    t->tc_SigAlloc = WAITER_SIG;
    park(0UL, t, WAITER_SIG);
    (void) fake_recycle(0, WAITER_SIG);
    t = fake_spawn(1, 0, fake_name_a);
    t->tc_SigAlloc = WAITER_SIG;
    park(1UL, t, WAITER_SIG);
    _tx_amiga_adopt_wake_scan((UINT) TX_TRUE);
    expect("final, one of each: Signal() calls", signal_calls, 1UL);
    expect("final, one of each: signalled the parked one",
           signal_task == &fake_task[1], 1UL);
    expect("final, one of each: waiting", _tx_amiga_adopt_waiting, 0UL);

    /* The ordinary pass is unchanged: a mismatch is retained, silently. */
    reset();
    t = fake_spawn(0, 0, fake_name_a);
    t->tc_SigAlloc = WAITER_SIG;
    park(0UL, t, WAITER_SIG);
    (void) fake_recycle(0, WAITER_SIG);
    _tx_amiga_adopt_wake_scan((UINT) TX_FALSE);
    expect("ordinary, recycled address: Signal() calls", signal_calls, 0UL);
    expect("ordinary, recycled address: entry retained",
           _tx_amiga_adopt_waiters[0].aw_task == &fake_task[0], 1UL);
    expect("ordinary, recycled address: waiting", _tx_amiga_adopt_waiting, 1UL);

    /* And a match is signalled and left for the waiter to clear itself. */
    reset();
    t = fake_spawn(0, 0, fake_name_a);
    t->tc_SigAlloc = WAITER_SIG;
    park(0UL, t, WAITER_SIG);
    _tx_amiga_adopt_wake_scan((UINT) TX_FALSE);
    expect("ordinary, stamp matches: Signal() calls", signal_calls, 1UL);
    expect("ordinary, stamp matches: entry retained",
           _tx_amiga_adopt_waiters[0].aw_task == t, 1UL);

    /* A removed Task is dropped by either pass, and never signalled. */
    reset();
    t = fake_spawn(0, 0, fake_name_a);
    t->tc_SigAlloc = WAITER_SIG;
    park(0UL, t, WAITER_SIG);
    fake_alive[0] = 0;
    _tx_amiga_adopt_wake_scan((UINT) TX_FALSE);
    expect("ordinary, removed: Signal() calls", signal_calls, 0UL);
    expect("ordinary, removed: waiting", _tx_amiga_adopt_waiting, 0UL);
}


int main(int argc, char **argv)
{
    const char *which = (argc > 1) ? argv[1] : "";

    if (strcmp(which, "wake") == 0)
        test_wake();
    else
    {
        printf("usage: %s wake\n", argv[0]);
        return 2;
    }

    printf("%s: %s\n", which, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
