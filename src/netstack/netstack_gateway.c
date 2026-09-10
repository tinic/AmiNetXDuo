/*
 * AmiNetXDuo, deterministic ownership of the one IPv4 default gateway.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_gateway.h"

static VOID ami_ns_gateway_offer(const AmiNsGatewayIface *iface, UWORD index,
                                 AmiNsGatewayCandidate *out, UWORD max,
                                 UWORD *written)
{
    UWORD j;
    ULONG gateway;

    if (!iface[index].present || iface[index].gateway == 0UL || *written >= max)
        return;

    gateway = iface[index].gateway;
    for (j = 0; j < *written; j++)
        if (out[j].gateway == gateway && out[j].iface == index)
            return;

    out[*written].gateway = gateway;
    out[*written].iface = index;
    (*written)++;
}

UWORD ami_ns_gateway_candidates(const AmiNsGatewayIface *iface, UWORD count,
                                UWORD preferred, UWORD skip,
                                AmiNsGatewayCandidate *out, UWORD max)
{
    UWORD written = 0;
    UWORD i;

    if (iface == NULL || out == NULL)
        return 0;

    if (count > (UWORD)AMI_CFG_MAX_ATTACHED)
        count = (UWORD)AMI_CFG_MAX_ATTACHED;

    if (preferred < count && preferred != skip)
        ami_ns_gateway_offer(iface, preferred, out, max, &written);

    for (i = 0; i < count && written < max; i++)
        if (i != preferred && i != skip)
            ami_ns_gateway_offer(iface, i, out, max, &written);

    return written;
}
