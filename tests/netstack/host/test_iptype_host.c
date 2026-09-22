/*
 * AmiNetXDuo, what the interface files' IPTYPE lines add up to.
 *
 * The bring-up starts the DHCP client when any interface asks for it, waits
 * for an address only when any interface can get one, and points the RFC 3927
 * fallback at the interface configured for it before any other.  Each of
 * those is a scan over the configured set, and each is here with the set
 * spelled out.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_iptype.h"

#include <stdio.h>
#include <string.h>


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


static void h_set(AmiIfConfig *iface, AmiIpType type, ULONG address)
{
    memset(iface, 0, sizeof(*iface));
    iface->iptype  = type;
    iface->address = address;
}


static void h_case_any_of_a_type(void)
{
    AmiIfConfig table[AMI_CFG_MAX_ATTACHED];

    h_set(&table[0], AMI_IPTYPE_STATIC, 0xc0a80102UL);
    h_set(&table[1], AMI_IPTYPE_DHCP, 0UL);
    h_set(&table[2], AMI_IPTYPE_LINKLOCAL, 0UL);
    h_set(&table[3], AMI_IPTYPE_NONE, 0UL);

    h_check(ami_ns_iptype_any(table, 4U, AMI_IPTYPE_DHCP),
            "DHCP is in the set");
    h_check(ami_ns_iptype_any(table, 4U, AMI_IPTYPE_LINKLOCAL),
            "LINKLOCAL is in the set");
    h_check(ami_ns_iptype_any(table, 4U, AMI_IPTYPE_NONE),
            "NONE is in the set");
    h_check(!ami_ns_iptype_any(table, 1U, AMI_IPTYPE_DHCP),
            "only the first `count` interfaces are looked at");
    h_check(!ami_ns_iptype_any(table, 0U, AMI_IPTYPE_STATIC),
            "no interfaces, nothing wanted");

    h_set(&table[1], AMI_IPTYPE_STATIC, 0UL);
    h_check(!ami_ns_iptype_any(table, 4U, AMI_IPTYPE_DHCP),
            "with the DHCP interface changed nothing wants DHCP");
}


static void h_case_any_ipv4(void)
{
    AmiIfConfig table[AMI_CFG_MAX_ATTACHED];

    h_set(&table[0], AMI_IPTYPE_NONE, 0UL);
    h_set(&table[1], AMI_IPTYPE_STATIC, 0UL);
    h_check(!ami_ns_iptype_any_ipv4(table, 2U),
            "NONE and a static interface with no ADDRESS want no IPv4");

    h_set(&table[1], AMI_IPTYPE_STATIC, 0x0a000002UL);
    h_check(ami_ns_iptype_any_ipv4(table, 2U),
            "a static interface with an ADDRESS wants IPv4");

    h_set(&table[1], AMI_IPTYPE_DHCP, 0UL);
    h_check(ami_ns_iptype_any_ipv4(table, 2U), "DHCP wants IPv4");

    h_set(&table[1], AMI_IPTYPE_LINKLOCAL, 0UL);
    h_check(ami_ns_iptype_any_ipv4(table, 2U), "LINKLOCAL wants IPv4");
    h_check(!ami_ns_iptype_any_ipv4(table, 1U),
            "the LINKLOCAL interface past `count` is not seen");
    h_check(!ami_ns_iptype_any_ipv4(table, 0U), "no interfaces, no IPv4");
}


static void h_case_autoip_picks_linklocal_first(void)
{
    AmiIfConfig table[AMI_CFG_MAX_ATTACHED];

    /* DHCP in slot 0, LINKLOCAL in slot 2: the fallback serves slot 2. */
    h_set(&table[0], AMI_IPTYPE_DHCP, 0UL);
    h_set(&table[1], AMI_IPTYPE_NONE, 0UL);
    h_set(&table[2], AMI_IPTYPE_LINKLOCAL, 0UL);
    h_set(&table[3], AMI_IPTYPE_LINKLOCAL, 0UL);

    h_check(ami_ns_iptype_autoip_slot(table, 4U, -1) == 2,
            "the first LINKLOCAL interface is served, over an earlier DHCP one");

    /* No LINKLOCAL: the first interface that wants IPv4 at all. */
    h_set(&table[2], AMI_IPTYPE_NONE, 0UL);
    h_set(&table[3], AMI_IPTYPE_STATIC, 0x0a000002UL);
    h_check(ami_ns_iptype_autoip_slot(table, 4U, -1) == 0,
            "with no LINKLOCAL the first IPv4 interface is served");

    h_set(&table[0], AMI_IPTYPE_STATIC, 0UL);
    h_check(ami_ns_iptype_autoip_slot(table, 4U, -1) == 3,
            "a static interface with no ADDRESS is passed over");

    h_set(&table[3], AMI_IPTYPE_NONE, 0UL);
    h_check(ami_ns_iptype_autoip_slot(table, 4U, -1) == -1,
            "nothing wants IPv4, nothing to serve");

    h_check(ami_ns_iptype_autoip_slot(table, 0U, -1) == -1,
            "no interfaces, nothing to serve");
}


static void h_case_autoip_requested_slot(void)
{
    AmiIfConfig table[AMI_CFG_MAX_ATTACHED];

    h_set(&table[0], AMI_IPTYPE_LINKLOCAL, 0UL);
    h_set(&table[1], AMI_IPTYPE_DHCP, 0UL);
    h_set(&table[2], AMI_IPTYPE_NONE, 0UL);
    h_set(&table[3], AMI_IPTYPE_STATIC, 0UL);

    /* A lease lost on slot 1: the fallback goes there, not to slot 0. */
    h_check(ami_ns_iptype_autoip_slot(table, 4U, 1) == 1,
            "a requested DHCP slot is taken as it is");
    h_check(ami_ns_iptype_autoip_slot(table, 4U, 0) == 0,
            "a requested LINKLOCAL slot is taken as it is");
    h_check(ami_ns_iptype_autoip_slot(table, 4U, 2) == -1,
            "a requested slot configured NONE is refused");
    h_check(ami_ns_iptype_autoip_slot(table, 4U, 3) == -1,
            "a requested static slot with no ADDRESS is refused");
    h_check(ami_ns_iptype_autoip_slot(table, 4U, 4) == -1,
            "a requested slot past the count is refused");
    h_check(ami_ns_iptype_autoip_slot(table, 1U, 1) == -1,
            "a requested slot the count does not reach is refused");
}


static void h_case_linklocal_range(void)
{
    h_check(ami_ns_iptype_linklocal(0xA9FE0000UL), "169.254.0.0 is link-local");
    h_check(ami_ns_iptype_linklocal(0xA9FE0101UL), "169.254.1.1 is link-local");
    h_check(ami_ns_iptype_linklocal(0xA9FEFFFFUL),
            "169.254.255.255 is link-local");
    h_check(!ami_ns_iptype_linklocal(0xA9FDFFFFUL),
            "169.253.255.255 is not");
    h_check(!ami_ns_iptype_linklocal(0xA9FF0000UL), "169.255.0.0 is not");
    h_check(!ami_ns_iptype_linklocal(0xC0A80101UL), "192.168.1.1 is not");
    h_check(!ami_ns_iptype_linklocal(0UL), "0.0.0.0 is not");
}


int main(void)
{
    h_case_any_of_a_type();
    h_case_any_ipv4();
    h_case_autoip_picks_linklocal_first();
    h_case_autoip_requested_slot();
    h_case_linklocal_range();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
