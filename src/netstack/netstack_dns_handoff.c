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
