/*
 * src/bsdsocket/errno.c and netx_call.c on the host: when SBTC_ERROR_HOOK
 * runs.
 *
 * A hook called from inside the bsd_nx_enter() bracket runs while every stack
 * thread is parked, so one that blocks stalls all traffic.  The shipping code
 * records a change made inside the bracket and makes the call from the outer
 * bsd_nx_leave().  Both translation units are compiled here as shipped; the
 * ThreadX bracket underneath them is the stub below, and `h.baton_held` is
 * what each assertion reads at the moment the hook runs.
 *
 * Built twice: as configured, over ami_netstack_enter_cached(), and again
 * with AMINETXDUO_NXCACHE=0 (test_errno_hook_nocache) over the plain
 * ami_netstack_enter(), which is what the pathswap arm ships.  Neither stub
 * writes nc_Task: the plain bracket never does, so the code under test may
 * not read it to learn whose bracket this is.
 *
 * transfer.c is not here: its _Static_asserts pin struct iovec to the
 * target's pointer width and the file compiles only in the host32 tier.  The
 * send path's shape -- enter, bsd_fail(), leave, return -1 -- is restated in
 * t_vector_fail() below, on the real bsd_nx_enter()/bsd_nx_leave() and the
 * real bsd_fail().  SocketBaseTagList()'s pointer-carrying tags store a
 * pointer through a ULONG slot for the same reason, so the hook and the
 * errno mirror are installed through the base field and SetErrnoPtr(); the
 * tag vector itself is driven with its value tags.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <string.h>

/* netx_call.c's own default, so the banner below names the path built. */
#ifndef AMINETXDUO_NXCACHE
#  define AMINETXDUO_NXCACHE 1
#endif

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

struct ExecBase *SysBase;

static struct Task          h_task;
static struct AmiSocketBase h_base;
static struct Hook          h_hook;
static struct Hook          h_hook2;       /* the replacement, case 11 */
static LONG                 h_mirror;      /* SBTC_ERRNOLONGPTR */

#define H_LOG   16

/* One row per hook call: what it was handed and what the bracket was doing. */
typedef struct HCall
{
    ULONG   action;
    LONG    code;
    BOOL    baton_held;
    LONG    nest;
    LONG    errno_seen;         /* sb_Errno at call time              */
    LONG    mirror_seen;        /* the SBTC_ERRNOLONGPTR variable      */
    ULONG   size;
} HCall;

static struct
{
    BOOL    baton_held;
    ULONG   nx_enters, nx_leaves, releases;
    BOOL    double_enter;       /* enter_cached() while already held   */
    BOOL    stray_leave;        /* leave_cached() while not held       */

    HCall   calls[H_LOG];
    ULONG   ncalls;

    /* t_uninstall_during_flush() and t_replace_during_flush(): what the
       hook does to sb_ErrorHook on its errno call. */
    BOOL    uninstall;
    struct Hook *replace_with;
    ULONG   second_calls;       /* calls that reached h_hook2_fn        */
    ULONG   poisoned_calls;     /* calls that reached the poisoned entry */

    /* The re-entering hook, t_reentry(). */
    BOOL    reenter;
    LONG    reenter_status;     /* SocketBaseTagList() result inside   */
    LONG    reenter_vector;     /* t_vector_fail() result inside       */
    ULONG   reenter_calls_seen; /* ncalls when the inner vector returned */
} h;

static VOID h_reset(VOID)
{
    memset(&h, 0, sizeof(h));
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_hook, 0, sizeof(h_hook));
    memset(&h_hook2, 0, sizeof(h_hook2));
    h_mirror = 0;

    h_base.sb_Task      = &h_task;
    h_base.sb_StackRefs = 1;
}

/* ---------------------------------------------------------------- exec -- */

struct Task *FindTask(const char *name)
{
    (VOID)name;
    return &h_task;
}

/* ------------------------------------------------------- the bracket ---- */

