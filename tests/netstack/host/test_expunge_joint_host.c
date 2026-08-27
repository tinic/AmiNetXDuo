/*
 * The expunge refusal at the joint.
 *
 * WHY THIS EXISTS.  The refusal has two halves and each was already proved on
 * its own.  bsd_lib_expunge() declines when netstack_can_unload() answers
 * FALSE, and tests/bsdsocket/host/test_expunge_host.c drives that with the
 * answer scripted.  netstack.c keeps ami_ns_kernel_started set when
 * tx_amiga_kernel_stop() fails, which is what makes the answer FALSE.  Nothing
 * joined them: no test asked whether a FAILED stop actually leaves the flag
 * set, and that is the whole of the claim.  If ami_ns_kernel_stop_locked()
 * cleared the flag before checking the status, both existing tests would stay
 * green and the library would agree to be expunged with ThreadX Tasks still
 * running on code in the hunk that is about to be freed.  There is no memory
 * protection, so that is a dead machine and not an error message.
 *
 * netstack.c is compiled whole; see tests/netstack/host/netstack_host_env.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_host_env.h"

#include "aminetxduo/netstack.h"

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

/* Leave nothing running for the next test, whatever this one did. */
static void h_teardown(void)
{
    nsh.tx_stop_status = TX_SUCCESS;
    netstack_shutdown();
    netstack_shutdown();
}

/*
 * Nothing has ever started: unloading is safe, and that is the direction the
 * whole mechanism has to be able to reach or the library can never expunge.
 */
static void t_idle(void)
{
    printf("expunge joint: nothing started\n");

    nsh_reset();

    CHECK(netstack_can_unload() == TRUE,
          "an untouched machine may be expunged");
    CHECK(nsh.tx_stops == 0, "and nothing was stopped to find that out");
}

/*
 * Up: ami_ns is not NULL, so unloading is refused however the kernel is.
 */
static void t_running(void)
{
    printf("expunge joint: while the stack is up\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "the stack comes up");
    CHECK(nsh.tx_starts == 1, "ThreadX was started once");
    CHECK(netstack_can_unload() == FALSE,
          "a running stack refuses to be expunged");

    h_teardown();
}

/*
 * The joint.  A stop that fails leaves ami_ns_kernel_started set, and
 * netstack_can_unload() must keep answering FALSE even though the singleton
 * is gone.  This is the claim nothing tested.
 */
static void t_failed_stop_holds_the_flag(void)
{
    printf("expunge joint: a failed stop keeps the refusal\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "up");

    nsh.tx_stop_status = TX_NOT_DONE;
    netstack_shutdown();

    CHECK(netstack_get() == NULL, "the singleton is gone");
    CHECK(nsh.tx_stops == 1, "the kernel stop was attempted");
    CHECK(nsh.tx_stops_ok == 0, "and it failed");

    CHECK(netstack_can_unload() == FALSE,
          "a failed stop still refuses the expunge");

    CHECK(nsh.baton_resets == 0,
          "the baton is not reset after a failed stop: a thread can still be "
          "inside a bracket");

    /* And it stays refused for as long as the stop keeps failing. */
    netstack_shutdown();
    CHECK(nsh.tx_stops == 2, "a later shutdown retries the stop");
    CHECK(netstack_can_unload() == FALSE, "and is still refused");

    /* The retry is what clears it.  Nothing else does. */
    nsh.tx_stop_status = TX_SUCCESS;
    netstack_shutdown();

    CHECK(nsh.tx_stops_ok == 1, "the retry stopped the kernel");
    CHECK(nsh.baton_resets == 1, "and only then is the baton reset");
    CHECK(netstack_can_unload() == TRUE,
          "a successful stop releases the refusal");
}

/*
 * The other direction, and the one a green test suite must not lose: a clean
 * shutdown has to let go.  A refusal that never lifts is a library that can
 * never be expunged, which is the failure mode the flag's owner has to avoid
 * as much as the one above.
 */
static void t_clean_stop_releases(void)
{
    printf("expunge joint: a clean stop lets go\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "up");
    netstack_shutdown();

    CHECK(nsh.tx_stops_ok == 1, "the kernel stopped");
    CHECK(nsh.baton_resets == 1, "the baton was reset");
    CHECK(netstack_can_unload() == TRUE, "and the expunge is allowed");
}

/*
 * netstack_startup() runs the same stop first, to clear a kernel left behind
 * by a previous load.  When that stop fails there is nothing safe to do, so
 * bring-up is refused rather than started on top of the old one.
 */
static void t_startup_refuses_over_a_failed_stop(void)
{
    printf("expunge joint: bring-up over a kernel that will not stop\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "up once");

    nsh.tx_stop_status = TX_NOT_DONE;
    netstack_shutdown();

    CHECK(netstack_can_unload() == FALSE, "refused, as above");

    {
        ULONG starts = nsh.tx_starts;

        CHECK(netstack_startup() == AMI_NET_ERR_KERNEL,
              "a second bring-up refuses while the old kernel is up");
        CHECK(nsh.tx_starts == starts,
              "and does not start ThreadX a second time");
        CHECK(netstack_get() == NULL, "nor publish a singleton");
    }

    h_teardown();
}

/*
 * bsd_lib_expunge() runs under Forbid() and cannot Wait(), so
 * netstack_can_unload() takes the lock with AttemptSemaphore().  A contended
 * lock is "cannot prove it is safe", which must read as a refusal.
 */
static void t_contended_lock_refuses(void)
{
    printf("expunge joint: a contended lock\n");

    nsh_reset();

    CHECK(netstack_can_unload() == TRUE, "idle, so allowed");

    nsh.attempt_semaphore_fails = TRUE;
    CHECK(netstack_can_unload() == FALSE,
          "a lock that cannot be taken refuses the expunge");

    nsh.attempt_semaphore_fails = FALSE;
    CHECK(netstack_can_unload() == TRUE, "and allows it again once free");
}

/*
 * A second opener holds the stack up.  The refusal must follow the reference
 * count, not the first close.
 */
static void t_refcount(void)
{
    printf("expunge joint: two openers\n");

    nsh_reset();

    CHECK(netstack_startup() == AMI_NET_OK, "first open");
    CHECK(netstack_startup() == AMI_NET_OK, "second open");
    CHECK(nsh.tx_starts == 1, "ThreadX is started once for both");

    netstack_shutdown();

    CHECK(netstack_get() != NULL, "one close does not take the stack down");
    CHECK(nsh.tx_stops == 0, "and does not stop the kernel");
    CHECK(netstack_can_unload() == FALSE, "so the expunge is still refused");

    netstack_shutdown();

    CHECK(netstack_get() == NULL, "the second close takes it down");
    CHECK(netstack_can_unload() == TRUE, "and the expunge is allowed");
}

int main(void)
{
    printf("netstack expunge joint host checks\n\n");

    t_idle();
    t_running();
    t_failed_stop_holds_the_flag();
    t_clean_stop_releases();
    t_startup_refuses_over_a_failed_stop();
    t_contended_lock_refuses();
    t_refcount();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
