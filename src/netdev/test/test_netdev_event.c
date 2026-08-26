/*
 * S2_ONEVENT delivery and the S2_PacketFilter hook, on the host.
 *
 * The expectations are the SANA-II specification's, not this code's output:
 * sana2device.spec S2_ONEVENT NOTES and RESULTS, and the PacketFilter autodoc
 * in copybuff.spec.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

#include "netdev_internal.h"

static int failures;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
    {
        printf("ok   %s = 0x%lx\n", what, got);
        return;
    }

    printf("FAIL %s: got 0x%lx, want 0x%lx\n", what, got, want);
    failures++;
}

static void expect_int(const char *what, int got, int want)
{
    if (got == want)
    {
        printf("ok   %s = %d\n", what, got);
        return;
    }

    printf("FAIL %s: got %d, want %d\n", what, got, want);
    failures++;
}

static void expect_ptr(const char *what, const void *got, const void *want)
{
    if (got == want)
    {
        printf("ok   %s\n", what);
        return;
    }

    printf("FAIL %s: got %p, want %p\n", what, (void *)got, (void *)want);
    failures++;
}

/* ------------------------------------------------------------- the exec -- */

static int disable_depth;
static int disable_min;        /* how deep the mask was when a reply landed */
static int replies;

VOID Disable(VOID)
{
    disable_depth++;
}

VOID Enable(VOID)
{
    disable_depth--;
    if (disable_depth < 0)
    {
        printf("FAIL Enable() without a matching Disable()\n");
        failures++;
        disable_depth = 0;
    }
}

/*
 * The one thing the driver must not do: complete somebody else's IORequest
 * while it is still on the list it was taken from.  ln_Succ is nulled by
 * Remove() below, so a reply from a node still linked is caught here.
 */
VOID ReplyMsg(struct Message *msg)
{
    replies++;
    if (disable_depth < disable_min)
        disable_min = disable_depth;
    if (msg->mn_Node.ln_Succ != NULL)
    {
        printf("FAIL ReplyMsg() on a node still linked into a list\n");
        failures++;
    }
    msg->mn_Node.ln_Type = NT_REPLYMSG;
}

