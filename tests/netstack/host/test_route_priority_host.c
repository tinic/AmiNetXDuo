/*
 * AmiNetXDuo, the interface file's PRIORITY at the route lookup, on the host.
 *
 * Two interfaces on one subnet -- the A1200's 3c589 in slot 0 and the
 * PiStorm32's own Ethernet in slot 1 -- built out of two filled-in
 * nx_ip_interface[] entries, against the real _nx_ip_route_find() and
 * _nx_ip_gateway_address_set().  Without a priority NetX Duo takes the lower
 * slot, which is the 3c589; with one it takes the higher priority, and with
 * that card's link down it falls back.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"

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
        printf("  FAIL %s\n", what);
        return;
    }
    printf("  ok   %s\n", what);
}

/* ------------------------------------------------ what the gateway set needs */

TX_THREAD *_tx_thread_current_ptr;
volatile ULONG _tx_thread_preempt_disable;

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (void)mutex_ptr; (void)wait_option;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    (void)mutex_ptr;
    return TX_SUCCESS;
}

UINT _tx_thread_interrupt_disable(void)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    (void)previous_posture;
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    printf("  FAIL NX_ASSERT fired\n");
    h_failures++;
    return TX_SUCCESS;
}

/* ---------------------------------------------------------- the machine --- */

#define H_MASK          0xFFFFFF00UL
#define H_IF0_ADDR      0xC0A801DBUL     /* 192.168.1.219, the 3c589   */
#define H_IF1_ADDR      0xC0A80174UL     /* 192.168.1.116, the GENET   */
#define H_PEER          0xC0A801B8UL     /* 192.168.1.184              */
#define H_GATEWAY       0xC0A80101UL     /* 192.168.1.1                */
#define H_ELSEWHERE     0x08080808UL     /* 8.8.8.8                    */

static NX_IP h_ip;

static void h_interface_set(UINT index, ULONG address, ULONG mask, INT priority)
{
    NX_INTERFACE *nxif = &h_ip.nx_ip_interface[index];

    memset(nxif, 0, sizeof(*nxif));
    nxif -> nx_interface_valid            = NX_TRUE;
    nxif -> nx_interface_link_up          = NX_TRUE;
    nxif -> nx_interface_index            = (UCHAR)index;
    nxif -> nx_interface_ip_address       = address;
    nxif -> nx_interface_ip_network_mask  = mask;
    nxif -> nx_interface_ip_network       = address & mask;
    nxif -> nx_interface_ip_mtu_size      = 1500;
    nxif -> nx_interface_priority         = priority;
}

static NX_INTERFACE *h_route(ULONG destination, ULONG *next_hop)
{
    NX_INTERFACE *chosen = NX_NULL;

    if (_nx_ip_route_find(&h_ip, destination, &chosen, next_hop) != NX_SUCCESS)
        return NX_NULL;

    return chosen;
}

int main(void)
{
    NX_INTERFACE *if0 = &h_ip.nx_ip_interface[0];
    NX_INTERFACE *if1 = &h_ip.nx_ip_interface[1];
    ULONG         next_hop;

    memset(&h_ip, 0, sizeof(h_ip));
    h_ip.nx_ip_id = NX_IP_ID;

    printf("RoutePriority: two interfaces on one subnet\n");

    /* No priorities: the lower slot, which is what NetX Duo always did. */
    h_interface_set(0, H_IF0_ADDR, H_MASK, 0);
    h_interface_set(1, H_IF1_ADDR, H_MASK, 0);
    h_check(h_route(H_PEER, &next_hop) == if0 && next_hop == H_PEER,
            "without a PRIORITY the lower slot carries the subnet");

    /* PRIORITY=5 on the second card: it carries the subnet. */
    h_interface_set(1, H_IF1_ADDR, H_MASK, 5);
    h_check(h_route(H_PEER, &next_hop) == if1 && next_hop == H_PEER,
            "PRIORITY=5 in slot 1 carries the subnet over slot 0");

    /* Its link goes: slot 0 carries it until it is back. */
    if1 -> nx_interface_link_up = NX_FALSE;
    h_check(h_route(H_PEER, &next_hop) == if0,
            "with the preferred link down the other card carries it");
    if1 -> nx_interface_link_up = NX_TRUE;
    h_check(h_route(H_PEER, &next_hop) == if1,
            "and takes it back when the link returns");

    /* A negative priority yields to a card that named none. */
    h_interface_set(1, H_IF1_ADDR, H_MASK, 0);
    h_interface_set(0, H_IF0_ADDR, H_MASK, -1);
    h_check(h_route(H_PEER, &next_hop) == if1,
            "PRIORITY=-1 in slot 0 yields the subnet to slot 1");

    /* The caller's own choice of interface is respected whatever the priority. */
    {
        NX_INTERFACE *chosen = if0;

        h_check(_nx_ip_route_find(&h_ip, H_PEER, &chosen, &next_hop) == NX_SUCCESS &&
                chosen == if0,
                "an interface the caller named is used regardless");
    }

    /* The default gateway binds to the highest-priority interface whose
       subnet holds it, and the route off the subnet follows. */
    h_interface_set(0, H_IF0_ADDR, H_MASK, 0);
    h_interface_set(1, H_IF1_ADDR, H_MASK, 5);
    h_check(_nx_ip_gateway_address_set(&h_ip, H_GATEWAY) == NX_SUCCESS &&
            h_ip.nx_ip_gateway_interface == if1,
            "the default gateway binds to the PRIORITY=5 card");
    h_check(h_route(H_ELSEWHERE, &next_hop) == if1 && next_hop == H_GATEWAY,
            "and traffic off the subnet leaves through it");

    h_interface_set(0, H_IF0_ADDR, H_MASK, 0);
    h_interface_set(1, H_IF1_ADDR, H_MASK, 0);
    h_check(_nx_ip_gateway_address_set(&h_ip, H_GATEWAY) == NX_SUCCESS &&
            h_ip.nx_ip_gateway_interface == if0,
            "without a PRIORITY the gateway binds to the lower slot");

    /* A broadcast with no interface named goes out the preferred card. */
    h_interface_set(1, H_IF1_ADDR, H_MASK, 5);
    h_check(h_route(NX_IP_LIMITED_BROADCAST, &next_hop) == if1,
            "a limited broadcast with no interface named takes the priority");

    printf("RoutePriority: %lu checks, %lu failures, %s\n", h_checks, h_failures,
           (h_failures == 0) ? "PASS" : "FAIL");

    return (h_failures == 0) ? 0 : 1;
}
