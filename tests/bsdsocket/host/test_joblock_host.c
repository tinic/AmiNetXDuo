/*
 * An interface job whose SANA-II driver call waits must not stop the library
 * (#383).  library.c is compiled whole and run on threads: task A runs
 * Online's job, and the job's driver call -- OpenDevice() for an attach,
 * the S2_ONLINE DoIO() for a link-up -- blocks until the test lets it go.
 * While it blocks, task B does what NetMeter does once a second and
 * NetShutdown does first: OpenLibrary(), the NETSTATUS_SYSTEM opener walk,
 * NETCTRL_STACK_NOTIFY and CloseLibrary().  B must finish on its own.  Task C
 * starts a second job, which must wait for A's: one job at a time stays.
 *
 * And the ARexx KILL against the last close: the close holds sb_Lock across
 * netstack_shutdown(), which waits for the ARexx host to exit.  A KILL the
 * host is still running must not then wait for sb_Lock.
 *
 * Exec is modelled with one mutex: Signal()/Wait() on tc_SigRecvd,
 * SignalSemaphores with owner and nest count, CreateNewProc() as a thread.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"
#include "aminetxduo/events.h"
#include "aminetxduo/sana2.h"

#include <dos/dostags.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

/* How long B may take while A's driver call waits.  Everything B does is
   memory work; a B stuck on a lock never finishes, so this only bounds the
   failing run. */
#define H_DEADLINE_MS   2000L

#define H_NEG       512U
#define H_POS       ((UWORD)sizeof(struct AmiSocketBase))

/* ------------------------------------------------------------ Exec model */

static pthread_mutex_t h_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  h_cv = PTHREAD_COND_INITIALIZER;
static __thread struct Task *h_self;

static struct Task *h_task_new(const char *name)
{
    struct Task *t = (struct Task *)calloc(1, sizeof(struct Process));

    if (t == NULL)
    {
        printf("  FAIL out of memory building a task\n");
        exit(1);
    }
    t->tc_Node.ln_Type = NT_PROCESS;
    t->tc_Node.ln_Name = (char *)name;
    return t;
}

/* Wait on h_cv until pred(arg), or until the deadline when ms > 0.  FALSE on
   the deadline.  Caller holds h_mx. */
static BOOL h_wait_until(BOOL (*pred)(const void *), const void *arg, long ms)
{
    struct timespec until;

    if (ms <= 0)
    {
        while (!pred(arg))
            pthread_cond_wait(&h_cv, &h_mx);
        return TRUE;
    }

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec  += ms / 1000L;
    until.tv_nsec += (ms % 1000L) * 1000000L;
    if (until.tv_nsec >= 1000000000L)
    {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }

    while (!pred(arg))
        if (pthread_cond_timedwait(&h_cv, &h_mx, &until) == ETIMEDOUT)
            return pred(arg);
    return TRUE;
}

struct Task *FindTask(const char *n) { (VOID)n; return h_self; }

VOID Signal(struct Task *t, ULONG s)
{
    pthread_mutex_lock(&h_mx);
    t->tc_SigRecvd |= s;
    pthread_cond_broadcast(&h_cv);
    pthread_mutex_unlock(&h_mx);
}

static BOOL h_signalled(const void *mask)
{
    return (h_self->tc_SigRecvd & *(const ULONG *)mask) != 0 ? TRUE : FALSE;
}

ULONG Wait(ULONG s)
{
    ULONG got;

    pthread_mutex_lock(&h_mx);
    (VOID)h_wait_until(h_signalled, &s, 0);
    got = h_self->tc_SigRecvd & s;
    h_self->tc_SigRecvd &= ~got;
    pthread_mutex_unlock(&h_mx);
    return got;
}

BYTE AllocSignal(LONG n) { (VOID)n; return 20; }
VOID FreeSignal(LONG n)  { (VOID)n; }

static BOOL h_sem_free(const void *s)
{
    const struct SignalSemaphore *sem = (const struct SignalSemaphore *)s;

    return (sem->ss_Owner == NULL || sem->ss_Owner == h_self) ? TRUE : FALSE;
}

