/*
 * AmiNetXDuo, milestone 1 exit-criterion soak test.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_amiga.h"

#include "aminetxduo/compat.h"
#include "aminetxduo/crashguard.h"

#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>


extern TX_THREAD       *_tx_thread_current_ptr;
extern TX_THREAD       *_tx_thread_execute_ptr;
extern volatile ULONG   _tx_thread_system_state;
extern TX_THREAD       *_tx_thread_created_ptr;
extern ULONG            _tx_thread_created_count;

extern volatile UINT    _tx_amiga_timer_stop;

#define S_CURRENT   (*(TX_THREAD * volatile *) &_tx_thread_current_ptr)
#define S_EXECUTE   (*(TX_THREAD * volatile *) &_tx_thread_execute_ptr)


#define S_TPS               ((ULONG) TX_TIMER_TICKS_PER_SECOND)

#ifndef S_SOAK_SECONDS
#define S_SOAK_SECONDS      30UL
#endif
#define S_SOAK_TICKS        (S_SOAK_SECONDS * S_TPS)

#define S_PHASE_ABORT_AT    (S_SOAK_TICKS / 4UL)
#define S_PHASE_SUSPEND_AT  (S_SOAK_TICKS / 2UL)

#ifndef S_SKIP_ABORT
#define S_SKIP_ABORT        0
#endif
#ifndef S_SKIP_SUSPEND
#define S_SKIP_SUSPEND      0
#endif
#ifndef S_SKIP_DELETE
#define S_SKIP_DELETE       0
#endif
#ifndef S_SKIP_CHURN
#define S_SKIP_CHURN        0           /* bisecting: no adopt/orphan churn  */
#endif
#ifndef S_SKIP_TASKS
#define S_SKIP_TASKS        0           /* bisecting: no raw AddTask workers */
#endif
#ifndef S_NO_REMTASK
#define S_NO_REMTASK        1
#endif

#define S_MUTEX_WAIT        (1UL * S_TPS)   /* never TX_WAIT_FOREVER in the   */

#define S_HOG_US            60000UL         /* baton hog window, microseconds */
#define S_HOG_PERIOD_TICKS  (4UL * S_TPS)   /* hog on 4 s, off 4 s            */

#define S_PROBE_SLEEP       5UL             /* latency probe sleep, ticks     */

#define S_TIMER_TICKS       3UL             /* TX_TIMER period                */

#define S_MIN_ITERS         50UL            /* starvation floor per worker    */
#define S_MIN_ITERS_FLOOR   10UL            /* ...and the floor under that,
                                               for a machine slow enough that
                                               the scaled one would vanish   */
#define S_MIN_CHURN         50UL            /* adopt/orphan cycles per churner*/

#define S_WORKERS           6U              /* 4 adopted + 2 ThreadX-created  */
#define S_CHURNERS          2U

#define S_WORKER_STACK      8192UL
#define S_TX_STACK          4096UL

#ifndef S_PROC_STACK
#define S_PROC_STACK        8192UL
#endif


#define S_LOG(...)      ami_log(AMI_LOG_WARN,  __VA_ARGS__)
#define S_ERR(...)      ami_log(AMI_LOG_ERROR, __VA_ARGS__)


static volatile ULONG   s_checks;
static volatile ULONG   s_failures;

static UINT s_check(UINT ok, const char *what, ULONG detail)
{

    Forbid();
    s_checks++;
    if (!ok)
    {
        s_failures++;
    }
    Permit();

    if (ok)
    {
        S_LOG("  ok   %s", what);
    }
    else
    {
        S_ERR("  FAIL %s (%ld / 0x%lx)", what, detail, detail);
    }

    return(ok);
}

#define S_CHECK(cond, what, detail)     s_check((UINT) ((cond) ? 1U : 0U), (what), (ULONG) (detail))

static UINT s_tx_ok(UINT status, const char *what)
{

    return(s_check((UINT) ((status == TX_SUCCESS) ? 1U : 0U), what, (ULONG) status));
}

#define S_TX_OK(status, what)           s_tx_ok((UINT) (status), (what))


enum
{
    V_IDENTITY = 0,         /* tx_thread_identify() is not us                 */
    V_IDENTITY_TASK,        /* our TX_THREAD is bound to another Exec Task    */
    V_CURRENT,              /* we are running but do not hold the baton       */
    V_EXCL,                 /* two contexts inside the mutex at once          */
    V_MUTEX_OWNER,          /* mutex owner is not the holder                  */
    V_MUTEX_COUNT,          /* ownership count wrong                          */
    V_MUTEX_NEST,           /* recursive get of an owned mutex failed         */
    V_MUTEX_STATUS,         /* unexpected status from a mutex service         */
    V_SLEEP_STATUS,         /* unexpected status from tx_thread_sleep()       */
    V_TICK_BACK,            /* tx_time_get() went backwards                   */
    V_PRIO_INHERIT,         /* holder not boosted above a waiting thread      */
    V_ADOPT,                /* tx_amiga_adopt_thread() failed                 */
    V_ORPHAN,               /* tx_amiga_orphan_thread() failed                */
    V_ADOPTED_LOOKUP,       /* tx_amiga_adopted_thread() disagrees            */
    V_BATON_AFTER_ORPHAN,   /* orphaned thread still holds the baton          */
    V_STUCK_BATON,          /* baton held by a Task that is not running       */
    V_TICK_STALL,           /* clock did not advance across a sample window   */
    V_COUNT
};

static const char *const s_viol_name[V_COUNT] =
{
    "identity: tx_thread_identify() matched the running thread",
    "identity: TX_THREAD stayed bound to its own Exec Task",
    "baton: running thread was always _tx_thread_current_ptr",
    "mutex: mutual exclusion never broken",
    "mutex: owner always the holder",
    "mutex: ownership count always correct",
    "mutex: recursive get always succeeded",
    "mutex: no unexpected service status",
    "sleep: no unexpected service status",
    "tick: never went backwards",
    "priority: holder always outranked its waiters (inheritance)",
    "adopt: tx_amiga_adopt_thread() never failed",
    "orphan: tx_amiga_orphan_thread() never failed",
    "adopt: tx_amiga_adopted_thread() always agreed",
    "orphan: baton never left with an orphaned thread",
    "watchdog: baton never held by a task that was not running",
    "watchdog: tick never stalled"
};

static volatile ULONG   s_viol[V_COUNT];

static VOID s_viol_hit(UINT which)
{

    Forbid();
    s_viol[which]++;
    Permit();
}


static ULONG    s_eclock_khz;

static ULONG s_eclock(VOID)
{

struct EClockVal    ev;


    if (TimerBase == (struct Device *) 0)
    {
        return(0UL);
    }
    (VOID) ReadEClock(&ev);
    return(ev.ev_lo);
}

/* E-Clock ticks -> microseconds.  Good for deltas up to about 5 seconds.  */
static ULONG s_us(ULONG delta)
{

    if (s_eclock_khz == 0UL)
    {
        return(0UL);
    }
    if (delta > 4000000UL)
    {
        return(0xFFFFFFFFUL);
    }
    return((delta * 1000UL) / s_eclock_khz);
}

/* Burn CPU without calling anything ThreadX knows about.  */
static VOID s_spin_us(ULONG us)
{

ULONG   start;
ULONG   want;


    if ((s_eclock_khz == 0UL) || (us == 0UL))
    {
        return;
    }

    start =  s_eclock();
    want  =  (us * s_eclock_khz) / 1000UL;

    while ((s_eclock() - start) < want)
    {
    }
}


static VOID s_newlist(struct List *list)
{

    list -> lh_Head     =  (struct Node *) &list -> lh_Tail;
    list -> lh_Tail     =  (struct Node *) 0;
    list -> lh_TailPred =  (struct Node *) &list -> lh_Head;
}


