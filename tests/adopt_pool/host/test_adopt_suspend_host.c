/*
 * AmiNetXDuo, tx_amiga_adopt_suspend()'s run-signal clear, without an Amiga.
 * SPDX-License-Identifier: MIT
 *
 * port/threadx-amiga/src/tx_amiga_adopt.c is compiled in whole.  Every send,
 * recv and WaitSelect leaves through tx_amiga_adopt_suspend(), and SetSignal()
 * is a Disable()/Enable() pair: on Emu68 that is two chip-bus INTENA writes.
 *
 *   clean    nothing latched on the run signal: no SetSignal() at all
 *   latched  the run signal latched: one SetSignal(0, run signal), which
 *            clears it and nothing else
 */

#include "tx_amiga_adopt.c"

#include <stdio.h>
#include <string.h>


#define RUN_SIG     (1UL << 20)
#define OTHER_SIG   (1UL << 12)

static struct Task      fake_task;
static unsigned         setsignal_calls;
static unsigned         failures;

/* The globals, typed as the headers declare them (the host shim widens ULONG). */
__typeof__(SysBase) SysBase;
__typeof__(_tx_amiga_kernel_up) _tx_amiga_kernel_up;
__typeof__(_tx_amiga_kernel_stopping) _tx_amiga_kernel_stopping;
__typeof__(_tx_amiga_baton_holder_task) _tx_amiga_baton_holder_task;
__typeof__(_tx_amiga_adopt_task) _tx_amiga_adopt_task;
__typeof__(_tx_amiga_adopt_signal) _tx_amiga_adopt_signal;
__typeof__(_tx_amiga_adopt_waiting) _tx_amiga_adopt_waiting;
__typeof__(_tx_thread_current_ptr) _tx_thread_current_ptr;
__typeof__(_tx_thread_execute_ptr) _tx_thread_execute_ptr;
__typeof__(_tx_thread_created_ptr) _tx_thread_created_ptr;
__typeof__(_tx_thread_created_count) _tx_thread_created_count;
__typeof__(_tx_thread_system_state) _tx_thread_system_state;
__typeof__(_tx_thread_preempt_disable) _tx_thread_preempt_disable;
__typeof__(_tx_timer_time_slice) _tx_timer_time_slice;

static struct _tx_amiga_adopt_slot fake_slot;

VOID  Forbid(VOID)  { }
VOID  Permit(VOID)  { }
struct Task *FindTask(const char *name) { (void) name; return &fake_task; }
VOID  Signal(struct Task *task, ULONG s) { task->tc_SigRecvd |= s; }
ULONG Wait(ULONG s)                     { return s; }
BYTE  AllocSignal(LONG n)               { (void) n; return -1; }
VOID  FreeSignal(LONG n)                { (void) n; }

ULONG SetSignal(ULONG n, ULONG s)
{
    ULONG old = fake_task.tc_SigRecvd;

    setsignal_calls++;
    fake_task.tc_SigRecvd = (old & ~s) | (n & s);
    return old;
}

struct _tx_amiga_adopt_slot *_tx_amiga_slot_held(TX_THREAD *t, ULONG g)
{
    (void) t; (void) g;
    return &fake_slot;
}

/* Off the suspend path. */
struct _tx_amiga_adopt_slot *_tx_amiga_slot_claim_or_park(struct Task *me, UINT r)
{ (void) me; (void) r; return 0; }
VOID  _tx_amiga_slot_publish_locked(struct _tx_amiga_adopt_slot *s) { (void) s; }
VOID  _tx_amiga_slot_release_locked(struct _tx_amiga_adopt_slot *s) { (void) s; }
VOID  _tx_amiga_adopt_wake_waiters_locked(VOID) { }
VOID  _tx_amiga_wake_scheduler(VOID) { }
UINT  _tx_amiga_thread_park(TX_THREAD *t) { (void) t; return TX_SUCCESS; }
BYTE  _tx_amiga_sigbit(ULONG m) { (void) m; return -1; }
ULONG _tx_amiga_task_stamp(struct Task *t) { (void) t; return 1UL; }
UINT  _tx_thread_create(TX_THREAD *t, CHAR *n, VOID (*e)(ULONG), ULONG i,
                        VOID *s, ULONG z, UINT p, UINT pt, ULONG ts, UINT a)
{ (void) t; (void) n; (void) e; (void) i; (void) s; (void) z; (void) p;
  (void) pt; (void) ts; (void) a; return TX_SUCCESS; }
UINT  _tx_thread_delete(TX_THREAD *t)    { (void) t; return TX_SUCCESS; }
UINT  _tx_thread_terminate(TX_THREAD *t) { (void) t; return TX_SUCCESS; }
UINT  _tx_thread_resume(TX_THREAD *t)    { (void) t; return TX_SUCCESS; }
UINT  _tx_thread_suspend(TX_THREAD *t)   { (void) t; return TX_SUCCESS; }

static TX_THREAD thread;

#define CHECK(c, what) \
    do { if (!(c)) { printf("  FAIL %s\n", what); failures++; } } while (0)

/* An adopted thread holding the baton, with `pending` on its Task. */
static UINT suspend_with(ULONG pending)
{
    memset(&fake_task, 0, sizeof(fake_task));
    memset(&thread, 0, sizeof(thread));
    thread.tx_thread_id               = TX_THREAD_ID;
    thread.tx_thread_amiga_task       = &fake_task;
    thread.tx_thread_amiga_flags      = TX_AMIGA_THREAD_ADOPTED;
    thread.tx_thread_amiga_run_signal = RUN_SIG;
    _tx_thread_current_ptr            = &thread;
    _tx_thread_execute_ptr            = TX_NULL;
    _tx_thread_system_state           = 0;
    fake_task.tc_SigRecvd             = pending;
    setsignal_calls                   = 0;

    return tx_amiga_adopt_suspend(&thread, 1UL);
}

static void test_clean(void)
{
    CHECK(suspend_with(0UL) == TX_SUCCESS, "suspend succeeds");
    CHECK(setsignal_calls == 0, "nothing latched: no SetSignal()");

    CHECK(suspend_with(OTHER_SIG) == TX_SUCCESS, "suspend succeeds");
    CHECK(setsignal_calls == 0, "another bit latched: no SetSignal()");
    CHECK(fake_task.tc_SigRecvd == OTHER_SIG, "and that bit is untouched");
}

static void test_latched(void)
{
    CHECK(suspend_with(RUN_SIG | OTHER_SIG) == TX_SUCCESS, "suspend succeeds");
    CHECK(setsignal_calls == 1, "run signal latched: one SetSignal()");
    CHECK(fake_task.tc_SigRecvd == OTHER_SIG,
          "it clears the run signal and nothing else");
}

int main(int argc, char **argv)
{
    const char *which = (argc > 1) ? argv[1] : "";

    if (strcmp(which, "clean") == 0)
        test_clean();
    else if (strcmp(which, "latched") == 0)
        test_latched();
    else
    {
        printf("usage: %s clean|latched\n", argv[0]);
        return 2;
    }

    printf("%s: %s\n", which, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