static LONG h_enter(AmiNetCaller *caller)
{
    h.nx_enters++;

    /* The deadlock detector: a hook that re-enters a vector must find the
       bracket already given back. */
    if (h.baton_held)
        h.double_enter = TRUE;

    h.baton_held       = TRUE;
    caller->nc_Adopted = TRUE;
    /* nc_Task is left as it was, see the header. */

    return AMI_NET_OK;
}

static VOID h_leave(AmiNetCaller *caller)
{
    h.nx_leaves++;

    if (!h.baton_held)
        h.stray_leave = TRUE;

    h.baton_held       = FALSE;
    caller->nc_Adopted = FALSE;
}

LONG ami_netstack_enter(AmiNetCaller *caller)         { return h_enter(caller); }
VOID ami_netstack_leave(AmiNetCaller *caller)         { h_leave(caller); }
LONG ami_netstack_enter_cached(AmiNetCaller *caller)  { return h_enter(caller); }
VOID ami_netstack_leave_cached(AmiNetCaller *caller)  { h_leave(caller); }

VOID ami_netstack_release(AmiNetCaller *caller)
{
    h.releases++;
    caller->nc_Live = FALSE;
    caller->nc_Task = NULL;
}

/* --------------------------------------- what else errno.c reaches for -- */

LONG bsd_table_size(struct AmiSocketBase *base)
{
    (VOID)base;
    return 64;
}

LONG bsd_table_resize(struct AmiSocketBase *base, LONG size)
{
    (VOID)base;
    (VOID)size;
    return 0;
}

struct Hook *bsd_log_hook_get(VOID)
{
    return NULL;
}

BOOL bsd_log_hook_set(struct AmiSocketBase *base, struct Hook *hook)
{
    (VOID)base;
    (VOID)hook;
    return TRUE;
}

/* The stack is down as far as SBTC_SYSTEM_STATUS can tell: it still takes
   the bracket, which is what t_reentry() drives it for. */
const AmiConfig *netstack_config(VOID)     { return NULL; }
AmiNetStack     *netstack_get(VOID)        { return NULL; }
BOOL   netstack_interface_is_up(UWORD i)   { (VOID)i; return FALSE; }
VOID   netstack_dns_absorb_pending(VOID)   { }

UINT _nxe_ip_gateway_address_get(NX_IP *ip, ULONG *a)
{
    (VOID)ip;
    *a = 0UL;
    return NX_NOT_FOUND;
}

UINT _nxe_ip_info_get(NX_IP *ip, ULONG *a, ULONG *b, ULONG *c, ULONG *d,
                      ULONG *e, ULONG *f, ULONG *g, ULONG *i, ULONG *j,
                      ULONG *k)
{
    (VOID)ip; (VOID)a; (VOID)b; (VOID)c; (VOID)d; (VOID)e; (VOID)f;
    (VOID)g; (VOID)i; (VOID)j; (VOID)k;
    return NX_NOT_ENABLED;
}

/* --------------------------------------------------------- the vector --- */

/*
 * The send path's shape, transfer.c: inside the bracket, the stack refuses,
 * bsd_fail() names the errno and the vector leaves and returns -1.
 */
static LONG t_vector_fail(struct AmiSocketBase *base, LONG code)
{
    if (bsd_nx_enter(base) != 0)
        return bsd_fail(base, AMI_ENETDOWN);

    (VOID)bsd_fail(base, code);

    bsd_nx_leave(base);

    return -1;
}

/* ------------------------------------------------------------ the hook -- */

typedef union HostHookEntry
{
    ULONG (*raw)(VOID);
    LONG  (*fn)(struct Hook *hook, APTR reserved, struct ErrorHookMsg *ehm);
} HostHookEntry;

static LONG h_poison_fn(struct Hook *hook, APTR reserved,
                        struct ErrorHookMsg *ehm);

static LONG h_hook_fn(struct Hook *hook, APTR reserved,
                      struct ErrorHookMsg *ehm)
{
    HCall *c;

    (VOID)hook;
    (VOID)reserved;

