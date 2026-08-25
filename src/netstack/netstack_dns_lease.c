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

    if (lease == NULL || interface_index >= AMI_CFG_MAX_ATTACHED ||
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

    if (lease == NULL || interface_index >= AMI_CFG_MAX_ATTACHED ||
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

    if (lease == NULL || interface_index >= AMI_CFG_MAX_ATTACHED)
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
    if (lease == NULL || interface_index >= AMI_CFG_MAX_ATTACHED)
        return 0U;

    return lease->count[interface_index];
}


ULONG ami_ns_dhcp_dns_lease_at(const AmiNsDhcpDnsLease *lease,
                               UWORD interface_index, UWORD at)
{
    if (lease == NULL || interface_index >= AMI_CFG_MAX_ATTACHED ||
        at >= lease->count[interface_index])
        return 0UL;

    return lease->server[interface_index][at];
}


static char ami_ns_search_fold(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}


static BOOL ami_ns_search_same(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (ami_ns_search_fold(*a++) != ami_ns_search_fold(*b++))
            return FALSE;
    }
    return (BOOL)(*a == *b);
}


BOOL ami_ns_dhcp_search_lease_has(const AmiNsDhcpSearchLease *lease,
                                  UWORD interface_index, const char *domain)
{
    UWORD i;

    if (lease == NULL || domain == NULL || domain[0] == '\0' ||
        interface_index >= AMI_CFG_MAX_ATTACHED)
        return FALSE;

    for (i = 0; i < lease->count[interface_index]; i++)
        if (ami_ns_search_same(lease->domain[interface_index][i], domain))
            return TRUE;
    return FALSE;
}


BOOL ami_ns_dhcp_search_lease_add(AmiNsDhcpSearchLease *lease,
                                  UWORD interface_index, const char *domain)
{
    UWORD count;
    UWORD i;

    if (lease == NULL || domain == NULL || domain[0] == '\0' ||
        interface_index >= AMI_CFG_MAX_ATTACHED ||
        ami_ns_dhcp_search_lease_has(lease, interface_index, domain))
        return FALSE;

    for (i = 0; domain[i] != '\0'; i++)
        if ((UWORD)(i + 1U) >= (UWORD)AMI_CFG_NAME_LEN)
            return FALSE;

    count = lease->count[interface_index];
    if (count >= (UWORD)AMI_CFG_MAX_SEARCH)
        return FALSE;

    for (i = 0; domain[i] != '\0'; i++)
        lease->domain[interface_index][count][i] = domain[i];
    lease->domain[interface_index][count][i] = '\0';
    lease->count[interface_index] = (UWORD)(count + 1U);
    return TRUE;
}


BOOL ami_ns_dhcp_search_lease_remove(AmiNsDhcpSearchLease *lease,
                                     UWORD interface_index,
                                     const char *domain)
{
    UWORD count;
    UWORD i;
    UWORD j;

    if (lease == NULL || domain == NULL ||
        interface_index >= AMI_CFG_MAX_ATTACHED)
        return FALSE;

    count = lease->count[interface_index];
    for (i = 0; i < count; i++)
        if (ami_ns_search_same(lease->domain[interface_index][i], domain))
            break;
    if (i == count)
        return FALSE;

    for (; (UWORD)(i + 1U) < count; i++)
        for (j = 0; j < (UWORD)AMI_CFG_NAME_LEN; j++)
            lease->domain[interface_index][i][j] =
                lease->domain[interface_index][i + 1U][j];

    count--;
    lease->domain[interface_index][count][0] = '\0';
    lease->count[interface_index] = count;
    return TRUE;
}


UWORD ami_ns_dhcp_search_lease_count(const AmiNsDhcpSearchLease *lease,
                                     UWORD interface_index)
{
    if (lease == NULL || interface_index >= AMI_CFG_MAX_ATTACHED)
        return 0U;
    return lease->count[interface_index];
}


const char *ami_ns_dhcp_search_lease_at(const AmiNsDhcpSearchLease *lease,
                                        UWORD interface_index, UWORD at)
{
    if (lease == NULL || interface_index >= AMI_CFG_MAX_ATTACHED ||
        at >= lease->count[interface_index])
        return NULL;
    return lease->domain[interface_index][at];
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
