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

#endif /* AMINETXDUO_POOL_H */