VOID InitSemaphore(struct SignalSemaphore *s) { memset(s, 0, sizeof(*s)); }

VOID ObtainSemaphore(struct SignalSemaphore *s)
{
    pthread_mutex_lock(&h_mx);
    (VOID)h_wait_until(h_sem_free, s, 0);
    s->ss_Owner = h_self;
    s->ss_NestCount++;
    pthread_mutex_unlock(&h_mx);
}

ULONG AttemptSemaphore(struct SignalSemaphore *s)
{
    ULONG got = 0;

    pthread_mutex_lock(&h_mx);
    if (h_sem_free(s))
    {
        s->ss_Owner = h_self;
        s->ss_NestCount++;
        got = 1;
    }
    pthread_mutex_unlock(&h_mx);
    return got;
}

VOID ReleaseSemaphore(struct SignalSemaphore *s)
{
    pthread_mutex_lock(&h_mx);
    if (--s->ss_NestCount == 0)
        s->ss_Owner = NULL;
    pthread_cond_broadcast(&h_cv);
    pthread_mutex_unlock(&h_mx);
}

/* Nothing here relies on Forbid() for exclusion: library.c's Forbid()s guard
   list and count updates that this model makes under h_mx or not at all. */
VOID Forbid(VOID)  { }
VOID Permit(VOID)  { }
VOID Disable(VOID) { }
VOID Enable(VOID)  { }
VOID CacheClearU(VOID) { }

APTR AllocMem(ULONG s, ULONG r) { (VOID)r; return calloc(1, (size_t)s); }
VOID FreeMem(APTR b, ULONG s)   { (VOID)s; free(b); }
VOID CopyMem(const APTR s, APTR d, ULONG n) { memmove(d, s, (size_t)n); }

VOID AddTail(struct List *l, struct Node *n)
{
    n->ln_Succ = (struct Node *)&l->lh_Tail;
    n->ln_Pred = l->lh_TailPred;
    l->lh_TailPred->ln_Succ = n;
    l->lh_TailPred = n;
}

VOID Remove(struct Node *n)
{
    n->ln_Pred->ln_Succ = n->ln_Succ;
    n->ln_Succ->ln_Pred = n->ln_Pred;
}

typedef struct
{
    struct Task *task;
    VOID       (*entry)(VOID);
} HProc;

static VOID *h_proc_main(VOID *arg)
{
    HProc *p = (HProc *)arg;

    h_self = p->task;
    p->entry();
    free(p->task);
    free(p);
    return NULL;
}

/* The job process: a thread of its own, detached, as a Process that
   exits is gone.  NP_Entry is a ULONG ti_Data, the low half of a host
   function pointer; the high half is this binary's, which library.c is
   linked into. */
struct Process *CreateNewProc(const struct TagItem *t)
{
    HProc    *p = (HProc *)calloc(1, sizeof(*p));
    pthread_t th;
    uintptr_t high = (uintptr_t)&h_proc_main &
                     ~(uintptr_t)0xFFFFFFFFUL;

    if (p == NULL)
        return NULL;
    for (; t->ti_Tag != TAG_DONE; t++)
        if (t->ti_Tag == NP_Entry)
            p->entry = (VOID (*)(VOID))(high | (uintptr_t)t->ti_Data);
    p->task = h_task_new("AmiNetXDuo stack");
    if (p->entry == NULL ||
        pthread_create(&th, NULL, h_proc_main, p) != 0)
    {
        free(p);
        return NULL;
    }
    pthread_detach(th);
    return (struct Process *)p->task;
}

UINT tx_amiga_exec_task_alive(VOID *task)  { (VOID)task; return TX_TRUE; }
UINT tx_amiga_exec_task_signal(VOID *task, ULONG mask)
{
    (VOID)task;
    (VOID)mask;
    return TX_TRUE;
}

/* ------------------------------------------------------- the driver stub */

/* The driver call every job here ends in.  It blocks until released; how
   many callers are inside it at once, and how many have been. */
static int  h_drv_inside;
static int  h_drv_entered;
static int  h_drv_peak;
static BOOL h_drv_released;

