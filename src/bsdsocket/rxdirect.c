/*
 * bsdsocket.library, the completer half of the pending-receive descriptor.
 *
 * Its own translation unit because it is the only part of the direct-complete
 * fork the host tier can compile: transfer.c asserts the m68k shapes of struct
 * iovec and struct msghdr, which no x86_64 host can satisfy.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/budget.h"

#ifdef AMINETXDUO_RX_DIRECT_COMPLETE

/*
 * Runs wherever the baton is: on the IP thread inside the receive notify
 * (protection mutex held, may_release FALSE), or on the caller inside its
 * bracket before it parks (may_release TRUE).  Never suspends.
 *
 * WHY may_release.  _nx_tcp_socket_state_data_check() reads the arriving
 * segment's TCP header again after the notify returns
 * (nx_tcp_socket_state_data_check.c:1286), and that segment is NOT always the
 * queue tail -- one that fills a hole is spliced in ahead of the out-of-order
 * packets behind it (ibid.:905/912).  A release on the IP thread can therefore
 * hand that very packet back to the pool, where a thread suspended on the pool
 * is resumed with it and overwrites the header still to be read.  On the IP
 * thread the pump never releases: it parks the drained packet on as_RxPending
 * instead, a shape bsd_recv_tcp() and bsd_readable() already know, and the
 * caller releases it on its next pass.
 */
VOID bsd_rxdirect_pump(AmiSocket *sock, BOOL may_release)
{
    if (sock->as_RxPending != NULL)
        return;

    while (sock->as_RxDFilled < sock->as_RxDWant)
    {
        NX_PACKET *packet = NX_NULL;
        ULONG      length, take, moved;
        UINT       status;

        status = nx_tcp_socket_receive(&sock->as_Nx.tcp, &packet, NX_NO_WAIT);
        if (status != NX_SUCCESS)
        {
            sock->as_RxDStatus = status;
            break;
        }

        length = 0;
        (VOID)nx_packet_length_get(packet, &length);

        take = sock->as_RxDWant - sock->as_RxDFilled;
        if (take > length)
            take = length;

        moved = 0;
        if (take > 0)
            (VOID)nx_packet_data_extract_offset(
                      packet, 0, sock->as_RxDDst + sock->as_RxDFilled,
                      take, &moved);

        sock->as_RxDFilled += moved;

        if (moved < length)
        {
            sock->as_RxPending = packet;
            sock->as_RxOffset  = moved;
            break;
        }

        if (!may_release ||
            sock->as_Nx.tcp.nx_tcp_socket_receive_queue_head == NX_NULL)
        {
            sock->as_RxPending = packet;
            sock->as_RxOffset  = length;
            break;
        }

        nx_packet_release(packet);
    }

    if (sock->as_RxDFilled > 0)
    {
        sock->as_RxDState = BSD_RXD_DONE;

#ifdef AMINETXDUO_RXPROBE
        ami_budget_fetch(ami_budget_clock());
        ami_budget_rx_direct();
#endif
    }
}

#endif /* AMINETXDUO_RX_DIRECT_COMPLETE */
