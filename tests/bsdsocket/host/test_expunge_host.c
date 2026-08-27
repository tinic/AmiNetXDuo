/*
 * bsd_lib_expunge() on the host: does it DECLINE while the netstack says the
 * segment cannot be unloaded, and does it stop declining once that clears.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_internal.h"

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

/* The list Exec keeps the library on, so Remove() has something real to do
   and "still in the list" is a question with an answer. */
static struct List           h_liblist;

static struct
{
    LONG    can_unload_calls;
    BOOL    can_unload_answer;

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
    LONG    alloc_signal_calls;
    BYTE    alloc_signal_result;
    LONG    free_signal_calls;
    LONG    create_proc_calls;
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

/*
 * Reached, and expected.
 */
BOOL netstack_can_unload(VOID)
{
    h.can_unload_calls++;
    return h.can_unload_answer;
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
VOID bsd_runtime_close(VOID)        { h.runtime_close_calls++; }

/* bsd_lib_open() calls this on every open, to hold usergroup.library resident
   for ixemul clients.  Nothing here depends on it, and the real one only opens
   a library, so it is a no-op rather than an h_unreachable(). */
VOID bsd_usergroup_open(VOID)       { }

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

VOID netstack_shutdown(VOID)        { h.shutdown_calls++; }

/* bsd_task_sweep() discards a dead opener's ThreadX registration through
   this. Nothing here adopts, so there is never one to discard; the stub
   exists because library.c is compiled whole. */
VOID ami_netstack_release(AmiNetCaller *caller) { (VOID)caller; }

/* Harmless, and reached by bsd_lib_close() on the way past. */
VOID ObtainSemaphore(struct SignalSemaphore *s)  { (VOID)s; }
VOID ReleaseSemaphore(struct SignalSemaphore *s) { (VOID)s; }
ULONG AttemptSemaphore(struct SignalSemaphore *s) { (VOID)s; return 1UL; }
VOID InitSemaphore(struct SignalSemaphore *s)    { (VOID)s; }
VOID Forbid(VOID)                                { }
VOID Permit(VOID)                                { }
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

APTR AllocMem(ULONG s, ULONG r)  { (VOID)s; (VOID)r; h_unreachable("AllocMem");  return NULL; }
VOID CopyMem(const APTR s, APTR d, ULONG n) { (VOID)s; (VOID)d; (VOID)n; h_unreachable("CopyMem"); }
VOID AddTail(struct List *l, struct Node *n) { (VOID)l; (VOID)n; h_unreachable("AddTail"); }
struct Task *FindTask(const char *n) { (VOID)n; return &h_task; }
VOID Signal(struct Task *t, ULONG s) { (VOID)t; (VOID)s; h_unreachable("Signal"); }
ULONG Wait(ULONG s) { (VOID)s; h_unreachable("Wait"); return 0UL; }
BYTE AllocSignal(LONG n) { (VOID)n; h.alloc_signal_calls++; return h.alloc_signal_result; }
VOID FreeSignal(LONG n) { (VOID)n; h.free_signal_calls++; }
VOID CloseDevice(struct IORequest *io) { (VOID)io; h_unreachable("CloseDevice"); }
struct Process *CreateNewProc(const struct TagItem *t) { (VOID)t; h.create_proc_calls++; return NULL; }

/* Not h_unreachable(): AMI_WARN is on paths this test drives, and it is
   compiled into every build now rather than out of the default one. */
VOID ami_log(int level, const char *fmt, ...) { (VOID)level; (VOID)fmt; }
VOID ami_free(APTR p) { (VOID)p; h_unreachable("ami_free"); }
VOID ami_mem_open_delta(LONG d) { (VOID)d; h_unreachable("ami_mem_open_delta"); }
LONG ami_netdb_load(VOID) { return 0; }
BYTE ami_signal_alloc(VOID) { h_unreachable("ami_signal_alloc"); return -1; }
VOID ami_signal_free(BYTE s) { (VOID)s; h_unreachable("ami_signal_free"); }
VOID bsd_bpf_close_all(struct AmiSocketBase *b) { (VOID)b; h_unreachable("bsd_bpf_close_all"); }
VOID bsd_close_all(struct AmiSocketBase *b) { (VOID)b; h_unreachable("bsd_close_all"); }
VOID bsd_handoff_flush(struct AmiSocketBase *b) { (VOID)b; h_unreachable("bsd_handoff_flush"); }
VOID bsd_handoff_init(struct AmiSocketBase *b) { (VOID)b; h_unreachable("bsd_handoff_init"); }
VOID bsd_nx_release(struct AmiSocketBase *b) { (VOID)b; h_unreachable("bsd_nx_release"); }
BOOL bsd_runtime_open(VOID) { h_unreachable("bsd_runtime_open"); return FALSE; }
VOID bsd_tcp_handler_start(struct AmiSocketBase *m) { (VOID)m; h_unreachable("bsd_tcp_handler_start"); }
LONG netstack_startup(VOID) { h.startup_calls++; return h.startup_result; }
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
    h_base->sb_StackRefs = 1;       /* the launching opener */

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
    CHECK(h.can_unload_calls == 1,
          "and records whether teardown made the segment unloadable");

    h_report("transient", 0, 0, 0, h_teardown_ran());
}

static VOID t_startup_fallback_ownership(VOID)
{
    struct AmiSocketBase *opened;

    printf("failed startup without a child Process\n");

    h_machine_reset(TRUE);
    h.startup_result = AMI_NET_ERR_CONFIG;
    h.alloc_signal_result = (BYTE)-1;

    opened = bsd_lib_open(0UL, h_base);

    CHECK(opened == NULL, "signal-exhausted startup refused the open");
    CHECK(h.startup_calls == 1, "signal fallback attempted startup once");
    CHECK(h.shutdown_calls == 1,
          "signal fallback released the failed startup reference");
    CHECK(h_base->sb_StackRefs == 0, "signal fallback published no stack reference");
    CHECK(h_base->sb_Lib.lib_OpenCnt == 0, "signal fallback restored the open count");

    h_machine_reset(TRUE);
    h.startup_result = AMI_NET_ERR_CONFIG;
    h.alloc_signal_result = 5;

    opened = bsd_lib_open(0UL, h_base);

    CHECK(opened == NULL, "process-creation failure refused the open");
    CHECK(h.create_proc_calls == 1, "the child Process was attempted once");
    CHECK(h.free_signal_calls == 1, "the unused startup signal was freed");
    CHECK(h.startup_calls == 1, "process fallback attempted startup once");
    CHECK(h.shutdown_calls == 1,
          "process fallback released the failed startup reference");
    CHECK(h_base->sb_StackRefs == 0, "process fallback published no stack reference");
    CHECK(h_base->sb_Lib.lib_OpenCnt == 0, "process fallback restored the open count");
}

int main(void)
{
    printf("bsd_lib_expunge() host tests\n");

    t_refusal_declines();
    t_refusal_clears();
    t_open_count_comes_first();
    t_other_refusals();
    t_last_close_retries();
    t_transient_stack_reference();
    t_startup_fallback_ownership();

    printf("expunge_refusal checks=%lu failures=%lu\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