static BOOL h_drv_open(const void *unused)
{
    (VOID)unused;
    return h_drv_released;
}

static LONG h_driver_call(VOID)
{
    pthread_mutex_lock(&h_mx);
    h_drv_inside++;
    h_drv_entered++;
    if (h_drv_inside > h_drv_peak)
        h_drv_peak = h_drv_inside;
    pthread_cond_broadcast(&h_cv);
    (VOID)h_wait_until(h_drv_open, NULL, 0);
    h_drv_inside--;
    pthread_mutex_unlock(&h_mx);
    return AMI_NET_OK;
}

/* OpenDevice() of an attach, and the S2_ONLINE/S2_OFFLINE/CloseDevice() of
   the link jobs: the paths netstack_interface_start/up/down/remove reach. */
LONG netstack_interface_start(const AmiIfConfig *c, UWORD *out)
{
    (VOID)c;
    if (out != NULL)
        *out = 1;
    return h_driver_call();
}
LONG netstack_interface_up(UWORD i)         { (VOID)i; return h_driver_call(); }
LONG netstack_interface_down(UWORD i)       { (VOID)i; return h_driver_call(); }
LONG netstack_interface_stack_down(UWORD i) { (VOID)i; return h_driver_call(); }
LONG netstack_interface_remove(UWORD i, BOOL force)
{ (VOID)i; (VOID)force; return h_driver_call(); }
LONG netstack_interface_remove_named(const char *name, BOOL force)
{ (VOID)name; (VOID)force; return h_driver_call(); }

/* --------------------------------------------- the rest of library.c's world */

static NX_IP          h_stack_ip;
static NX_PACKET_POOL h_stack_pool;

NX_IP *netstack_ip(VOID)               { return &h_stack_ip; }
NX_PACKET_POOL *netstack_pool(VOID)    { return &h_stack_pool; }
/* The KILL case: netstack_shutdown() as the real one runs it -- ami_ns
   unpublished, then the wait for the ARexx host to exit. */
static BOOL h_kill_case;
static BOOL h_stack_gone;
static BOOL h_in_shutdown;
static BOOL h_host_back;
static BOOL h_shutdown_saw_host;
static VOID (*h_shutdown_hook)(VOID);

static BOOL h_host_returned(const void *unused)
{
    (VOID)unused;
    return h_host_back;
}

