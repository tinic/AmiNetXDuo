/*
 * AmiNetXDuo, the resolver's search-list walk. See netstack_dns_search.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_search.h"

/* A name with no dot in it carries no domain, so the search list applies. */
static BOOL ami_ns_unqualified(const char *name)
{
    ULONG i;

    for (i = 0; name[i] != '\0'; i++)
        if (name[i] == '.')
            return FALSE;

    return TRUE;
}

/* Unsigned elapsed time, so a tick-counter wrap costs nothing. */
static ULONG ami_ns_search_left(ULONG start, ULONG budget)
{
    ULONG spent = tx_time_get() - start;

    return (spent >= budget) ? 0UL : (budget - spent);
}

LONG ami_ns_search(const AmiNsSearchOps *ops, const char *name, VOID *out,
                   ULONG timeout_ticks, AmiNetGiveUpFn give_up,
                   VOID *give_up_arg)
{
    char  qualified[AMI_DNS_NAME_MAX];
    ULONG start;
    LONG  err;
    UWORD count;
    UWORD i;

    if (name == NULL || *name == '\0' || out == NULL)
        return AMI_NET_ERR_CONFIG;

    start = tx_time_get();

    err = ops->once(name, out, timeout_ticks, give_up, give_up_arg);
    if (err == AMI_NET_OK)
        return err;

    /*
     * Qualify only after a definite no: TIMEOUT and NOSERVER say nothing about
     * the name, and ABORTED is the caller leaving.  A zero budget asked no
     * server, so its TIMEOUT still gets the hosts file under each suffix.
     */
    if (err != AMI_NET_ERR_NONAME && err != AMI_NET_ERR_STATE &&
        !(timeout_ticks == 0UL && err == AMI_NET_ERR_TIMEOUT))
        return err;

    if (!ami_ns_unqualified(name))
        return err;

    count = 0;
    (VOID)ops->suffix_at(name, 0, qualified, (ULONG)sizeof(qualified), &count);

    for (i = 0; i < count; i++)
    {
        ULONG left;
        LONG  next;

        if (give_up != NULL && give_up(give_up_arg))
            return AMI_NET_ERR_ABORTED;

        if (!ops->suffix_at(name, i, qualified, (ULONG)sizeof(qualified), NULL))
            continue;

        left = ami_ns_search_left(start, timeout_ticks);
        next = (left == 0UL) ? ops->local(qualified, out)
                             : ops->once(qualified, out, left, give_up,
                                         give_up_arg);

        if (next == AMI_NET_OK || next == AMI_NET_ERR_ABORTED)
            return next;

        if (next != AMI_NET_ERR_NONAME && next != AMI_NET_ERR_STATE)
            break;
    }

    /* The caller asked about the bare name, so report the first failure. */
    return err;
}
