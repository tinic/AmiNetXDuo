/* AmiNetXDuo, how many packets the pool holds and what window that backs.
 *
 * Two numbers, and the second is derived from the first: AvailMem() sizes the
 * packet pool, and the pool sizes the receive window every TCP socket
 * advertises.  They live apart from the code that reads AvailMem() and the
 * code that creates the socket because they are arithmetic with no Amiga in
 * them, and the coupling between them is what the host tier gates --
 * tests/netstack/host/test_pool_window_host.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_POOL_H
#define AMINETXDUO_POOL_H

#include <exec/types.h>

/* Packet pool sizing: computed from AvailMem() at startup and clamped to this
   range.  AMI_POOL_MAX_PACKETS also bounds BSD_TCP_WINDOW_CEILING
   (src/bsdsocket/bsdsocket_window.h) and through it the ACK threshold. */
#define AMI_POOL_PAYLOAD        1568        /* 1500 MTU + 14 eth + slack, 4-aligned */
#define AMI_POOL_MIN_PACKETS    16
#define AMI_POOL_MAX_PACKETS    512

/* Fraction of AvailMem() the packet pool can claim (1/AMI_POOL_MEM_DIVISOR). */
#define AMI_POOL_MEM_DIVISOR        16

/* Below AMI_POOL_WORKING_PACKETS the eighth-share TCP budget cannot back the
   8192-byte floor window, and the machine spends the transfer advertising
   zero.  Reached by taking 1/AMI_POOL_MEM_DIVISOR_LOW instead, never more. */
#define AMI_POOL_WORKING_PACKETS    96
#define AMI_POOL_MEM_DIVISOR_LOW    8

/*
 * How many packets `avail` bytes of free memory buy at `stride` bytes each,
 * clamped to AMI_POOL_MIN_PACKETS..AMI_POOL_MAX_PACKETS.  `divisor` is
 * AMI_POOL_MEM_DIVISOR, or whatever ENV:ANXDPOOLDIV replaced it with.
 */
ULONG ami_ns_pool_packets_for(ULONG avail, ULONG divisor, ULONG stride);

#endif /* AMINETXDUO_POOL_H */
