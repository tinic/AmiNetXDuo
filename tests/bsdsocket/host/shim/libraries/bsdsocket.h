/*
 * <libraries/bsdsocket.h> for the bsdsocket host tests.
 *
 * The SBTC_* tags are ABI constants and are NOT restated here: a second copy
 * of a number the NDK owns is a number that can disagree with it.  What is
 * here is only what the include chain needs to parse.  A test that reaches a
 * tag adds it, from the NDK, with the autodoc reference beside it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_BSD_TEST_LIBRARIES_BSDSOCKET_H
#define AMINETXDUO_BSD_TEST_LIBRARIES_BSDSOCKET_H
#include <exec/types.h>
#include <exec/libraries.h>
#include <utility/tagitem.h>

/*
 * The routing API's five tags, the whole of its vocabulary.  NDK 3.2
 * SANA+RoadshowTCP-IP/netinclude/libraries/bsdsocket.h:358-371; the meanings
 * are in doc/bsdsocket.doc under AddRouteTagList and DeleteRouteTagList,
 * except ChangeRouteTagList's, which has no page (src/bsdsocket/routing.c).
 */
#define RTA_BASE                (TAG_USER + 1600)
#define RTA_Destination         (RTA_BASE + 1)
#define RTA_Gateway             (RTA_BASE + 2)
#define RTA_DefaultGateway      (RTA_BASE + 3)
#define RTA_DestinationHost     (RTA_BASE + 4)
#define RTA_DestinationNet      (RTA_BASE + 5)

/*
 * The seven event bits, the vocabulary of the AmiTCP event API.  NDK 3.2
 * SANA+RoadshowTCP-IP/netinclude/libraries/bsdsocket.h:344-350; the meanings
 * are in doc/bsdsocket.doc under SetSocketSignals, GetSocketEvents and
 * setsockopt(SO_EVENTMASK).
 *
 * select.c posts them from the NetX Duo callbacks and options.c is where
 * SO_EVENTMASK reads and writes one.  They are values, not merely names, so
 * a host test can assert which bit a callback set.
 */
#define FD_ACCEPT               0x01    /* there is a connection to accept() */
#define FD_CONNECT              0x02    /* connect() completed               */
#define FD_OOB                  0x04    /* socket has out-of-band data       */
#define FD_READ                 0x08    /* socket is readable                */
#define FD_WRITE                0x10    /* socket is writeable               */
#define FD_ERROR                0x20    /* asynchronous error on socket      */
#define FD_CLOSE                0x40    /* connection closed                 */

/*
 * The monitoring hook types and the one message shape transfer.c builds.  NDK
 * 3.2 netinclude/libraries/bsdsocket.h:688-757; the meanings are in
 * doc/bsdsocket.doc under AddNetMonitorHook.  MHT_Send is the hook that can
 * refuse a send before any of it happens, and netmonitor.c asserts its number
 * is the list index, so both the numbers and the member order are ABI.
 */
#define MHT_ICMP                0
#define MHT_UDP                 1
#define MHT_TCP_Connect         2
#define MHT_Connect             3
#define MHT_Send                4
#define MHT_Packet              5
#define MHT_Bind                6

struct SendMonitorMessage
{
    LONG             smm_Size;
    STRPTR           smm_Caller;
    LONG             smm_Socket;
    APTR             smm_Buffer;
    LONG             smm_Len;
    LONG             smm_Flags;
    struct sockaddr *smm_To;
    LONG             smm_ToLen;
    struct msghdr   *smm_Msg;
};

#endif