    if (h.ncalls < H_LOG)
    {
        c = &h.calls[h.ncalls];
        c->action      = ehm->ehm_Action;
        c->code        = ehm->ehm_Code;
        c->size        = ehm->ehm_Size;
        c->baton_held  = h.baton_held;
        c->nest        = h_base.sb_NxNest;
        c->errno_seen  = h_base.sb_Errno;
        c->mirror_seen = h_mirror;
    }
    h.ncalls++;

    /* t_uninstall_during_flush() / t_replace_during_flush(): the hook takes
       itself out, or puts another in, on the errno call.  Through the base
       field: SocketBaseTagList(SBTM_SETVAL(SBTC_ERROR_HOOK)) stores the
       pointer through a ULONG slot and cannot carry one on this host (see
       the header).  The store the tag makes is this one. */
    if ((h.uninstall || h.replace_with != NULL) &&
        ehm->ehm_Action == EHMA_Set_errno)
    {
        HostHookEntry poison;

        h_base.sb_ErrorHook = h.replace_with;   /* NULL for the uninstall */
        h.uninstall         = FALSE;
        h.replace_with      = NULL;

        /* The freed Hook, reused: a stale call lands in h_poison_fn. */
        poison.fn     = h_poison_fn;
        hook->h_Entry = poison.raw;
    }

    /* t_reentry(): the hook itself is a bsdsocket caller.  Once only, or the
       inner vector's own errno would bring it straight back here. */
    if (h.reenter)
    {
        struct TagItem tags[2];

        h.reenter = FALSE;

        tags[0].ti_Tag  = SBTM_GETVAL(SBTC_SYSTEM_STATUS);
        tags[0].ti_Data = 0;
        tags[1].ti_Tag  = TAG_DONE;
        tags[1].ti_Data = 0;

        h.reenter_status     = bsd_SocketBaseTagList(tags, &h_base);
        h.reenter_vector     = t_vector_fail(&h_base, AMI_EINTR);
        h.reenter_calls_seen = h.ncalls;
    }

    return 0;
}

/* The replacement hook: counts, and logs like the first. */
static LONG h_hook2_fn(struct Hook *hook, APTR reserved,
                       struct ErrorHookMsg *ehm)
{
    (VOID)hook;
    (VOID)reserved;

    h.second_calls++;
    if (h.ncalls < H_LOG)
    {
        h.calls[h.ncalls].action     = ehm->ehm_Action;
        h.calls[h.ncalls].code       = ehm->ehm_Code;
        h.calls[h.ncalls].size       = ehm->ehm_Size;
        h.calls[h.ncalls].baton_held = h.baton_held;
        h.calls[h.ncalls].nest       = h_base.sb_NxNest;
    }
    h.ncalls++;

    return 0;
}

/* What a freed struct Hook's h_Entry is made to point at: a call through a
   stale pointer lands here and is counted rather than crashing. */
static LONG h_poison_fn(struct Hook *hook, APTR reserved,
                        struct ErrorHookMsg *ehm)
{
    (VOID)hook;
    (VOID)reserved;
    (VOID)ehm;

    h.poisoned_calls++;

    return 0;
}

static VOID h_install(VOID)
{
    HostHookEntry e;

    e.fn = h_hook_fn;
    h_hook.h_Entry = e.raw;

    h_base.sb_ErrorHook = &h_hook;

    /* SBTC_ERRNOLONGPTR, through the vector that takes the pointer as a
       pointer.  Publishes the current errno at once, as AmiTCP does. */
    bsd_SetErrnoPtr(&h_mirror, 4, &h_base);
}

static BOOL h_call_is(ULONG n, ULONG action, LONG code)
{
    return n < h.ncalls && n < H_LOG &&
           h.calls[n].action == action && h.calls[n].code == code &&
           h.calls[n].size == 12UL;
}

/* ------------------------------------------------------------- tests ---- */

