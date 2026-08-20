/*
 * AmiNetXDuo, DHCP resolver callback handoff regression.
 *
 * Permit() injects a second DHCP BOUND notification exactly after a caller
 * takes the first pending-interface snapshot.  The new notification must
 * remain pending for the next caller.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_handoff.h"

#include <stdio.h>
#include <string.h>


static unsigned long h_checks;
static unsigned long h_failures;
static unsigned long h_forbid_depth;
static unsigned long h_forbid_max;

static AmiNsDnsPending *h_inject_pending;
static UWORD            h_inject_iface;


static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


VOID Forbid(VOID)
{
    h_forbid_depth++;
    if (h_forbid_depth > h_forbid_max)
        h_forbid_max = h_forbid_depth;
}


VOID Permit(VOID)
{
    AmiNsDnsPending *pending;
    UWORD            iface;

    h_check(h_forbid_depth != 0, "Permit has a matching Forbid");
    if (h_forbid_depth == 0)
        return;

    h_forbid_depth--;
    if (h_forbid_depth != 0 || h_inject_pending == NULL)
        return;

    pending = h_inject_pending;
    iface = h_inject_iface;
    h_inject_pending = NULL;

    ami_ns_dns_pending_mark(pending, iface);
}


static void h_case_interfaces_coalesce(void)
{
    AmiNsDnsPending pending;
    ULONG           snapshot;

    memset(&pending, 0, sizeof(pending));

    ami_ns_dns_pending_mark(&pending, 0U);
    ami_ns_dns_pending_mark(&pending, 1U);
    ami_ns_dns_pending_mark(&pending, 0U);

    h_check(ami_ns_dns_pending_any(&pending),
            "at least one DHCP interface is pending");

    snapshot = ami_ns_dns_pending_take(&pending);
    h_check(snapshot == 3UL,
            "one snapshot contains each changed interface once");
    h_check(!ami_ns_dns_pending_any(&pending),
            "taking a snapshot clears the interfaces it contains");
}


static void h_case_change_after_snapshot(void)
{
    AmiNsDnsPending pending;
    ULONG           first;
    ULONG           second;

    memset(&pending, 0, sizeof(pending));
    ami_ns_dns_pending_mark(&pending, 0U);

    h_inject_pending = &pending;
    h_inject_iface = 1U;

    first = ami_ns_dns_pending_take(&pending);
    h_check(first == 1UL, "the first snapshot is not changed after Permit");
    h_check(ami_ns_dns_pending_any(&pending),
            "a later BOUND notification remains pending");

    second = ami_ns_dns_pending_take(&pending);
    h_check(second == 2UL,
            "the next snapshot receives the later interface notification");
}


static void h_case_invalid_interface(void)
{
    AmiNsDnsPending pending;

    memset(&pending, 0, sizeof(pending));
    ami_ns_dns_pending_mark(&pending, 32U);
    h_check(!ami_ns_dns_pending_any(&pending),
            "an interface outside the bit set is refused");
}


/*
 * The reconcile takes the mark before it does the work, so a reconcile that
 * gives up without re-marking has no next pass until T1.  Re-marking a
 * decision that cannot change is the opposite defect: the mark is taken on
 * every lookup, so it becomes a bracket, three reconciles and an option
 * retrieve under the client mutex on every lookup, forever.
 */
static void h_case_option_policy(void)
{
    h_check(ami_ns_dns_option_usable(AMI_NS_DNS_OPTION_READ),
            "an option that was read is acted on");
    h_check(!ami_ns_dns_option_retry(AMI_NS_DNS_OPTION_READ),
            "an option that was read does not ask for another pass");

    h_check(ami_ns_dns_option_usable(AMI_NS_DNS_OPTION_ABSENT),
            "an option this lease does not carry is a coherent empty set");
    h_check(!ami_ns_dns_option_retry(AMI_NS_DNS_OPTION_ABSENT),
            "an absent option does not ask for another pass");

    h_check(!ami_ns_dns_option_usable(AMI_NS_DNS_OPTION_REFUSED),
            "a refused option is not acted on");
    h_check(!ami_ns_dns_option_retry(AMI_NS_DNS_OPTION_REFUSED),
            "a refusal the buffer decides is not retried");

    h_check(!ami_ns_dns_option_usable(AMI_NS_DNS_OPTION_FAILED),
            "a failed retrieve is not acted on");
    h_check(ami_ns_dns_option_retry(AMI_NS_DNS_OPTION_FAILED),
            "a failed retrieve asks for another pass");
}


/*
 * What the re-mark is for, end to end: a caller takes the snapshot, its
 * reconcile cannot read one option, and the interface has to be in the next
 * snapshot rather than waiting for a DHCP state change that will not come
 * until T1.
 */
static void h_case_failed_reconcile_is_retried(void)
{
    AmiNsDnsPending pending;
    ULONG           first;
    ULONG           second;

    memset(&pending, 0, sizeof(pending));
    ami_ns_dns_pending_mark(&pending, 2U);

    first = ami_ns_dns_pending_take(&pending);
    h_check(first == 4UL, "the marked interface is in the first snapshot");

    if (ami_ns_dns_option_retry(AMI_NS_DNS_OPTION_FAILED))
        ami_ns_dns_pending_mark(&pending, 2U);

    second = ami_ns_dns_pending_take(&pending);
    h_check(second == 4UL,
            "an interface whose reconcile failed is in the next snapshot");

    if (ami_ns_dns_option_retry(AMI_NS_DNS_OPTION_REFUSED))
        ami_ns_dns_pending_mark(&pending, 2U);

    h_check(!ami_ns_dns_pending_any(&pending),
            "an interface whose option was refused is not marked again");
}


int main(void)
{
    h_case_interfaces_coalesce();
    h_case_change_after_snapshot();
    h_case_invalid_interface();
    h_case_option_policy();
    h_case_failed_reconcile_is_retried();

    h_check(h_forbid_depth == 0, "all handoff critical sections are balanced");
    h_check(h_forbid_max == 1, "the handoff does not nest a critical section");

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
