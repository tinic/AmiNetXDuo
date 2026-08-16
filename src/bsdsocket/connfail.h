/*
 * AmiNetXDuo, why a connect attempt died.
 *
 * A header rather than a function in select.c so the host test that drives the
 * SYN ladder to its end can ask the same question the library asks:
 * tests/netstack/host/test_tcp_retries_host.c.  It needs the NetX Duo socket
 * and nx_user.h's budget and nothing of the Amiga.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSD_CONNFAIL_H
#define AMINETXDUO_BSD_CONNFAIL_H

#include "nx_api.h"

/*
 * Did a connect attempt run out of retransmissions, rather than meet a refusal?
 *
 * _nx_tcp_socket_connection_reset() is the single exit from a pending connect,
 * and an RST reaches it by the same door as the SYN ladder giving up
 * (nx_tcp_socket_packet_process.c:406, nx_tcp_fast_periodic_processing.c:383),
 * so being called says only that the attempt is over.  Reporting all of it as
 * ECONNREFUSED is what made `whois -6' answer "connection refused" after 191
 * seconds in which nothing answered at all.
 *
 * The retry count separates them.  The ladder is what increments it, the reset
 * path does not clear it (nx_tcp_socket_block_cleanup.c), and a peer that
 * answers with an RST answers before the budget is spent.  The ceiling is the
 * one the fast timer applied: for a socket still connecting that is nx_user.h's
 * SYN budget rather than the socket's own, seven retries and 191 s.
 *
 * A TCP_USER_TIMEOUT deadline is the one timeout this cannot see.  It fires
 * with the count still low, and the stall clock it fired on is cleared before
 * the callback runs, so it reads as a refusal.  Separating it needs a mark set
 * where the deadline is applied, in the vendored tree.
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
