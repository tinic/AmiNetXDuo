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
 * The monitoring hook types and the three message shapes socket.c and
 * transfer.c build.  NDK 3.2 netinclude/libraries/bsdsocket.h:688-757; the
 * meanings are in doc/bsdsocket.doc under AddNetMonitorHook.  MHT_Send is the
 * hook that can refuse a send before any of it happens, and netmonitor.c
 * asserts its number is the list index, so both the numbers and the member
 * order are ABI.
 */
#define MHT_ICMP                0
#define MHT_UDP                 1
#define MHT_TCP_Connect         2
#define MHT_Connect             3
#define MHT_Send                4
#define MHT_Packet              5
#define MHT_Bind                6

struct ConnectMonitorMsg
{
    LONG             cmm_Size;
    STRPTR           cmm_Caller;
    LONG             cmm_Socket;
    struct sockaddr *cmm_Name;
    LONG             cmm_NameLen;
};

struct BindMonitorMsg
{
    LONG             bmm_Size;
    STRPTR           bmm_Caller;
    LONG             bmm_Socket;
    struct sockaddr *bmm_Name;
    LONG             bmm_NameLen;
};

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

/*
 * What SBTC_LOG_HOOK's hook is handed.  NDK 3.2
 * netinclude/libraries/bsdsocket.h:303-316; loghook.c fills one per line and
 * the member order is ABI.
 */
#include <dos/dos.h>

struct LogHookMessage
{
    LONG             lhm_Size;
    LONG             lhm_Priority;
    struct DateStamp lhm_Date;
    STRPTR           lhm_Tag;
    ULONG            lhm_ID;
    STRPTR           lhm_Message;
};

/*
 * SocketBaseTagList()'s vocabulary, for errno.c.  NDK 3.2
 * netinclude/libraries/bsdsocket.h:70-97 (the tag macros), 100-260 (the
 * codes) and 1250-1268 (the SBTC_ERROR_HOOK message); the meanings are in
 * doc/bsdsocket.doc under SocketBaseTagList.  The codes are the wire of the
 * tag list and the message's member order is ABI.
 */
#define SBTF_VAL                0x0000  /* ti_Data is the value              */
#define SBTF_REF                0x8000  /* ti_Data points at the value       */
#define SBTB_CODE               1
#define SBTS_CODE               0x3FFF
#define SBTM_CODE(td)           (((td) >> SBTB_CODE) & SBTS_CODE)
#define SBTF_GET                0
#define SBTF_SET                1
#define SBTM_GETREF(code) \
    (TAG_USER | SBTF_REF | (((code) & SBTS_CODE) << SBTB_CODE) | SBTF_GET)
#define SBTM_GETVAL(code) \
    (TAG_USER | SBTF_VAL | (((code) & SBTS_CODE) << SBTB_CODE) | SBTF_GET)
#define SBTM_SETREF(code) \
    (TAG_USER | SBTF_REF | (((code) & SBTS_CODE) << SBTB_CODE) | SBTF_SET)
#define SBTM_SETVAL(code) \
    (TAG_USER | SBTF_VAL | (((code) & SBTS_CODE) << SBTB_CODE) | SBTF_SET)

#define SBTC_BREAKMASK                      1
#define SBTC_SIGIOMASK                      2
#define SBTC_SIGURGMASK                     3
#define SBTC_SIGEVENTMASK                   4
#define SBTC_ERRNO                          6
#define SBTC_HERRNO                         7
#define SBTC_DTABLESIZE                     8
#define SBTC_FDCALLBACK                     9
/*
 * The fd-callback actions.  NDK 3.2 netinclude/libraries/bsdsocket.h:126-128
 * ("don't use these in new code"); socket.c sends them through sb_FDCallback.
 * FDCB_ALLOC may refuse and a refusal fails the allocation; FDCB_FREE is sent
 * after the slot is cleared and its return is not read; FDCB_CHECK is
 * deliberately never sent (socket.c says why).
 */
#define FDCB_FREE                           0
#define FDCB_ALLOC                          1
#define FDCB_CHECK                          2
#define SBTC_LOGSTAT                        10
#define SBTC_LOGTAGPTR                      11
#define SBTC_LOGFACILITY                    12
#define SBTC_LOGMASK                        13
#define SBTC_ERRNOSTRPTR                    14
#define SBTC_HERRNOSTRPTR                   15
#define SBTC_IOERRNOSTRPTR                  16
#define SBTC_S2ERRNOSTRPTR                  17
#define SBTC_S2WERRNOSTRPTR                 18
#define SBTC_ERRNOBYTEPTR                   21
#define SBTC_ERRNOWORDPTR                   22
#define SBTC_ERRNOLONGPTR                   24
#define SBTC_HERRNOLONGPTR                  25
#define SBTC_RELEASESTRPTR                  29
#define SBTC_NUM_PACKET_FILTER_CHANNELS     40
#define SBTC_HAVE_ROUTING_API               41
#define SBTC_UDP_CHECKSUM                   42
#define SBTC_IP_FORWARDING                  43
#define SBTC_IP_DEFAULT_TTL                 44
#define SBTC_ICMP_MASK_REPLY                45
#define SBTC_ICMP_SEND_REDIRECTS            46
#define SBTC_HAVE_INTERFACE_API             47
#define SBTC_ICMP_PROCESS_ECHO              48
#define SBTC_ICMP_PROCESS_TSTAMP            49
#define SBTC_HAVE_MONITORING_API            50
#define SBTC_CAN_SHARE_LIBRARY_BASES        51
#define SBTC_LOG_FILE_NAME                  52
#define SBTC_HAVE_STATUS_API                53
#define SBTC_HAVE_DNS_API                   54
#define SBTC_LOG_HOOK                       55
#define SBTC_SYSTEM_STATUS                  56
#define SBTC_SIG_ADDRESS_CHANGE_MASK        57
#define SBTC_IPF_API_VERSION                58
#define SBTC_HAVE_LOCAL_DATABASE_API        59
#define SBTC_HAVE_ADDRESS_CONVERSION_API    60
#define SBTC_HAVE_KERNEL_MEMORY_API         61
#define SBTC_IP_FILTER_HOOK                 62
#define SBTC_HAVE_SERVER_API                63
#define SBTC_GET_BYTES_RECEIVED             64
#define SBTC_GET_BYTES_SENT                 65
#define SBTC_IDN_DEFAULT_CHARACTER_SET      66
#define SBTC_HAVE_ROADSHOWDATA_API          67
#define SBTC_ERROR_HOOK                     68
#define SBTC_HAVE_GETHOSTADDR_R_API         69

/* What SBTC_SYSTEM_STATUS reports.  NDK 3.2 netinclude/libraries/bsdsocket.h:281-294. */
#define SBSYSSTAT_Interfaces        (1L<<0)
#define SBSYSSTAT_PTP_Interfaces    (1L<<1)
#define SBSYSSTAT_BCast_Interfaces  (1L<<2)
#define SBSYSSTAT_Resolver          (1L<<3)
#define SBSYSSTAT_Routes            (1L<<4)
#define SBSYSSTAT_DefaultRoute      (1L<<5)

struct ErrorHookMsg
{
    ULONG   ehm_Size;       /* >= 12 */
    ULONG   ehm_Action;
    LONG    ehm_Code;
};

#define EHMA_Set_errno          1
#define EHMA_Set_h_errno        2

#endif
