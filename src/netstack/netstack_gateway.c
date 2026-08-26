/*
 * AmiNetXDuo, who carries the default gateway after an interface goes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_gateway.h"

UWORD ami_ns_gateway_candidates(const AmiNsGatewayIface *iface, UWORD count,
                                UWORD removed, ULONG *out, UWORD max)
{
    UWORD written = 0;
    UWORD i;
    UWORD j;

    if (iface == NULL || out == NULL)
        return 0;

    if (count > (UWORD)AMI_CFG_MAX_ATTACHED)
        count = (UWORD)AMI_CFG_MAX_ATTACHED;

    for (i = 0; i < count && written < max; i++)
    {
        if (i == removed || !iface[i].present || iface[i].gateway == 0UL)
            continue;

        for (j = 0; j < written; j++)
            if (out[j] == iface[i].gateway)
                break;

        if (j != written)
            continue;

        out[written++] = iface[i].gateway;
    }

    return written;
}
