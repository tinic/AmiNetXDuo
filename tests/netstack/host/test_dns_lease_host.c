/*
 * AmiNetXDuo, per-interface DHCP DNS ownership regression.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_lease.h"

#include <stdio.h>
#include <string.h>


#define DNS_A 0x0a000001UL
#define DNS_B 0x0a000002UL
#define DNS_C 0x0a000003UL
#define DNS_D 0x0a000004UL
#define DNS_E 0x0a000005UL

static unsigned long h_checks;
static unsigned long h_failures;


static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


static void h_case_interfaces_own_independently(void)
{
    AmiNsDhcpDnsLease lease;

    memset(&lease, 0, sizeof(lease));

    h_check(ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_A),
            "interface zero accepts its first server");
    h_check(ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_B),
            "interface zero accepts its second server");
    h_check(ami_ns_dhcp_dns_lease_add(&lease, 1U, DNS_B),
            "the other interface can own the same server");
    h_check(!ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_B),
            "one lease cannot own a duplicate twice");

    h_check(ami_ns_dhcp_dns_lease_remove(&lease, 0U, DNS_B),
            "renewal withdraws a server from one interface");
    h_check(!ami_ns_dhcp_dns_lease_has(&lease, 0U, DNS_B),
            "the renewed interface no longer owns the server");
    h_check(ami_ns_dhcp_dns_lease_has(&lease, 1U, DNS_B),
            "withdrawal on one interface preserves the other's ownership");

    h_check(ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_C),
            "renewal can add a replacement server");
    h_check(ami_ns_dhcp_dns_lease_count(&lease, 0U) == 2U &&
            ami_ns_dhcp_dns_lease_at(&lease, 0U, 0U) == DNS_A &&
            ami_ns_dhcp_dns_lease_at(&lease, 0U, 1U) == DNS_C,
            "withdrawal compacts the renewed interface's set");
}


static void h_case_capacity_and_clear(void)
{
    AmiNsDhcpDnsLease lease;

    memset(&lease, 0, sizeof(lease));

    h_check(ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_A) &&
            ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_B) &&
            ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_C) &&
            ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_D),
            "one lease fills every resolver slot");
    h_check(!ami_ns_dhcp_dns_lease_add(&lease, 0U, DNS_E),
            "a lease cannot exceed the resolver capacity");

    h_check(ami_ns_dhcp_dns_lease_remove(&lease, 0U, DNS_A) &&
            ami_ns_dhcp_dns_lease_remove(&lease, 0U, DNS_B) &&
            ami_ns_dhcp_dns_lease_remove(&lease, 0U, DNS_C) &&
            ami_ns_dhcp_dns_lease_remove(&lease, 0U, DNS_D),
            "lease loss can withdraw every owned server");
    h_check(ami_ns_dhcp_dns_lease_count(&lease, 0U) == 0U,
            "an empty lease retains no stale ownership");
}


static void h_case_invalid_values(void)
{
    AmiNsDhcpDnsLease lease;

    memset(&lease, 0, sizeof(lease));

    h_check(!ami_ns_dhcp_dns_lease_add(&lease, 0U, 0UL),
            "zero is not a name server");
    h_check(!ami_ns_dhcp_dns_lease_add(&lease,
                                       AMI_CFG_MAX_INTERFACES, DNS_A),
            "an invalid interface cannot own a server");
    h_check(ami_ns_dhcp_dns_lease_at(&lease, 0U, 0U) == 0UL,
            "an absent slot reads as empty");
}


static void h_case_shared_reference_counts(void)
{
    LONG use;

    use = ami_ns_dns_use_deepen(-1);
    h_check(use == -2, "DHCP deepens a static server without making it dynamic");
    use = ami_ns_dns_use_shallow(use);
    h_check(use == -1, "DHCP withdrawal preserves the static owner");

    use = ami_ns_dns_use_deepen(1);
    h_check(use == 2, "two dynamic sources share one server");
    use = ami_ns_dns_use_shallow(use);
    h_check(use == 1, "one DHCP interface can withdraw while another remains");
    use = ami_ns_dns_use_shallow(use);
    h_check(use == 0, "the final dynamic owner permits removal");

    h_check(ami_ns_dns_use_deepen(0) == -2,
            "a legacy zero count is treated as one static owner");
}


int main(void)
{
    h_case_interfaces_own_independently();
    h_case_capacity_and_clear();
    h_case_invalid_values();
    h_case_shared_reference_counts();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
