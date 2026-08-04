/*
 * AmiNetXDuo, what a failed lookup is reported as.
 *
 * "Host not found" is a claim about the name.  Three of the things that reach
 * this map are claims about the machine instead, nobody to ask, nobody
 * answered, and another task holding the DNS client, and a program that
 * retries on "try again" has to be able to tell them apart.  RFC 3493 6.1
 * requires EAI_AGAIN for exactly that, and h_errno has TRY_AGAIN for the same
 * reason.
 *
 * The case that made this worth pinning is TX_NOT_AVAILABLE.  It is not a DNS
 * status at all: every entry point in addons/dns takes the client's mutex with
 * the caller's timeout and returns it when somebody else still holds it.  It
 * had no case here, so it fell through to the default and the second of two
 * concurrent lookups was told the name does not exist.
 *
 * Real, compiled into this binary: src/netstack/netstack_dns_status.c, which is
 * the whole map.  Nothing is stubbed; there is nothing to stub.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_status.h"

#include <stdio.h>


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


int main(void)
{
    /* The loser of a race for the DNS client. */
    h_check(ami_ns_dns_error(TX_NOT_AVAILABLE) == AMI_NET_ERR_TIMEOUT,
            "DNS mutex contention is temporary, not host-not-found");
    h_check(ami_ns_dns_again(TX_NOT_AVAILABLE),
            "DNS mutex contention is worth asking again");

    /* Nobody to ask, and nobody answering. */
    h_check(ami_ns_dns_error(NX_DNS_NO_SERVER) == AMI_NET_ERR_NOSERVER,
            "an empty server list is not host-not-found");
    h_check(ami_ns_dns_error(NX_DNS_EMPTY_DNS_SERVER_LIST) ==
                AMI_NET_ERR_NOSERVER,
            "an empty server list, by its other name");
    h_check(ami_ns_dns_error(NX_DNS_TIMEOUT) == AMI_NET_ERR_TIMEOUT,
            "a timeout is a timeout");
    h_check(ami_ns_dns_again(NX_DNS_TIMEOUT), "a timeout is worth repeating");

    /*
     * The one that is genuinely about the name.  A blocking query folds every
     * per-server failure into this before returning, so it also covers a server
     * that answered NXDOMAIN, which is why it is retryable here and separated
     * by timing in netstack_retry.c rather than by status.
     */
    h_check(ami_ns_dns_error(NX_DNS_QUERY_FAILED) == AMI_NET_ERR_NONAME,
            "a query that found nothing is host-not-found");
    h_check(ami_ns_dns_again(NX_DNS_QUERY_FAILED),
            "and is worth repeating, since it also covers silence");

    /*
     * RCODE 3.  An authoritative "no such name" is an answer, so it is
     * host-not-found like the others AND it must not be retried: asking the
     * next server on the list cannot produce a different answer, and doing so
     * costs the caller the whole retry ladder for a name that does not exist.
     */
    h_check(ami_ns_dns_error(NX_DNS_NAME_ERROR) == AMI_NET_ERR_NONAME,
            "NXDOMAIN is host-not-found");
    h_check(!ami_ns_dns_again(NX_DNS_NAME_ERROR),
            "and is not worth repeating, unlike a query that merely failed");

    /* A caller's mistake, which no retry fixes. */
    h_check(ami_ns_dns_error(NX_DNS_PARAM_ERROR) == AMI_NET_ERR_CONFIG,
            "a bad argument is a configuration error");
    h_check(ami_ns_dns_error(NX_DNS_SIZE_ERROR) == AMI_NET_ERR_CONFIG,
            "a buffer too small is a configuration error");
    h_check(!ami_ns_dns_again(NX_DNS_PARAM_ERROR),
            "a bad argument is not worth repeating");
    h_check(!ami_ns_dns_again(NX_DNS_NO_SERVER),
            "asking again with no servers cannot help");

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