AmiNetStack *netstack_get(VOID)
{
    return h_stack_gone ? NULL : (AmiNetStack *)&h_stack_ip;
}
LONG netstack_startup(VOID)            { return AMI_NET_OK; }
LONG netstack_startup_loopback(VOID)   { return AMI_NET_OK; }
LONG netstack_shutdown(VOID)
{
    if (!h_kill_case)
        return AMI_NET_OK;

    pthread_mutex_lock(&h_mx);
    h_stack_gone  = TRUE;
    h_in_shutdown = TRUE;
    pthread_cond_broadcast(&h_cv);
    /* ami_netstack_rexx_stop() waits for ever; bounded here to report. */
    h_shutdown_saw_host = h_wait_until(h_host_returned, NULL, H_DEADLINE_MS);
    pthread_mutex_unlock(&h_mx);
    return AMI_NET_OK;
}
LONG Delay(ULONG ticks)
{
    struct timespec ts = { 0, 20000000L };

    while (ticks-- != 0)
        nanosleep(&ts, NULL);
    return 0;
}
BOOL netstack_can_unload(VOID)         { return FALSE; }
UWORD netstack_retained_count(VOID)    { return 0; }
BOOL bsd_tcp_handler_alive(VOID)       { return FALSE; }
BOOL bsd_aam_busy(VOID)                { return FALSE; }
BOOL bsd_netmon_busy(VOID)             { return FALSE; }
VOID ami_netdb_free(VOID)              { }
LONG ami_netdb_load(VOID)              { return 0; }
ULONG ami_netdb_unloaded(VOID)         { return 0; }
VOID bsd_log_hook_init(VOID)           { }
VOID bsd_log_hook_exit(VOID)           { }
VOID bsd_log_hook_drop_owner(struct AmiSocketBase *b) { (VOID)b; }
VOID bsd_runtime_close(VOID)           { }
BOOL bsd_runtime_open(VOID)            { return TRUE; }
VOID bsd_timer_teardown(struct AmiSocketBase *b) { (VOID)b; }
VOID bsd_usergroup_open(VOID)          { }
VOID ami_set_address_change_hook(VOID (*hook)(VOID)) { (VOID)hook; }
VOID ami_set_second_hook(VOID (*hook)(VOID))         { (VOID)hook; }
VOID ami_set_shutdown_hook(VOID (*hook)(VOID))       { h_shutdown_hook = hook; }
VOID ami_log(int level, const char *fmt, ...) { (VOID)level; (VOID)fmt; }
VOID ami_free(APTR p)                  { free(p); }
VOID ami_mem_open_delta(LONG d)        { (VOID)d; }
BYTE ami_signal_alloc(VOID)            { return 21; }
VOID ami_signal_free(BYTE s)           { (VOID)s; }
VOID ami_event(UWORD code, UWORD index, ULONG value)
{ (VOID)code; (VOID)index; (VOID)value; }
VOID ami_event_publish(VOID)           { }
VOID ami_event_unpublish(VOID)         { }
VOID bsd_bpf_close_all(struct AmiSocketBase *b) { (VOID)b; }
VOID bsd_bpf_zone_read(VOID)           { }
AMIGA_ASM_ARGS VOID bsd_tcp_window_recap(NX_IP *ip, struct AmiSana2If *sana)
{ (VOID)ip; (VOID)sana; }
VOID ami_sana2_set_capacity_hook(AmiSana2CapacityHook hook) { (VOID)hook; }
VOID bsd_close_all(struct AmiSocketBase *b) { (VOID)b; }
VOID bsd_handoff_take(struct AmiSocketBase *m, struct MinList *out)
{
    (VOID)m;
    out->mlh_Head     = (struct MinNode *)&out->mlh_Tail;
    out->mlh_Tail     = NULL;
    out->mlh_TailPred = (struct MinNode *)&out->mlh_Head;
}
VOID bsd_handoff_flush(struct AmiSocketBase *b, struct MinList *list,
                       BOOL bracketed)
{ (VOID)b; (VOID)list; (VOID)bracketed; }
VOID bsd_closing_drain(VOID)           { }
VOID bsd_defer_sweep(struct AmiSocketBase *b) { (VOID)b; }
VOID bsd_netmon_detach_owner(struct AmiSocketBase *owner) { (VOID)owner; }
VOID bsd_netmon_drop_owner(struct AmiSocketBase *owner)   { (VOID)owner; }
AmiSocket *bsd_closing_head;
AmiSocket *bsd_defer_head;
VOID bsd_orphans_reclaim(VOID)         { }
BOOL bsd_handoff_pending(struct AmiSocketBase *m) { (VOID)m; return FALSE; }
LONG bsd_nx_enter(struct AmiSocketBase *b) { (VOID)b; return 0; }
VOID bsd_nx_leave(struct AmiSocketBase *b) { (VOID)b; }
VOID ami_netstack_health_set_sblock(APTR sem) { (VOID)sem; }
VOID bsd_handoff_init(struct AmiSocketBase *b) { (VOID)b; }
VOID bsd_nx_release(struct AmiSocketBase *b) { (VOID)b; }
VOID ami_netstack_release(AmiNetCaller *caller) { (VOID)caller; }
VOID bsd_tcp_handler_start(struct AmiSocketBase *m) { (VOID)m; }
VOID n68k_cpu_select(ULONG a)          { (VOID)a; }
VOID CloseDevice(struct IORequest *io) { (VOID)io; }
VOID AddSemaphore(struct SignalSemaphore *s) { (VOID)s; }
VOID RemSemaphore(struct SignalSemaphore *s) { (VOID)s; }
ULONG ami_millis_quick(VOID)           { return 0UL; }

const APTR BsdVectorTable[] = { (APTR)-1 };

/* ---------------------------------------------------------------- fixture */

