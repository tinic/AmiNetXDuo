/*
 * AmiNetXDuo -- kill the receive stash when a packet leaves the pool.
 *
 * Transmit reaches _nx_ip_checksum_compute() too, to INSERT a checksum rather
 * than verify one, and a transmit packet is a receive packet's buffer
 * reissued -- one pool serves both directions.  The sentinel lives in
 * NX_PACKET, which the pool does not clear, so a received frame that never
 * reached its transport checksum leaves a live one behind: a datagram for
 * another host, one whose IP header checksum failed, a fragment.  All that is
 * then needed is an outgoing segment whose data_length happens to equal the
 * stale sentinel's length, and the stack writes a cached sum over quite
 * different bytes into a frame we are about to send.  Nothing local sees it;
 * the peer drops the segment and the connection stalls.
 *
 * Invalidating on use closes the ordinary receive path, and the copy hook
 * writes a dead sentinel for frames it did not sum, but neither reaches a
 * packet dropped before the transport layer.  This does: every packet, both
 * directions, one store.
 *
 * A pool has no allocation callback, so this takes the route the checksum
 * already takes -- the top-level CMakeLists compiles the vendored file under
 * a different name and this supplies _nx_packet_allocate() instead.  Not a
 * line of vendored logic is duplicated.
 *
 * It is compiled into libnetxduo.a rather than net68k's own archive: the
 * renamed vendored function calls _nx_packet_pool_cleanup() in the core and
 * the core calls this, so two archives would each need the other.
 *
 * SPDX-License-Identifier: MIT
 */

#include "net68k.h"

UINT n68k_packet_allocate_vendored(NX_PACKET_POOL *pool_ptr,
                                   NX_PACKET **packet_ptr,
                                   ULONG packet_type, ULONG wait_option);

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{

UINT    status;


    status =  n68k_packet_allocate_vendored(pool_ptr, packet_ptr, packet_type,
                                            wait_option);

    if (status == NX_SUCCESS)
    {
        N68K_RX_SENTINEL(*packet_ptr) =  0UL;
    }

    return(status);
}
