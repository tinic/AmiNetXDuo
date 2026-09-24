/*
 * AmiNetXDuo, the packet pool holds exactly the packets it was sized for.
 *
 * netstack.c plans a packet count (ami_ns_pool_packets_for, clamped to
 * AMI_POOL_MAX_PACKETS) and hands nx_packet_pool_create() the bytes for it.
 * NetX then carves the memory itself, at its own packet size.  The stride the
 * bytes were computed with added NX_PACKET_ALIGNMENT to every packet on top of
 * NetX's layout, so NetX found room for more than were planned: on the m68k,
 * 64-byte NX_PACKET, 4-byte alignment and 1568-byte payload, a 1636 stride
 * against NetX's 1632 turned the 4096 clamp into 4106, and run-bigmem.sh's
 * 128 MB arm failed on it every night the Zorro III memory reached the guest.
 *
 * This links the real nx_packet_pool_create() and checks that the count it
 * carves equals the planned count for the pool sizes the memory matrix
 * produces, at every start misalignment NetX has to round away.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_packet.h"

#include "aminetxduo/pool.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check_eq(const char *what, unsigned long got, unsigned long want)
{
    h_checks++;
    if (got != want)
    {
        h_failures++;
        printf("FAIL %s: got %lu, want %lu\n", what, got, want);
    }
}

/* --------------------------------------------------------------- stubs ---- */

TX_THREAD *_tx_thread_current_ptr;
volatile ULONG _tx_thread_preempt_disable;

UINT _tx_thread_interrupt_disable(void)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    (void)previous_posture;
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    printf("FAIL NX_ASSERT fired\n");
    h_failures++;
    return TX_SUCCESS;
}

/* ----------------------------------------------------------------- arms ---- */

/* The counts the memory matrix produces: the floor, the 8 MB A1200, the
   32 MB A3000, and the clamp the 128 MB A3000 sits on. */
static const ULONG h_planned[] = { 16UL, 301UL, 1265UL, 4096UL };
#define H_PLANNED_N ((int)(sizeof(h_planned) / sizeof(h_planned[0])))

/* NetX's own carving, what it answers for the pool's packet count. */
static ULONG h_netx_count(ULONG planned, ULONG misalign)
{
    ULONG          stride = ami_ns_packet_size_for((ULONG)sizeof(NX_PACKET),
                                                   (ULONG)AMI_POOL_PAYLOAD,
                                                   (ULONG)NX_PACKET_ALIGNMENT);
    ULONG          bytes  = ami_ns_pool_bytes_for(planned, stride,
                                                  (ULONG)NX_PACKET_ALIGNMENT);
    NX_PACKET_POOL pool;
    CHAR          *mem    = malloc(bytes + 16UL);
    UINT           status;
    ULONG          total;

    if (mem == NULL)
    {
        printf("FAIL out of host memory\n");
        h_failures++;
        return 0;
    }

    _nx_packet_pool_initialize();
    status = _nx_packet_pool_create(&pool, (CHAR *)"stride", AMI_POOL_PAYLOAD,
                                    mem + misalign, bytes);
    total  = (status == NX_SUCCESS) ? pool.nx_packet_pool_total : 0UL;
    free(mem);
    return total;
}

static void netx_carves_exactly_the_planned_count(void)
{
    char  what[96];
    int   i;
    ULONG mis;

    for (i = 0; i < H_PLANNED_N; i++)
        for (mis = 0; mis < (ULONG)NX_PACKET_ALIGNMENT; mis++)
        {
            snprintf(what, sizeof(what), "NetX count, planned %lu, start +%lu",
                     (unsigned long)h_planned[i], (unsigned long)mis);
            h_check_eq(what, h_netx_count(h_planned[i], mis), h_planned[i]);
        }
}

/*
 * The m68k figures, which the host's NX_PACKET cannot reproduce: 64 bytes of
 * header, 4-byte alignment, 1568 of payload.  Walks NetX's carve (start
 * rounded up, size rounded down to the alignment, whole packets) over the
 * bytes each stride asks for.
 */
static ULONG h_carve(ULONG bytes, ULONG misalign, ULONG align, ULONG packet)
{
    ULONG lost = (align - (misalign % align)) % align;

    if (bytes < lost)
        return 0;
    return (((bytes - lost) / align) * align) / packet;
}

static void m68k_figures(void)
{
    ULONG netx = ami_ns_packet_size_for(64UL, 1568UL, 4UL);
    ULONG old  = (1568UL + 64UL + 4UL + 3UL) & ~3UL;
    ULONG mis;

    h_check_eq("m68k NetX packet size", netx, 1632UL);
    h_check_eq("the old stride, for the record", old, 1636UL);
    h_check_eq("the old stride carved the CI failure's 4106",
               h_carve(4096UL * old, 0UL, 4UL, netx), 4106UL);

    for (mis = 0; mis < 4UL; mis++)
        h_check_eq("the new bytes carve 4096 at any start",
                   h_carve(ami_ns_pool_bytes_for(4096UL, netx, 4UL), mis, 4UL,
                           netx), 4096UL);
}

int main(void)
{
    netx_carves_exactly_the_planned_count();
    m68k_figures();

    if (h_failures != 0)
    {
        printf("pool_stride: %lu of %lu checks failed\n", h_failures, h_checks);
        return 1;
    }
    printf("pool_stride: %lu checks ok\n", h_checks);
    return 0;
}
