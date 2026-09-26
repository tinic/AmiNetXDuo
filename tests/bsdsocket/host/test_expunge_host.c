/*
 * bsd_lib_expunge() on the host: does it DECLINE while the netstack says the
 * segment cannot be unloaded, and does it stop declining once that clears.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"
#include "aminetxduo/events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define H_GONE  (-1)

static VOID h_report(const char *name, LONG declined, LONG delexp,
                     LONG seglist_back, LONG teardown_ran)
{
    printf("expunge case=%s declined=%ld delexp=%ld seglist_returned=%ld "
           "teardown_ran=%ld\n",
           name, (long)declined, (long)delexp, (long)seglist_back,
           (long)teardown_ran);
}

#define H_SEGLIST   ((APTR)0x600DBEEFUL)

#define H_NEG       512U
#define H_POS       ((UWORD)sizeof(struct AmiSocketBase))

static struct ExecBase   h_sysbase;
static struct Task       h_task;

/* The block the base lives in, allocated the way bsd_lib_init() does it. */
static UBYTE                *h_block;
static struct AmiSocketBase *h_base;
static NX_IP                  h_stack_ip;
static NX_PACKET_POOL         h_stack_pool;

/* The list Exec keeps the library on, so Remove() has something real to do
   and "still in the list" is a question with an answer. */
static struct List           h_liblist;

static struct
{
    LONG    can_unload_calls;
    BOOL    can_unload_answer;
    UWORD   retained_answer;        /* netstack_retained_count()          */

    LONG    tcp_alive_calls;
    BOOL    tcp_alive_answer;
    LONG    aam_busy_calls;
    BOOL    aam_busy_answer;
    LONG    netmon_busy_calls;
    BOOL    netmon_busy_answer;

    LONG    netdb_free_calls;
    LONG    runtime_close_calls;
    LONG    hook_clears;

    LONG    freemem_calls;
    APTR    freemem_block;
    ULONG   freemem_size;

    LONG    remove_calls;
    APTR    remove_node;

    LONG    shutdown_calls;
    LONG    startup_calls;
    LONG    startup_result;
    BOOL    stack_running;
    LONG    alloc_signal_calls;
    BYTE    alloc_signal_result;
    LONG    free_signal_calls;
    LONG    create_proc_calls;
    LONG    forbid_depth;
    LONG    blocking_under_forbid;
} h;

static VOID h_machine_reset(BOOL can_unload)
{
    memset(&h, 0, sizeof(h));
    h.can_unload_answer = can_unload;

    memset(&h_sysbase, 0, sizeof(h_sysbase));

    /* Exec's list, with the library on it. */
    h_liblist.lh_Head     = (struct Node *)&h_liblist.lh_Tail;
    h_liblist.lh_Tail     = NULL;
    h_liblist.lh_TailPred = (struct Node *)&h_liblist;

    if (h_block != NULL)
        free(h_block);

    h_block = (UBYTE *)calloc(1, (size_t)(H_NEG + H_POS));
    if (h_block == NULL)
    {
        printf("  FAIL out of memory building the fixture\n");
        exit(1);
    }

    h_base = (struct AmiSocketBase *)(h_block + H_NEG);

    h_base->sb_Lib.lib_Node.ln_Type = NT_LIBRARY;
    h_base->sb_Lib.lib_Node.ln_Name = (char *)"bsdsocket.library";
    h_base->sb_Lib.lib_NegSize      = (UWORD)H_NEG;
    h_base->sb_Lib.lib_PosSize      = H_POS;
    h_base->sb_Lib.lib_OpenCnt      = 0;
    h_base->sb_Lib.lib_Flags        = 0;
    h_base->sb_SegList              = H_SEGLIST;
    h_base->sb_SysBase              = &h_sysbase;
    h_base->sb_Master               = NULL;
    h_base->sb_StackRefs            = 0;
    h_base->sb_TransientStackRefs   = 0;

    h_base->sb_Children.mlh_Head     = (struct MinNode *)&h_base->sb_Children.mlh_Tail;
    h_base->sb_Children.mlh_Tail     = NULL;
    h_base->sb_Children.mlh_TailPred = (struct MinNode *)&h_base->sb_Children;

    /* On the list, at the tail, the way AddLibrary() would leave it. */
    {
        struct Node *n = (struct Node *)h_base;

        n->ln_Pred            = h_liblist.lh_TailPred;
        n->ln_Succ            = (struct Node *)&h_liblist.lh_Tail;
        h_liblist.lh_TailPred->ln_Succ = n;
        h_liblist.lh_TailPred = n;
    }
}

/* Is the library still on Exec's list? */
static BOOL h_still_listed(VOID)
{
    struct Node *n;

    for (n = h_liblist.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        if (n == (struct Node *)h_base)
            return TRUE;
    }

    return FALSE;
}

/* Did any part of the teardown run?  Any one of these on a declined expunge
   is a library left half dismantled. */
static LONG h_teardown_ran(VOID)
{
    return (h.netdb_free_calls != 0 || h.runtime_close_calls != 0 ||
            h.hook_clears != 0 || h.freemem_calls != 0 ||
            h.remove_calls != 0) ? 1 : 0;
}

/* With h_model_refs the netstack's own reference count is modelled:
   every bring-up adds one, every shutdown takes one, and it can unload only
   at zero -- the rule ami_ns_startup()/netstack_shutdown()/
   netstack_can_unload() keep (netstack.c). */
static BOOL  h_model_refs;
static ULONG h_ns_refs;

/*
 * Reached, and expected.
 */
BOOL netstack_can_unload(VOID)
{
    h.can_unload_calls++;
    if (h_model_refs)
        return (h_ns_refs == 0) ? TRUE : FALSE;
    return h.can_unload_answer;
}

/* Asked only on a refusal, to name its reason in the event ring. */
UWORD netstack_retained_count(VOID)
{
    return h.retained_answer;
}

BOOL bsd_tcp_handler_alive(VOID)
{
    h.tcp_alive_calls++;
    return h.tcp_alive_answer;
}