/* (1) A non-blocking send refused inside the bracket. */
static VOID t_send_would_block(VOID)
{
    printf("errno hook: a send refused inside the bracket\n");

    h_reset();
    h_install();
    h.ncalls = 0;               /* the install's own publication */

    CHECK(t_vector_fail(&h_base, AMI_EWOULDBLOCK) == -1, "the send fails");
    CHECK(h.ncalls == 1, "the hook was called exactly once");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_EWOULDBLOCK),
          "with EHMA_Set_errno and EWOULDBLOCK, in a twelve-byte message");
    CHECK(h.calls[0].baton_held == FALSE, "after the bracket was given back");
    CHECK(h.calls[0].nest == 0, "with the nest count at zero");
    CHECK(h.calls[0].errno_seen == AMI_EWOULDBLOCK,
          "sb_Errno already held the code");
    CHECK(h.calls[0].mirror_seen == AMI_EWOULDBLOCK,
          "and so did the SBTC_ERRNOLONGPTR mirror");
    CHECK(h.nx_enters == 1 && h.nx_leaves == 1, "one bracket, given back");
    CHECK(h_base.sb_ErrPending == 0, "nothing is left pending");
}

/* (2) Two errno sets inside one bracket: one call, the last code. */
static VOID t_last_wins(VOID)
{
    printf("errno hook: two changes inside one bracket\n");

    h_reset();
    h_install();
    h.ncalls = 0;

    CHECK(bsd_nx_enter(&h_base) == 0, "entered");
    (VOID)bsd_fail(&h_base, AMI_EAGAIN);
    (VOID)bsd_fail(&h_base, AMI_ECONNRESET);
    CHECK(h.ncalls == 0, "nothing delivered while the bracket is held");
    bsd_nx_leave(&h_base);

    CHECK(h.ncalls == 1, "one call");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_ECONNRESET),
          "carrying the last code set");
}

/* (3) errno and h_errno inside one bracket: two calls, errno first. */
static VOID t_both_variables(VOID)
{
    printf("errno hook: errno and h_errno inside one bracket\n");

    h_reset();
    h_install();
    h.ncalls = 0;

    CHECK(bsd_nx_enter(&h_base) == 0, "entered");
    bsd_set_herrno(&h_base, HOST_NOT_FOUND);
    (VOID)bsd_fail(&h_base, AMI_ETIMEDOUT);
    CHECK(h.ncalls == 0, "nothing delivered while the bracket is held");
    bsd_nx_leave(&h_base);

    CHECK(h.ncalls == 2, "two calls after the leave");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_ETIMEDOUT), "errno first");
    CHECK(h_call_is(1, EHMA_Set_h_errno, HOST_NOT_FOUND), "then h_errno");
    CHECK(h.calls[0].baton_held == FALSE && h.calls[1].baton_held == FALSE,
          "both outside the bracket");
    CHECK(h_base.sb_HErrno == HOST_NOT_FOUND, "h_errno itself was set");
}

/* (4) A nested bracket: the call comes at the outer leave. */
static VOID t_nested_bracket(VOID)
{
    printf("errno hook: a nested bracket\n");

    h_reset();
    h_install();
    h.ncalls = 0;

    CHECK(bsd_nx_enter(&h_base) == 0, "outer enter");
    CHECK(bsd_nx_enter(&h_base) == 0, "inner enter");
    CHECK(h.nx_enters == 1, "which nests rather than adopting again");
    (VOID)bsd_fail(&h_base, AMI_EPIPE);

    bsd_nx_leave(&h_base);
    CHECK(h.ncalls == 0, "the inner leave delivers nothing");
    CHECK(h.baton_held == TRUE, "because the bracket is still held");

    bsd_nx_leave(&h_base);
    CHECK(h.ncalls == 1, "the outer leave delivers it");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_EPIPE), "with the code");
    CHECK(h.calls[0].baton_held == FALSE, "outside the bracket");
}

