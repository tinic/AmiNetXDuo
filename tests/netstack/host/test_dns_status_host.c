/*
 * Proves src/netstack/netstack_dns_status.c maps each DNS failure to the right
 * error and retryability: RFC 3493 6.1 requires EAI_AGAIN to be distinguishable
 * from host-not-found.
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
    h_check(ami_ns_dns_error(TX_NOT_AVAILABLE) == AMI_NET_ERR_TIMEOUT,
            "DNS mutex contention is temporary, not host-not-found");
    h_check(ami_ns_dns_again(TX_NOT_AVAILABLE),
            "DNS mutex contention is worth asking again");

    h_check(ami_ns_dns_error(NX_DNS_NO_SERVER) == AMI_NET_ERR_NOSERVER,
            "an empty server list is not host-not-found");
    h_check(ami_ns_dns_error(NX_DNS_EMPTY_DNS_SERVER_LIST) ==
                AMI_NET_ERR_NOSERVER,
            "an empty server list, by its other name");
    h_check(ami_ns_dns_error(NX_DNS_TIMEOUT) == AMI_NET_ERR_TIMEOUT,
            "a timeout is a timeout");
    h_check(ami_ns_dns_again(NX_DNS_TIMEOUT), "a timeout is worth repeating");

    h_check(ami_ns_dns_error(NX_DNS_QUERY_FAILED) == AMI_NET_ERR_NONAME,
            "a query that found nothing is host-not-found");
    h_check(ami_ns_dns_again(NX_DNS_QUERY_FAILED),
            "and is worth repeating, since it also covers silence");

    h_check(ami_ns_dns_error(NX_DNS_NAME_ERROR) == AMI_NET_ERR_NONAME,
            "NXDOMAIN is host-not-found");
    h_check(!ami_ns_dns_again(NX_DNS_NAME_ERROR),
            "and is not worth repeating, unlike a query that merely failed");

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