static UBYTE                *h_block;
static struct AmiSocketBase *h_master;

/* A running library: the master base with the stack held, as AddNetInterface
   leaves it, so no open below brings the stack up. */
static VOID h_fixture(VOID)
{
    h_block = (UBYTE *)calloc(1, (size_t)(H_NEG + H_POS));
    if (h_block == NULL)
    {
        printf("  FAIL out of memory building the fixture\n");
        exit(1);
    }

    h_master = (struct AmiSocketBase *)(h_block + H_NEG);
    h_master->sb_Lib.lib_Node.ln_Type = NT_LIBRARY;
    h_master->sb_Lib.lib_Node.ln_Name = (char *)"bsdsocket.library";
    h_master->sb_Lib.lib_NegSize      = (UWORD)H_NEG;
    h_master->sb_Lib.lib_PosSize      = H_POS;
    h_master->sb_Lib.lib_OpenCnt      = 1;
    h_master->sb_StackRefs            = 1;
    h_master->sb_StackHeld            = TRUE;
    h_master->sb_StackIp              = &h_stack_ip;
    h_master->sb_StackPool            = &h_stack_pool;
    InitSemaphore(&h_master->sb_Lock);
    h_master->sb_Children.mlh_Head =
        (struct MinNode *)&h_master->sb_Children.mlh_Tail;
    h_master->sb_Children.mlh_Tail = NULL;
    h_master->sb_Children.mlh_TailPred =
        (struct MinNode *)&h_master->sb_Children.mlh_Head;
}

/* ------------------------------------------------------------ the tasks */

typedef struct
{
    const char *name;
    UWORD       job;        /* BSD_JOB_ATTACH or a link job */
    BOOL        done;
    LONG        rc;
    LONG        openers;    /* B: what the NETSTATUS_SYSTEM walk counted */
    LONG        notified;   /* B: NETCTRL_STACK_NOTIFY's answer */
    pthread_t   th;
} HTask;

static BOOL h_task_done(const void *t)
{
    return ((const HTask *)t)->done;
}

static VOID h_finish(HTask *t)
{
    pthread_mutex_lock(&h_mx);
    t->done = TRUE;
    pthread_cond_broadcast(&h_cv);
    pthread_mutex_unlock(&h_mx);
}

/* A and C: Online, through its own base. */
static VOID *h_online_main(VOID *arg)
{
    HTask                *t = (HTask *)arg;
    struct AmiSocketBase *base;
    AmiIfConfig           cfg;
    UWORD                 index = 0;

    h_self = h_task_new(t->name);
    base   = bsd_lib_open(4, h_master);
    if (base == NULL)
        t->rc = -100;
    else
    {
        memset(&cfg, 0, sizeof(cfg));
        t->rc = (t->job == BSD_JOB_ATTACH)
                    ? bsd_stack_interface_start(base, &cfg, &index)
                    : bsd_stack_interface_link(base, t->job, 0, FALSE);
        (VOID)bsd_lib_close(base);
    }
    h_finish(t);
    free(h_self);
    return NULL;
}

/* B: NetMeter's second, and the first half of NetShutdown. */
static VOID *h_status_main(VOID *arg)
{
    HTask                *t = (HTask *)arg;
    struct AmiSocketBase *base;
    LONG                  avail = 0;
    ULONG                 signalled = 0;

    h_self = h_task_new(t->name);
    base   = bsd_lib_open(4, h_master);
    if (base == NULL)
        t->rc = -100;
    else
    {
        t->openers  = bsd_openers_list(base, NULL, 0, &avail);
        t->openers  = avail;
        t->notified = bsd_stack_notify(base, &signalled);
        (VOID)bsd_lib_close(base);
        t->rc = 0;
    }
    h_finish(t);
    free(h_self);
    return NULL;
}

static BOOL h_drv_reached(const void *n)
{
    return h_drv_entered >= *(const int *)n ? TRUE : FALSE;
}

static VOID h_start(HTask *t, VOID *(*fn)(VOID *))
{
    if (pthread_create(&t->th, NULL, fn, t) != 0)
    {
        printf("  FAIL cannot start task %s\n", t->name);
        exit(1);
    }
}

