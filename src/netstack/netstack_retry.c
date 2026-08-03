/*
 * AmiNetXDuo -- the resolver's retransmission ladder. See netstack_retry.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_retry.h"

AmiNetLadderResult ami_net_ask_until(AmiNetAskFn ask, VOID *arg, ULONG budget,
                                     AmiNetGiveUpFn give_up, VOID *give_up_arg)
{
    ULONG wait = AMI_NET_ASK_FIRST;

    if (ask == NULL)
        return AMI_NET_LADDER_REFUSED;

    /* A caller that asked not to wait still gets one query, so a cached or
       hosts-file answer is reachable without blocking. */
    if (budget == 0UL)
        return (ask(arg, 0UL) == AMI_NET_ASK_ANSWERED) ? AMI_NET_LADDER_ANSWERED
                                                       : AMI_NET_LADDER_REFUSED;

    for (;;)
    {
        AmiNetAskResult result;
        ULONG           start;
        ULONG           spent;

        /* Before the first query as well as between them: a task that already
           has its break signal set is not made to wait for one round first. */
        if (give_up != NULL && give_up(give_up_arg))
            return AMI_NET_LADDER_ABORTED;

        if (budget == 0UL)
            return AMI_NET_LADDER_SILENT;

        if (wait > budget)
            wait = budget;

        start  = tx_time_get();
        result = ask(arg, wait);
        spent  = tx_time_get() - start;     /* unsigned: correct across a wrap */

        if (result == AMI_NET_ASK_ANSWERED)
            return AMI_NET_LADDER_ANSWERED;
        if (result == AMI_NET_ASK_REFUSED)
            return AMI_NET_LADDER_REFUSED;

        /* Came back with time to spare, so somebody answered -- see the
           header. */
        if (spent < wait)
            return AMI_NET_LADDER_REFUSED;

        budget = (spent < budget) ? (budget - spent) : 0UL;

        wait <<= 1;
        if (wait > AMI_NET_ASK_CEILING)
            wait = AMI_NET_ASK_CEILING;
    }
}
