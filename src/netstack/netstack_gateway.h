/*
 * AmiNetXDuo, deterministic ownership of the one IPv4 default gateway.
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
    BOOL  present;              /* attached, valid and able to route    */
    ULONG gateway;              /* its next hop, 0 = it offers none     */
} AmiNsGatewayIface;

typedef struct AmiNsGatewayCandidate
{
    ULONG gateway;
    UWORD iface;
} AmiNsGatewayCandidate;

#define AMI_NS_GATEWAY_NO_IFACE ((UWORD)-1)

typedef enum AmiNsGatewayMode
{
    AMI_NS_GATEWAY_AUTO = 0,    /* selected interface, then live failover */
    AMI_NS_GATEWAY_FIXED,       /* configuration or a runtime route       */
    AMI_NS_GATEWAY_CLEARED      /* an explicit runtime deletion           */
} AmiNsGatewayMode;

/*
 * Put `preferred` first, then every other slot in stable slot order.  `skip`
 * is omitted even if its table entry has not been cleared yet.  Zero gateways
 * are omitted. Equal addresses on different interfaces remain separate: the
 * interface is part of the route, especially when two cards share one subnet.
 */
UWORD ami_ns_gateway_candidates(const AmiNsGatewayIface *iface, UWORD count,
                                UWORD preferred, UWORD skip,
                                AmiNsGatewayCandidate *out, UWORD max);

#endif /* AMINETXDUO_NETSTACK_GATEWAY_H */
