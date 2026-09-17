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

    /* One pass per distinct priority, highest first: within a pass the
       order is the old one.  A handful of slots, so the quadratic walk is
       cheaper than sorting them. */
    {
        LONG  level;
        BOOL  have_level = FALSE;

        for (;;)
        {
            LONG next = 0;
            BOOL have_next = FALSE;

            /* The highest priority below the last level served. */
            for (i = 0; i < count; i++)
            {
                if (!iface[i].present || iface[i].gateway == 0UL || i == skip)
                    continue;
                if (have_level && iface[i].priority >= level)
                    continue;
                if (!have_next || iface[i].priority > next)
                {
                    next = iface[i].priority;
                    have_next = TRUE;
                }
            }

            if (!have_next)
                break;

            level = next;
            have_level = TRUE;

            if (preferred < count && preferred != skip &&
                iface[preferred].priority == level)
                ami_ns_gateway_offer(iface, preferred, out, max, &written);

            for (i = 0; i < count && written < max; i++)
                if (i != preferred && i != skip && iface[i].priority == level)
                    ami_ns_gateway_offer(iface, i, out, max, &written);
        }
    }

    return written;
}
