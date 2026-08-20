/*
 * AmiNetXDuo, DHCP resolver producer/consumer handoff.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_handoff.h"

#include <proto/exec.h>


VOID ami_ns_dns_pending_mark(AmiNsDnsPending *pending, UWORD interface_index)
{
    if (pending == NULL || interface_index >= 32U)
        return;

    /* The DHCP callback is a ThreadX task and the consumer is an Exec caller
       task. Cover both with the same scheduler exclusion. */
    Forbid();
    pending->interfaces |= 1UL << interface_index;
    Permit();
}


ULONG ami_ns_dns_pending_take(AmiNsDnsPending *pending)
{
    ULONG interfaces;

    if (pending == NULL)
        return 0UL;

    /* Clear while taking the snapshot. A callback after Permit() sets a new
       bit which remains for the next caller instead of being erased here. */
    Forbid();
    interfaces = pending->interfaces;
    pending->interfaces = 0UL;
    Permit();

    return interfaces;
}


BOOL ami_ns_dns_pending_any(const AmiNsDnsPending *pending)
{
    return (BOOL)(pending != NULL && pending->interfaces != 0UL);
}


/*
 * Whether the value that came back may be acted on.
 *
 * An option the lease does not carry is a coherent empty set and withdraws
 * whatever the last lease offered.  A refusal and a failure both leave the
 * caller holding nothing it may act on, so both keep the last coherent option
 * set rather than reconciling against a partial read.
 */
BOOL ami_ns_dns_option_usable(AmiNsDnsOptionRead read)
{
    return (BOOL)(read == AMI_NS_DNS_OPTION_READ ||
                  read == AMI_NS_DNS_OPTION_ABSENT);
}


/*
 * Whether the interface wants to be looked at again.
 *
 * This is the whole of the distinction.  A lease that sits at BOUND produces
 * no further state change until T1, so an interface whose reconcile gave up
 * without re-marking has no next pass for the rest of the lease -- that is the
 * hole.  But the mark is taken at the top of every absorb and an absorb is on
 * the path of every lookup, so a mark that is set again by a decision which
 * cannot change is not a retry, it is a bracket, three reconciles and an
 * option retrieve under the client mutex on every DNS lookup, forever, with
 * the same answer each time.
 *
 * So a refusal that the buffer and the option size decide between them -- a
 * server offering more DNS servers than the buffer holds, a domain name longer
 * than the buffer -- keeps what it has and says so once.  Everything else may
 * read differently on a later pass and asks for one.
 */
BOOL ami_ns_dns_option_retry(AmiNsDnsOptionRead read)
{
    return (BOOL)(read == AMI_NS_DNS_OPTION_FAILED);
}
