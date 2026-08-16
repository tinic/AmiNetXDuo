/*
 * AmiNetXDuo, what a NetX Duo DNS status means. See netstack_dns_status.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_status.h"

BOOL ami_ns_dns_again(UINT status)
{
    /* NX_DNS_NAME_ERROR is deliberately absent: RCODE 3 is an answer about
       the name, so asking another server cannot change it. */
    return (BOOL)(status == NX_DNS_QUERY_FAILED ||
                  status == NX_DNS_TIMEOUT ||
                  status == TX_NOT_AVAILABLE);
}

LONG ami_ns_dns_error(UINT status)
{
    switch (status)
    {
        case NX_DNS_NO_SERVER:
        case NX_DNS_EMPTY_DNS_SERVER_LIST:
        case NX_DNS_SERVER_NOT_FOUND:
            return AMI_NET_ERR_NOSERVER;

        case NX_DNS_TIMEOUT:
            return AMI_NET_ERR_TIMEOUT;

        /*
         * Not a DNS status at all. Every entry point in addons/dns takes the
         * mutex of the client with the caller's wait_option, and hands this
         * back when another task still holds it. Two programs resolving at
         * once is the ordinary case. The second of them used to be told that
         * the name does not exist, the default below, rather than to try
         * again.
         */
        case TX_NOT_AVAILABLE:
            return AMI_NET_ERR_TIMEOUT;

        /*
         * RCODE 3 is an answer and not a failure. The server that gave it is
         * authoritative for the zone, so no other server says anything
         * different.  NX_DNS_ERROR_MASK matched it the same way it matches
         * RCODE 2, so both arrived as NX_DNS_SERVER_AUTH_ERROR and the
         * resolver asked every remaining server, once per retry rung, for an
         * answer it already had.
         */
        case NX_DNS_NAME_ERROR:
            return AMI_NET_ERR_NONAME;

        case NX_DNS_QUERY_FAILED:
        case NX_DNS_MISMATCHED_RESPONSE:
        case NX_DNS_BAD_ID_ERROR:
        case NX_DNS_SERVER_AUTH_ERROR:
            /* The servers were asked and none has the name. A mistyped name
               is the likeliest cause. */
            return AMI_NET_ERR_NONAME;

        case NX_DNS_PARAM_ERROR:
        case NX_DNS_BAD_ADDRESS_ERROR:
        case NX_DNS_SIZE_ERROR:
            return AMI_NET_ERR_CONFIG;

        default:
            return AMI_NET_ERR_NONAME;
    }
}
