/*
 * AmiNetXDuo, the TCP-level socket options the NDK does not define.
 * Both are IPPROTO_TCP options on a SOCK_STREAM socket.
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TCP_H
#define AMINETXDUO_TCP_H

#include <exec/types.h>

/*
 * LONG milliseconds; 0 (the default) disables.  Measured from the last
 * acknowledgement that moved the connection forward, covers the handshake,
 * expires as ECONNRESET.  Not inherited across accept().
 */
#define TCP_USER_TIMEOUT    0x1001

/* How the socket is getting on, into a struct TcpStallInfo.  Read only. */
#define TCP_STALLINFO       0x1002

struct TcpStallInfo
{
    /*
     * Milliseconds since the peer last acknowledged something outstanding,
     * counted only while waiting.  Zero means nothing is outstanding, not
     * that the socket is healthy.
     */
    ULONG   tsi_Stalled;

    /*
     * Consecutive retransmissions of the segment currently outstanding, reset
     * by any acknowledgement that moves the connection forward, not a
     * lifetime total.  0 to 6 on data, 0 to 7 during the handshake.
     */
    ULONG   tsi_Retransmits;

    /* Milliseconds left on the retransmission timer, 0 if none is armed. */
    ULONG   tsi_Rto;

    /* TCP_USER_TIMEOUT as it was set, so one call answers both. */
    ULONG   tsi_UserTimeout;
};

#endif /* AMINETXDUO_TCP_H */