BOOL bsd_aam_busy(VOID)
{
    h.aam_busy_calls++;
    return h.aam_busy_answer;
}

BOOL bsd_netmon_busy(VOID)
{
    h.netmon_busy_calls++;
    return h.netmon_busy_answer;
}

VOID ami_netdb_free(VOID)           { h.netdb_free_calls++; }
VOID bsd_log_hook_init(VOID)        { }
VOID bsd_log_hook_exit(VOID)        { }
VOID bsd_log_hook_drop_owner(struct AmiSocketBase *base) { (VOID)base; }
VOID bsd_runtime_close(VOID)        { h.runtime_close_calls++; }
/* select.c takes WaitSelect()'s timer request back before the base closes
   timer.device; the base under test never armed one. */
VOID bsd_timer_teardown(struct AmiSocketBase *base) { (VOID)base; }

#ifdef AMINETXDUO_TCP_CORK
/* The cork's timer comes and goes with the stack (library.c); cork.c is
   test_cork's. */
static BOOL  h_cork_busy;         /* bsd_cork_stop(): a pass in flight */
static ULONG h_cork_stops;

VOID bsd_cork_start(NX_IP *ip) { (VOID)ip; }
BOOL bsd_cork_stop(VOID)       { h_cork_stops++; return !h_cork_busy; }
#endif

/* bsd_lib_open() calls this on every open, to hold usergroup.library resident
   for ixemul clients.  Nothing here depends on it, and the real one only opens
   a library, so it is a no-op rather than an h_unreachable(). */
VOID bsd_usergroup_open(VOID)
{
    if (h.forbid_depth > 0)
        h.blocking_under_forbid++;
}

VOID ami_set_address_change_hook(VOID (*hook)(VOID))
{
    h.hook_clears++;
    (VOID)hook;
}

VOID ami_set_second_hook(VOID (*hook)(VOID))
{
    h.hook_clears++;
    (VOID)hook;
}

VOID ami_set_shutdown_hook(VOID (*hook)(VOID))
{
    h.hook_clears++;
    (VOID)hook;
}

VOID FreeMem(APTR block, ULONG size)
{
    h.freemem_calls++;
    h.freemem_block = block;
    h.freemem_size  = size;
    /* Not free()d: every assertion after the expunge reads the block it was
       given, and this test is not about the allocator. */
}

VOID Remove(struct Node *node)
{
    h.remove_calls++;
    h.remove_node = (APTR)node;

    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}


VOID netstack_shutdown(VOID)
{
    h.shutdown_calls++;
    if (h_model_refs && h_ns_refs > 0)
        h_ns_refs--;
}
NX_IP *netstack_ip(VOID)            { return &h_stack_ip; }
NX_PACKET_POOL *netstack_pool(VOID) { return &h_stack_pool; }
VOID bsd_netmon_drop_owner(struct AmiSocketBase *owner) { (VOID)owner; }

/* bsd_task_sweep() discards a dead opener's ThreadX registration through
   this. Nothing here adopts, so there is never one to discard; the stub
   exists because library.c is compiled whole. */
VOID ami_netstack_release(AmiNetCaller *caller) { (VOID)caller; }

/* The shipping implementation lives in the Exec port.  This harness never
   leaves a child base behind, so neither helper is reached. */
UINT tx_amiga_exec_task_alive(VOID *task)
{
    (VOID)task;
    return TX_FALSE;
}

UINT tx_amiga_exec_task_signal(VOID *task, ULONG mask)
{
    (VOID)task;
    (VOID)mask;
    return TX_FALSE;
}

/* Harmless, and reached by bsd_lib_close() on the way past. */
static LONG h_lock_depth;
static LONG h_in_bracket;
static LONG h_lock_under_bracket;
VOID ObtainSemaphore(struct SignalSemaphore *s)
{
    (VOID)s;
    if (h.forbid_depth > 0)
        h.blocking_under_forbid++;
    if (h_in_bracket > 0)
        h_lock_under_bracket++;
    h_lock_depth++;
}
VOID ReleaseSemaphore(struct SignalSemaphore *s) { (VOID)s; h_lock_depth--; }
ULONG AttemptSemaphore(struct SignalSemaphore *s) { (VOID)s; return 1UL; }
VOID InitSemaphore(struct SignalSemaphore *s)    { (VOID)s; }
VOID Forbid(VOID)                                { h.forbid_depth++; }
VOID Permit(VOID)                                { h.forbid_depth--; }
VOID Disable(VOID)                               { }
VOID Enable(VOID)                                { }

static struct SignalSemaphore *host_semaphore;

VOID AddSemaphore(struct SignalSemaphore *s)  { host_semaphore = s; }
VOID RemSemaphore(struct SignalSemaphore *s)
{
    if (host_semaphore == s)
        host_semaphore = NULL;
}

BOOL host_event_mark_published(VOID)
{
    return (host_semaphore != NULL) ? TRUE : FALSE;
}

/* The clock the ring asks for, which never opens timer.device. */
ULONG ami_millis_quick(VOID) { return 0UL; }
VOID CacheClearU(VOID)                           { }

static VOID h_unreachable(const char *what)
{
    printf("  FAIL %s was called; nothing on this path may reach it\n", what);
    exit(1);
}

/* t_failed_open_drains(): the child base of an open cannot be made.  A
   closer's gate runs at once if C does not hold sb_Lock there, which is the
   window; otherwise it waits for C to let go, as ObtainSemaphore() would. */
