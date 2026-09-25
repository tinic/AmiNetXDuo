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
   range.  The pool sizes the TCP receive budget (src/bsdsocket/bsdsocket_window.h)
   and through it the window every socket advertises and its ACK cadence.

   AMI_POOL_MAX_PACKETS is a safety clamp, not the working size.  It was 512
   (856 KB at the m68k stride) until 2026-09-15, which made every machine
   with more than about 14 MB free the same machine: an eighth-share budget
   of 64 packets, one socket at 100,352 bytes, and at 22 ms to the far end
   that window IS the transfer rate.  At 4096 the clamp is reached only past
   107 MB free (1/16 of it at the m68k stride), the budget is 512 packets,
   and what bounds a socket is bsdsocket_window.h's policy, not memory.

   THE POOL'S SIZE IS NOT A SPEED.  On the real A1200 + PiStorm32 through our
   GENET core, pools of 1024, 2048 and 4096 all received at 62-68 Mbit/s
   where 512 and 768 received at 134, and an afternoon went to the pool
   before the variable was found: the receive WINDOW the iperf socket got,
   which is the budget shared with the web shell's own connection -- 50 KB
   at 512, 75 KB at 768, 100 KB from 1024 up -- and a 100 KB window on a
   1 Gbit link is what the shim's 32 posted reads could not absorb (the
   ring was never the limit: bsdsocket_window.h, sana2_internal.h
   AMI_SANA2_RX_MAX_DEPTH).
   A 512 pool moved 1 MB up in memory was as fast as one in place; the
   pool is handed out LIFO with at most ~41 packets outstanding, and nothing
   walks it.  Do not size the pool by throughput again; size the window.
   A machine with 8 MB gets exactly what it got before: the divisor, not
   the clamp, decides there. */
#define AMI_POOL_PAYLOAD        1568        /* 1500 MTU + 14 eth + slack, 4-aligned */
#define AMI_POOL_MIN_PACKETS    16
#ifndef AMI_POOL_MAX_PACKETS
#define AMI_POOL_MAX_PACKETS    4096
#endif

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

/*
 * One packet as nx_packet_pool_create() lays it out: the header rounded up to
 * `align`, then header plus payload rounded up to `align`.  No per-packet
 * slack: a stride with `align` more per packet made NetX carve more packets
 * than were planned, 4106 where the clamp said 4096 (run-bigmem.sh).
 */
ULONG ami_ns_packet_size_for(ULONG header, ULONG payload, ULONG align);

/*
 * The bytes that make nx_packet_pool_create() carve exactly `packets` packets
 * of `stride`: the packets, plus align - 1 once for the start it rounds up.
 */
ULONG ami_ns_pool_bytes_for(ULONG packets, ULONG stride, ULONG align);

/*
 * The free bytes the pool is sized from: those of the FASTEST memory class,
 * which is every Fast RAM header at the highest priority Exec holds one at,
 * summed.  `pri[i]` and `free[i]` describe the Fast RAM headers, n of them;
 * 0 when there are none, and the caller falls back to AvailMem().
 *
 * Why not AvailMem(): an A3000 with 12 MB of 32-bit motherboard RAM at
 * priority 30 ahead of 256 MB on a Zorro III card at 20 computed the pool
 * from 268 MB and hit the 4,096 cap, 6.7 MB that filled the fast block to
 * its last 2.7 KB; 93 packets were ever in use, and everything loaded after
 * the network ran from the card's RAM, 26% slower for a 1 MB read.  Sized
 * from the 12 MB it is 480 packets.  One memory class, which is every
 * other machine in the lab, gives the number AvailMem() gave.
 */
ULONG ami_ns_pool_avail_of(const LONG *pri, const ULONG *free, ULONG n);

/* Exec's Fast RAM headers as (priority, free bytes), at most `max` of them:
   the number filled in.  netstack_memlist.c walks the list under Forbid();
   the host tier answers 0. */
#define AMI_NS_POOL_HEADERS 16
ULONG ami_ns_fast_headers(LONG *pri, ULONG *free, ULONG max);

#endif /* AMINETXDUO_POOL_H */