/* (5) No bracket at all: the call is immediate, same ABI. */
static VOID t_outside_bracket(VOID)
{
    struct TagItem tags[2];

    printf("errno hook: a change outside any bracket\n");

    h_reset();
    h_install();
    h.ncalls = 0;

    (VOID)bsd_fail(&h_base, AMI_EBADF);
    CHECK(h.ncalls == 1, "called at once");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_EBADF), "with the code");
    CHECK(h.nx_enters == 0, "no bracket was taken");
    CHECK(h.calls[0].errno_seen == AMI_EBADF &&
          h.calls[0].mirror_seen == AMI_EBADF,
          "sb_Errno and the mirror already held it");

    /* SocketBaseTagList(SBTC_ERRNO) is the one vector that sets errno
       without a bracket: the shipping entry point, same path. */
    tags[0].ti_Tag  = SBTM_SETVAL(SBTC_ERRNO);
    tags[0].ti_Data = (uintptr_t)AMI_EINVAL;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    CHECK(bsd_SocketBaseTagList(tags, &h_base) == 0, "the tag is serviced");
    CHECK(h.ncalls == 2 && h_call_is(1, EHMA_Set_errno, AMI_EINVAL),
          "and the hook is called at once");

    bsd_set_herrno(&h_base, TRY_AGAIN);
    CHECK(h.ncalls == 3 && h_call_is(2, EHMA_Set_h_errno, TRY_AGAIN),
          "h_errno too");
}

/* (6) A hook that is itself a bsdsocket caller. */
static VOID t_reentry(VOID)
{
    printf("errno hook: a hook that calls a vector\n");

    h_reset();
    h_install();
    h.ncalls  = 0;
    h.reenter = TRUE;

    CHECK(t_vector_fail(&h_base, AMI_EWOULDBLOCK) == -1, "the send fails");

    CHECK(h.reenter == FALSE, "the hook ran and re-entered");
    CHECK(h.double_enter == FALSE,
          "the nested vector found the bracket free, not held: no deadlock");
    CHECK(h.stray_leave == FALSE, "and every leave matched an enter");
    CHECK(h.reenter_status == 0, "SocketBaseTagList() inside the hook succeeded");
    CHECK(h.reenter_vector == -1, "the inner failing vector failed as asked");
    CHECK(h.nx_enters == 3 && h.nx_leaves == 3,
          "three brackets: the send, the tag query, the inner vector");

    CHECK(h.ncalls == 2, "two hook calls in all");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_EWOULDBLOCK),
          "the outer vector's code first");
    CHECK(h_call_is(1, EHMA_Set_errno, AMI_EINTR),
          "then the inner vector's, on its own leave");
    CHECK(h.reenter_calls_seen == 2,
          "which had run by the time the inner vector returned");
    CHECK(h.calls[1].baton_held == FALSE && h.calls[1].nest == 0,
          "outside the inner bracket");
    CHECK(h_base.sb_Errno == AMI_EINTR,
          "and errno is the inner vector's, the last one set");
    CHECK(h_base.sb_ErrPending == 0, "nothing is left pending");
}

/* (7) bsd_nx_release() with a code pending: dropped, not delivered.  Then
   bsd_task_sweep()'s sequence on a base whose task has gone (library.c),
   which resets the same fields by hand: the same drop. */
static VOID t_release_drops(VOID)
{
    printf("errno hook: release with a code pending\n");

    h_reset();
    h_install();
    h.ncalls = 0;

    CHECK(bsd_nx_enter(&h_base) == 0, "entered");
    (VOID)bsd_fail(&h_base, AMI_ENOTCONN);
    CHECK(h_base.sb_ErrPending != 0, "the code is pending");

    bsd_nx_release(&h_base);

    CHECK(h.releases == 1, "the registration was released");
    CHECK(h.ncalls == 0, "the hook was not called");
    CHECK(h_base.sb_ErrPending == 0, "and nothing is pending");
    CHECK(h_base.sb_NxNest == 0, "the nest count is reset");
    CHECK(h_base.sb_NxTask == NULL, "and the holder");
    CHECK(h_base.sb_Errno == AMI_ENOTCONN, "errno itself kept the code");

    /* A later change is delivered at once: the base is outside. */
    (VOID)bsd_fail(&h_base, AMI_EBADF);
    CHECK(h.ncalls == 1 && h_call_is(0, EHMA_Set_errno, AMI_EBADF),
          "the next change, outside, is delivered at once");

    printf("errno hook: the task sweep with a code pending\n");

    h_reset();
    h_install();
    h.ncalls = 0;

    CHECK(bsd_nx_enter(&h_base) == 0, "entered");
    (VOID)bsd_fail(&h_base, AMI_ENOTCONN);
    CHECK(h_base.sb_ErrPending != 0, "the code is pending");

    /* bsd_task_sweep(), library.c, on a child whose task has exited. */
    h_base.sb_NxNest     = 0;
    h_base.sb_NxTask     = NULL;
    h_base.sb_ErrPending = 0;
    ami_netstack_release(&h_base.sb_NxCaller);

    CHECK(h.ncalls == 0, "the hook was not called");
    CHECK(h_base.sb_ErrPending == 0, "nothing is pending");

    /* Nothing arrives later either: the code is gone, not deferred. */
    CHECK(bsd_nx_enter(&h_base) == 0, "a fresh bracket");
    bsd_nx_leave(&h_base);
    CHECK(h.ncalls == 0, "delivers nothing from before the sweep");
}

