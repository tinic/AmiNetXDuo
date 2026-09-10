/*
 * AmiNetXDuo, deterministic default-gateway selection.
 *
 * nx_ip_interface_detach() clears the machine's gateway when the detached
 * interface carried it.  The survivor's next hop has to replace it, and the
 * one that just went must never be offered back.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_gateway.h"

#include <stdio.h>
#include <string.h>


#define GW_A 0xc0a80101UL       /* 192.168.1.1, interface A */
#define GW_B 0x0a000001UL       /* 10.0.0.1,    interface B */

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


static void h_two(AmiNsGatewayIface *table)
{
    memset(table, 0, sizeof(*table) * (size_t)AMI_CFG_MAX_ATTACHED);
    table[0].present = TRUE;
    table[0].gateway = GW_A;
    table[1].present = TRUE;
    table[1].gateway = GW_B;
}


static void h_case_survivor_replaces_the_lost_gateway(void)
{
    AmiNsGatewayIface table[AMI_CFG_MAX_ATTACHED];
    AmiNsGatewayCandidate out[AMI_CFG_MAX_ATTACHED];
    UWORD             n;

    h_two(table);
    table[0].present = FALSE;           /* A is the one being removed */
    table[0].gateway = 0UL;

    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED, 0U, 0U, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);

    h_check(n == 1U, "one survivor offers one next hop");
    h_check(n == 1U && out[0].gateway == GW_B && out[0].iface == 1U,
            "the survivor's gateway replaces the one detach took");

    h_two(table);
    table[1].present = FALSE;
    table[1].gateway = 0UL;

    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED, 0U, 1U, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);

    h_check(n == 1U && out[0].gateway == GW_A && out[0].iface == 0U,
            "removing the other interface leaves the first one's gateway");
}


static void h_case_the_removed_slot_is_never_offered(void)
{
    AmiNsGatewayIface table[AMI_CFG_MAX_ATTACHED];
    AmiNsGatewayCandidate out[AMI_CFG_MAX_ATTACHED];
    UWORD             n;
    UWORD             i;

    /* The slot is still populated at the moment of the call: removal clears
       ns_Iface[] only after the detach. */
    h_two(table);

    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED, 1U, 0U, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);

    for (i = 0; i < n; i++)
        h_check(out[i].gateway != GW_A,
                "the interface being removed never carries the gateway");
    h_check(n == 1U && out[0].gateway == GW_B && out[0].iface == 1U,
            "the survivor is still offered");
}


static void h_case_shared_and_empty(void)
{
    AmiNsGatewayIface table[AMI_CFG_MAX_ATTACHED];
    AmiNsGatewayCandidate out[AMI_CFG_MAX_ATTACHED];
    UWORD             n;

    h_two(table);
    table[1].gateway = GW_A;            /* both on one router */
    table[2].present = TRUE;
    table[2].gateway = GW_A;

    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED, 0U, 0U, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);
    h_check(n == 2U && out[0].gateway == GW_A && out[0].iface == 1U &&
            out[1].gateway == GW_A && out[1].iface == 2U,
            "one shared next hop stays associated with each interface");

    memset(table, 0, sizeof(table));
    table[1].present = TRUE;            /* survives, offers no gateway */

    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED, 0U, 0U, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);
    h_check(n == 0U, "a survivor without a next hop offers nothing");

    memset(table, 0, sizeof(table));
    n = ami_ns_gateway_candidates(NULL, (UWORD)AMI_CFG_MAX_ATTACHED, 0U, 0U, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);
    h_check(n == 0U, "no table offers nothing");
}


static void h_case_primary_wins_arrival_order(void)
{
    AmiNsGatewayIface table[AMI_CFG_MAX_ATTACHED];
    AmiNsGatewayCandidate out[AMI_CFG_MAX_ATTACHED];
    UWORD             n;

    memset(table, 0, sizeof(table));

    /* Wi-Fi binds first, so it is a valid temporary failover. */
    table[0].present = TRUE;
    table[0].gateway = GW_A;
    table[1].present = TRUE;
    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED,
                                  1U, AMI_NS_GATEWAY_NO_IFACE, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);
    h_check(n == 1U && out[0].gateway == GW_A && out[0].iface == 0U,
            "a bound secondary carries traffic while the primary waits");

    /* genet binds second and must move to the front because it is primary. */
    table[1].gateway = GW_B;
    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED,
                                  1U, AMI_NS_GATEWAY_NO_IFACE, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);
    h_check(n == 2U && out[0].gateway == GW_B && out[0].iface == 1U &&
            out[1].gateway == GW_A && out[1].iface == 0U,
            "the selected primary wins after binding second");

    /* A later secondary renewal changes its offer, not ownership. */
    table[0].gateway = 0xc0a801feUL;
    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED,
                                  1U, AMI_NS_GATEWAY_NO_IFACE, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);
    h_check(n == 2U && out[0].gateway == GW_B && out[0].iface == 1U,
            "a later secondary renewal cannot steal the default");
}


static void h_case_primary_loss_fails_over(void)
{
    AmiNsGatewayIface table[AMI_CFG_MAX_ATTACHED];
    AmiNsGatewayCandidate out[AMI_CFG_MAX_ATTACHED];
    UWORD             n;

    h_two(table);
    n = ami_ns_gateway_candidates(table, (UWORD)AMI_CFG_MAX_ATTACHED,
                                  1U, 1U, out,
                                  (UWORD)AMI_CFG_MAX_ATTACHED);
    h_check(n == 1U && out[0].gateway == GW_A && out[0].iface == 0U,
            "loss of the primary promotes the first live survivor");
}


int main(void)
{
    h_case_survivor_replaces_the_lost_gateway();
    h_case_the_removed_slot_is_never_offered();
    h_case_shared_and_empty();
    h_case_primary_wins_arrival_order();
    h_case_primary_loss_fails_over();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
