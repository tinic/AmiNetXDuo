/*
 * AmiNetXDuo, packet pool sizing.
 *
 * Split out of netstack.c so that the host tier can drive it over every
 * machine size instead of over the one the emulator happens to boot with.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aminetxduo/pool.h"

ULONG ami_ns_pool_packets_for(ULONG avail, ULONG divisor, ULONG stride)
{
    ULONG packets;

    if (divisor == 0UL)
        divisor = (ULONG)AMI_POOL_MEM_DIVISOR;
    if (stride == 0UL)
        return (ULONG)AMI_POOL_MIN_PACKETS;

    packets = (avail / divisor) / stride;

    /* A machine with no Fast RAM lands here: a sixteenth of what is free is a
       pool too small to hold the window this stack then advertises, and the
       transfer is spent at a zero window.  See AMI_POOL_WORKING_PACKETS. */
    if (packets < (ULONG)AMI_POOL_WORKING_PACKETS)
    {
        ULONG afford = (avail / (ULONG)AMI_POOL_MEM_DIVISOR_LOW) / stride;

        if (afford > (ULONG)AMI_POOL_WORKING_PACKETS)
            afford = (ULONG)AMI_POOL_WORKING_PACKETS;
        if (afford > packets)
            packets = afford;
    }

    if (packets < (ULONG)AMI_POOL_MIN_PACKETS)
        packets = (ULONG)AMI_POOL_MIN_PACKETS;
    if (packets > (ULONG)AMI_POOL_MAX_PACKETS)
        packets = (ULONG)AMI_POOL_MAX_PACKETS;

    return packets;
}