/* (8) No hook: the fast path records nothing. */
static VOID t_no_hook(VOID)
{
    struct Hook blank;

    printf("errno hook: no hook installed\n");

    h_reset();
    bsd_SetErrnoPtr(&h_mirror, 4, &h_base);

    CHECK(bsd_nx_enter(&h_base) == 0, "entered");
    (VOID)bsd_fail(&h_base, AMI_ECONNREFUSED);
    bsd_set_herrno(&h_base, NO_DATA);
    CHECK(h_base.sb_ErrPending == 0, "nothing is recorded with no hook");
    bsd_nx_leave(&h_base);

    CHECK(h.ncalls == 0, "nothing was called");
    CHECK(h_base.sb_Errno == AMI_ECONNREFUSED && h_mirror == AMI_ECONNREFUSED,
          "errno and the mirror were set all the same");
    CHECK(h_base.sb_HErrno == NO_DATA, "and h_errno");

    /* A hook with no h_Entry is no hook. */
    memset(&blank, 0, sizeof(blank));
    h_base.sb_ErrorHook = &blank;

    CHECK(bsd_nx_enter(&h_base) == 0, "entered");
    (VOID)bsd_fail(&h_base, AMI_EACCES);
    CHECK(h_base.sb_ErrPending == 0, "a hook with no entry records nothing");
    bsd_nx_leave(&h_base);
    CHECK(h.ncalls == 0, "and is not called");
}

/*
 * (9) The bracket that never names its task.  ami_netstack_enter() writes
 * nc_Task on no path, and a NetX thread calling in gets AMI_NET_OK before
 * anything is written; the plain (AMINETXDUO_NXCACHE=0) bracket is what the
 * pathswap arm ships.  The deferral has to hold with nc_Task NULL throughout.
 */
static VOID t_task_not_named(VOID)
{
    printf("errno hook: a bracket that never writes nc_Task\n");

    h_reset();
    h_install();
    h.ncalls = 0;

    CHECK(h_base.sb_NxCaller.nc_Task == NULL, "nc_Task starts NULL");

    CHECK(bsd_nx_enter(&h_base) == 0, "outer enter");
    CHECK(h_base.sb_NxCaller.nc_Task == NULL, "and the bracket leaves it so");
    CHECK(h_base.sb_NxTask == FindTask(NULL), "the base names its holder itself");

    CHECK(bsd_nx_enter(&h_base) == 0, "inner enter");
    (VOID)bsd_fail(&h_base, AMI_ECONNRESET);
    bsd_set_herrno(&h_base, NO_RECOVERY);
    CHECK(h.ncalls == 0, "no call inside the bracket");
    CHECK(h_base.sb_ErrPending == 0x03, "both are pending");

    bsd_nx_leave(&h_base);
    CHECK(h.ncalls == 0, "the nested leave flushes nothing");
    CHECK(h_base.sb_ErrPending == 0x03, "and drops nothing");

    bsd_nx_leave(&h_base);
    CHECK(h.ncalls == 2, "the outermost leave delivers both");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_ECONNRESET) &&
          h_call_is(1, EHMA_Set_h_errno, NO_RECOVERY),
          "errno first, then h_errno");
    CHECK(h.calls[0].baton_held == FALSE && h.calls[1].baton_held == FALSE,
          "after the bracket was given back");
    CHECK(h_base.sb_NxTask == NULL, "and the holder is cleared");
    CHECK(h_base.sb_NxCaller.nc_Task == NULL, "nc_Task was never written");
}

