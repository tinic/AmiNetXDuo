/*
 * Stateless receive-checksum verification shared by the GENET and classic
 * direct-receive paths.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_VERIFY_H
#define AMINETXDUO_NETDEV_VERIFY_H

#include <exec/types.h>

typedef struct NetdevRxSegment
{
    ULONG   addr[8];        /* two words for IPv4, eight for IPv6           */
    UBYTE   words;          /* how many of them                             */
    ULONG   ports;
    ULONG   seq;
    ULONG   ack;
    UWORD   win;
    UWORD   data;           /* TCP payload bytes                            */
    UBYTE   flags;
    UBYTE   tcp;            /* a TCP segment with no options                */
} NetdevRxSegment;

/* `sum` is the one's-complement running sum of the complete IP packet. */
UBYTE netdev_rx_verify4(const UBYTE *ip, UWORD plen, ULONG sum);
UBYTE netdev_rx_verify6(const UBYTE *ip, UWORD plen, ULONG sum);
UBYTE netdev_rx_verify(const UBYTE *ip, UWORD plen, ULONG sum);

/* Build the GRO key after verification.  Keeping this separate lets a driver
   which only wants a verdict link no stream-tracking code at all. */
VOID netdev_rx_segment4(const UBYTE *ip, NetdevRxSegment *seg);
VOID netdev_rx_segment6(const UBYTE *ip, NetdevRxSegment *seg);

#endif /* AMINETXDUO_NETDEV_VERIFY_H */
