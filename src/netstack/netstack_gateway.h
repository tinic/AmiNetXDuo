/*
 * AmiNetXDuo, who carries the default gateway after an interface goes.
 *
 * nx_ip_interface_detach() clears nx_ip_gateway_address when the gateway
 * belonged to the detached interface, machine-wide: the survivors are left
 * with no route off their own subnets.  The order below is what gets tried.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSTACK_GATEWAY_H
#define AMINETXDUO_NETSTACK_GATEWAY_H

#include <exec/types.h>

#include "aminetxduo/config.h"

typedef struct AmiNsGatewayIface
{
    BOOL  present;              /* the slot still carries an interface  */
    ULONG gateway;              /* its next hop, 0 = it offers none     */
} AmiNsGatewayIface;

/*
 * The next hops that can replace a lost default gateway once `removed` is
 * gone, in slot order, without duplicates and without zeroes.  Returns how
 * many were written; the caller offers them to the IP instance in turn
 * because only the stack knows which one is on a live subnet.
 */
UWORD ami_ns_gateway_candidates(const AmiNsGatewayIface *iface, UWORD count,
                                UWORD removed, ULONG *out, UWORD max);

#endif /* AMINETXDUO_NETSTACK_GATEWAY_H */
