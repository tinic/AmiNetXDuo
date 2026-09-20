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

/* Turn a GEM-style hardware verdict (2 = IP+TCP, 3 = IP+UDP) into the
 * published VERIFIED promise, after checking the structural restrictions
 * that promise makes but without recomputing either checksum. */
UBYTE netdev_rx_trust4(const UBYTE *ip, UWORD plen, UBYTE verdict);

/* Validate the full Ethernet/IPv4 frame promised by ANXD_S2IOF_L4_CSUM and
 * return the byte offset of the TCP or UDP checksum which a full-offload
 * device must clear before transmission. */
UBYTE netdev_tx_csum4(const UBYTE *frame, UWORD len, UBYTE supported,
                      UWORD *checksum_offset);

/* Build the GRO key after verification.  Keeping this separate lets a driver
   which only wants a verdict link no stream-tracking code at all. */
VOID netdev_rx_segment4(const UBYTE *ip, NetdevRxSegment *seg);
VOID netdev_rx_segment6(const UBYTE *ip, NetdevRxSegment *seg);

/*
 * THE CONTINUES MARK, for a core that delivers frames in order and wants the
 * opener to chain them (aminetxduo/anxs2ext.h ANXD_S2_RXF_CONTINUES).  A
 * verified TCP segment with no options, carrying data, whose flags are ACK
 * or ACK+PSH, is the next of the same stream as the previous one when the
 * four-tuple, the acknowledgment and the window repeat and its sequence
 * number is where the previous one ended.  Anything else starts a new run
 * (a TCP segment) or ends the run (anything else): the opener holds at most
 * one head, and a frame that is not the next of that stream makes it
 * deliver the head, so a mark across it would be a lie.
 *
 * PSH is not part of the key.  The GENET core's own version (genet.c
 * ge_continues) requires the flag byte to repeat, and a Linux sender sets
 * PSH on the last segment of every socket buffer it hands the card, about
 * every fifth: on the A3000 that cut every 32-frame burst into runs of two,
 * and 8,435 segments cost 431 acknowledgments.  What the opener chains is
 * payload; the head's TCP header goes up as it was, and NetX delivers
 * queued data without regard to PSH, so a push inside or at the end of a
 * run changes nothing the application can see.
 */
typedef struct NetdevRxGro
{
    ULONG   addr[8];        /* source, destination: two words for IPv4,
                               eight for IPv6                                */
    ULONG   ports;          /* source port << 16 | destination port         */
    ULONG   seq;            /* the next in-order sequence number            */
    ULONG   ack;
    UWORD   win;
    UBYTE   flags;          /* the TCP flag byte: ACK, or ACK|PSH           */
    UBYTE   live;
    UBYTE   run;            /* frames marked in this run, against `max`     */
    UBYTE   words;          /* address words that make the key: 2 or 8      */
} NetdevRxGro;

/* `verified` is the verdict for this frame; `seg` is only read when it is
   non-zero.  Returns ANXD_S2_RXF_CONTINUES or 0, and updates the state. */
UBYTE netdev_rx_continues(NetdevRxGro *g, const NetdevRxSegment *seg,
                          UBYTE verified, UBYTE max);

#endif /* AMINETXDUO_NETDEV_VERIFY_H */
