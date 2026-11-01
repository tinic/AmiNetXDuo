/*
 * F-042: bsd_nx_enter()'s nesting counter is the library base's, and the
 * ThreadX bracket it stands for is one Task's.  A second Task reaching the
 * same base while the holder is inside must not nest on it: it would run NetX
 * with no TX_THREAD and no baton, and its leave could end the holder's
 * bracket.  It is refused, as ami_netstack_enter_cached() refuses a second
 * Task at depth 0.
 *
 * netx_call.c is #included; the netstack bracket below counts what it is
 * asked to do, and FindTask() answers whichever Task the case is.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
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

static struct Task  h_task_a;
static struct Task  h_task_b;
static struct Task *h_me = &h_task_a;
static int          h_enters;
static int          h_leaves;

/* bsd_nx_park() (#383): what the bracket looked like inside its Wait(). */
static struct AmiSocketBase *h_park_base;
static int                   h_wait_leaves;
static LONG                  h_wait_nest;
static int                   h_wait_refused;
static LONG                  h_enter_result = AMI_NET_OK;

struct Task *FindTask(const char *name) { (void)name; return h_me; }

LONG ami_netstack_enter_cached(AmiNetCaller *caller)
{
    (void)caller;
    h_enters++;
    return h_enter_result;
}

VOID ami_netstack_leave_cached(AmiNetCaller *caller)
{
    (void)caller;
    h_leaves++;
}

LONG ami_netstack_enter(AmiNetCaller *caller)
{
    return ami_netstack_enter_cached(caller);
}

VOID ami_netstack_leave(AmiNetCaller *caller)
{
    ami_netstack_leave_cached(caller);
}

VOID ami_netstack_release(AmiNetCaller *caller) { (void)caller; }

/* bsd_nx_park() puts a break back for the re-enter; none comes here. */
ULONG SetSignal(ULONG newSignals, ULONG signalSet)
{
    (void)newSignals;
    (void)signalSet;
    return 0UL;
}

/* bsd_nx_park()'s Wait(): what the bracket looked like while it waited. */
ULONG Wait(ULONG signalSet)
{
    struct Task *me = h_me;

    h_wait_leaves = h_leaves;
    h_wait_nest   = h_park_base->sb_NxNest;

    /* Task B tries the base while A is parked. */
    h_me = &h_task_b;
    h_wait_refused = (bsd_nx_enter(h_park_base) != 0);
    h_me = me;

    return signalSet;
}
VOID bsd_error_hook_flush(struct AmiSocketBase *base) { (void)base; }

#include "netx_call.c"

int main(void)
{
    static struct AmiSocketBase base;

    /* Task A takes the bracket and nests in it, as an entry point that calls
       another does. */
    h_me = &h_task_a;
    CHECK(bsd_nx_enter(&base) == 0, "A enters");
    CHECK(bsd_nx_enter(&base) == 0, "A nests");
    CHECK(base.sb_NxNest == 2 && h_enters == 1, "one bracket, depth two");

    /* THE CASE: Task B on the same base while A is inside.  Refused: no
       depth taken, no NetX context pretended. */
    h_me = &h_task_b;
    CHECK(bsd_nx_enter(&base) != 0, "B is refused while A holds the bracket");
    CHECK(base.sb_NxNest == 2, "and takes no depth");
    CHECK(base.sb_NxTask == &h_task_a, "the bracket is still A's");

    /* A unwinds its own bracket, and only its last leave ends it. */
    h_me = &h_task_a;
    bsd_nx_leave(&base);
    CHECK(base.sb_NxNest == 1 && h_leaves == 0, "A's inner leave keeps it");
    bsd_nx_leave(&base);
    CHECK(base.sb_NxNest == 0 && h_leaves == 1 && base.sb_NxTask == NULL,
          "A's outer leave ends it");

    /* Outside any bracket, B takes the ordinary path (where
       ami_netstack_enter_cached() decides about a second Task). */
    h_me = &h_task_b;
    CHECK(bsd_nx_enter(&base) == 0 && h_enters == 2, "B enters once A is out");
    CHECK(base.sb_NxTask == &h_task_b, "and holds it");
    bsd_nx_leave(&base);
    CHECK(base.sb_NxNest == 0 && h_leaves == 2, "and leaves");

    /* #383: A parks in a blocking receive.  The slot goes back for the
       Wait() (one leave), the bracket stays A's (depth 1, B refused), and
       A has it again afterwards (one enter). */
    {
        static struct AmiSocketBase pbase;
        ULONG received = 0;
        int   enters, leaves;

        h_park_base   = &pbase;
        pbase.sb_Task = &h_task_a;
        h_me = &h_task_a;
        CHECK(bsd_nx_enter(&pbase) == 0, "park: A enters");
        enters = h_enters;
        leaves = h_leaves;

        CHECK(bsd_nx_park(&pbase, 0x30UL, &received) == BSD_PARK_DONE,
              "park: done");
        CHECK(received == 0x30UL, "park: Wait()'s answer is handed back");
        CHECK(h_wait_leaves == leaves + 1, "park: the slot is given back first");
        CHECK(h_wait_nest == 1, "park: the bracket stays at depth 1");
        CHECK(h_wait_refused, "park: B is refused while A waits");
        CHECK(h_enters == enters + 1, "park: and taken again after");
        CHECK(pbase.sb_NxNest == 1 && pbase.sb_NxTask == &h_task_a,
              "park: A holds the bracket once more");

        /* Nested, the wait is not made here: an inner vector does not own
           the bracket. */
        CHECK(bsd_nx_enter(&pbase) == 0, "park: A nests");
        leaves = h_leaves;
        CHECK(bsd_nx_park(&pbase, 0x30UL, &received) == BSD_PARK_NONE,
              "park: refused at depth 2");
        CHECK(h_leaves == leaves, "park: and nothing left");
        bsd_nx_leave(&pbase);

        /* Not the opener: the event signal is not this Task's. */
        pbase.sb_Task = &h_task_b;
        CHECK(bsd_nx_park(&pbase, 0x30UL, &received) == BSD_PARK_NONE,
              "park: refused for a Task that did not open the base");
        pbase.sb_Task = &h_task_a;

        /* The stack is gone when A comes back: the bracket is closed. */
        h_enter_result = AMI_NET_ERR_STATE;
        CHECK(bsd_nx_park(&pbase, 0x30UL, &received) == BSD_PARK_LOST,
              "park: lost when the stack cannot be entered");
        CHECK(pbase.sb_NxNest == 0 && pbase.sb_NxTask == NULL,
              "park: and the bracket is closed");
        h_enter_result = AMI_NET_OK;
    }

    printf("nx_nest: %lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures != 0) ? 1 : 0;
}