static struct Task *s_spawn_task(const char *name, BYTE pri, VOID (*entry)(VOID),
                                 APTR stack, ULONG stack_size, APTR user,
                                 APTR *block_out, ULONG *block_size_out)
{

struct MemList  *memlist;
struct Task     *task;
ULONG            block_size;


    block_size =  (ULONG) sizeof(struct Task);

    memlist =  (struct MemList *) AllocMem((ULONG) sizeof(struct MemList),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (memlist == (struct MemList *) 0)
    {
        return((struct Task *) 0);
    }

    task =  (struct Task *) AllocMem(block_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (task == (struct Task *) 0)
    {
        FreeMem((APTR) memlist, (ULONG) sizeof(struct MemList));
        return((struct Task *) 0);
    }

    memlist -> ml_NumEntries      =  1;
    memlist -> ml_ME[0].me_Addr   =  (APTR) task;
    memlist -> ml_ME[0].me_Length =  block_size;

    task -> tc_Node.ln_Type =  NT_TASK;
    task -> tc_Node.ln_Pri  =  pri;
    task -> tc_Node.ln_Name =  (char *) name;
    task -> tc_SPLower      =  stack;
    task -> tc_SPUpper      =  (APTR) (((UBYTE *) stack) + stack_size);
    task -> tc_SPReg        =  task -> tc_SPUpper;
    task -> tc_UserData     =  user;

    s_newlist(&task -> tc_MemEntry);
    AddTail(&task -> tc_MemEntry, (struct Node *) memlist);

    if (AddTask(task, (APTR) entry, (APTR) 0) == (APTR) 0)
    {
        /* Never added, so RemTask() will not free these for us. */
        FreeMem((APTR) task, block_size);
        FreeMem((APTR) memlist, (ULONG) sizeof(struct MemList));
        return((struct Task *) 0);
    }

    if (block_out != (APTR *) 0)
    {
        *block_out =  (APTR) task;
    }
    if (block_size_out != (ULONG *) 0)
    {
        *block_size_out =  block_size;
    }

    return(task);
}


static struct Process *s_spawn_proc(const char *name, LONG pri, VOID (*entry)(VOID),
                                    ULONG stack_size, APTR user)
{

struct Process  *proc;


    Forbid();

    proc =  CreateNewProcTags(NP_Entry,     (ULONG) entry,
                              NP_Name,      (ULONG) name,
                              NP_Priority,  (ULONG) pri,
                              NP_StackSize, (ULONG) stack_size,
                              NP_Cli,       (ULONG) FALSE,
                              TAG_DONE);

    if (proc != (struct Process *) 0)
    {
        proc -> pr_Task.tc_UserData =  user;
    }

    Permit();

    return(proc);
}


#define S_KIND_TASK     0U      /* AddTask() + adopt      */
#define S_KIND_PROC     1U      /* CreateNewProc() + adopt*/
#define S_KIND_TX       2U      /* tx_thread_create()     */

struct s_worker
{
    TX_THREAD       thread;
    const char     *name;
    ULONG           index;
    UINT            kind;
    UINT            priority;
    UINT            hog;            /* runs the deferred-preemption spin      */
    UINT            hold_over_sleep;/* holds the mutex across tx_thread_sleep */
    volatile ULONG  started;
    volatile ULONG  finished;
    volatile ULONG  iters;
    volatile ULONG  mutex_ops;
    volatile ULONG  mutex_timeouts;
    volatile ULONG  inherit_seen;
    volatile ULONG  flag_gets;
    ULONG           last_tick;
    struct Task    *task;
    APTR            memblock;       /* what s_spawn_task() AllocMem'd        */
    ULONG           memblock_size;
};

struct s_churner
{
    TX_THREAD       thread;
    const char     *name;
    ULONG           index;
    UINT            priority;
    volatile ULONG  cycles;
    volatile ULONG  inherit_seen;
    volatile ULONG  fast_path;      /* baton was free when we adopted         */
    volatile ULONG  slow_path;      /* somebody held it: scheduler round trip */
    volatile ULONG  adopt_us_max;
    volatile ULONG  adopt_us_sum;
    volatile ULONG  adopt_fast_us_max;
    volatile ULONG  finished;
    struct Process *proc;
};

static struct s_worker      s_worker[S_WORKERS];
static struct s_churner     s_churner[S_CHURNERS];

static const struct
{
    const char *name;
    UINT        kind;
    UINT        priority;
    UINT        hog;
    UINT        hold_over_sleep;
}
s_worker_cfg[S_WORKERS] =
{
    { "soak-w0-task",  S_KIND_TASK, 16U, 1U, 0U },
    { "soak-w1-task",  S_KIND_TASK, 18U, 0U, 1U },
    { "soak-w2-proc",  S_KIND_PROC, 20U, 0U, 0U },
    { "soak-w3-proc",  S_KIND_PROC, 22U, 0U, 1U },
    { "soak-w4-tx",    S_KIND_TX,   17U, 0U, 1U },
    { "soak-w5-tx",    S_KIND_TX,   19U, 0U, 0U }
};

static TX_MUTEX             s_mutex;         /* the contended one (TX_INHERIT) */
static TX_MUTEX             s_mutex2;        /* poked from the timer           */
static TX_MUTEX             s_pt_mutex;      /* private to the threshold phase */
static TX_TIMER             s_timer;
static TX_SEMAPHORE         s_never;         /* nobody ever puts this          */
static TX_SEMAPHORE         s_probe_sem;
static TX_EVENT_FLAGS_GROUP s_flags;

static TX_THREAD            s_main_thread;   /* main(), adopted                */
static TX_THREAD            s_probe_thread;
static TX_THREAD            s_pt_thread;
static TX_THREAD            s_pt_victim;

static ULONG    s_worker_stack[S_WORKERS][S_WORKER_STACK / sizeof(ULONG)];
static ULONG    s_probe_stack[S_TX_STACK / sizeof(ULONG)];
static ULONG    s_pt_stack[S_TX_STACK / sizeof(ULONG)];
static ULONG    s_victim_stack[S_TX_STACK / sizeof(ULONG)];

static volatile ULONG   s_stop;
static volatile ULONG   s_start_tick;
static volatile ULONG   s_cs_inside;        /* contexts inside the mutex       */
static volatile ULONG   s_cs_unsafe;        /* bumped non-atomically inside it */
static volatile ULONG   s_cs_safe;          /* bumped under Forbid inside it   */
static volatile ULONG   s_adopt_total;
static volatile ULONG   s_adopted_live;
static volatile ULONG   s_adopted_peak;
static volatile ULONG   s_timer_fires;
static volatile ULONG   s_timer_mutex_gets;
static volatile ULONG   s_tasks_exited;

static volatile ULONG   s_abort_request;     /* worker index + 1               */
static volatile ULONG   s_abort_ready;
static volatile ULONG   s_abort_done;
static volatile ULONG   s_abort_seen;        /* times the victim took the branch */
static volatile UINT    s_abort_status;

static volatile ULONG   s_pt_flag;
static volatile ULONG   s_pt_done;

static volatile ULONG   s_probe_samples[2];      /* [0] = idle, [1] = hogging  */
static volatile ULONG   s_probe_late_ticks[2];   /* summed                     */
static volatile ULONG   s_probe_late_max[2];     /* ticks                      */
static volatile ULONG   s_probe_late_us_max[2];

static volatile ULONG   s_watchdog_stop;
static volatile ULONG   s_watchdog_samples;

/* The starvation floor, scaled at the end from the work this run did; see
   the check itself for why it cannot be a constant. */
static ULONG            s_starve_floor;
static volatile ULONG   s_watchdog_done;
static volatile ULONG   s_wedge_dumps;


static ULONG s_hog_window(VOID)
{

ULONG   elapsed;


    if ((s_start_tick == 0UL) || (s_stop != 0UL))
    {
        return(0UL);
    }
    elapsed =  tx_time_get() - s_start_tick;
    return((elapsed / S_HOG_PERIOD_TICKS) & 1UL);
}


struct s_snap
{
    TX_THREAD      *tp;
    CHAR           *name;
    UINT            state;
    UINT            priority;
    ULONG           runs;
    struct Task    *task;
    UBYTE           tstate;
    ULONG           sigs;
    ULONG           runsig;
};

static VOID s_dump_state(const char *why)
{

struct s_snap   snap[16];
TX_THREAD      *tp;
TX_THREAD      *cur;
TX_THREAD      *exe;
ULONG           state;
ULONG           count;
ULONG           i;
ULONG           n;


    Forbid();

    cur   =  _tx_thread_current_ptr;
    exe   =  _tx_thread_execute_ptr;
    state =  _tx_thread_system_state;
    count =  _tx_thread_created_count;
    tp    =  _tx_thread_created_ptr;

    n =  0UL;
    for (i = 0UL; (i < count) && (n < 16UL) && (tp != TX_NULL); i++)
    {
        snap[n].tp       =  tp;
        snap[n].name     =  tp -> tx_thread_name;
        snap[n].state    =  tp -> tx_thread_state;
        snap[n].priority =  tp -> tx_thread_priority;
        snap[n].runs     =  tp -> tx_thread_run_count;
        snap[n].task     =  (struct Task *) tp -> tx_thread_amiga_task;
        snap[n].tstate   =  (snap[n].task != (struct Task *) 0) ? snap[n].task -> tc_State : (UBYTE) 0;
        snap[n].sigs     =  (snap[n].task != (struct Task *) 0) ? snap[n].task -> tc_SigRecvd : 0UL;
        snap[n].runsig   =  tp -> tx_thread_amiga_run_signal;
        n++;
        tp =  tp -> tx_thread_created_next;
    }

    Permit();

    S_ERR("DUMP (%s): current=0x%lx execute=0x%lx system_state=%ld threads=%ld",
          why, (ULONG) cur, (ULONG) exe, state, count);
    S_ERR("DUMP: tick=%ld cs_inside=%ld mutex owner=0x%lx count=%ld suspended=%ld",
          tx_time_get(), s_cs_inside, (ULONG) s_mutex.tx_mutex_owner,
          (ULONG) s_mutex.tx_mutex_ownership_count,
          (ULONG) s_mutex.tx_mutex_suspended_count);

    for (i = 0UL; i < n; i++)
    {
        S_ERR("DUMP: %s tx=0x%lx st=%ld pri=%ld runs=%ld task=0x%lx tc_State=%ld sig=0x%lx run=0x%lx",
              (snap[i].name != (CHAR *) 0) ? snap[i].name : "(unnamed)",
              (ULONG) snap[i].tp, (ULONG) snap[i].state, (ULONG) snap[i].priority,
              snap[i].runs, (ULONG) snap[i].task, (ULONG) snap[i].tstate,
              snap[i].sigs, snap[i].runsig);
    }
}


static VOID s_critical_section(TX_THREAD *self, ULONG iter, UINT hold_over_sleep,
                               volatile ULONG *inherit_seen)
{

CHAR       *name;
ULONG       count;
TX_THREAD  *owner;
TX_THREAD  *first;
ULONG       suspended;
TX_MUTEX   *next;
ULONG       n;
ULONG       v;
UINT        mine;
UINT        waiter;
UINT        user_pri;


    Forbid();
    n =  ++s_cs_inside;
    Permit();

    if (n != 1UL)
    {
        s_viol_hit(V_EXCL);
    }

    /* Deliberately racy read-modify-write; only the mutex makes it safe.  */
    v =  s_cs_unsafe;
    s_spin_us(40UL);
    s_cs_unsafe =  v + 1UL;

    Forbid();
    s_cs_safe++;
    Permit();

    if (tx_mutex_info_get(&s_mutex, &name, &count, &owner, &first, &suspended, &next) != TX_SUCCESS)
    {
        s_viol_hit(V_MUTEX_STATUS);
    }
    else
    {
        if (owner != self)
        {
            s_viol_hit(V_MUTEX_OWNER);
        }
        if (count != 1UL)
        {
            s_viol_hit(V_MUTEX_COUNT);
        }
    }

    if (tx_mutex_get(&s_mutex, TX_NO_WAIT) == TX_SUCCESS)
    {
        if (tx_mutex_info_get(&s_mutex, &name, &count, &owner, &first, &suspended, &next) == TX_SUCCESS)
        {
            if (count != 2UL)
            {
                s_viol_hit(V_MUTEX_COUNT);
            }
        }
        if (tx_mutex_put(&s_mutex) != TX_SUCCESS)
        {
            s_viol_hit(V_MUTEX_STATUS);
        }
    }
    else
    {
        s_viol_hit(V_MUTEX_NEST);
    }

    if ((hold_over_sleep != 0U) && ((iter & 7UL) == 3UL))
    {
        if (tx_thread_sleep(2UL) != TX_SUCCESS)
        {
            s_viol_hit(V_SLEEP_STATUS);
        }

        Forbid();
        n =  s_cs_inside;
        Permit();
        if (n != 1UL)
        {
            s_viol_hit(V_EXCL);
        }
    }

    Forbid();
    suspended =  (ULONG) s_mutex.tx_mutex_suspended_count;
    first     =  s_mutex.tx_mutex_suspension_list;
    waiter    =  (first != TX_NULL) ? first -> tx_thread_priority : (UINT) TX_MAX_PRIORITIES;
    mine      =  self -> tx_thread_priority;
    user_pri  =  self -> tx_thread_user_priority;
    Permit();

    if ((suspended != 0UL) && (first != TX_NULL))
    {
        if (mine > waiter)
        {
            s_viol_hit(V_PRIO_INHERIT);
        }
        if (mine < user_pri)
        {
            Forbid();
            (*inherit_seen)++;
            Permit();
        }
    }

    Forbid();
    s_cs_inside--;
    Permit();
}


static VOID s_wait_abort_victim(struct s_worker *w)
{

UINT    status;


    Forbid();
    s_abort_seen++;
    s_abort_ready =  1UL;
    Permit();

    /* Suspend forever on something nobody will ever post.  The coordinator
       breaks us out with tx_thread_wait_abort(), the mechanism WaitSelect()
       needs in order to honour an Exec break signal.  */
    status =  tx_semaphore_get(&s_never, TX_WAIT_FOREVER);

    Forbid();
    s_abort_status  =  status;
    s_abort_ready   =  0UL;
    s_abort_request =  0UL;
    s_abort_done    =  1UL;
    Permit();

    if (tx_thread_identify() != &w -> thread)
    {
        s_viol_hit(V_IDENTITY);
    }
}


static VOID s_worker_body(struct s_worker *w)
{

TX_THREAD  *me;
ULONG       t;
UINT        status;
ULONG       actual;


    while (s_stop == 0UL)
    {

        w -> iters++;

        if (s_abort_request == (w -> index + 1UL))
        {
            s_wait_abort_victim(w);
        }

        me =  tx_thread_identify();
        if (me != &w -> thread)
        {
            s_viol_hit(V_IDENTITY);
        }
        else if (me -> tx_thread_amiga_task != (VOID *) FindTask((STRPTR) 0))
        {
            s_viol_hit(V_IDENTITY_TASK);
        }

        if (S_CURRENT != &w -> thread)
        {
            s_viol_hit(V_CURRENT);
        }

        t =  tx_time_get();
        if (t < w -> last_tick)
        {
            s_viol_hit(V_TICK_BACK);
        }
        w -> last_tick =  t;

        status =  tx_mutex_get(&s_mutex, S_MUTEX_WAIT);
        if (status == TX_SUCCESS)
        {
            s_critical_section(&w -> thread, w -> iters, w -> hold_over_sleep,
                               &w -> inherit_seen);
            if (tx_mutex_put(&s_mutex) != TX_SUCCESS)
            {
                s_viol_hit(V_MUTEX_STATUS);
            }
            w -> mutex_ops++;
        }
        else if (status == TX_NOT_AVAILABLE)
        {
            w -> mutex_timeouts++;
        }
        else
        {
            s_viol_hit(V_MUTEX_STATUS);
        }

        if ((w -> iters & 7UL) == 5UL)
        {
            if (tx_event_flags_get(&s_flags, 1UL, TX_OR_CLEAR, &actual,
                                   S_TPS / 2UL) == TX_SUCCESS)
            {
                w -> flag_gets++;
            }
        }

        if ((w -> hog != 0U) && (s_hog_window() != 0UL))
        {
            s_spin_us(S_HOG_US);
        }

        status =  tx_thread_sleep(1UL + (w -> index & 3UL));
        if (status != TX_SUCCESS)
        {
            s_viol_hit(V_SLEEP_STATUS);
        }

    }
}


static VOID s_worker_run(struct s_worker *w)
{

UINT    status;
ULONG   live;


    status =  tx_amiga_adopt_thread(&w -> thread, (CHAR *) w -> name, w -> priority);
    if (status != TX_SUCCESS)
    {
        s_viol_hit(V_ADOPT);
        Forbid();
        w -> finished =  1UL;
        Permit();
        return;
    }

    Forbid();
    s_adopt_total++;
    live =  ++s_adopted_live;
    if (live > s_adopted_peak)
    {
        s_adopted_peak =  live;
    }
    w -> started =  1UL;
    Permit();

    if (tx_amiga_adopted_thread() != &w -> thread)
    {
        s_viol_hit(V_ADOPTED_LOOKUP);
    }

    s_worker_body(w);

    status =  tx_amiga_orphan_thread(&w -> thread);
    if (status != TX_SUCCESS)
    {
        s_viol_hit(V_ORPHAN);
    }
    if (S_CURRENT == &w -> thread)
    {
        s_viol_hit(V_BATON_AFTER_ORPHAN);
    }

    Forbid();
    s_adopted_live--;
    w -> finished =  1UL;
    Permit();
}


static VOID s_worker_task_entry(VOID)
{

struct s_worker *w;


    w =  (struct s_worker *) FindTask((STRPTR) 0) -> tc_UserData;

    Wait(SIGF_SINGLE);              /* released by main, all workers at once */

    s_worker_run(w);

    {
        struct Task    *me;
        struct MemList *ml;

        me =  FindTask((STRPTR) 0);
        ml =  (struct MemList *) me -> tc_MemEntry.lh_Head;

        S_LOG("%s: exiting, task 0x%lx block 0x%lx/%ld memlist 0x%lx",
              w -> name, (ULONG) me, (ULONG) w -> memblock, w -> memblock_size,
              (ULONG) ml);

        if ((ml != (struct MemList *) 0) && (ml -> ml_Node.ln_Succ != (struct Node *) 0))
        {
            S_LOG("%s: memlist entries %ld addr 0x%lx len %ld",
                  w -> name, (ULONG) ml -> ml_NumEntries,
                  (ULONG) ml -> ml_ME[0].me_Addr, ml -> ml_ME[0].me_Length);
        }
        else
        {
            S_ERR("%s: tc_MemEntry is EMPTY or corrupt before RemTask", w -> name);
        }
    }

    Forbid();
    s_tasks_exited++;
#if !S_NO_REMTASK
    RemTask((struct Task *) 0);
#else
    Permit();
    Wait(0UL);
#endif
}


static VOID s_worker_proc_entry(VOID)
{

struct s_worker *w;


    w =  (struct s_worker *) FindTask((STRPTR) 0) -> tc_UserData;

    Wait(SIGF_SINGLE);

    s_worker_run(w);

    Forbid();
    s_tasks_exited++;
    Permit();
    /* Returning is legal for a Process: dos.library unwinds it.  */
}


static VOID s_worker_tx_entry(ULONG id)
{

struct s_worker *w;


    w =  &s_worker[id];

    Forbid();
    w -> started =  1UL;
    Permit();

    s_worker_body(w);

    Forbid();
    w -> finished =  1UL;
    Permit();
}


static VOID s_churn_body(struct s_churner *c)
{

UINT    status;
UINT    contended;
ULONG   t0;
ULONG   us;


    while (s_stop == 0UL)
    {

        Forbid();
        contended =  (UINT) ((S_CURRENT != TX_NULL) ? 1U : 0U);
        Permit();

        t0 =  s_eclock();
        status =  tx_amiga_adopt_thread(&c -> thread, (CHAR *) c -> name, c -> priority);
        us =  s_us(s_eclock() - t0);

        if (status != TX_SUCCESS)
        {
            s_viol_hit(V_ADOPT);
            Delay(2L);
            continue;
        }

        Forbid();
        s_adopt_total++;
        if (contended != 0U)
        {
            c -> slow_path++;
        }
        else
        {
            c -> fast_path++;
            if (us > c -> adopt_fast_us_max)
            {
                c -> adopt_fast_us_max =  us;
            }
        }
        c -> adopt_us_sum +=  us;
        if (us > c -> adopt_us_max)
        {
            c -> adopt_us_max =  us;
        }
        Permit();

        if (tx_thread_identify() != &c -> thread)
        {
            s_viol_hit(V_IDENTITY);
        }
        if (tx_amiga_adopted_thread() != &c -> thread)
        {
            s_viol_hit(V_ADOPTED_LOOKUP);
        }
        if (S_CURRENT != &c -> thread)
        {
            s_viol_hit(V_CURRENT);
        }
        if (c -> thread.tx_thread_amiga_task != (VOID *) FindTask((STRPTR) 0))
        {
            s_viol_hit(V_IDENTITY_TASK);
        }

        if (tx_mutex_get(&s_mutex, S_MUTEX_WAIT) == TX_SUCCESS)
        {
            s_critical_section(&c -> thread, c -> cycles, 0U, &c -> inherit_seen);
            (VOID) tx_mutex_put(&s_mutex);
        }

        (VOID) tx_thread_sleep(1UL);

        status =  tx_amiga_orphan_thread(&c -> thread);
        if (status != TX_SUCCESS)
        {
            s_viol_hit(V_ORPHAN);
        }
        if (S_CURRENT == &c -> thread)
        {
            s_viol_hit(V_BATON_AFTER_ORPHAN);
        }
        if (tx_amiga_adopted_thread() != TX_NULL)
        {
            s_viol_hit(V_ADOPTED_LOOKUP);
        }

        c -> cycles++;

        /* Legal only because we are an ordinary Exec Process again.  */
        Delay(1L);
    }

    Forbid();
    c -> finished =  1UL;
    Permit();
}


static VOID s_churn_entry(VOID)
{

struct s_churner *c;


    c =  (struct s_churner *) FindTask((STRPTR) 0) -> tc_UserData;

    Wait(SIGF_SINGLE);

    s_churn_body(c);
}


static VOID s_watchdog_entry(VOID)
{

TX_THREAD      *cur;
TX_THREAD      *last_cur;
struct Task    *task;
ULONG           runs;
ULONG           last_runs;
ULONG           sigs;
ULONG           runsig;
UBYTE           tstate;
ULONG           ticks;
ULONG           last_ticks;
ULONG           total;
ULONG           last_total;
ULONG           stuck;
ULONG           stalled;
ULONG           idle;
ULONG           i;


    Wait(SIGF_SINGLE);

    last_cur   =  TX_NULL;
    last_runs  =  0UL;
    last_ticks =  0UL;
    last_total =  0UL;
    stuck      =  0UL;
    stalled    =  0UL;
    idle       =  0UL;

    while (s_watchdog_stop == 0UL)
    {

        Delay(3L);                      /* ~60 ms */

        Forbid();
        cur    =  _tx_thread_current_ptr;
        runs   =  (cur != TX_NULL) ? cur -> tx_thread_run_count : 0UL;
        task   =  (cur != TX_NULL) ? (struct Task *) cur -> tx_thread_amiga_task
                                   : (struct Task *) 0;
        runsig =  (cur != TX_NULL) ? cur -> tx_thread_amiga_run_signal : 0UL;
        tstate =  (task != (struct Task *) 0) ? task -> tc_State : (UBYTE) 0;
        sigs   =  (task != (struct Task *) 0) ? task -> tc_SigRecvd : 0UL;
        Permit();

        ticks =  tx_time_get();
        s_watchdog_samples++;

        if ((cur != TX_NULL) && (cur == last_cur) && (runs == last_runs) &&
            (tstate == (UBYTE) TS_WAIT) && ((sigs & runsig) == 0UL))
        {
            stuck++;
            if (stuck == 8UL)           /* ~0.5 s */
            {
                s_viol_hit(V_STUCK_BATON);
                if (s_wedge_dumps < 4UL)
                {
                    s_wedge_dumps++;
                    s_dump_state("stuck baton");
                }
            }
        }
        else
        {
            stuck =  0UL;
        }

        last_cur  =  cur;
        last_runs =  runs;

        if (ticks == last_ticks)
        {
            stalled++;
            if (stalled == 8UL)
            {
                s_viol_hit(V_TICK_STALL);
            }
        }
        else
        {
            stalled =  0UL;
        }
        last_ticks =  ticks;

        total =  0UL;
        for (i = 0UL; i < (ULONG) S_WORKERS; i++)
        {
            total +=  s_worker[i].iters;
        }
        for (i = 0UL; i < (ULONG) S_CHURNERS; i++)
        {
            total +=  s_churner[i].cycles;
        }

        if (total == last_total)
        {
            idle++;
            if (((idle % 80UL) == 0UL) && (s_wedge_dumps < 4UL))     /* ~5 s */
            {
                s_wedge_dumps++;
                s_dump_state("no progress");
            }
        }
        else
        {
            idle =  0UL;
        }
        last_total =  total;
    }

    Forbid();
    s_watchdog_done =  1UL;
    Permit();
}


static VOID s_probe_entry(ULONG id)
{

ULONG   tick0;
ULONG   tick1;
ULONG   e0;
ULONG   us;
ULONG   late_ticks;
ULONG   late_us;
ULONG   slot;


    (VOID) id;

    while (s_stop == 0UL)
    {

        tick0 =  tx_time_get();
        e0    =  s_eclock();

        if (tx_thread_sleep(S_PROBE_SLEEP) != TX_SUCCESS)
        {
            s_viol_hit(V_SLEEP_STATUS);
        }

        tick1 =  tx_time_get();
        us    =  s_us(s_eclock() - e0);

        slot =  (s_hog_window() != 0UL) ? 1UL : 0UL;

        late_ticks =  (tick1 - tick0);
        late_ticks =  (late_ticks > S_PROBE_SLEEP) ? (late_ticks - S_PROBE_SLEEP) : 0UL;

        late_us =  (us > (S_PROBE_SLEEP * 10000UL)) ? (us - (S_PROBE_SLEEP * 10000UL)) : 0UL;

        Forbid();
        s_probe_samples[slot]++;
        s_probe_late_ticks[slot] +=  late_ticks;
        if (late_ticks > s_probe_late_max[slot])
        {
            s_probe_late_max[slot] =  late_ticks;
        }
        if (late_us > s_probe_late_us_max[slot])
        {
            s_probe_late_us_max[slot] =  late_us;
        }
        Permit();
    }
}


static VOID s_pt_victim_entry(ULONG id)
{

    (VOID) id;

    Forbid();
    s_pt_flag =  1UL;
    Permit();

    while (s_stop == 0UL)
    {
        (VOID) tx_thread_sleep(S_TPS / 2UL);
    }
}


static VOID s_pt_entry(ULONG id)
{

UINT    status;
UINT    old_threshold;


    (VOID) id;

    (VOID) tx_thread_sleep(S_TPS);          /* let the soak settle */

    Forbid();
    s_pt_flag =  0UL;
    Permit();

    status =  tx_thread_resume(&s_pt_victim);
    (VOID) S_TX_OK(status, "threshold: resumed a priority-8 thread from priority 10/threshold 5");

    (VOID) S_CHECK(s_pt_flag == 0UL,
                   "threshold: the resumed thread did not preempt us", s_pt_flag);

    (VOID) tx_thread_sleep(3UL);

    (VOID) S_CHECK(s_pt_flag == 1UL,
                   "threshold: the resumed thread ran once we blocked", s_pt_flag);

    status =  tx_thread_preemption_change(&s_pt_thread, 10U, &old_threshold);
    (VOID) S_TX_OK(status, "threshold: tx_thread_preemption_change accepted");
    (VOID) S_CHECK(old_threshold == 5U, "threshold: old threshold reported correctly",
                   old_threshold);

    Forbid();
    s_pt_done =  1UL;
    Permit();

    while (s_stop == 0UL)
    {
        if (tx_mutex_get(&s_mutex, S_MUTEX_WAIT) == TX_SUCCESS)
        {
            s_spin_us(200UL);
            (VOID) tx_mutex_put(&s_mutex);
        }
        (VOID) tx_thread_sleep(5UL);
    }
}


static VOID s_timer_expiry(ULONG id)
{

ULONG   fires;


    (VOID) id;

    Forbid();
    fires =  ++s_timer_fires;
    Permit();

    /* Timer expirations run on the ThreadX system timer thread, so ThreadX
       services are legal here, including taking a mutex.  */
    if (tx_mutex_get(&s_mutex2, TX_NO_WAIT) == TX_SUCCESS)
    {
        Forbid();
        s_timer_mutex_gets++;
        Permit();
        (VOID) tx_mutex_put(&s_mutex2);
    }

    (VOID) tx_event_flags_set(&s_flags, 1UL, TX_OR);

    if ((fires & 3UL) == 0UL)
    {
        (VOID) tx_semaphore_put(&s_probe_sem);
    }
}


VOID tx_application_define(VOID *first_unused_memory)
{

UINT    status;
ULONG   i;


    (VOID) first_unused_memory;

    status =  tx_mutex_create(&s_mutex, "soak mutex", TX_INHERIT);
    (VOID) S_TX_OK(status, "define: contended mutex (priority inheritance)");

    status =  tx_mutex_create(&s_mutex2, "soak timer mutex", TX_NO_INHERIT);
    (VOID) S_TX_OK(status, "define: timer mutex");

    status =  tx_mutex_create(&s_pt_mutex, "soak threshold mutex", TX_NO_INHERIT);
    (VOID) S_TX_OK(status, "define: threshold mutex");

    status =  tx_semaphore_create(&s_never, "soak never", 0U);
    (VOID) S_TX_OK(status, "define: never-posted semaphore");

    status =  tx_semaphore_create(&s_probe_sem, "soak probe", 0U);
    (VOID) S_TX_OK(status, "define: probe semaphore");

    status =  tx_event_flags_create(&s_flags, "soak flags");
    (VOID) S_TX_OK(status, "define: event flags");

    status =  tx_timer_create(&s_timer, "soak timer", s_timer_expiry, 0UL,
                              S_TIMER_TICKS, S_TIMER_TICKS, TX_AUTO_ACTIVATE);
    (VOID) S_TX_OK(status, "define: periodic timer");

    status =  tx_thread_create(&s_probe_thread, "soak probe", s_probe_entry, 0UL,
                               (VOID *) s_probe_stack, (ULONG) sizeof(s_probe_stack),
                               1U, 1U, TX_NO_TIME_SLICE, TX_AUTO_START);
    (VOID) S_TX_OK(status, "define: latency probe thread");

    status =  tx_thread_create(&s_pt_victim, "soak pt victim", s_pt_victim_entry, 0UL,
                               (VOID *) s_victim_stack, (ULONG) sizeof(s_victim_stack),
                               8U, 8U, TX_NO_TIME_SLICE, TX_DONT_START);
    (VOID) S_TX_OK(status, "define: threshold victim thread");

    status =  tx_thread_create(&s_pt_thread, "soak pt", s_pt_entry, 0UL,
                               (VOID *) s_pt_stack, (ULONG) sizeof(s_pt_stack),
                               10U, 5U, TX_NO_TIME_SLICE, TX_AUTO_START);
    (VOID) S_TX_OK(status, "define: threshold thread (priority 10, threshold 5)");

    for (i = 0UL; i < (ULONG) S_WORKERS; i++)
    {
        if (s_worker_cfg[i].kind != S_KIND_TX)
        {
            continue;
        }

        status =  tx_thread_create(&s_worker[i].thread, (CHAR *) s_worker_cfg[i].name,
                                   s_worker_tx_entry, i,
                                   (VOID *) s_worker_stack[i], S_WORKER_STACK,
                                   s_worker_cfg[i].priority, s_worker_cfg[i].priority,
                                   TX_NO_TIME_SLICE, TX_AUTO_START);
        (VOID) S_TX_OK(status, "define: ThreadX-created worker");
    }
}


static VOID s_phase_wait_abort(VOID)
{

struct s_worker *w;
CHAR            *name;
UINT             state;
ULONG            runs;
UINT             pri;
UINT             thr;
ULONG            slice;
TX_THREAD       *nt;
TX_THREAD       *ns;
UINT             status;
ULONG            i;


    w =  &s_worker[1];                  /* an adopted raw Exec Task */

    S_LOG("phase: wait-abort on adopted %s", w -> name);

    Forbid();
    s_abort_done    =  0UL;
    s_abort_status  =  0xFFFFU;
    s_abort_request =  w -> index + 1UL;
    Permit();

    for (i = 0UL; (i < 200UL) && (s_abort_ready == 0UL); i++)
    {
        (VOID) tx_thread_sleep(10UL);           /* up to 20 s */
    }
    if (!S_CHECK(s_abort_ready != 0UL, "wait-abort: victim reached the suspension", i))
    {
        S_ERR("wait-abort: victim %s iters %ld state %ld; branch taken %ld times, status %ld, done %ld",
              w -> name, w -> iters, (ULONG) w -> thread.tx_thread_state,
              s_abort_seen, (ULONG) s_abort_status, s_abort_done);
        return;
    }

    /* Wait until it really is suspended on the semaphore, not merely about to
       be, otherwise the abort would race the suspend and prove nothing.  */
    state =  0U;
    for (i = 0UL; i < 200UL; i++)
    {
        if (tx_thread_info_get(&w -> thread, &name, &state, &runs, &pri, &thr,
                               &slice, &nt, &ns) != TX_SUCCESS)
        {
            break;
        }
        if (state == TX_SEMAPHORE_SUSP)
        {
            break;
        }
        (VOID) tx_thread_sleep(1UL);
    }
    (VOID) S_CHECK(state == TX_SEMAPHORE_SUSP,
                   "wait-abort: adopted thread suspended inside ThreadX", state);

    status =  tx_thread_wait_abort(&w -> thread);
    (VOID) S_TX_OK(status, "wait-abort: tx_thread_wait_abort accepted");

    for (i = 0UL; (i < 200UL) && (s_abort_done == 0UL); i++)
    {
        (VOID) tx_thread_sleep(5UL);            /* up to 10 s */
    }
    (VOID) S_CHECK(s_abort_done != 0UL, "wait-abort: adopted thread woke up", i);
    (VOID) S_CHECK(s_abort_status == TX_WAIT_ABORTED,
                   "wait-abort: it saw TX_WAIT_ABORTED", s_abort_status);
}


static VOID s_phase_suspend_resume(VOID)
{

struct s_worker *w;
UINT             status;
ULONG            a;
ULONG            b;
ULONG            i;


    w =  &s_worker[2];                  /* an adopted AmigaDOS Process */

    S_LOG("phase: suspend/resume on adopted %s", w -> name);

    status =  tx_thread_suspend(&w -> thread);
    (VOID) S_TX_OK(status, "suspend: tx_thread_suspend on an adopted thread");

    (VOID) tx_thread_sleep(S_TPS * 5UL);

    a =  w -> iters;
    (VOID) tx_thread_sleep(S_TPS * 2UL);
    b =  w -> iters;

    (VOID) S_CHECK(a == b, "suspend: the suspended adopted thread made no progress", b - a);

    status =  tx_thread_resume(&w -> thread);
    (VOID) S_TX_OK(status, "suspend: tx_thread_resume on an adopted thread");

    for (i = 0UL; (i < 250UL) && (w -> iters <= b); i++)      /* up to 10 s */
    {
        (VOID) tx_thread_sleep(4UL);
    }

    (VOID) S_CHECK(w -> iters > b, "suspend: it ran again after the resume",
                   w -> iters - b);
}


static VOID s_stop_tick(VOID)
{

ULONG   i;


    _tx_amiga_timer_stop =  TX_TRUE;

    if (_tx_amiga_timer_task != (VOID *) 0)
    {
        Signal((struct Task *) _tx_amiga_timer_task, SIGF_SINGLE);
    }

    for (i = 0UL; (i < 250UL) && (_tx_amiga_timer_task != (VOID *) 0); i++)
    {
        Delay(1L);                              /* up to ~5 s */
    }

    (VOID) S_CHECK(_tx_amiga_timer_task == (VOID *) 0,
                   "teardown: the ThreadX tick task exited", i);
}


static ULONG s_exit_count(VOID)
{

ULONG   done;
ULONG   i;


    done =  0UL;
    for (i = 0UL; i < (ULONG) S_WORKERS; i++)
    {
        done +=  s_worker[i].finished;
    }
    for (i = 0UL; i < (ULONG) S_CHURNERS; i++)
    {
        done +=  s_churner[i].finished;
    }
    return(done);
}


static ULONG s_wait_for_exit(ULONG rounds)
{

ULONG   done;
ULONG   i;


    for (i = 0UL; i < rounds; i++)
    {
        done =  s_exit_count();
        if (done == (ULONG) (S_WORKERS + S_CHURNERS))
        {
            return(done);
        }
        (VOID) tx_thread_sleep(5UL);
    }
    return(s_exit_count());
}


static VOID s_report_measurements(ULONG wall_ms, ULONG ticks_elapsed)
{

ULONG   i;
ULONG   mean;


    S_LOG("");
    S_LOG("---- measurements ----");
    S_LOG("wall %ld ms, ticks %ld (expected %ld at %ld Hz)",
          wall_ms, ticks_elapsed, wall_ms / (1000UL / S_TPS), S_TPS);
    S_LOG("timer fired %ld times, took the timer mutex %ld times",
          s_timer_fires, s_timer_mutex_gets);
    S_LOG("adoptions %ld total, peak %ld adopted contexts live at once",
          s_adopt_total, s_adopted_peak);

    for (i = 0UL; i < (ULONG) S_WORKERS; i++)
    {
        S_LOG("  %s pri %ld: %ld iters, %ld mutex, %ld timeouts, %ld inherit, %ld flags",
              s_worker[i].name, (ULONG) s_worker[i].priority, s_worker[i].iters,
              s_worker[i].mutex_ops, s_worker[i].mutex_timeouts,
              s_worker[i].inherit_seen, s_worker[i].flag_gets);
    }

    for (i = 0UL; i < (ULONG) S_CHURNERS; i++)
    {
        mean =  (s_churner[i].cycles != 0UL)
                    ? (s_churner[i].adopt_us_sum / s_churner[i].cycles) : 0UL;
        S_LOG("  %s: %ld adopt/orphan cycles, fast %ld slow %ld, adopt mean %ld us max %ld us",
              s_churner[i].name, s_churner[i].cycles, s_churner[i].fast_path,
              s_churner[i].slow_path, mean, s_churner[i].adopt_us_max);
    }

    S_LOG("deferred preemption (priority-1 thread, %ld-tick sleep):", S_PROBE_SLEEP);
    for (i = 0UL; i < 2UL; i++)
    {
        mean =  (s_probe_samples[i] != 0UL)
                    ? ((s_probe_late_ticks[i] * 100UL) / s_probe_samples[i]) : 0UL;
        S_LOG("  %s: %ld samples, late mean %ld.%02ld ticks, max %ld ticks (%ld us)",
              (i == 0UL) ? "no baton hog " : "baton hog on",
              s_probe_samples[i], mean / 100UL, mean % 100UL,
              s_probe_late_max[i], s_probe_late_us_max[i]);
    }
    S_LOG("  (hog windows: one worker spins %ld us outside ThreadX per iteration)",
          S_HOG_US);
}


static ULONG s_append_num(char *buf, ULONG at, ULONG value)
{

char    digits[12];
ULONG   n;


    n =  0UL;
    do
    {
        digits[n++] =  (char) ('0' + (value % 10UL));
        value /=  10UL;
    }
    while ((value != 0UL) && (n < 11UL));

    while (n != 0UL)
    {
        buf[at++] =  digits[--n];
    }
    return(at);
}


static VOID s_write_result(const char *stage)
{

BPTR    out;
char    line[160];
ULONG   n;


    out =  Open((CONST_STRPTR) "DH0:soak-result.txt", MODE_NEWFILE);
    if (out == (BPTR) 0)
    {
        return;
    }

    n =  0UL;
    while ((*stage != '\0') && (n < 60UL))
    {
        line[n++] =  *stage++;
    }
    line[n++] =  ':';
    line[n++] =  ' ';

    n =  s_append_num(line, n, s_checks);
    line[n++] =  ' ';
    line[n++] =  'c';
    line[n++] =  'h';
    line[n++] =  'e';
    line[n++] =  'c';
    line[n++] =  'k';
    line[n++] =  's';
    line[n++] =  ',';
    line[n++] =  ' ';
    n =  s_append_num(line, n, s_failures);
    line[n++] =  ' ';
    line[n++] =  'f';
    line[n++] =  'a';
    line[n++] =  'i';
    line[n++] =  'l';
    line[n++] =  ' ';
    line[n++] =  '-';
    line[n++] =  '-';
    line[n++] =  ' ';
    if (s_failures == 0UL)
    {
        line[n++] =  'P';
        line[n++] =  'A';
        line[n++] =  'S';
        line[n++] =  'S';
    }
    else
    {
        line[n++] =  'F';
        line[n++] =  'A';
        line[n++] =  'I';
        line[n++] =  'L';
    }
    line[n++] =  '\n';

    (VOID) Write(out, (APTR) line, (LONG) n);
    (VOID) Close(out);
}


int main(void)
{

UINT    status;
ULONG   i;
ULONG   start_tick;
ULONG   end_tick;
ULONG   start_ms;
ULONG   wall_ms;
ULONG   elapsed;
ULONG   done;
ULONG   spawned;
struct EClockVal ev;


    S_LOG("AmiNetXDuo, ThreadX Exec port soak (milestone 1 exit criterion)");
    S_LOG("  %ld s, %ld Hz tick, %ld workers (%ld adopted), %ld churners",
          S_SOAK_SECONDS, S_TPS, (ULONG) S_WORKERS, 4UL, (ULONG) S_CHURNERS);

    (VOID) S_CHECK(ami_crash_install_alert_hook() != FALSE,
                   "main: Exec Alert hook installed", 0);

    /* ami_millis() opens timer.device and sets the shared TimerBase; after
       that ReadEClock() is ours to use for sub-tick timing.  */
    (VOID) ami_millis();
    if (TimerBase != (struct Device *) 0)
    {
        s_eclock_khz =  ReadEClock(&ev) / 1000UL;
    }
    (VOID) S_CHECK(s_eclock_khz != 0UL, "main: E-Clock available for timing",
                   s_eclock_khz);

    status =  tx_amiga_kernel_start();
    if (!S_TX_OK(status, "main: ThreadX kernel started"))
    {
        return(20);
    }

    spawned =  0UL;
    for (i = 0UL; i < (ULONG) S_WORKERS; i++)
    {
        s_worker[i].name            =  s_worker_cfg[i].name;
        s_worker[i].index           =  i;
        s_worker[i].kind            =  s_worker_cfg[i].kind;
        s_worker[i].priority        =  s_worker_cfg[i].priority;
        s_worker[i].hog             =  s_worker_cfg[i].hog;
        s_worker[i].hold_over_sleep =  s_worker_cfg[i].hold_over_sleep;

        if (s_worker_cfg[i].kind == S_KIND_TASK)
        {
#if S_SKIP_TASKS
            s_worker[i].finished =  1UL;
            s_worker[i].started  =  1UL;
            s_worker[i].iters    =  S_MIN_ITERS;
            s_worker[i].mutex_ops =  1UL;
            continue;
#endif
            s_worker[i].task =  s_spawn_task(s_worker_cfg[i].name, 0,
                                             s_worker_task_entry,
                                             (APTR) s_worker_stack[i], S_WORKER_STACK,
                                             (APTR) &s_worker[i],
                                             &s_worker[i].memblock,
                                             &s_worker[i].memblock_size);
            if (s_worker[i].task != (struct Task *) 0)
            {
                spawned++;
            }
        }
        else if (s_worker_cfg[i].kind == S_KIND_PROC)
        {
            s_worker[i].task =  (struct Task *)
                s_spawn_proc(s_worker_cfg[i].name, 0L, s_worker_proc_entry,
                             S_PROC_STACK, (APTR) &s_worker[i]);
            if (s_worker[i].task != (struct Task *) 0)
            {
                spawned++;
            }
        }
    }
    (VOID) S_CHECK(spawned == 4UL, "main: 4 ordinary Exec contexts created for adoption",
                   spawned);

    s_churner[0].name     =  "soak-churn0";
    s_churner[0].priority =  14U;
    s_churner[0].index    =  0UL;
    s_churner[1].name     =  "soak-churn1";
    s_churner[1].priority =  15U;
    s_churner[1].index    =  1UL;

    spawned =  0UL;
    for (i = 0UL; i < (ULONG) S_CHURNERS; i++)
    {
#if S_SKIP_CHURN
        s_churner[i].finished  =  1UL;
        s_churner[i].cycles    =  S_MIN_CHURN;
        s_churner[i].fast_path =  1UL;
        s_churner[i].slow_path =  1UL;
        spawned++;
        continue;
#endif
        s_churner[i].proc =  s_spawn_proc(s_churner[i].name, 0L, s_churn_entry,
                                          S_PROC_STACK, (APTR) &s_churner[i]);
        if (s_churner[i].proc != (struct Process *) 0)
        {
            spawned++;
        }
    }
    (VOID) S_CHECK(spawned == (ULONG) S_CHURNERS, "main: churn processes created", spawned);

    {
        struct Process *wd;

        wd =  s_spawn_proc("soak-watchdog", 5L, s_watchdog_entry, S_PROC_STACK, (APTR) 0);
        (VOID) S_CHECK(wd != (struct Process *) 0, "main: watchdog process created", 0);
        if (wd != (struct Process *) 0)
        {
            Signal(&wd -> pr_Task, SIGF_SINGLE);
        }
    }

    status =  tx_amiga_adopt_thread(&s_main_thread, "soak coordinator", 9U);
    if (!S_TX_OK(status, "main: adopted this AmigaDOS Process"))
    {
        return(20);
    }

    Forbid();
    s_adopt_total++;
    s_adopted_live++;
    s_adopted_peak =  s_adopted_live;
    Permit();

    (VOID) S_CHECK(tx_thread_identify() == &s_main_thread,
                   "main: tx_thread_identify() is us", 0);
    (VOID) S_CHECK(S_CURRENT == &s_main_thread,
                   "main: we hold the baton after adoption", (ULONG) S_CURRENT);

    Forbid();
    for (i = 0UL; i < (ULONG) S_WORKERS; i++)
    {
        if ((s_worker_cfg[i].kind != S_KIND_TX) && (s_worker[i].task != (struct Task *) 0))
        {
            Signal(s_worker[i].task, SIGF_SINGLE);
        }
    }
    for (i = 0UL; i < (ULONG) S_CHURNERS; i++)
    {
        if (s_churner[i].proc != (struct Process *) 0)
        {
            Signal(&s_churner[i].proc -> pr_Task, SIGF_SINGLE);
        }
    }
    Permit();

    start_tick   =  tx_time_get();
    start_ms     =  ami_millis();
    s_start_tick =  start_tick;

    S_LOG("soak: running");

    {
        UINT done_abort =  0U;
        UINT done_susp  =  0U;

        for (;;)
        {
            elapsed =  tx_time_get() - start_tick;
            if (elapsed >= S_SOAK_TICKS)
            {
                break;
            }


            if (tx_thread_identify() != &s_main_thread)
            {
                s_viol_hit(V_IDENTITY);
            }
            if (S_CURRENT != &s_main_thread)
            {
                s_viol_hit(V_CURRENT);
            }

            if ((done_abort == 0U) && (elapsed > S_PHASE_ABORT_AT))
            {
                done_abort =  1U;
#if !S_SKIP_ABORT
                s_phase_wait_abort();
#endif
            }
            else if ((done_susp == 0U) && (elapsed > S_PHASE_SUSPEND_AT))
            {
                done_susp =  1U;
#if !S_SKIP_SUSPEND
                s_phase_suspend_resume();
#endif
            }
            else
            {
                (VOID) tx_thread_sleep(10UL);
            }
        }
    }

    end_tick =  tx_time_get();
    wall_ms  =  ami_millis() - start_ms;

    S_LOG("soak: stopping");
    s_stop       =  1UL;

    S_LOG("teardown: waiting for %ld contexts", (ULONG) (S_WORKERS + S_CHURNERS));

    done =  s_wait_for_exit(600UL);                  /* up to ~30 s */

    if (done != (ULONG) (S_WORKERS + S_CHURNERS))
    {
        s_dump_state("teardown timeout");
        S_LOG("teardown: unsticking with tx_thread_wait_abort()");

        for (i = 0UL; i < (ULONG) S_WORKERS; i++)
        {
            if (s_worker[i].finished == 0UL)
            {
                S_ERR("teardown: %s did not finish (iters %ld)",
                      s_worker[i].name, s_worker[i].iters);
                (VOID) tx_thread_wait_abort(&s_worker[i].thread);
            }
        }
        for (i = 0UL; i < (ULONG) S_CHURNERS; i++)
        {
            if (s_churner[i].finished == 0UL)
            {
                S_ERR("teardown: %s did not finish (cycles %ld)",
                      s_churner[i].name, s_churner[i].cycles);
            }
        }

        done =  s_wait_for_exit(200UL);              /* another ~10 s */
    }

    (VOID) S_CHECK(done == (ULONG) (S_WORKERS + S_CHURNERS),
                   "teardown: every worker and churner finished", done);

    (VOID) S_CHECK(s_cs_inside == 0UL, "final: nobody left inside the mutex", s_cs_inside);
    (VOID) S_CHECK(s_cs_unsafe == s_cs_safe,
                   "final: no lost update inside the mutex (mutual exclusion held)",
                   s_cs_safe - s_cs_unsafe);
    (VOID) S_CHECK(s_mutex.tx_mutex_owner == TX_NULL,
                   "final: mutex is unowned", (ULONG) s_mutex.tx_mutex_owner);
    (VOID) S_CHECK(end_tick > start_tick, "final: the tick advanced", end_tick - start_tick);
    (VOID) S_CHECK(s_adopted_peak >= 5UL,
                   "final: at least 5 adopted contexts were live at once", s_adopted_peak);
    (VOID) S_CHECK(s_adopt_total >= 100UL,
                   "final: adoption ran hundreds of times", s_adopt_total);
    (VOID) S_CHECK(s_timer_fires >= ((S_SOAK_TICKS / S_TIMER_TICKS) / 2UL),
                   "final: the timer kept firing", s_timer_fires);
    (VOID) S_CHECK(s_watchdog_samples > 100UL, "final: the watchdog sampled the baton",
                   s_watchdog_samples);
    (VOID) S_CHECK(s_pt_done != 0UL, "final: the preemption-threshold phase completed",
                   s_pt_done);

    {
        ULONG total = 0UL;
        ULONG floor;

        for (i = 0UL; i < (ULONG) S_WORKERS; i++)
            total += s_worker[i].iters;

        floor = total / 60UL;
        if (floor > S_MIN_ITERS)
            floor = S_MIN_ITERS;
        if (floor < S_MIN_ITERS_FLOOR)
            floor = S_MIN_ITERS_FLOOR;

        s_starve_floor = floor;
    }

    for (i = 0UL; i < (ULONG) S_WORKERS; i++)
    {
        (VOID) S_CHECK(s_worker[i].started != 0UL, "final: worker started",
                       i);
        (VOID) S_CHECK(s_worker[i].iters >= s_starve_floor,
                       "final: worker was not starved",
                       s_worker[i].iters);
        (VOID) S_CHECK(s_worker[i].mutex_ops > 0UL, "final: worker got the mutex",
                       s_worker[i].mutex_ops);
    }

    {
        ULONG fast = 0UL;
        ULONG slow = 0UL;

        for (i = 0UL; i < (ULONG) S_CHURNERS; i++)
        {
            (VOID) S_CHECK(s_churner[i].cycles >= S_MIN_CHURN,
                           "final: churner completed many adopt/orphan cycles",
                           s_churner[i].cycles);
            fast += s_churner[i].fast_path;
            slow += s_churner[i].slow_path;
        }

        (VOID) S_CHECK(fast > 0UL,
                       "final: the adoption fast path was exercised", fast);
        (VOID) S_CHECK(slow > 0UL,
                       "final: the adoption scheduler round trip was exercised",
                       slow);
    }

    for (i = 0UL; i < (ULONG) V_COUNT; i++)
    {
        (VOID) S_CHECK(s_viol[i] == 0UL, s_viol_name[i], s_viol[i]);
    }

    {
        ULONG expected;
        ULONG actual;
        ULONG drift;

        /* Derive from S_TPS, hardcoding 10 ms per tick silently breaks the
           moment TX_TIMER_TICKS_PER_SECOND changes (it did: 100 -> 50). */
        expected =  (wall_ms / (1000UL / S_TPS));
        actual   =  end_tick - start_tick;
        drift    =  (actual > expected) ? (actual - expected) : (expected - actual);
        (VOID) S_CHECK((expected == 0UL) || ((drift * 100UL) <= (expected * 10UL)),
                       "final: the tick tracked the wall clock within 10%",
                       (expected != 0UL) ? ((drift * 100UL) / expected) : 0UL);
    }

    s_report_measurements(wall_ms, end_tick - start_tick);

    /* Record the tally now: everything below can and does take the machine
       down, and the result has to reach the host first.  */
    s_write_result("soak");

    (VOID) S_TX_OK(tx_timer_deactivate(&s_timer), "teardown: timer deactivated");
    (VOID) S_TX_OK(tx_timer_delete(&s_timer), "teardown: timer deleted");

#if !S_SKIP_DELETE
    {
        TX_THREAD *victims[5];
        ULONG      v;

        victims[0] =  &s_probe_thread;
        victims[1] =  &s_pt_thread;
        victims[2] =  &s_pt_victim;
        victims[3] =  &s_worker[4].thread;
        victims[4] =  &s_worker[5].thread;

        for (v = 0UL; v < 5UL; v++)
        {
            (VOID) tx_thread_terminate(victims[v]);
            (VOID) S_TX_OK(tx_thread_delete(victims[v]),
                           "teardown: ThreadX thread deleted (Exec Task reaped)");
        }
    }
#endif

    status =  tx_amiga_orphan_thread(&s_main_thread);
    (VOID) S_TX_OK(status, "main: orphaned this Process");
    (VOID) S_CHECK(tx_amiga_adopted_thread() == TX_NULL,
                   "main: no longer a ThreadX thread", 0);

    s_watchdog_stop =  1UL;
    for (i = 0UL; (i < 100UL) && (s_watchdog_done == 0UL); i++)
    {
        Delay(2L);
    }
    (VOID) S_CHECK(s_watchdog_done != 0UL, "teardown: the watchdog exited", i);

    s_stop_tick();

    /* Raw Exec Tasks RemTask() themselves; wait before their stacks (static
       here, but the pattern matters) are considered dead.  */
    Delay(10L);

    ami_crash_remove_alert_hook();

    S_LOG("note: %ld ThreadX threads still exist; the ThreadX scheduler and tick",
          _tx_thread_created_count);
    S_LOG("note: Tasks outlive this program, the port has no kernel-stop path");

    S_LOG("");
    S_LOG("%ld checks, %ld failures, %s", s_checks, s_failures,
          (s_failures == 0UL) ? "PASS" : "FAIL");

    s_write_result("soak+teardown");

    {
        BPTR out;

        out =  Output();
        if (out != (BPTR) 0)
        {
            if (s_failures == 0UL)
            {
                (VOID) Write(out, (APTR) "soak: PASS\n", 11L);
            }
            else
            {
                (VOID) Write(out, (APTR) "soak: FAIL\n", 11L);
            }
        }
    }

    return((s_failures == 0UL) ? 0 : 20);
}
