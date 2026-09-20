/*
 * AmiNetXDuo, packet-pool sizing from the running machine.
 *
 * The arithmetic remains in netstack_pool.c so host tests can exercise it
 * without an Exec environment.  This file owns the machine-facing inputs and
 * the explicit configuration override.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_internal.h"

#include "aminetxduo/pool.h"

#include <exec/memory.h>
#include <proto/exec.h>

ULONG ami_ns_packet_stride(VOID)
{
    ULONG stride;

    stride = (ULONG)AMI_POOL_PAYLOAD + (ULONG)sizeof(NX_PACKET) +
             (ULONG)NX_PACKET_ALIGNMENT;

    return (stride + 3UL) & ~3UL;
}

/* The bytes the pool is sized from: the fastest memory class's
   (netstack_memlist.c, pool.h says why), else what AvailMem() says. */
static ULONG ami_ns_pool_avail(VOID)
{
    LONG  pri[AMI_NS_POOL_HEADERS];
    ULONG free[AMI_NS_POOL_HEADERS];
    ULONG n     = ami_ns_fast_headers(pri, free, (ULONG)AMI_NS_POOL_HEADERS);
    ULONG avail = ami_ns_pool_avail_of(pri, free, n);

    return (avail != 0UL) ? avail : AvailMem(MEMF_PUBLIC);
}

ULONG ami_ns_pool_packets(VOID)
{
    ULONG avail;
    ULONG divisor;
    ULONG packets;

    avail = ami_ns_pool_avail();
    divisor = ami_config_pool_divisor((ULONG)AMI_POOL_MEM_DIVISOR);

    /* The arithmetic is in netstack_pool.c, where the host tier can drive it
       over every machine size: test_pool_window_host.c. */
    packets = ami_ns_pool_packets_for(avail, divisor, ami_ns_packet_stride());

    /* Told outright: ENV:ANXDPOOLPACKETS (config.h says for which machine). */
    if (ami_config_pool_packets() != 0UL)
        packets = ami_config_pool_packets();

    AMI_INFO("netstack: %lu bytes free / %lu, pool = %lu x %lu",
             (unsigned long)avail, (unsigned long)divisor,
             (unsigned long)packets, (unsigned long)AMI_POOL_PAYLOAD);

    return packets;
}

/*
 * Plain loads of the NetX Duo counters, with no baton taken: a diagnostic that
 * blocks on the stack it describes is useless.  ms_PoolLow is a running
 * minimum, so it is sampled on the way out of each stack operation.
 */
VOID netstack_pool_mark_low(VOID)
{
    NX_PACKET_POOL *pool = netstack_pool();
    AmiMemStats    *m;
    ULONG           now;

    if (pool == NULL)
        return;

    m   = ami_mem_stats();
    now = pool->nx_packet_pool_available;

    if (m->ms_PoolTotal == 0UL || now < m->ms_PoolLow)
        m->ms_PoolLow = now;
}

VOID netstack_pool_sample(VOID)
{
    NX_PACKET_POOL *pool = netstack_pool();
    AmiMemStats    *m;
    ULONG           now;

    if (pool == NULL)
        return;

    m   = ami_mem_stats();
    now = pool->nx_packet_pool_available;

    if (m->ms_PoolTotal == 0UL)
        m->ms_PoolLow = now;
    else if (now < m->ms_PoolLow)
        m->ms_PoolLow = now;

    m->ms_PoolTotal      = pool->nx_packet_pool_total;
    m->ms_PoolFree       = now;
    m->ms_PoolPayload    = pool->nx_packet_pool_payload_size;
    m->ms_PoolEmpty      = pool->nx_packet_pool_empty_requests;
    m->ms_PoolWaited     = pool->nx_packet_pool_empty_suspensions;
    m->ms_PoolBadRelease = pool->nx_packet_pool_invalid_releases;
}