/* One case: A's job blocks in its driver; B must come back; C's job waits. */
static VOID h_case(const char *name, UWORD job)
{
    HTask a = { "Online", 0, FALSE, 0, 0, 0, 0 };
    HTask b = { "NetMeter", 0, FALSE, 0, 0, 0, 0 };
    HTask c = { "Offline", BSD_JOB_DOWN, FALSE, 0, 0, 0, 0 };
    int   one = 1;
    BOOL  b_back;
    BOOL  c_waited;
    BOOL  a_back;
    BOOL  c_back;

    a.job          = job;
    h_drv_inside   = 0;
    h_drv_entered  = 0;
    h_drv_peak     = 0;
    h_drv_released = FALSE;

    h_start(&a, h_online_main);
    pthread_mutex_lock(&h_mx);
    (VOID)h_wait_until(h_drv_reached, &one, 0);
    pthread_mutex_unlock(&h_mx);

    h_start(&b, h_status_main);
    h_start(&c, h_online_main);

    pthread_mutex_lock(&h_mx);
    b_back   = h_wait_until(h_task_done, &b, H_DEADLINE_MS);
    c_waited = (h_drv_entered == 1 && !c.done) ? TRUE : FALSE;
    h_drv_released = TRUE;
    pthread_cond_broadcast(&h_cv);
    a_back = h_wait_until(h_task_done, &a, H_DEADLINE_MS);
    c_back = h_wait_until(h_task_done, &c, H_DEADLINE_MS);
    if (!b_back)
        (VOID)h_wait_until(h_task_done, &b, H_DEADLINE_MS);
    pthread_mutex_unlock(&h_mx);

    printf("joblock case=%s status_returned_while_driver_waits=%d "
           "status_rc=%ld openers=%ld second_job_waited=%d driver_peak=%d "
           "job_rc=%ld\n",
           name, b_back ? 1 : 0, (long)b.rc, (long)b.openers,
           c_waited ? 1 : 0, h_drv_peak, (long)a.rc);

    CHECK(b_back, "OpenLibrary + NETSTATUS_SYSTEM + NOTIFY + CloseLibrary "
                  "returned while the job's driver call waited");
    CHECK(b.rc == 0, "the status task opened the library");
    CHECK(b.openers >= 2, "the status walk saw Online's base and its own");
    CHECK(b.notified == 0, "NETCTRL_STACK_NOTIFY answered");
    CHECK(c_waited, "a second job waited for the first");
    CHECK(h_drv_peak == 1, "one job in a driver at a time");
    CHECK(a_back && a.rc == AMI_NET_OK, "the job finished once released");
    CHECK(c_back && c.rc == AMI_NET_OK, "the second job ran after it");

    if (a_back)
        pthread_join(a.th, NULL);
    if (b.done)
        pthread_join(b.th, NULL);
    if (c_back)
        pthread_join(c.th, NULL);
    if (!a_back || !b.done || !c_back)
    {
        printf("joblock_host=FAIL a task never returned\n");
        exit(1);
    }
}

/* ------------------------------------------------ KILL against last close */

typedef struct AmiSocketBase *(*HLibInit)(struct AmiSocketBase *, APTR,
                                          struct ExecBase *);

extern const struct Resident bsd_romtag;   /* library.c */
static struct ExecBase       h_sysbase;

/* The ARexx host running KILL: ami_rx_kill() -> ami_shutdown_notify(). */
static VOID *h_host_main(VOID *arg)
{
    HTask *t = (HTask *)arg;

    h_self = h_task_new(t->name);
    h_shutdown_hook();
    pthread_mutex_lock(&h_mx);
    h_host_back = TRUE;
    pthread_mutex_unlock(&h_mx);
    h_finish(t);
    free(h_self);
    return NULL;
}

/* The last opener's CloseLibrary(): sb_Lock, the reference to zero, and
   netstack_shutdown() under it. */
static VOID *h_last_close_main(VOID *arg)
{
    HTask *t = (HTask *)arg;

    h_self = h_task_new(t->name);
    t->rc  = bsd_stack_close_release(h_master) ? 1 : 0;
    h_finish(t);
    free(h_self);
    return NULL;
}