/*
 * (10) A hook that uninstalls itself on the errno call, with h_errno still to
 * deliver.  The flush must not call through the pointer it read before the
 * first call: the caller may have freed that struct Hook by then.  The hook
 * poisons its own h_Entry as it uninstalls -- what a freed and reused Hook
 * would look like -- so a call through the stale pointer is counted.
 */
static VOID t_uninstall_during_flush(VOID)
{
    printf("errno hook: the hook uninstalls itself mid-flush\n");

    h_reset();
    h_install();
    h.ncalls    = 0;
    h.uninstall = TRUE;

    CHECK(bsd_nx_enter(&h_base) == 0, "entered");
    (VOID)bsd_fail(&h_base, AMI_ETIMEDOUT);
    bsd_set_herrno(&h_base, HOST_NOT_FOUND);
    CHECK(h_base.sb_ErrPending == 0x03, "both pending");

    bsd_nx_leave(&h_base);

    CHECK(h.ncalls == 1, "one call reached the hook");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_ETIMEDOUT), "the errno call");
    CHECK(h_base.sb_ErrorHook == NULL, "which uninstalled the hook");
    CHECK(h.poisoned_calls == 0, "the h_errno call did not go through the "
          "old pointer");
    CHECK(h_base.sb_ErrPending == 0, "nothing is left pending");
    CHECK(h_base.sb_HErrno == HOST_NOT_FOUND, "h_errno itself was set");

    /* And the base is quiet afterwards: no hook, no calls. */
    (VOID)bsd_fail(&h_base, AMI_EBADF);
    CHECK(h.ncalls == 1 && h.poisoned_calls == 0, "and stays uninstalled");
}

/*
 * (11) A hook that replaces itself on the errno call: the h_errno call goes
 * to the new hook, not the old.
 */
static VOID t_replace_during_flush(VOID)
{
    HostHookEntry e;

    printf("errno hook: the hook replaces itself mid-flush\n");

    h_reset();
    h_install();
    h.ncalls = 0;

    e.fn = h_hook2_fn;
    h_hook2.h_Entry = e.raw;
    h.replace_with  = &h_hook2;

    CHECK(bsd_nx_enter(&h_base) == 0, "entered");
    (VOID)bsd_fail(&h_base, AMI_ECONNREFUSED);
    bsd_set_herrno(&h_base, TRY_AGAIN);

    bsd_nx_leave(&h_base);

    CHECK(h.ncalls == 2, "two calls in all");
    CHECK(h_call_is(0, EHMA_Set_errno, AMI_ECONNREFUSED),
          "the errno call went to the first hook");
    CHECK(h.second_calls == 1 && h_call_is(1, EHMA_Set_h_errno, TRY_AGAIN),
          "the h_errno call went to its replacement");
    CHECK(h.poisoned_calls == 0, "and not through the old pointer");
    CHECK(h_base.sb_ErrorHook == &h_hook2, "which stays installed");

    /* The old hook's entry was poisoned as it was replaced; a later change
       reaches only the new one. */
    (VOID)bsd_fail(&h_base, AMI_EBADF);
    CHECK(h.second_calls == 2 && h.poisoned_calls == 0,
          "a later change reaches the new hook only");
}

int main(void)
{
#if AMINETXDUO_NXCACHE
    printf("errno.c / netx_call.c host checks: SBTC_ERROR_HOOK, cached bracket\n\n");
#else
    printf("errno.c / netx_call.c host checks: SBTC_ERROR_HOOK, plain bracket\n\n");
#endif

    t_send_would_block();
    t_last_wins();
    t_both_variables();
    t_nested_bracket();
    t_outside_bracket();
    t_reentry();
    t_release_drops();
    t_no_hook();
    t_task_not_named();
    t_uninstall_during_flush();
    t_replace_during_flush();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