static BOOL                  h_alloc_fails;
static struct AmiSocketBase *h_alloc_gate;
static BOOL                  h_alloc_gate_waited;
static VOID h_gate(struct AmiSocketBase *child);
APTR AllocMem(ULONG s, ULONG r)
{
    (VOID)s; (VOID)r;
    if (!h_alloc_fails)
        h_unreachable("AllocMem");
    if (h_alloc_gate != NULL && h_lock_depth == 0)
    {
        h_gate(h_alloc_gate);
        h_alloc_gate = NULL;
    }
    h_alloc_gate_waited = (h_alloc_gate != NULL);
    return NULL;
}
VOID CopyMem(const APTR s, APTR d, ULONG n) { (VOID)s; (VOID)d; (VOID)n; h_unreachable("CopyMem"); }
VOID AddTail(struct List *l, struct Node *n) { (VOID)l; (VOID)n; h_unreachable("AddTail"); }
struct Task *FindTask(const char *n) { (VOID)n; return &h_task; }
VOID Signal(struct Task *t, ULONG s) { (VOID)t; (VOID)s; h_unreachable("Signal"); }
ULONG Wait(ULONG s) { (VOID)s; h_unreachable("Wait"); return 0UL; }
BYTE AllocSignal(LONG n)
{
    (VOID)n;
    h.alloc_signal_calls++;
    if (h.forbid_depth > 0)
        h.blocking_under_forbid++;
    return h.alloc_signal_result;
}
VOID FreeSignal(LONG n) { (VOID)n; h.free_signal_calls++; }
VOID CloseDevice(struct IORequest *io) { (VOID)io; h_unreachable("CloseDevice"); }
struct Process *CreateNewProc(const struct TagItem *t)
{
    (VOID)t;
    h.create_proc_calls++;
    if (h.forbid_depth > 0)
        h.blocking_under_forbid++;
    return NULL;
}

/* Not h_unreachable(): AMI_WARN is on paths this test drives, and it is
   compiled into every build now rather than out of the default one. */
VOID ami_log(int level, const char *fmt, ...) { (VOID)level; (VOID)fmt; }
VOID ami_free(APTR p) { (VOID)p; h_unreachable("ami_free"); }
/* The rest of a child base's close, reached by t_tableless_last_closer(). */
VOID ami_mem_open_delta(LONG d) { (VOID)d; }
LONG ami_netdb_load(VOID)
{
    if (h.forbid_depth > 0)
        h.blocking_under_forbid++;
    return 0;
}
BYTE ami_signal_alloc(VOID) { h_unreachable("ami_signal_alloc"); return -1; }
VOID ami_signal_free(BYTE s) { (VOID)s; }
VOID bsd_bpf_close_all(struct AmiSocketBase *b) { (VOID)b; }
/* A base with no table, or the kernel down: bsd_close_all() does nothing. */
static LONG h_close_alls;
VOID bsd_close_all(struct AmiSocketBase *b) { (VOID)b; h_close_alls++; }
/* What the drain gate and the last opener's close do, counted. */
static LONG h_takes;
static LONG h_flushes;
static LONG h_flushes_bracketed;
static LONG h_drains;
static LONG h_nx_enters;
static LONG h_nx_leaves;
static LONG h_nx_enter_result;
static LONG h_lock_depth;          /* sb_Lock, modelled                      */
static LONG h_enter_under_lock;    /* a bracket taken holding sb_Lock        */
static LONG h_lock_under_bracket;  /* sb_Lock taken holding the bracket      */
static LONG h_in_bracket;
VOID bsd_handoff_take(struct AmiSocketBase *m, struct MinList *out)
{
    (VOID)m;
    h_takes++;
    out->mlh_Head     = (struct MinNode *)&out->mlh_Tail;
    out->mlh_Tail     = NULL;
    out->mlh_TailPred = (struct MinNode *)&out->mlh_Head;
}
VOID bsd_handoff_flush(struct AmiSocketBase *b, struct MinList *list,
                       BOOL bracketed)
{
    (VOID)b; (VOID)list;
    h_flushes++;
    if (bracketed)
        h_flushes_bracketed++;
}
VOID bsd_closing_drain(VOID) { h_drains++; }
/* socket.c's parked closes, emptied by a last close that cannot drain. */
AmiSocket *bsd_closing_head;
static AmiSocket h_parked;
BOOL bsd_handoff_pending(struct AmiSocketBase *m) { (VOID)m; return FALSE; }
LONG bsd_nx_enter(struct AmiSocketBase *b)
{
    (VOID)b;
    h_nx_enters++;
    if (h_lock_depth > 0)
        h_enter_under_lock++;
    if (h_nx_enter_result == 0)
        h_in_bracket++;
    return h_nx_enter_result;
}
VOID bsd_nx_leave(struct AmiSocketBase *b) { (VOID)b; h_nx_leaves++; h_in_bracket--; }
/* Published at init and withdrawn at expunge, for the tick's lock-free reclaim. */
VOID ami_netstack_health_set_sblock(APTR sem) { (VOID)sem; }
VOID bsd_handoff_init(struct AmiSocketBase *b) { (VOID)b; h_unreachable("bsd_handoff_init"); }
VOID bsd_nx_release(struct AmiSocketBase *b) { (VOID)b; }
BOOL bsd_runtime_open(VOID) { h_unreachable("bsd_runtime_open"); return FALSE; }
VOID bsd_tcp_handler_start(struct AmiSocketBase *m) { (VOID)m; h_unreachable("bsd_tcp_handler_start"); }
LONG netstack_startup(VOID) { h.startup_calls++; return h.startup_result; }
LONG netstack_startup_loopback(VOID)
{
    h.startup_calls++;
    return h.startup_result;
}
AmiNetStack *netstack_get(VOID)
{
    return h.stack_running ? (AmiNetStack *)(ULONG)1 : NULL;
}
LONG netstack_interface_start(const AmiIfConfig *c, UWORD *out)
{
    (VOID)c;
    (VOID)out;
    h_unreachable("netstack_interface_start");
    return AMI_NET_ERR_STATE;
}

