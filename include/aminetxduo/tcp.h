/*
 * AmiNetXDuo, the TCP-level socket options the NDK does not define.
 *
 * A socket whose peer stops answering says nothing for 127 seconds and then
 * fails with ECONNRESET, 191 if the peer stopped before the handshake
 * finished.  That is the retransmission ladder running out, and it is what
 * RFC 1122 4.2.3.5 asks for, but nothing about it is visible from outside the
 * stack: an application cannot tell a connection in trouble from a peer with
 * nothing to say, and neither can the person watching it.
 *
 * TCP_STALLINFO answers the first, TCP_USER_TIMEOUT the second.  Both are
 * IPPROTO_TCP options on a SOCK_STREAM socket.  A socket that asks for
 * neither behaves exactly as it did.
 *
 * <netinet/tcp.h> defines TCP_NODELAY and TCP_MAXSEG and nothing else, so
 * these take numbers well clear of the range 4.4BSD and its descendants
 * allocate from, the way SO_EVENTMASK does at the socket level.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TCP_H
#define AMINETXDUO_TCP_H

#include <exec/types.h>

/*
 * Fail a connection whose peer has left something unacknowledged for this
 * many milliseconds, rather than waiting out the ladder.  LONG, and 0 -- the
 * default for every socket -- leaves the ladder in sole charge.
 *
 * The deadline covers the handshake as well as data: set before connect(), it
 * bounds the 191 seconds too.  It is measured from the last acknowledgement
 * that moved the connection forward, not from the call, so a transfer making
 * progress is never cut off however long it runs.  Expiry is reported the way
 * running out of retries is: ECONNRESET, and any waiting recv/send returns.
 *
 * Not inherited across accept(): set it on the accepted socket.
 */
#define TCP_USER_TIMEOUT    0x1001

/*
 * How the socket is getting on, into a struct TcpStallInfo.  Read only.
 */
#define TCP_STALLINFO       0x1002

struct TcpStallInfo
{
    /*
     * Milliseconds since the peer last acknowledged something this socket was
     * waiting on, counted only while it is waiting.  Zero means nothing is
     * outstanding, which is the ordinary state of an idle connection and of
     * one whose data is all acknowledged -- it is not evidence of health, and
     * a poll that only ever sees zero has learnt that the socket has nothing
     * in flight.
     */
    ULONG   tsi_Stalled;

    /*
     * Retransmissions of the segment currently outstanding.  Reset to zero by
     * any acknowledgement that moves the connection forward, so this is a
     * count of consecutive failures rather than a lifetime total.  0 to 6 on
     * data, 0 to 7 during the handshake.
     */
    ULONG   tsi_Retransmits;

    /* Milliseconds left on the retransmission timer, 0 if none is armed. */
    ULONG   tsi_Rto;

    /* TCP_USER_TIMEOUT as it was set, so one call answers both. */
    ULONG   tsi_UserTimeout;
};

#endif /* AMINETXDUO_TCP_H */
