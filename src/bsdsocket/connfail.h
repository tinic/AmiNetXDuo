/*
 * AmiNetXDuo, why a connect attempt died.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSD_CONNFAIL_H
#define AMINETXDUO_BSD_CONNFAIL_H

#include "nx_api.h"

/*
 * Did a connect attempt run out of retransmissions, rather than meet a refusal?
 */
static UINT bsd_connect_ladder_spent(const NX_TCP_SOCKET *tcp)
{
    ULONG max = tcp -> nx_tcp_socket_timeout_max_retries;

    if (max < NX_TCP_SYN_MAXIMUM_RETRIES)
    {
        max = NX_TCP_SYN_MAXIMUM_RETRIES;
    }

    return (tcp -> nx_tcp_socket_timeout_retries >= max) ? 1u : 0u;
}

#endif /* AMINETXDUO_BSD_CONNFAIL_H */