static BOOL h_shutting_down(const void *unused)
{
    (VOID)unused;
    return h_in_shutdown;
}

static VOID h_kill_cases(VOID)
{
    const APTR *table = (const APTR *)bsd_romtag.rt_Init;
    HTask       close_task = { "IBrowse", 0, FALSE, 0, 0, 0, 0 };
    HTask       host = { "AmiNetXDuo arexx", 0, FALSE, 0, 0, 0, 0 };
    BOOL        close_back;
    BOOL        host_back;

    /* bsd_lib_init() registers the hook and publishes the master base. */
    if (((HLibInit)table[3])(h_master, NULL, &h_sysbase) != h_master ||
        h_shutdown_hook == NULL)
    {
        printf("joblock_host=FAIL bsd_lib_init did not register KILL\n");
        exit(1);
    }
    h_master->sb_StackIp   = &h_stack_ip;
    h_master->sb_StackPool = &h_stack_pool;

    /* Uncontended: KILL signals and gives the stack hold back. */
    h_master->sb_Lib.lib_OpenCnt = 2;
    h_master->sb_StackRefs       = 2;
    h_master->sb_StackHeld       = TRUE;
    h_start(&host, h_host_main);
    pthread_mutex_lock(&h_mx);
    host_back = h_wait_until(h_task_done, &host, H_DEADLINE_MS);
    pthread_mutex_unlock(&h_mx);
    if (!host_back)
    {
        printf("joblock_host=FAIL KILL never returned\n");
        exit(1);
    }
    pthread_join(host.th, NULL);
    printf("joblock case=kill_uncontended held=%d refs=%lu\n",
           h_master->sb_StackHeld ? 1 : 0,
           (unsigned long)h_master->sb_StackRefs);
    CHECK(!h_master->sb_StackHeld && h_master->sb_StackRefs == 1,
          "KILL gave the stack hold back");

    /* The last close holds sb_Lock and waits for the host; the host is
       inside KILL. */
    h_kill_case   = TRUE;
    h_host_back   = FALSE;
    host.done     = FALSE;
    h_start(&close_task, h_last_close_main);
    pthread_mutex_lock(&h_mx);
    (VOID)h_wait_until(h_shutting_down, NULL, 0);
    pthread_mutex_unlock(&h_mx);

    h_start(&host, h_host_main);

    pthread_mutex_lock(&h_mx);
    close_back = h_wait_until(h_task_done, &close_task, 2 * H_DEADLINE_MS);
    host_back  = h_wait_until(h_task_done, &host, 2 * H_DEADLINE_MS);
    pthread_mutex_unlock(&h_mx);

    printf("joblock case=kill_vs_last_close host_returned_while_close_waited=%d "
           "close_returned=%d host_returned=%d refs=%lu\n",
           h_shutdown_saw_host ? 1 : 0, close_back ? 1 : 0, host_back ? 1 : 0,
           (unsigned long)h_master->sb_StackRefs);
    CHECK(h_shutdown_saw_host, "the ARexx host's KILL returned while the last "
                               "close held sb_Lock and waited for it");
    CHECK(close_back && host_back, "both tasks returned");

    if (close_back)
        pthread_join(close_task.th, NULL);
    if (host_back)
        pthread_join(host.th, NULL);
    if (!close_back || !host_back)
    {
        printf("joblock_host=FAIL a task never returned\n");
        exit(1);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    h_self = h_task_new("main");
    h_fixture();

    /* bsd_lib_init() is not run here; its InitSemaphore() of the job lock
       is a memset of a static that is already zero. */
    h_case("attach_opendevice", BSD_JOB_ATTACH);
    h_case("online_doio", BSD_JOB_UP);
    h_kill_cases();

    printf("joblock_host=%s checks=%lu failures=%lu\n",
           (h_failures == 0) ? "PASS" : "FAIL", h_checks, h_failures);
    free(h_block);
    free(h_self);
    return (h_failures == 0) ? 0 : 1;
}
