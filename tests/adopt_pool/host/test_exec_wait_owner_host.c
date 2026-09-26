/*
 * AmiNetXDuo, which of a Task's threads an Exec-wait bracket is about.
 * SPDX-License-Identifier: MIT
 *
 * One Task, two adoptions: every OpenLibrary() base caches its own caller
 * thread, so a Task that used base A and then calls on base B has A's thread
 * DORMANT (suspended between calls) ahead of B's on the created list.  The
 * bracket must pick B.  Picking A, release found it was not current and
 * returned with B still holding the baton, and the Task then Wait()ed holding
 * it: the whole stack stalled.
 *
 * Real, compiled into this binary: port/threadx-amiga/src/tx_amiga_exec_wait.c
 * (#included below) and src/netstack/netstack_baton.c.  Faked: Exec, and
 * ThreadX's suspend/resume, which record the thread they were given.
 *
 *   release  B current: release suspends B, never A
 *   acquire  B inside the bracket, another Task's thread current: acquire
 *            resumes B, never A
 *   nested   a second release and its acquire only count, on B
 */

#include "tx_amiga_exec_wait.c"
#include "aminetxduo/compat.h"

#include <stdio.h>
#include <string.h>

VOID ami_netstack_baton_release(VOID);
VOID ami_netstack_baton_acquire(VOID);


/* ------------------------------------------------ fake ThreadX + Exec --- */

TX_THREAD      *_tx_thread_current_ptr;
TX_THREAD      *_tx_thread_execute_ptr;
TX_THREAD      *_tx_thread_created_ptr;
ULONG           _tx_thread_created_count;
volatile ULONG  _tx_thread_system_state;
ULONG           _tx_timer_time_slice;
VOID           *_tx_amiga_baton_holder_task;
struct ExecBase *SysBase;

static struct Task  fake_me, fake_other;
static struct Task *fake_current;

static TX_THREAD   *suspended[8];
static unsigned     suspend_calls;
static TX_THREAD   *resumed[8];
static unsigned     resume_calls;

UINT _tx_thread_suspend(TX_THREAD *thread_ptr)
{
    suspended[suspend_calls++ & 7U] = thread_ptr;
    thread_ptr->tx_thread_state = TX_SUSPENDED;
    return TX_SUCCESS;
}

UINT _tx_thread_resume(TX_THREAD *thread_ptr)
{
    resumed[resume_calls++ & 7U] = thread_ptr;
    thread_ptr->tx_thread_state = TX_READY;
    return TX_SUCCESS;
}

UINT _tx_amiga_dispatch_or_wake(VOID)               { return TX_FALSE; }
UINT _tx_amiga_thread_park(TX_THREAD *thread_ptr)   { (void) thread_ptr; return TX_TRUE; }
VOID _tx_amiga_wake_scheduler(VOID)                 { }

/* netstack_baton.c's other entry points, not reached by these tests. */
UINT  tx_amiga_adopted_task_dead(TX_THREAD *t)      { (void) t; return TX_FALSE; }
ULONG tx_amiga_adopt_generation(TX_THREAD *t)       { (void) t; return 0UL; }
UINT  tx_amiga_discard_thread(TX_THREAD *t, ULONG g) { (void) t; (void) g; return TX_SUCCESS; }
TX_AMIGA_TICK_STATS *tx_amiga_tick_stats_live(VOID) { return (TX_AMIGA_TICK_STATS *) 0; }
AmiMemStats *ami_mem_stats(VOID)                     { return (AmiMemStats *) 0; }

VOID  Forbid(VOID)  { }
VOID  Permit(VOID)  { }

struct Task *FindTask(const char *name)
{
    (void) name;
    return fake_current;
}

VOID InitSemaphore(struct SignalSemaphore *s)       { (void) s; }
VOID AddSemaphore(struct SignalSemaphore *s)        { (void) s; }
VOID RemSemaphore(struct SignalSemaphore *s)        { (void) s; }
struct SignalSemaphore *FindSemaphore(const UBYTE *name) { (void) name; return 0; }


/* ------------------------------------------------------------ harness --- */

static int failures;

static const char *name(const TX_THREAD *t)
{
    return (t == TX_NULL) ? "none" : t->tx_thread_name;
}

static void expect(const char *what, const TX_THREAD *got, const TX_THREAD *want)
{
    if (got != want)
    {
        printf("FAIL %s: got %s, want %s\n", what, name(got), name(want));
        failures++;
    }
    else
        printf("ok   %s\n", what);
}

static void expect_n(const char *what, unsigned long got, unsigned long want)
{
    if (got != want)
    {
        printf("FAIL %s: got %lu, want %lu\n", what, got, want);
        failures++;
    }
    else
        printf("ok   %s = %lu\n", what, got);
}

static TX_THREAD thread_a, thread_b, thread_c;

/* A before B on the created list, both the calling Task's.  A is a cached
   adoption between calls; B holds the baton.  C is another Task's thread. */
