/*
 * AmiNetXDuo, per-interface DHCP DNS ownership.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_lease.h"


BOOL ami_ns_dhcp_dns_lease_has(const AmiNsDhcpDnsLease *lease,
                               UWORD interface_index, ULONG server)
{
    UWORD i;

    if (lease == NULL || interface_index >= AMI_CFG_MAX_INTERFACES ||
        server == 0UL)
        return FALSE;

    for (i = 0; i < lease->count[interface_index]; i++)
        if (lease->server[interface_index][i] == server)
            return TRUE;

    return FALSE;
}


BOOL ami_ns_dhcp_dns_lease_add(AmiNsDhcpDnsLease *lease,
                               UWORD interface_index, ULONG server)
{
    UWORD count;

    if (lease == NULL || interface_index >= AMI_CFG_MAX_INTERFACES ||
        server == 0UL ||
        ami_ns_dhcp_dns_lease_has(lease, interface_index, server))
        return FALSE;

    count = lease->count[interface_index];
    if (count >= AMI_CFG_MAX_NAMESERVERS)
        return FALSE;

    lease->server[interface_index][count] = server;
    lease->count[interface_index] = (UWORD)(count + 1U);
    return TRUE;
}


BOOL ami_ns_dhcp_dns_lease_remove(AmiNsDhcpDnsLease *lease,
                                  UWORD interface_index, ULONG server)
{
    UWORD count;
    UWORD i;

    if (lease == NULL || interface_index >= AMI_CFG_MAX_INTERFACES)
        return FALSE;

    count = lease->count[interface_index];
    for (i = 0; i < count; i++)
        if (lease->server[interface_index][i] == server)
            break;

    if (i == count)
        return FALSE;

    for (; (UWORD)(i + 1U) < count; i++)
        lease->server[interface_index][i] =
            lease->server[interface_index][i + 1U];

    count--;
    lease->server[interface_index][count] = 0UL;
    lease->count[interface_index] = count;
    return TRUE;
}


UWORD ami_ns_dhcp_dns_lease_count(const AmiNsDhcpDnsLease *lease,
                                  UWORD interface_index)
{
    if (lease == NULL || interface_index >= AMI_CFG_MAX_INTERFACES)
        return 0U;

    return lease->count[interface_index];
}


ULONG ami_ns_dhcp_dns_lease_at(const AmiNsDhcpDnsLease *lease,
                               UWORD interface_index, UWORD at)
{
    if (lease == NULL || interface_index >= AMI_CFG_MAX_INTERFACES ||
        at >= lease->count[interface_index])
        return 0UL;

    return lease->server[interface_index][at];
}


static LONG ami_ns_dns_use(LONG stored)
{
    return (stored != 0) ? stored : -1;
}


LONG ami_ns_dns_use_deepen(LONG stored)
{
    LONG use = ami_ns_dns_use(stored);

    return (use < 0) ? (use - 1) : (use + 1);
}


LONG ami_ns_dns_use_shallow(LONG stored)
{
    LONG use = ami_ns_dns_use(stored);

    return (use < 0) ? (use + 1) : (use - 1);
}
