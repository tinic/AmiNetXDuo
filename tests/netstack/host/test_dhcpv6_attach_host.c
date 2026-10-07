/*
 * AddNetInterface after library-open must initialize the DHCPv6 lifecycle.
 * netstack.c runs whole against the host environment. The real peer tests
 * assert the Solicit, lease, renewal and Release on the wire; this regression
 * holds the joint between dynamic attachment and the DHCPv6 translation unit.
 * SPDX-License-Identifier: MIT
 */
#include "netstack_host_env.h"
#include "aminetxduo/netstack.h"
#include "aminetxduo/netstatus.h"
#include "netstack_gateway.h"

#include <stdio.h>
#include <string.h>

static ULONG checks;
static ULONG failures;

#define CHECK(cond, what) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s\n", (what)); } } while (0)

static VOID attach_policy(AmiIp6Type policy)
{
    AmiIfConfig cfg;
    UWORD index = 99;
    ULONG before;
    ULONG outside;
    UWORD second = 99;

    nsh_reset();
    CHECK(netstack_startup_loopback() == AMI_NET_OK, "loopback starts");
    netstack_get()->ns_GatewayMode = (UBYTE)AMI_NS_GATEWAY_CLEARED;
    before = nsh.dhcpv6_configures;
    outside = nsh.dhcpv6_without_caller;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.name, "eth0");
    strcpy(cfg.device, "a2065.device");
    cfg.iptype = AMI_IPTYPE_NONE;
    cfg.ip6type = policy;
    cfg.up = TRUE;
    cfg.configured = TRUE;

    CHECK(netstack_interface_start(&cfg, &index) == AMI_NET_OK,
          "IPv6-only interface attaches after library-open");
    CHECK(nsh.dhcpv6_configures == before + 1,
          "attachment reaches DHCPv6 initialization");
    CHECK(nsh.dhcpv6_iface_count == 1,
          "DHCPv6 sees the newly attached physical slot");
    CHECK(nsh.dhcpv6_slot == index,
          "DHCPv6 is configured for the attached slot");
    CHECK(nsh.dhcpv6_without_caller == outside,
          "DHCPv6 initialization has an adopted ThreadX caller");
    CHECK(netstack_get()->ns_Config.interfaces[index].ip6type == policy,
          "attached interface retains its DHCP or AUTO policy");

    before = nsh.dhcpv6_configures;
    CHECK(netstack_interface_start(&cfg, NULL) != AMI_NET_OK,
          "duplicate interface is refused");
    CHECK(nsh.dhcpv6_configures == before,
          "refused attachment does not initialize DHCPv6");

    strcpy(cfg.name, "eth1");
    cfg.unit = 1;
    CHECK(netstack_interface_start(&cfg, &second) == AMI_NET_OK,
          "a second interface may still use the other protocols");
    CHECK(nsh.dhcpv6_configures == before &&
          netstack_get()->ns_Dhcpv6Iface == (UBYTE)index,
          "a second DHCPv6 request cannot change the first binding");
    CHECK(nsh.last_event == NETEVENT_DHCP6_LIMIT &&
          nsh.last_event_index == second && nsh.last_event_value == index,
          "the unsupported second DHCPv6 request is diagnosed");
    before = nsh.dhcpv6_destroys;
    CHECK(netstack_interface_remove(second, TRUE) == AMI_NET_OK,
          "the second interface is removed");
    CHECK(nsh.dhcpv6_destroys == before,
          "removing another interface preserves DHCPv6");
    CHECK(netstack_interface_remove(index, TRUE) == AMI_NET_OK,
          "the DHCPv6 interface is removed");
    CHECK(nsh.dhcpv6_destroys == before + 1 &&
          !netstack_get()->ns_Dhcpv6WorkReady,
          "removal discards the old DHCPv6 binding before slot reuse");
    before = nsh.dhcpv6_configures;
    CHECK(netstack_interface_start(&cfg, &second) == AMI_NET_OK,
          "a new card can start after the first client was destroyed");
    CHECK(nsh.dhcpv6_configures == before + 1 &&
          nsh.dhcpv6_slot == second,
          "a reused slot gets a fresh DHCPv6 client");
    netstack_shutdown();
}

int main(VOID)
{
    attach_policy(AMI_IP6TYPE_DHCP);
    attach_policy(AMI_IP6TYPE_AUTO);
    printf("%lu checks, %lu failures\n", (unsigned long)checks,
           (unsigned long)failures);
    return failures == 0 ? 0 : 1;
}
