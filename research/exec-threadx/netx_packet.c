/* Research guards around untouched packet ownership services.
 * SPDX-License-Identifier: MIT */
#define NX_SOURCE_CODE
#include "tx_bridge.h"
#include "nx_api.h"
#include "nx_packet.h"

UINT anx_nx_original_packet_allocate(NX_PACKET_POOL *, NX_PACKET **, ULONG, ULONG);
UINT anx_nx_original_packet_pool_delete(NX_PACKET_POOL *);
UINT _nx_packet_allocate(NX_PACKET_POOL *pool, NX_PACKET **out, ULONG offset, ULONG wait)
{
    anx_tx_require_context(wait!=NX_NO_WAIT);
    if (!pool || pool->nx_packet_pool_id!=NX_PACKET_POOL_ID || !out) return NX_PTR_ERROR;
    return anx_nx_original_packet_allocate(pool,out,offset,wait);
}
UINT _nx_packet_pool_delete(NX_PACKET_POOL *pool)
{
    anx_tx_require_context(0);
    if (!pool || pool->nx_packet_pool_id!=NX_PACKET_POOL_ID) return NX_PTR_ERROR;
    /* No storage retirement or delete/allocate race is implemented. Producers
     * must be quiesced and every packet returned before this bounded deletion. */
    if (pool->nx_packet_pool_suspended_count || pool->nx_packet_pool_suspension_list ||
        pool->nx_packet_pool_available!=pool->nx_packet_pool_total)
        anx_tx_unsupported("packet pool deletion before quiescence");
    return anx_nx_original_packet_pool_delete(pool);
}
/* The UDP producer links optional ICMP generators. This slice has no IP route
 * or transmit implementation. Trap these paths; never fabricate a send result.
 * A future replacement-only link gate must exclude these research sentinels. */
VOID _nx_icmpv4_send_error_message(NX_IP *ip, NX_PACKET *packet, ULONG kind, ULONG pointer)
{
    (void)ip; (void)packet; (void)kind; (void)pointer;
    anx_tx_unsupported("ICMPv4 transmission outside packet ownership slice");
}
VOID _nx_icmpv6_send_error_message(NX_IP *ip, NX_PACKET *packet, ULONG kind, ULONG pointer)
{
    (void)ip; (void)packet; (void)kind; (void)pointer;
    anx_tx_unsupported("ICMPv6 transmission outside packet ownership slice");
}
