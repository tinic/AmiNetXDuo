/*
 * AmiNetXDuo -- what a NetX Duo DNS status means. See netstack_dns_status.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_status.h"

BOOL ami_ns_dns_again(UINT status)
{
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
         * Not a DNS status at all: every entry point in addons/dns takes the
         * client's mutex with the caller's wait_option and hands this back when
         * another task still holds it. Two programs resolving at once is the
         * ordinary case, and the second of them was being told the name does
         * not exist -- the default below -- rather than to try again.
         */
        case TX_NOT_AVAILABLE:
            return AMI_NET_ERR_TIMEOUT;

        case NX_DNS_QUERY_FAILED:
        case NX_DNS_MISMATCHED_RESPONSE:
        case NX_DNS_BAD_ID_ERROR:
        case NX_DNS_SERVER_AUTH_ERROR:
            /* The servers were asked and none has the name; a mistyped name
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
