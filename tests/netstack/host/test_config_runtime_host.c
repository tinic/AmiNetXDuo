/*
 * AmiNetXDuo, live-configuration facade tests against the whole stack.
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

static void t_mapping_and_optional_mdns(void)
{
    const AmiIfConfig *cfg;

    printf("live config: interface mapping and optional mDNS\n");

    nsh_reset();
    CHECK(netstack_startup() == AMI_NET_OK, "the stack starts");

    cfg = netstack_iface_config(0);
    CHECK(cfg != NULL, "the NetX slot maps to a configuration");
    CHECK(cfg != NULL && cfg->configured,
          "the mapped configuration is the attached definition");
    CHECK(netstack_iface_config((UWORD)AMI_CFG_MAX_ATTACHED) == NULL,
          "an out-of-range slot has no configuration");
    CHECK(netstack_config() == &netstack_get()->ns_Config,
          "the live configuration is the singleton's configuration");

    /* This whole-stack tier deliberately compiles mDNS out. */
    CHECK(!netstack_iface_mdns(0), "mDNS reports disabled in the off build");
    CHECK(netstack_iface_mdns_set(0, TRUE) == AMI_NET_ERR_NODEV,
          "the off build refuses an mDNS policy change");

    h_down();
}

static void t_hostname_offer_boundary(void)
{
    AmiNetStack *ns;

    printf("live config: hostname offer boundary\n");

    nsh_reset();
    CHECK(netstack_startup() == AMI_NET_OK, "the DHCP-shaped stack starts");
    ns = netstack_get();
    CHECK(ns != NULL && ns->ns_DhcpCreated,
          "the test has the DHCP client whose stable name is updated");

    CHECK(netstack_hostname_offer((UWORD)AMI_HOSTNAME_ENV, NULL) ==
              AMI_NET_ERR_CONFIG,
          "a null hostname is refused before entering the policy");
    CHECK(nsh.hostname_offers == 0UL,
          "invalid input never reaches the hostname ranker");

    nsh.hostname_offer_accept = FALSE;
    CHECK(netstack_hostname_offer((UWORD)AMI_HOSTNAME_ENV, "refused") ==
              AMI_NET_ERR_CONFIG,
          "a lower-ranked offer remains refused");
    CHECK(nsh.hostname_offers == 1UL && nsh.hostname_displaces == 0UL,
          "a refusal does not displace DHCP state");

    nsh.hostname_offer_accept = TRUE;
    CHECK(netstack_hostname_offer((UWORD)AMI_HOSTNAME_ENV, "workbench") ==
              AMI_NET_OK,
          "an accepted offer reaches the live configuration");
    CHECK(strcmp(ns->ns_Config.hostname, "workbench") == 0,
          "the accepted name is visible in live configuration");
    CHECK(strcmp(ns->ns_DhcpName, "workbench") == 0,
          "the DHCP client's stable option-12 storage follows it");
    CHECK(nsh.hostname_displaces == 1UL,
          "the previous DHCP hostname state is displaced once");

    h_down();
}

int main(void)
{
    t_mapping_and_optional_mdns();
    t_hostname_offer_boundary();

    printf("\n%lu checks, %lu failure(s)\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