/* The link jobs the same launcher runs (library.c): nothing here opens one. */
LONG netstack_interface_up(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_up"); return -1; }
LONG netstack_interface_down(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_down"); return -1; }
LONG netstack_interface_stack_down(UWORD i)
{ (VOID)i; h_unreachable("netstack_interface_stack_down"); return -1; }
LONG netstack_interface_remove(UWORD i, BOOL force)
{ (VOID)i; (VOID)force; h_unreachable("netstack_interface_remove"); return -1; }
LONG netstack_interface_remove_named(const char *name, BOOL force)
{ (VOID)name; (VOID)force; h_unreachable("netstack_interface_remove_named");
  return -1; }
VOID n68k_cpu_select(ULONG a) { (VOID)a; h_unreachable("n68k_cpu_select"); }

const APTR BsdVectorTable[] = { (APTR)-1 };

static VOID t_refusal_declines(VOID)
{
    APTR r;

    printf("a stack that cannot be unloaded\n");

    h_machine_reset(FALSE);

    r = bsd_lib_expunge(h_base);

    CHECK(r == NULL, "expunge returned no segment");
    CHECK(h.can_unload_calls == 1, "and it asked the netstack, once");
    CHECK((h_base->sb_Lib.lib_Flags & LIBF_DELEXP) != 0,
          "LIBF_DELEXP is set, so a later close retries");
    CHECK(h_still_listed(), "the library is still on Exec's list");
    CHECK(h.remove_calls == 0, "Remove() was not called");
    CHECK(h.freemem_calls == 0, "the base was not freed");
    CHECK(h.netdb_free_calls == 0, "the netdb tables were not freed");
    CHECK(h.runtime_close_calls == 0, "the runtime was not closed");
    CHECK(h.hook_clears == 0,
          "the hooks the netstack calls back through were left installed");
    CHECK(h_base->sb_SegList == H_SEGLIST, "and the base still knows its segment");

    h_report("refused", r == NULL, (h_base->sb_Lib.lib_Flags & LIBF_DELEXP) != 0,
             r == H_SEGLIST, h_teardown_ran());
}

/* A SANA-II device still holding requests is its own reason, not ThreadX. */
static VOID t_refusal_names_a_retained_device(VOID)
{
    static NetStatusEvent ev[256];      /* more than the ring holds */
    ULONG          held = 0;
    ULONG          n;
    ULONG          i;
    BOOL           seen = FALSE;

    printf("a refusal over a retained SANA-II interface\n");

    h_machine_reset(FALSE);
    h.retained_answer = 1;

    CHECK(bsd_lib_expunge(h_base) == NULL, "expunge declined");

    n = ami_event_snapshot(ev, 256, &held);
    for (i = 0; i < n && i < 256; i++)
    {
        if (ev[i].nse_Code == NETEVENT_EXPUNGE_DECLINED)
            seen = (ev[i].nse_Value == NETEVENT_EXP_RETAINED) ? TRUE : FALSE;
    }
    CHECK(seen, "and the event ring says a device still holds requests");
}

static VOID t_refusal_clears(VOID)
{
    APTR r;

    printf("the same library once the stack is down\n");

    h_machine_reset(FALSE);

    r = bsd_lib_expunge(h_base);
    CHECK(r == NULL, "the first expunge declined");

    /* The condition clears: on a real machine this is netstack_startup()
       retrying the stop, or the thread the stop refused over exiting and a
       later shutdown getting TX_SUCCESS. */
    h.can_unload_answer = TRUE;

    r = bsd_lib_expunge(h_base);

    CHECK(r == H_SEGLIST, "the second expunge handed the segment back");
    CHECK(h.can_unload_calls == 2, "having asked the netstack again");
    CHECK(!h_still_listed(), "the library came off Exec's list");
    CHECK(h.remove_calls == 1 && h.remove_node == (APTR)h_base,
          "Remove() took the library itself");
    CHECK(h.freemem_calls == 1, "the base was freed, once");
    CHECK(h.freemem_block == (APTR)h_block,
          "from the start of the block, not from the base");
    CHECK(h.freemem_size == (ULONG)(H_NEG + H_POS),
          "for the whole of it, negative half included");
    CHECK(h.netdb_free_calls == 1, "the netdb tables went with it");
    CHECK(h.runtime_close_calls == 1, "and the runtime");
    CHECK(h.hook_clears == 3,
          "all three hooks were deregistered before the segment went");

    h_report("cleared", r == NULL, H_GONE, r == H_SEGLIST, h_teardown_ran());
}

static VOID t_open_count_comes_first(VOID)
{
    APTR r;

    printf("an expunge with an opener still holding the library\n");

    h_machine_reset(TRUE);
    h_base->sb_Lib.lib_OpenCnt = 1;

    r = bsd_lib_expunge(h_base);

    CHECK(r == NULL, "expunge returned no segment");
    CHECK((h_base->sb_Lib.lib_Flags & LIBF_DELEXP) != 0, "LIBF_DELEXP is set");
    CHECK(h.can_unload_calls == 0, "and the netstack was never asked");
    CHECK(h_base->sb_Lib.lib_OpenCnt == 1, "the open count is untouched");
    CHECK(h_teardown_ran() == 0, "nothing of the teardown ran");

    h_report("opener", r == NULL, (h_base->sb_Lib.lib_Flags & LIBF_DELEXP) != 0,
             r == H_SEGLIST, h_teardown_ran());
}

static VOID t_other_refusals(VOID)
{
    APTR r;

    printf("the refusals that are not about the netstack\n");

    h_machine_reset(TRUE);
    h.tcp_alive_answer = TRUE;
    r = bsd_lib_expunge(h_base);
    CHECK(r == NULL && (h_base->sb_Lib.lib_Flags & LIBF_DELEXP) != 0,
          "a live TCP: handler declines the expunge");
    CHECK(h_teardown_ran() == 0, "and nothing of the teardown ran");
    h_report("tcp", r == NULL, 1, r == H_SEGLIST, h_teardown_ran());

    h_machine_reset(TRUE);
    h.aam_busy_answer = TRUE;
    r = bsd_lib_expunge(h_base);
    CHECK(r == NULL && (h_base->sb_Lib.lib_Flags & LIBF_DELEXP) != 0,
          "a running address allocation declines the expunge");
    CHECK(h_teardown_ran() == 0, "and nothing of the teardown ran");
    h_report("addralloc", r == NULL, 1, r == H_SEGLIST, h_teardown_ran());

    h_machine_reset(TRUE);
    h.netmon_busy_answer = TRUE;
    r = bsd_lib_expunge(h_base);
    CHECK(r == NULL && (h_base->sb_Lib.lib_Flags & LIBF_DELEXP) != 0,
          "an installed monitoring hook declines the expunge");
    CHECK(h_teardown_ran() == 0, "and nothing of the teardown ran");
    h_report("netmon", r == NULL, 1, r == H_SEGLIST, h_teardown_ran());
}

static VOID t_last_close_retries(VOID)
{
    APTR r;

    printf("the last close, with LIBF_DELEXP already set\n");

    h_machine_reset(FALSE);
    h_base->sb_Lib.lib_OpenCnt  = 1;
    h_base->sb_Lib.lib_Flags   |= LIBF_DELEXP;

    r = bsd_lib_close(h_base);

    CHECK(r == NULL, "the close handed back no segment");
    CHECK(h_base->sb_Lib.lib_OpenCnt == 0, "the open count reached zero");
    CHECK(h.can_unload_calls == 1, "the close reached the expunge");
    CHECK((h_base->sb_Lib.lib_Flags & LIBF_DELEXP) != 0,
          "LIBF_DELEXP survives the declined retry");
    CHECK(h_still_listed(), "and the library is still there");
    CHECK(h_teardown_ran() == 0, "nothing of the teardown ran");
    /* The stack is only torn down by the last CHILD close.  A master closed
       directly must not reach netstack_shutdown(), or the last opener's
       teardown would run a second time on a stack that is already down. */
    CHECK(h.shutdown_calls == 0, "and the master close did not shut the stack down");
    h_report("close-refused", r == NULL, 1, r == H_SEGLIST, h_teardown_ran());

    h_machine_reset(TRUE);
    h_base->sb_Lib.lib_OpenCnt  = 1;
    h_base->sb_Lib.lib_Flags   |= LIBF_DELEXP;

    r = bsd_lib_close(h_base);

    CHECK(r == H_SEGLIST, "and with the stack down the close expunges");
    CHECK(!h_still_listed(), "the library came off Exec's list");
    CHECK(h.freemem_calls == 1, "the base was freed");
    h_report("close-expunged", r == NULL, H_GONE, r == H_SEGLIST,
             h_teardown_ran());
}

static VOID t_transient_stack_reference(VOID)
{
    LONG rc;

    printf("a transient worker stack reference\n");

    h_machine_reset(TRUE);
    h_base->sb_StackIp   = &h_stack_ip;
    h_base->sb_StackPool = &h_stack_pool;
    CHECK(bsd_stack_ip(h_base) == NULL && bsd_stack_pool(h_base) == NULL,
          "published pointers without a reference cannot be acquired");

    h_base->sb_StackRefs = 1;       /* the launching opener */
    CHECK(bsd_stack_ip(h_base) == &h_stack_ip &&
              bsd_stack_pool(h_base) == &h_stack_pool,
          "the opener reference leases both published NetX objects");

    rc = bsd_stack_transient_hold(h_base);
    CHECK(rc != 0, "an API opener without a network is not a stack reference");
    CHECK(h_base->sb_StackRefs == 1,
          "refusing the worker leaves the opener count alone");

    h.stack_running = TRUE;

    rc = bsd_stack_transient_hold(h_base);
    CHECK(rc == 0, "the worker acquired a running stack");
    CHECK(h_base->sb_StackRefs == 2,
          "the worker added one stack reference");
    CHECK(h_base->sb_TransientStackRefs == 1,
          "and that reference is identified as transient");

    bsd_stack_transient_release(h_base);
    CHECK(h_base->sb_StackRefs == 1,
          "release leaves the launching opener's reference");
    CHECK(h_base->sb_TransientStackRefs == 0,
          "release consumes the transient reference");
    CHECK(h.shutdown_calls == 0,
          "a remaining opener prevents netstack shutdown");

    rc = bsd_stack_transient_hold(h_base);
    CHECK(rc == 0, "a second worker reference was acquired");
    h_base->sb_StackRefs--;         /* the opener closes before the worker */

    bsd_stack_transient_release(h_base);
    CHECK(h_base->sb_StackRefs == 0,
          "the last worker release reaches zero references");
    CHECK(h_base->sb_TransientStackRefs == 0,
          "the last transient count also reaches zero");
    CHECK(h.shutdown_calls == 1,
          "the last worker release shuts the netstack down");
    CHECK(h_base->sb_StackIp == NULL && h_base->sb_StackPool == NULL,
          "and retires both published NetX pointers before shutdown");
    CHECK(h.can_unload_calls == 1,
          "and records whether teardown made the segment unloadable");

    h_report("transient", 0, 0, 0, h_teardown_ran());
}

/*
 * #53's leftover: two openers close at once.  bsd_lib_close() Wait()s in the
 * bracket and on sb_Lock between A's drain gate (bsd_close_all()) and A's
 * release, so B's gate can run in that window.  Both used to see two
 * references and skip the drain, and the stack went down with parked sockets.
 * The real gate and release, driven in that interleaved order.
 */
static struct AmiSocketBase h_child_a;
static struct AmiSocketBase h_child_b;
static struct AmiSocketBase h_child_c;
static LONG                 h_gate_last;

static VOID h_closers_reset(ULONG openers, BOOL worker)
{
    h_machine_reset(TRUE);
    h.stack_running      = TRUE;
    h_base->sb_StackRefs = openers;
    if (worker)
        CHECK(bsd_stack_transient_hold(h_base) == 0, "a worker holds the stack");
    h_child_a.sb_Master = h_base;
    h_child_b.sb_Master = h_base;
    h_child_c.sb_Master = h_base;
    h_gate_last         = 0;
    h_takes             = 0;
    h_flushes           = 0;
    h_flushes_bracketed = 0;
    h_drains            = 0;
    h_nx_enters         = 0;
    h_nx_leaves         = 0;
    h_nx_enter_result   = 0;
    h_close_alls        = 0;
    h_lock_depth        = 0;
    h_in_bracket        = 0;
    h_enter_under_lock  = 0;
    h_lock_under_bracket = 0;
}

/* One closer's gate, as bsd_child_close_gate() calls it. */
static VOID h_gate(struct AmiSocketBase *child)
{
    struct MinList handoffs;

    if (bsd_stack_close_gate(child, &handoffs))
        h_gate_last++;
}

/*
 * Issue #53: tool T launches an async DHCP job (a transient hold) and closes;
 * app A parks a closing socket and closes; T's job then releases the last
 * reference.  A's gate is the last one, so A drains before the worker's
 * release tears the stack down.
 */
static VOID t_transient_last_opener_drains(VOID)
{
    LONG t_drains;
    LONG a_drains;

    printf("the last opener's close with a transient worker outstanding\n");

    h_closers_reset(2, TRUE);                       /* T, A and T's job   */

    h_gate(&h_child_a);                             /* T's gate           */
    t_drains = h_gate_last;
    (VOID)bsd_stack_close_release(h_base);          /* T's release        */

    h_gate(&h_child_b);                             /* A's gate           */
    a_drains = h_gate_last - t_drains;
    (VOID)bsd_stack_close_release(h_base);          /* A's release        */

    CHECK(h.shutdown_calls == 0, "the worker still holds the stack");
    bsd_stack_transient_release(h_base);            /* the job completes  */

    printf("transient_last_opener t_drains=%ld a_drains=%ld shutdowns=%ld\n",
           (long)t_drains, (long)a_drains, (long)h.shutdown_calls);
    CHECK(t_drains == 0, "T's close leaves A's sockets alone");
    CHECK(a_drains == 1, "A's close drains the parked sockets");
    CHECK(h.shutdown_calls == 1, "the worker's release tears the stack down");
    CHECK(h_base->sb_StackClosing == 0 && h_base->sb_StackRefs == 0,
          "every gate and reference is given back");

    /* A second opener still open: the closer is not last. */
    h_closers_reset(2, FALSE);
    h_gate(&h_child_a);
    CHECK(h_gate_last == 0, "a held stack is not drained");
    (VOID)bsd_stack_close_release(h_base);
    h_gate(&h_child_b);
    CHECK(h_gate_last == 1, "the last plain opener drains");
    (VOID)bsd_stack_close_release(h_base);
}

static VOID t_concurrent_closers_drain(VOID)
{
    printf("two openers closing at the same time\n");

    /* A and B: gate, gate, release, release. */
    h_closers_reset(2, FALSE);
    h_gate(&h_child_a);
    h_gate(&h_child_b);
    (VOID)bsd_stack_close_release(h_base);
    (VOID)bsd_stack_close_release(h_base);
    printf("concurrent_close openers=2 worker=0 last=%ld takes=%ld "
           "shutdowns=%ld closing=%lu\n", (long)h_gate_last, (long)h_takes,
           (long)h.shutdown_calls, (unsigned long)h_base->sb_StackClosing);
    CHECK(h_gate_last == 1, "exactly one of two interleaved closers is last");
    CHECK(h_takes == 1, "and takes the handoff registry");
    CHECK(h.shutdown_calls == 1, "the second release tears the stack down");
    CHECK(h_base->sb_StackClosing == 0, "every gate is given back");

    /* The same with a worker's transient hold outstanding: the worker's
       release is the one that tears down, after the drain. */
    h_closers_reset(2, TRUE);
    h_gate(&h_child_a);
    h_gate(&h_child_b);
    (VOID)bsd_stack_close_release(h_base);
    (VOID)bsd_stack_close_release(h_base);
    CHECK(h.shutdown_calls == 0, "the worker still holds the stack");
    bsd_stack_transient_release(h_base);
    printf("concurrent_close openers=2 worker=1 last=%ld shutdowns=%ld\n",
           (long)h_gate_last, (long)h.shutdown_calls);
    CHECK(h_gate_last == 1, "one drain before the worker's teardown");
    CHECK(h.shutdown_calls == 1, "the worker's release tears the stack down");

    /* Three closers: A gates, B gates, A releases, C gates, C and B release.
       C is the last to gate and the only one to drain. */
    h_closers_reset(3, FALSE);
    h_gate(&h_child_a);
    h_gate(&h_child_b);
    CHECK(h_gate_last == 0, "no drain while C is still open");
    (VOID)bsd_stack_close_release(h_base);
    h_gate(&h_child_c);
    CHECK(h_gate_last == 1, "C's gate drains");
    (VOID)bsd_stack_close_release(h_base);
    (VOID)bsd_stack_close_release(h_base);
    CHECK(h_gate_last == 1 && h.shutdown_calls == 1,
          "one drain, one teardown, with three interleaved closers");

    /* Serialised, as before: gate, release, gate, release. */
    h_closers_reset(2, FALSE);
    h_gate(&h_child_a);
    (VOID)bsd_stack_close_release(h_base);
    CHECK(h_gate_last == 0, "the first of two serial closers leaves the sockets");
    h_gate(&h_child_b);
    (VOID)bsd_stack_close_release(h_base);
    CHECK(h_gate_last == 1 && h.shutdown_calls == 1,
          "the second serial closer drains");
    CHECK(h_lock_under_bracket == 0 && h_enter_under_lock == 0,
          "the gate takes sb_Lock with no bracket held");
}

/*
 * The worst early exit: A has parked sockets and passed its gate but not yet
 * released; B, the last opener, never made a descriptor, so bsd_close_all()
 * returns at once (socket.c, sb_Table == NULL).  B's CloseLibrary() through the
 * real bsd_lib_close() must still drain and flush, under a bracket it takes
 * after sb_Lock is released.
 */
static UBYTE *h_child_block;

static struct AmiSocketBase *h_tableless_child(VOID)
{
    struct AmiSocketBase *c;

    free(h_child_block);
    h_child_block = (UBYTE *)calloc(1, (size_t)(H_NEG + H_POS));
    if (h_child_block == NULL)
    {
        printf("  FAIL out of memory building the child\n");
        exit(1);
    }
    c = (struct AmiSocketBase *)(h_child_block + H_NEG);
    c->sb_Lib.lib_NegSize = (UWORD)H_NEG;
    c->sb_Lib.lib_PosSize = H_POS;
    c->sb_Master          = h_base;
    c->sb_Task            = NULL;
    c->sb_Table           = NULL;

    /* On the master's child list, so bsd_child_destroy()'s Remove() works. */
    c->sb_Node.mln_Succ = (struct MinNode *)&h_base->sb_Children.mlh_Tail;
    c->sb_Node.mln_Pred = h_base->sb_Children.mlh_TailPred;
    h_base->sb_Children.mlh_TailPred->mln_Succ = &c->sb_Node;
    h_base->sb_Children.mlh_TailPred = &c->sb_Node;
    return c;
}

static VOID t_tableless_last_closer(VOID)
{
    struct AmiSocketBase *b;

    printf("the last closer never made a descriptor\n");

    h_closers_reset(2, FALSE);
    h_base->sb_Lib.lib_OpenCnt = 3;
    b = h_tableless_child();

    h_gate(&h_child_a);                      /* A parked, gated, not released */
    CHECK(h_gate_last == 0, "A is not the last opener");

    (VOID)bsd_lib_close(b);                  /* B: no table                   */
    printf("tableless_last close_all=%ld takes=%ld flushes=%ld drains=%ld "
           "enters=%ld leaves=%ld lock_under_bracket=%ld\n",
           (long)h_close_alls, (long)h_takes, (long)h_flushes_bracketed,
           (long)h_drains, (long)h_nx_enters, (long)h_nx_leaves,
           (long)h_lock_under_bracket);
    CHECK(h_close_alls == 1, "B's close went through bsd_close_all()");
    CHECK(h_takes == 1, "B, last, took the handoff registry");
    CHECK(h_flushes_bracketed == 1, "and flushed it under a bracket");
    CHECK(h_drains == 1, "and drained A's parked sockets");
    CHECK(h_nx_enters == 1 && h_nx_leaves == 1, "one bracket, given back");
    CHECK(h_lock_under_bracket == 0 && h_enter_under_lock == 0,
          "sb_Lock and the bracket were never held together");
    CHECK(h.shutdown_calls == 0, "A still holds the stack");

    (VOID)bsd_stack_close_release(h_base);   /* A's release                   */
    CHECK(h.shutdown_calls == 1 && h_base->sb_StackClosing == 0,
          "A's release tears down with nothing owed");

    /* The kernel down for B's bracket: the registry is still taken off the
       master and its entries abandoned with a warning; nothing is drained. */
    h_closers_reset(2, FALSE);
    h_base->sb_Lib.lib_OpenCnt = 3;
    b = h_tableless_child();
    h_nx_enter_result = -1;
    h_gate(&h_child_a);
    bsd_closing_head = &h_parked;            /* A's parked socket             */
    (VOID)bsd_lib_close(b);
    CHECK(h_takes == 1 && h_flushes == 1 && h_flushes_bracketed == 0,
          "kernel down: the registry is emptied unbracketed");
    CHECK(h_drains == 0 && h_nx_leaves == 0, "and no NetX call is made");
    CHECK(bsd_closing_head == NULL,
          "the parked sockets nobody can drain are forgotten, so no sweep "
          "after the teardown reaches them");
    (VOID)bsd_stack_close_release(h_base);
    CHECK(h_base->sb_StackClosing == 0, "the count still comes back");
}

#ifdef AMINETXDUO_TCP_CORK
/* One open/close cycle of the stack: a bring-up takes a netstack reference
   (bsd_lib_open(), netstack.c:1864), and the last library reference going
   runs bsd_netstack_shutdown_owned() with the cork's pass busy or not. */
static VOID h_cork_cycle(BOOL busy)
{
    h_ns_refs++;
    h_base->sb_StackIp            = &h_stack_ip;
    h_base->sb_StackPool          = &h_stack_pool;
    h_base->sb_StackRefs          = 1;
    h_base->sb_TransientStackRefs = 1;
    h_cork_busy = busy;
    bsd_stack_transient_release(h_base);
}

/*
 * The stack's last reference goes while the cork's IP pass is still inside a
 * send (cork.c, bsd_cork_stop() answering FALSE): the netstack is kept, not
 * torn down under it, and every reference kept that way is given back, once
 * each, by the first shutdown that finds no pass.
 */
static VOID t_cork_pass_keeps_stack(VOID)
{
    printf("the stack's last reference with a cork pass in flight\n");

    /* One refused cycle, then one that succeeds. */
    h_machine_reset(FALSE);
    h_model_refs = TRUE;
    h_ns_refs    = 0;
    h_cork_stops = 0;

    h_cork_cycle(TRUE);
    CHECK(h_cork_stops == 1, "the cork is stopped first");
    CHECK(h.shutdown_calls == 0 && h_ns_refs == 1,
          "a pass in flight: the netstack is not torn down under it");
    CHECK(h_base->sb_StackIp == NULL && h_base->sb_StackPool == NULL,
          "though no library call can reach it any more");
    CHECK(h.can_unload_calls == 1 && netstack_can_unload() == FALSE,
          "and the library cannot be unloaded while it is kept");

    h_cork_cycle(FALSE);
    CHECK(h.shutdown_calls == 2 && h_ns_refs == 0,
          "the next shutdown gives back the kept reference and its own");
    CHECK(netstack_can_unload() == TRUE, "and then the library can go");

    h_cork_cycle(FALSE);
    CHECK(h.shutdown_calls == 3 && h_ns_refs == 0,
          "after that, one each time again");

    /* Two refused cycles, reopened in between, then one that succeeds. */
    h_machine_reset(FALSE);
    h_model_refs = TRUE;
    h_ns_refs    = 0;

    h_cork_cycle(TRUE);
    h_cork_cycle(TRUE);
    CHECK(h.shutdown_calls == 0 && h_ns_refs == 2,
          "two refused shutdowns keep two references");
    CHECK(netstack_can_unload() == FALSE, "the library stays resident");

    h_cork_cycle(FALSE);
    CHECK(h.shutdown_calls == 3,
          "the first clean shutdown calls netstack_shutdown() exactly three "
          "times: two kept references and its own");
    CHECK(h_ns_refs == 0, "every reference given back, none twice");
    CHECK(netstack_can_unload() == TRUE, "and the library can unload");

    h_cork_cycle(FALSE);
    CHECK(h.shutdown_calls == 4 && h_ns_refs == 0,
          "nothing left owed: the next one is its own alone");

    h_model_refs = FALSE;
    h_cork_busy  = FALSE;
}
#endif

/*
 * A failed open between two closes.  A, the sole opener, has sockets parked
 * and starts CloseLibrary() while C starts OpenLibrary(), and C's child base
 * cannot be made.  C's open used to count its stack reference, drop sb_Lock,
 * fail to make the base and give the reference back with no drain gate: A,
 * gating in that window, was not last, and A's release then took the stack
 * down with A's sockets still created.  The reference is now counted with the
 * base made, under one hold of sb_Lock, so A's gate waits it out and finds
 * itself last.
 */
static VOID t_failed_open_drains(VOID)
{
    struct AmiSocketBase *opened;

    printf("an open that fails while the last opener closes\n");

    h_closers_reset(1, FALSE);                     /* A                   */
    h_base->sb_Lib.lib_OpenCnt = 1;
    h.forbid_depth = 1;                            /* Exec's Forbid       */
    h_alloc_fails  = TRUE;
    h_alloc_gate   = &h_child_a;                   /* A's close           */

    opened = bsd_lib_open(4UL, h_base);            /* C                   */

    if (h_alloc_gate != NULL)                      /* A waited on sb_Lock */
        h_gate(h_alloc_gate);
    h_alloc_fails = FALSE;
    h_alloc_gate  = NULL;
    printf("failed_open a_waited=%ld a_last=%ld refs=%lu closing=%lu\n",
           (long)h_alloc_gate_waited, (long)h_gate_last,
           (unsigned long)h_base->sb_StackRefs,
           (unsigned long)h_base->sb_StackClosing);
    CHECK(opened == NULL, "C's open failed");
    CHECK(h_alloc_gate_waited, "C held sb_Lock while making its base");
    CHECK(h_gate_last == 1, "A's gate is last, so A drains its sockets");
    CHECK(h_base->sb_StackRefs == 1 && h_base->sb_StackClosing == 1,
          "C left no reference behind; A's is still counted");
    CHECK(h_base->sb_Lib.lib_OpenCnt == 1 && h.forbid_depth == 1,
          "C gave its open count back under Exec's Forbid");

    (VOID)bsd_stack_close_release(h_base);         /* A's release         */
    CHECK(h.shutdown_calls == 1 && h_base->sb_StackRefs == 0 &&
              h_base->sb_StackClosing == 0,
          "A's release tears down with nothing owed");

    /* A stays open: C's failed open changes nothing. */
    h_closers_reset(1, FALSE);
    h.forbid_depth = 1;
    h_alloc_fails  = TRUE;
    opened = bsd_lib_open(4UL, h_base);
    h_alloc_fails = FALSE;
    CHECK(opened == NULL && h.shutdown_calls == 0 &&
              h_base->sb_StackRefs == 1 && h_base->sb_StackClosing == 0,
          "with an opener left, a failed open takes no reference");
}

static VOID t_loopback_startup_failure_ownership(VOID)
{
    struct AmiSocketBase *opened;

    printf("failed loopback startup during library open\n");

    h_machine_reset(TRUE);
    h.startup_result = AMI_NET_ERR_CONFIG;
    h.alloc_signal_result = (BYTE)-1;
    h.forbid_depth = 1;            /* Exec's library-list critical section */

    opened = bsd_lib_open(4UL, h_base);

    CHECK(opened == NULL, "signal-exhausted startup refused the open");

    /* AND IT DID NOT RUN BRING-UP HERE.  There used to be a caller-stack
       fallback, on the argument that refusing an open helps nobody.  Bring-up
       is 2876 bytes against a Shell's 4096 with the calling program's frames
       already in it, and there is no MMU, so the fallback did not fail -- it
       wrote over whatever was below and the machine broke later.  Refusing is
       the whole point now, so the refusal is what is asserted. */
    CHECK(h.startup_calls == 0,
          "a signal-exhausted open does not bring up on the caller's stack");
    CHECK(h.shutdown_calls == 0, "and takes no reference to release");
    CHECK(h_base->sb_Lib.lib_OpenCnt == 0,
          "failed startup returned the library open count");
    CHECK(h_base->sb_StackRefs == 0,
          "failed startup created no opener stack reference");
    CHECK(h.blocking_under_forbid == 0,
          "no blocking open work ran inside Exec's Forbid");
    CHECK(h.forbid_depth == 1,
          "the failed open restored Exec's Forbid nesting");

    h_machine_reset(TRUE);
    h.startup_result = AMI_NET_ERR_CONFIG;
    h.alloc_signal_result = 5;
    h.forbid_depth = 1;

    opened = bsd_lib_open(4UL, h_base);

    CHECK(opened == NULL, "process-creation failure refused the open");
    CHECK(h.create_proc_calls == 1, "the child Process was attempted once");
    CHECK(h.free_signal_calls == 1, "the unused startup signal was freed");
    CHECK(h.startup_calls == 0,
          "and a process it could not create is not run here instead");
    CHECK(h.shutdown_calls == 0, "with nothing to release");
    CHECK(h_base->sb_Lib.lib_OpenCnt == 0,
          "the second failed startup returned the open count");
    CHECK(h.blocking_under_forbid == 0,
          "process creation also ran with task switching enabled");
    CHECK(h.forbid_depth == 1,
          "the process failure restored Exec's Forbid nesting");
}

int main(void)
{
    printf("bsd_lib_expunge() host tests\n");

    t_refusal_declines();
    t_refusal_clears();
    t_refusal_names_a_retained_device();
    t_open_count_comes_first();
    t_other_refusals();
    t_last_close_retries();
    t_transient_stack_reference();
    t_transient_last_opener_drains();
    t_concurrent_closers_drain();
    t_tableless_last_closer();
    t_failed_open_drains();
    t_loopback_startup_failure_ownership();
#ifdef AMINETXDUO_TCP_CORK
    t_cork_pass_keeps_stack();
#endif
    printf("expunge_refusal checks=%lu failures=%lu\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