static void setup(void)
{
    memset(&thread_a, 0, sizeof(thread_a));
    memset(&thread_b, 0, sizeof(thread_b));
    memset(&thread_c, 0, sizeof(thread_c));

    thread_a.tx_thread_name = (CHAR *) "A";
    thread_b.tx_thread_name = (CHAR *) "B";
    thread_c.tx_thread_name = (CHAR *) "C";

    thread_a.tx_thread_amiga_task  = &fake_me;
    thread_a.tx_thread_state       = TX_SUSPENDED;
    thread_a.tx_thread_amiga_flags = TX_AMIGA_THREAD_ADOPTED |
                                     TX_AMIGA_THREAD_DORMANT |
                                     TX_AMIGA_THREAD_CACHED;

    thread_b.tx_thread_amiga_task  = &fake_me;
    thread_b.tx_thread_state       = TX_READY;
    thread_b.tx_thread_amiga_flags = TX_AMIGA_THREAD_ADOPTED |
                                     TX_AMIGA_THREAD_CACHED;

    thread_c.tx_thread_amiga_task  = &fake_other;
    thread_c.tx_thread_state       = TX_READY;

    thread_a.tx_thread_created_next     = &thread_b;
    thread_b.tx_thread_created_next     = &thread_c;
    thread_c.tx_thread_created_next     = &thread_a;
    thread_a.tx_thread_created_previous = &thread_c;
    thread_b.tx_thread_created_previous = &thread_a;
    thread_c.tx_thread_created_previous = &thread_b;
    _tx_thread_created_ptr   = &thread_a;
    _tx_thread_created_count = 3UL;

    _tx_thread_current_ptr = &thread_b;
    fake_current           = &fake_me;
    suspend_calls = resume_calls = 0U;
    memset(suspended, 0, sizeof(suspended));
    memset(resumed, 0, sizeof(resumed));
}

/* B's Task sits in Exec inside a bracket, and another Task's thread has the
   baton.  Set directly, so acquire is tested on its own lookup. */
static void b_in_bracket(void)
{
    thread_b.tx_thread_state                   = TX_SUSPENDED;
    thread_b.tx_thread_amiga_exec_wait_nesting = 1U;
    _tx_thread_current_ptr                     = &thread_c;
}

static void check_a_untouched(void)
{
    expect_n("A still DORMANT",
             thread_a.tx_thread_amiga_flags & TX_AMIGA_THREAD_DORMANT,
             TX_AMIGA_THREAD_DORMANT);
    expect_n("A nesting", thread_a.tx_thread_amiga_exec_wait_nesting, 0UL);
}

static void test_release(void)
{
    setup();
    Forbid();
    expect("owner is current B", tx_amiga_exec_wait_owner_locked(), &thread_b);
    Permit();

    ami_netstack_baton_release();
    expect_n("suspend calls", suspend_calls, 1UL);
    expect("suspended B", suspended[0], &thread_b);
    expect("baton let go", _tx_thread_current_ptr, TX_NULL);
    expect_n("B nesting", thread_b.tx_thread_amiga_exec_wait_nesting, 1UL);
    check_a_untouched();
}

static void test_acquire(void)
{
    setup();
    b_in_bracket();

    Forbid();
    expect("owner is bracketed B", tx_amiga_exec_wait_owner_locked(), &thread_b);
    Permit();

    ami_netstack_baton_acquire();
    expect_n("resume calls", resume_calls, 1UL);
    expect("resumed B", resumed[0], &thread_b);
    expect_n("B nesting", thread_b.tx_thread_amiga_exec_wait_nesting, 0UL);
    check_a_untouched();
}

static void test_nested(void)
{
    setup();
    b_in_bracket();

    Forbid();
    expect("nested owner is B", tx_amiga_exec_wait_owner_locked(), &thread_b);
    Permit();

    ami_netstack_baton_release();
    expect_n("inner release: suspend calls", suspend_calls, 0UL);
    expect_n("inner release: B nesting",
             thread_b.tx_thread_amiga_exec_wait_nesting, 2UL);

    ami_netstack_baton_acquire();
    expect_n("inner acquire: resume calls", resume_calls, 0UL);
    expect_n("inner acquire: B nesting",
             thread_b.tx_thread_amiga_exec_wait_nesting, 1UL);

    ami_netstack_baton_acquire();
    expect_n("outer acquire: resume calls", resume_calls, 1UL);
    expect("outer acquire: resumed B", resumed[0], &thread_b);
    check_a_untouched();
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s release|acquire|nested\n", argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "release") == 0)
        test_release();
    else if (strcmp(argv[1], "acquire") == 0)
        test_acquire();
    else if (strcmp(argv[1], "nested") == 0)
        test_nested();
    else
    {
        fprintf(stderr, "unknown test: %s\n", argv[1]);
        return 2;
    }

    printf("%s: %d failure(s)\n", argv[1], failures);
    return failures == 0 ? 0 : 1;
}
