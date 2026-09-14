/*
 * Persistent DEVS:Internet/routes behaviour in the shipping netstack.c.
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
            printf("  FAIL %s\n", (what));                                   \
        }                                                                     \
    } while (0)

static void h_down(void)
{
    nsh.tx_stop_status = TX_SUCCESS;
    netstack_shutdown();
    netstack_shutdown();
}

static void t_startup_installs(void)
{
    printf("configured routes: startup install\n");

    nsh_reset();
    nsh.cfg_iptype = (UWORD)AMI_IPTYPE_STATIC;
    nsh.cfg_static_routes = 1;

    CHECK(netstack_startup() == AMI_NET_OK, "the stack starts");
    CHECK(nsh.static_route_adds == 1, "the configured route is attempted once");
    CHECK(nsh.static_route_dest == 0x0A140000UL, "its destination survives");
    CHECK(nsh.static_route_mask == 0xFFFF0000UL, "its mask survives");
    CHECK(nsh.static_route_gateway == 0xC0A80101UL, "its gateway survives");
    CHECK(netstack_get()->ns_ConfigRouteInstalled[0] == 1U,
          "a successful install is remembered");

    CHECK(netstack_interface_up(0) == AMI_NET_OK, "a later reconcile succeeds");
    CHECK(nsh.static_route_adds == 1, "an installed route is not added twice");

    h_down();
}

static void t_late_address_and_operator_delete(void)
{
    printf("configured routes: late address and live delete\n");

    nsh_reset();
    nsh.cfg_static_routes = 1;
    nsh.static_route_status = NX_IP_ADDRESS_ERROR;

    CHECK(netstack_startup() == AMI_NET_OK, "the DHCP-shaped stack starts");
    CHECK(nsh.static_route_adds == 1, "the unavailable next hop is attempted");
    CHECK(netstack_get()->ns_ConfigRouteInstalled[0] == 0U,
          "an address error leaves the route pending");

    nsh.static_route_status = NX_SUCCESS;
    CHECK(netstack_interface_up(0) == AMI_NET_OK,
          "the address-change-shaped reconcile succeeds");
    CHECK(nsh.static_route_adds == 2, "the pending route is retried");
    CHECK(netstack_get()->ns_ConfigRouteInstalled[0] == 1U,
          "the retry is remembered");

    netstack_config_route_deleted(0x0A140063UL, 0xFFFF0000UL);
    CHECK(netstack_get()->ns_ConfigRouteInstalled[0] == 2U,
          "a live delete suppresses the file entry");
    CHECK(netstack_interface_up(0) == AMI_NET_OK, "another reconcile succeeds");
    CHECK(nsh.static_route_adds == 2,
          "the operator's deletion remains authoritative");

    h_down();
}

/* A normal install starts with loopback, then AddNetInterface names the card.
   A routes file may contain only a specific route and no default gateway; the
   interface start must still run the route reconciler. */
static void t_loopback_then_specific_route(void)
{
    AmiIfConfig cfg;

    printf("configured routes: loopback then named interface\n");

    nsh_reset();
    nsh.cfg_static_routes = 1;
    nsh.static_route_status = NX_IP_ADDRESS_ERROR;

    CHECK(netstack_startup_loopback() == AMI_NET_OK, "loopback starts");
    CHECK(nsh.static_route_adds == 1, "the route starts pending");

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.name, "genet");
    strcpy(cfg.device, "genet.device");
    cfg.iptype = AMI_IPTYPE_STATIC;
    cfg.address = 0xC0A80132UL;
    cfg.netmask = 0xFFFFFF00UL;
    cfg.up = TRUE;
    cfg.configured = TRUE;

    nsh.static_route_status = NX_SUCCESS;
    CHECK(netstack_interface_start(&cfg, NULL) == AMI_NET_OK,
          "the named interface starts without a default gateway");
    CHECK(nsh.static_route_adds == 2,
          "its start retries the specific route anyway");
    CHECK(netstack_get()->ns_ConfigRouteInstalled[0] == 1U,
          "the specific route is installed");

    h_down();
}

int main(void)
{
    t_startup_installs();
    t_late_address_and_operator_delete();
    t_loopback_then_specific_route();

    printf("\n%lu checks, %lu failure(s)\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