VOID NewList(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

VOID AddTail(struct List *l, struct Node *n)
{
    n->ln_Succ            = (struct Node *)&l->lh_Tail;
    n->ln_Pred            = l->lh_TailPred;
    l->lh_TailPred->ln_Succ = n;
    l->lh_TailPred        = n;
}

VOID AddHead(struct List *l, struct Node *n)
{
    n->ln_Succ         = l->lh_Head;
    n->ln_Pred         = (struct Node *)&l->lh_Head;
    l->lh_Head->ln_Pred = n;
    l->lh_Head         = n;
}

VOID Remove(struct Node *n)
{
    n->ln_Pred->ln_Succ = n->ln_Succ;
    n->ln_Succ->ln_Pred = n->ln_Pred;
    n->ln_Succ = NULL;
    n->ln_Pred = NULL;
}

struct Node *RemHead(struct List *l)
{
    struct Node *n = l->lh_Head;

    if (n->ln_Succ == NULL)
        return NULL;
    Remove(n);
    return n;
}

/* ------------------------------------------------------------- the hook -- */

/*
 * netdev_hook_call() is three pinned address registers and a jsr on the
 * Amiga.  On the host it is this, and what the test checks is that the three
 * arguments arrive in the roles the autodoc gives them:
 * a0 = hook, a2 = object (the IOSana2Req), a1 = message (the packet data).
 */
static APTR  hook_saw_hook;
static APTR  hook_saw_object;
static APTR  hook_saw_message;
static int   hook_calls;
static BOOL  hook_answer = TRUE;

BOOL netdev_hook_call(APTR hook, APTR object, APTR message)
{
    hook_calls++;
    hook_saw_hook    = hook;
    hook_saw_object  = object;
    hook_saw_message = message;
    return hook_answer;
}

/* netdev_event.c calls it through netdev_reply(), which lives in
   netdev_device.c and is three lines of it. */
VOID netdev_reply(struct IOSana2Req *io, LONG err, ULONG wire)
{
    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError    = wire;

    if ((io->ios2_Req.io_Flags & IOF_QUICK) != 0)
        return;

    ReplyMsg(&io->ios2_Req.io_Message);
}

/* ------------------------------------------------------------ the fixture */

static NetdevUnit   unit;
static NetdevOpener opener_a;
static NetdevOpener opener_b;

static void reset_fixture(void)
{
    memset(&unit, 0, sizeof(unit));
    memset(&opener_a, 0, sizeof(opener_a));
    memset(&opener_b, 0, sizeof(opener_b));

    NewList(&unit.nu_OpenerList);
    NewList(&unit.nu_Writes);

    opener_a.op_Hw = &unit;
    opener_b.op_Hw = &unit;
    NewList(&opener_a.op_Reads);
    NewList(&opener_a.op_Orphans);
    NewList(&opener_a.op_Events);
    NewList(&opener_b.op_Reads);
    NewList(&opener_b.op_Orphans);
    NewList(&opener_b.op_Events);

    AddTail(&unit.nu_OpenerList, (struct Node *)&opener_a.op_Node);
    AddTail(&unit.nu_OpenerList, (struct Node *)&opener_b.op_Node);

    replies      = 0;
    disable_min  = 0x7fffffff;
    hook_calls   = 0;
    hook_answer  = TRUE;
}

static void wait_for(NetdevOpener *op, struct IOSana2Req *io, ULONG mask)
{
    memset(io, 0, sizeof(*io));
    io->ios2_Req.io_Unit  = &op->op_Unit;
    io->ios2_Req.io_Flags = IOF_QUICK;      /* as a caller's DoIO() leaves it */
    io->ios2_WireError    = mask;
    netdev_event_wait(&unit, io);
}

static int queued(struct List *l)
{
    struct Node *n;
    int          c = 0;

    for (n = l->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        c++;
    return c;
}

/* ------------------------------------------------------------- the tests -- */

/* Queueing: IOF_QUICK must be cleared, or the reply never reaches the
   caller's port, and the gate word must carry the mask. */
static void test_queue(void)
{
    struct IOSana2Req io;

    printf("\n-- S2_ONEVENT queueing\n");
    reset_fixture();
    wait_for(&opener_a, &io, S2EVENT_TX | S2EVENT_RX);

    expect_int("queued on the opener", queued(&opener_a.op_Events), 1);
    expect_int("IOF_QUICK cleared", (io.ios2_Req.io_Flags & IOF_QUICK) == 0, 1);
    expect_int("ln_Type is NT_MESSAGE",
               io.ios2_Req.io_Message.mn_Node.ln_Type, NT_MESSAGE);
    expect_u32("nu_EventMask", unit.nu_EventMask,
               S2EVENT_TX | S2EVENT_RX);
    expect_int("nothing replied yet", replies, 0);
    expect_int("Disable() balanced", disable_depth, 0);
}

static void test_queue_head(void)
{
    struct IOSana2Req first;
    struct IOSana2Req retry;
    struct Node       *node;

    printf("\n-- late-busy transmit requeue\n");
    reset_fixture();
    memset(&first, 0, sizeof(first));
    memset(&retry, 0, sizeof(retry));
    first.ios2_Req.io_Flags = IOF_QUICK;
    retry.ios2_Req.io_Flags = IOF_QUICK;

    netdev_queue_tail(&unit.nu_Writes, &first);
    netdev_queue_head(&unit.nu_Writes, &retry);

    expect_int("two writes queued", queued(&unit.nu_Writes), 2);
    expect_ptr("busy retry returned to the head", unit.nu_Writes.lh_Head,
               &retry.ios2_Req.io_Message.mn_Node);
    expect_int("retry IOF_QUICK cleared",
               (retry.ios2_Req.io_Flags & IOF_QUICK) == 0, 1);
    expect_int("retry ln_Type is NT_MESSAGE",
               retry.ios2_Req.io_Message.mn_Node.ln_Type, NT_MESSAGE);

    node = RemHead(&unit.nu_Writes);
    expect_ptr("retry dequeued first", node,
               &retry.ios2_Req.io_Message.mn_Node);
    netdev_reply(&retry, 0, 0);
    expect_int("eventual completion was replied", replies, 1);
}

/* ONLINE/OFFLINE are level-like events: a wait completes when its requested
   state is already true and must never be linked for a future transition. */
static void test_current_state(void)
{
    struct IOSana2Req io;

    printf("\n-- current ONLINE/OFFLINE state\n");
    reset_fixture();
    memset(&io, 0, sizeof(io));
    io.ios2_Req.io_Unit = &opener_a.op_Unit;
    io.ios2_WireError   = S2EVENT_OFFLINE;

    netdev_event_wait(&unit, &io);

    expect_int("OFFLINE returned immediately", replies, 1);
    expect_int("the request was not queued", queued(&opener_a.op_Events), 0);
    expect_u32("the returned state", io.ios2_WireError, S2EVENT_OFFLINE);
    expect_u32("the gate stayed empty", unit.nu_EventMask, 0);

    reset_fixture();
    unit.nu_Online = 1;
    memset(&io, 0, sizeof(io));
    io.ios2_Req.io_Unit = &opener_a.op_Unit;
    io.ios2_WireError   = S2EVENT_ONLINE | S2EVENT_TX;

    netdev_event_wait(&unit, &io);

    expect_int("ONLINE returned immediately", replies, 1);
    expect_u32("only the current state was returned", io.ios2_WireError,
               S2EVENT_ONLINE);
    expect_int("ONLINE was not queued", queued(&opener_a.op_Events), 0);
}

/*
 * A request matches on any bit in common, not on equality.  Three cases, and
 * the middle one is the spec's own worked example: a buffer-management failure
 * during receive posts ERROR|RX|BUFF, and a caller that asked only for
 * S2EVENT_ERROR is woken by it.
 */
static void test_overlap(void)
{
    struct IOSana2Req err, rx, buff, state;

    printf("\n-- any bit in common is a match\n");
    reset_fixture();
    wait_for(&opener_a, &err,  S2EVENT_ERROR);
    wait_for(&opener_a, &rx,   S2EVENT_RX | S2EVENT_TX);
    wait_for(&opener_b, &buff, S2EVENT_BUFF);
    wait_for(&opener_b, &state, S2EVENT_ONLINE);

    expect_u32("gate holds every queued bit", unit.nu_EventMask,
               S2EVENT_ERROR | S2EVENT_RX | S2EVENT_TX | S2EVENT_BUFF |
               S2EVENT_ONLINE);

    netdev_event(&unit, S2EVENT_ERROR | S2EVENT_RX | S2EVENT_BUFF);

    expect_int("three of the four returned", replies, 3);
    expect_int("the ONLINE waiter is still queued",
               queued(&opener_b.op_Events), 1);
    expect_int("opener a's list is empty", queued(&opener_a.op_Events), 0);

    /* "ios2_WireError - Mask of events that occured": the whole posted mask,
       not the part this caller asked about. */
    expect_u32("ERROR waiter's WireError", err.ios2_WireError,
               S2EVENT_ERROR | S2EVENT_RX | S2EVENT_BUFF);
    expect_u32("RX|TX waiter's WireError", rx.ios2_WireError,
               S2EVENT_ERROR | S2EVENT_RX | S2EVENT_BUFF);
    expect_int("io_Error is zero", err.ios2_Req.io_Error, 0);
    expect_int("ONLINE waiter untouched", state.ios2_Req.io_Error, 0);
    expect_u32("ONLINE waiter's mask survives", state.ios2_WireError,
               S2EVENT_ONLINE);

    expect_u32("gate now holds only what is left", unit.nu_EventMask,
               S2EVENT_ONLINE);
    expect_int("replies happened under Disable()", disable_min > 0, 1);
    expect_int("Disable() balanced", disable_depth, 0);
}

/* No bit in common: nothing is completed and nothing is disturbed. */
static void test_no_overlap(void)
{
    struct IOSana2Req io;

    printf("\n-- a post with no bit in common\n");
    reset_fixture();
    wait_for(&opener_a, &io, S2EVENT_BUFF);

    netdev_event(&unit, S2EVENT_ERROR | S2EVENT_RX);

    expect_int("nothing replied", replies, 0);
    expect_int("still queued", queued(&opener_a.op_Events), 1);
    expect_u32("mask untouched", io.ios2_WireError, S2EVENT_BUFF);
    expect_u32("gate untouched", unit.nu_EventMask, S2EVENT_BUFF);
}

/* "All pending requests for a particular event will be returned": two waiters
   for the same event on the same opener both come back. */
static void test_all_pending(void)
{
    struct IOSana2Req a, b, c;

    printf("\n-- every pending request, not the first\n");
    reset_fixture();
    wait_for(&opener_a, &a, S2EVENT_ONLINE);
    wait_for(&opener_a, &b, S2EVENT_ONLINE);
    wait_for(&opener_b, &c, S2EVENT_ONLINE | S2EVENT_ERROR);

    netdev_event(&unit, S2EVENT_ONLINE);

    expect_int("all three returned", replies, 3);
    expect_int("both lists empty",
               queued(&opener_a.op_Events) + queued(&opener_b.op_Events), 0);
    expect_u32("gate cleared", unit.nu_EventMask, 0);
}

/*
 * The gate is what makes an unwatched driver free, so it must be exactly the
 * union of what is queued.  Any less, and an event is silently swallowed.
 * netdev_event_rescan() is what the abort and close paths use to keep it so.
 */
static void test_gate(void)
{
    struct IOSana2Req a, b;

    printf("\n-- the nu_EventMask gate\n");
    reset_fixture();
    expect_u32("idle driver's gate", unit.nu_EventMask, 0);

    netdev_event(&unit, S2EVENT_ERROR | S2EVENT_RX);
    expect_int("a post with nobody waiting does nothing", replies, 0);
    expect_int("and takes no mask", disable_depth, 0);

    wait_for(&opener_a, &a, S2EVENT_TX);
    wait_for(&opener_b, &b, S2EVENT_HARDWARE);
    expect_u32("gate is the union", unit.nu_EventMask,
               S2EVENT_TX | S2EVENT_HARDWARE);

    /* An abort takes one off its list.  The rescan is what the driver runs
       next, and the other opener's bit must survive it. */
    Remove(&b.ios2_Req.io_Message.mn_Node);
    netdev_event_rescan(&unit);
    expect_u32("rescan drops only the removed one", unit.nu_EventMask,
               S2EVENT_TX);

    Remove(&a.ios2_Req.io_Message.mn_Node);
    netdev_event_rescan(&unit);
    expect_u32("rescan clears an empty driver", unit.nu_EventMask, 0);
}

/*
 * What the filter hook and CopyToBuff are shown.  "The data should NOT include
 * any hardware specific headers (unless of course the CMD_READ request wanted
 * RAW packets)".  RAW is either an opener property or a per-request flag, so
 * both must work.
 */
static void test_payload(void)
{
    static const UBYTE frame[64] = { 0 };
    struct IOSana2Req  io;
    const UBYTE       *p;
    ULONG              plen = 0;

    printf("\n-- what the filter is shown\n");
    reset_fixture();
    memset(&io, 0, sizeof(io));
    io.ios2_Req.io_Unit = &opener_a.op_Unit;

    p = netdev_payload(&opener_a, &io, frame, 64, &plen);
    expect_ptr("cooked: past the 14-byte header", p, frame + NETDEV_HDR_LEN);
    expect_u32("cooked: length", plen, 64 - NETDEV_HDR_LEN);

    io.ios2_Req.io_Flags = SANA2IOF_RAW;
    p = netdev_payload(&opener_a, &io, frame, 64, &plen);
    expect_ptr("SANA2IOF_RAW on the request: the whole frame", p, frame);
    expect_u32("SANA2IOF_RAW: length", plen, 64);

    io.ios2_Req.io_Flags = 0;
    opener_a.op_Raw = 1;
    p = netdev_payload(&opener_a, &io, frame, 64, &plen);
    expect_ptr("opener opened RAW: the whole frame", p, frame);
    expect_u32("opener RAW: length", plen, 64);
    opener_a.op_Raw = 0;
}

/* Accept, reject, and the free path for an opener that installed no hook. */
static void test_filter(void)
{
    static const UBYTE frame[64] = { 0 };
    static ULONG       fake_hook;
    struct IOSana2Req  io;

    printf("\n-- the S2_PacketFilter hook\n");
    reset_fixture();
    memset(&io, 0, sizeof(io));
    io.ios2_Req.io_Unit = &opener_a.op_Unit;

    expect_int("no hook installed: accepted",
               netdev_filter_ok(&opener_a, &io, frame), 1);
    expect_int("and nothing was called", hook_calls, 0);

    opener_a.op_Filter = &fake_hook;
    hook_answer = TRUE;
    expect_int("hook returned TRUE: accepted",
               netdev_filter_ok(&opener_a, &io, frame + NETDEV_HDR_LEN), 1);
    expect_int("the hook was called once", hook_calls, 1);
    expect_ptr("a0 is the hook itself", hook_saw_hook, &fake_hook);
    expect_ptr("a2 is the IOSana2Req", hook_saw_object, &io);
    expect_ptr("a1 is the packet data", hook_saw_message,
               (const void *)(frame + NETDEV_HDR_LEN));

    hook_answer = FALSE;
    expect_int("hook returned FALSE: rejected",
               netdev_filter_ok(&opener_a, &io, frame), 0);
    expect_int("the hook was called again", hook_calls, 2);
    expect_int("a rejection replies to nothing", replies, 0);
}

int main(void)
{
    test_queue();
    test_queue_head();
    test_current_state();
    test_overlap();
    test_no_overlap();
    test_all_pending();
    test_gate();
    test_payload();
    test_filter();

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
