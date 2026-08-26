/*
 * bsdsocket.library, internal interfaces.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_INTERNAL_H
#define AMINETXDUO_BSDSOCKET_INTERNAL_H

/*
 * ThreadX and NetX Duo first, always. tx_port.h typedefs VOID, CHAR, UINT and
 * others. <exec/types.h> guards its own definitions with #ifndef, so it
 * tolerates being second but not first.
 */
#include "tx_api.h"
#include "nx_api.h"
#ifdef AMINETXDUO_GREEN_REALM
/* TX_AMIGA_GATE, embedded per opener below.  Guarded so the host tier's
   shim builds, which have no port header and no green realm, do not need
   one. */
#include "tx_amiga.h"
#endif

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <proto/exec.h>         /* SetSignal for the inline fallbacks below */
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>

/*
 * <sys/socket.h> uses size_t/ssize_t but does not pull them in itself, and the
 * ThreadX port header does not always include <stdlib.h> first.
 */
#include <stddef.h>
#include <sys/types.h>

#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <libraries/bsdsocket.h>

#include "aminetxduo/compat.h"
#include "aminetxduo/netstack.h"
#include "aminetxduo/cmsg.h"
#include "aminetxduo/netstatus.h"

#include "aminetxduo/in6.h"

/*
 * ABI constants: errno values here are 4.4BSD's, which the newlib <errno.h>
 * in this toolchain does not match. The AMI_ prefix keeps the two apart.
 */
#define AMI_EPERM               1
#define AMI_ENOENT              2
#define AMI_ESRCH               3
#define AMI_EINTR               4
#define AMI_EIO                 5
#define AMI_ENXIO               6
#define AMI_EBADF               9
#define AMI_ENOMEM             12
#define AMI_EACCES             13
#define AMI_EFAULT             14
#define AMI_EBUSY              16
#define AMI_EEXIST             17
#define AMI_EINVAL             22
#define AMI_ENFILE             23
#define AMI_EMFILE             24
#define AMI_ENOTTY             25
#define AMI_ENOSPC             28
#define AMI_EPIPE              32
#define AMI_ERANGE             34
#define AMI_EWOULDBLOCK        35
#define AMI_EAGAIN             AMI_EWOULDBLOCK
#define AMI_EINPROGRESS        36
#define AMI_EALREADY           37
#define AMI_ENOTSOCK           38
#define AMI_EDESTADDRREQ       39
#define AMI_EMSGSIZE           40
#define AMI_EPROTOTYPE         41
#define AMI_ENOPROTOOPT        42
#define AMI_EPROTONOSUPPORT    43
#define AMI_ESOCKTNOSUPPORT    44
#define AMI_EOPNOTSUPP         45
#define AMI_EPFNOSUPPORT       46
#define AMI_EAFNOSUPPORT       47
#define AMI_EADDRINUSE         48
#define AMI_EADDRNOTAVAIL      49
#define AMI_ENETDOWN           50
#define AMI_ENETUNREACH        51
#define AMI_ENETRESET          52
#define AMI_ECONNABORTED       53
#define AMI_ECONNRESET         54
#define AMI_ENOBUFS            55
#define AMI_EISCONN            56
#define AMI_ENOTCONN           57
#define AMI_ESHUTDOWN          58
#define AMI_ETOOMANYREFS       59
#define AMI_ETIMEDOUT          60
#define AMI_ECONNREFUSED       61
#define AMI_ENAMETOOLONG       63
#define AMI_EHOSTDOWN          64
#define AMI_EHOSTUNREACH       65
#define AMI_ENOSYS             78

#define AMI_IPV6_V6ONLY_BSD         IPV6_V6ONLY
#define AMI_IPV6_V6ONLY_LINUX       26

/* IPV6_UNICAST_HOPS: 4 in BSD, 16 in Linux. Same argument, same treatment. */
#define AMI_IPV6_UNICAST_HOPS_BSD   IPV6_UNICAST_HOPS
#define AMI_IPV6_UNICAST_HOPS_LINUX 16

#define AMI_IPV6_TCLASS_BSD         IPV6_TCLASS
#define AMI_IPV6_TCLASS_LINUX       67

#define AMI_IPV6_MULTICAST_IF_BSD     IPV6_MULTICAST_IF
#define AMI_IPV6_MULTICAST_IF_LINUX   17
#define AMI_IPV6_MULTICAST_HOPS_BSD   IPV6_MULTICAST_HOPS
#define AMI_IPV6_MULTICAST_HOPS_LINUX 18
#define AMI_IPV6_MULTICAST_LOOP_BSD   IPV6_MULTICAST_LOOP
#define AMI_IPV6_MULTICAST_LOOP_LINUX 19
#define AMI_IPV6_JOIN_GROUP_BSD       IPV6_JOIN_GROUP
#define AMI_IPV6_JOIN_GROUP_LINUX     20
#define AMI_IPV6_LEAVE_GROUP_BSD      IPV6_LEAVE_GROUP
#define AMI_IPV6_LEAVE_GROUP_LINUX    21

#define AMI_INET6_ADDRSTRLEN        INET6_ADDRSTRLEN

#define BSD_LIB_NAME        "bsdsocket.library"
#define BSD_LIB_VERSION     4
/*
 * lib_Version is the ABI number callers pass to OpenLibrary() and never
 * changes. lib_Revision identifies which of our builds this is, so a command
 * can tell a library that has the netstatus vectors from one that does not.
 */
#define BSD_LIB_REVISION    7

/* SBTC_LOGFACILITY's documented default. The NDK's <sys/syslog.h> ships the
   priority codes only, so the BSD facility value is spelled out here. */
#define BSD_LOG_USER        (1L << 3)

/*
 * 256, matching Roadshow's documented default. <sys/types.h> makes FD_SETSIZE
 * 256, so `WaitSelect(FD_SETSIZE, ...)`, common in ported code, failed
 * with EINVAL on the 64 we used to ship.
 */
#define BSD_DEFAULT_DTABLESIZE 256
#define BSD_MAX_DTABLESIZE     1024

#define BSD_FD_BITS         32
#define BSD_FD_WORDS        ((BSD_MAX_DTABLESIZE + BSD_FD_BITS - 1) / BSD_FD_BITS)

typedef struct
{
    ULONG   read[BSD_FD_WORDS];
    ULONG   write[BSD_FD_WORDS];
    ULONG   except[BSD_FD_WORDS];
} BsdFdSets;

/* NetX Duo's listen queue depth for a bound port. */
#define BSD_MAX_BACKLOG          8

#ifndef BSD_TCP_WINDOW
#define BSD_TCP_WINDOW        8192
#endif

#define BSD_TCP_RX_MSS_REF      1460
#define BSD_TCP_RX_QUEUE_SLACK  4

/* The UDP receive queue, in datagrams. bsd_udp_queue_max() in socket.c derives
   the per-socket default from the pool between these two. options.c bounds a
   SO_RCVBUF request by the ceiling. */
#define BSD_UDP_QUEUE_MIN       8
#define BSD_UDP_QUEUE_CEILING   64
#define BSD_UDP_POOL_SHARE      4       /* 1/N of the pool per socket       */

#ifndef BSD_TCP_WINDOW_POOL_SHARE
#define BSD_TCP_WINDOW_POOL_SHARE   8
#endif

#ifndef BSD_TCP_WINDOW_CEILING
#ifdef AMINETXDUO_TCP_WINDOW_SCALING
#define BSD_TCP_WINDOW_CEILING                                                \
    (((ULONG)AMI_POOL_MAX_PACKETS / (ULONG)BSD_TCP_WINDOW_POOL_SHARE) *        \
     (ULONG)AMI_POOL_PAYLOAD)
#else
/*
 * Without the option, the field itself is the ceiling. The window goes on the
 * wire in sixteen bits and there is nothing to scale it by, so 65535 is what
 * the wire format allows rather than a policy. nxe_tcp_socket_create.c:170
 */
#define BSD_TCP_WINDOW_CEILING  65535UL
#endif
#endif

/* Room for one dotted quad plus terminator, used by Inet_NtoA(). */
#define BSD_NTOA_BUFLEN         16

struct AmiSocket;

/* Dup2Socket(-1, fd) reserves a descriptor without a socket object. */
#define BSD_FD_RESERVED ((struct AmiSocket *)(ULONG)1UL)

/*
 * Where our seglist is, for a profiler.
 *
 */
#define BSD_PROF_SEGTAG_MAGIC   0x50534731UL    /* 'PSG1' */

struct BsdProfSegTag
{
    ULONG   bst_Magic;
    ULONG   bst_Size;
    ULONG   bst_LibBase;
    ULONG   bst_SegList;
    ULONG   bst_Sum;
};

/*
 * One of these per OpenLibrary(). The master base (sb_Master == NULL) owns
 * the segment list and the child list. Every opener gets a byte-for-byte
 * clone of it (jump table included) with its own descriptor table, errno
 * pointer and tag state. docs/RESEARCH.md S3.1: SocketBase is never shared.
 */
struct AmiSocketBase
{
    struct Library          sb_Lib;
    UWORD                   sb_Pad;
    APTR                    sb_SegList;
    struct ExecBase        *sb_SysBase;

    struct AmiSocketBase   *sb_Master;      /* NULL in the master base       */
    struct MinNode          sb_Node;        /* child link, master's list     */

    struct SignalSemaphore  sb_Lock;        /* guards the child list + stack */
    struct MinList          sb_Children;
    ULONG                   sb_StackRefs;   /* netstack_startup() references */
    ULONG                   sb_TransientStackRefs; /* async workers, no base */

    BOOL                    sb_StackHeld;

    /* Cross-base descriptor hand-off (handoff.c). Two tasks' sockets meet
     * only here, so it lives in the master base. */
    struct MinList          sb_Handoffs;
    LONG                    sb_NextHandoffId;

    struct Task            *sb_Task;        /* the task that opened us       */

    /*
     * ThreadX context for this opener's task (netx_call.c). NetX Duo's
     * THREADS_ONLY vectors reject a caller that is not a TX_THREAD, so every
     * NetX-touching entry point brackets itself with bsd_nx_enter()/leave(),
     * the shared ami_netstack_enter()/leave() bracket plus a nesting counter.
     */
    AmiNetCaller            sb_NxCaller;

    BsdFdSets               sb_SelIn;
    BsdFdSets               sb_SelReady;   /* and its result sets    */
    LONG                    sb_NxNest;      /* bracket depth, 0 == outside   */

#ifdef AMINETXDUO_GREEN_REALM
    TX_AMIGA_GATE           sb_NxGate;
    BOOL                    sb_NxGated;
    BOOL                    sb_NxGateDead;
    BOOL                    sb_NxSweepSeen;
#endif

#ifdef AMINETXDUO_NXCENSUS
    ULONG                   sb_NxCount;     /* brackets actually taken       */
    ULONG                   sb_NxNested;    /* and the ones that nested      */
    ULONG                   sb_NxEnterTicks;
    ULONG                   sb_NxLeaveTicks;
    ULONG                   sb_NxSlow;      /* enters that took over 1 ms    */
    ULONG                   sb_NxWorst;     /* the worst one, E-Clock ticks  */
#endif

    struct AmiSocket      **sb_Table;       /* descriptor table              */
    LONG                    sb_TableSize;

    LONG                    sb_Errno;
    APTR                    sb_ErrnoPtr;    /* caller-supplied, can be NULL  */
    LONG                    sb_ErrnoSize;   /* 1, 2 or 4                     */
    LONG                    sb_HErrno;
    LONG                   *sb_HErrnoPtr;

    ULONG                   sb_BreakMask;   /* SBTC_BREAKMASK, Ctrl-C default*/
    ULONG                   sb_SigIOMask;   /* SBTC_SIGIOMASK                */
    ULONG                   sb_SigUrgMask;  /* SBTC_SIGURGMASK               */
    ULONG                   sb_SigEventMask;/* SBTC_SIGEVENTMASK             */

    ULONG                   sb_SigAddressChangeMask;

    /* Retained in the private layout. The public capability is read-only
       FALSE: signals and timer.device state belong to the opening task. */
    ULONG                   sb_CanShareBases;

    STRPTR                  sb_LogTag;      /* SBTC_LOGTAGPTR                */
    LONG                    sb_LogStat;
    LONG                    sb_LogFacility; /* SBTC_LOGFACILITY, BSD_LOG_USER*/
    LONG                    sb_LogMask;     /* SBTC_LOGMASK, 0xFF            */
    /*
     * SBTC_FDCALLBACK.  amitcp/socketbasetags.h states the convention:
     *
     *     int fd = fdCallback(int fd, int action);
     *         D0                  D0      D1
     *
     * so the arguments arrive in registers rather than on the stack, and the
     * field is typed that way.  A plain C prototype here would put them on
     * the stack and the callee would read whatever D0 and D1 held.
     */
    LONG                  (*sb_FDCallback)(register LONG fd     __asm("d0"),
                                           register LONG action __asm("d1"));

    /* SBTC_ERROR_HOOK: called on every errno/h_errno change. */
    struct Hook            *sb_ErrorHook;

    BYTE                    sb_EventSignal;
    ULONG                   sb_EventSigMask;

    struct MsgPort          sb_TimerPort;
    struct timerequest      sb_TimerReq;
    BOOL                    sb_TimerOpen;
    BYTE                    sb_TimerSignal;
    ULONG                   sb_TimerSigMask;

    /* Scratch returned to callers by the non-reentrant entry points. Per
     * opener, never file statics: two tasks have two bases, and a shared
     * static means one task's lookup overwrites the struct the other is
     * still holding. */
    char                    sb_NtoABuf[BSD_NTOA_BUFLEN];
    struct servent          sb_ServEnt;
    struct protoent         sb_ProtoEnt;
    struct netent           sb_NetEnt;
    ULONG                   sb_ServCursor;  /* get{serv,proto,net}ent()      */
    ULONG                   sb_ProtoCursor;
    ULONG                   sb_NetCursor;
    struct hostent          sb_HostEnt;
    char                   *sb_HostAddrList[2];
    char                   *sb_HostAliases[1];
    ULONG                   sb_HostAddr;
    char                    sb_HostName[256];

    /* Last, so nothing above it moves. Anywhere in the positive half will do:
       the profiler scans for it rather than being told an offset. */
    struct BsdProfSegTag    sb_ProfSegTag;
};

#define ASF_TCP         (1UL <<  0)
#define ASF_UDP         (1UL <<  1)
#define ASF_NONBLOCK    (1UL <<  2)
#define ASF_BOUND       (1UL <<  3)
#define ASF_LISTENING   (1UL <<  4)
#define ASF_CONNECTED   (1UL <<  5)
#define ASF_CONNECTING  (1UL <<  6)
#define ASF_RDSHUT      (1UL <<  7)
#define ASF_WRSHUT      (1UL <<  8)
#define ASF_EOF         (1UL <<  9)   /* peer closed, no more data coming   */
#define ASF_INCOMING    (1UL << 10)   /* listen slave, no descriptor yet     */
#define ASF_ACCEPTPEND  (1UL << 11)   /* listen callback fired              */
#define ASF_REUSEADDR   (1UL << 12)
#define ASF_BROADCAST   (1UL << 13)
#define ASF_KEEPALIVE   (1UL << 14)
#define ASF_DELETED     (1UL << 16)   /* NX socket already torn down        */
#define ASF_NXBOUND     (1UL << 17)   /* NetX Duo holds the port            */
#define ASF_SERVER      (1UL << 18)   /* came off a listen port: unaccept   */
#define ASF_ORPHANED    (1UL << 19)   /* NX would not delete it; leaked     */
#define ASF_INET6       (1UL << 20)   /* created with domain AF_INET6       */
#define ASF_V6ONLY      (1UL << 21)   /* IPV6_V6ONLY, see options.c         */
#define ASF_RAW         (1UL << 22)   /* SOCK_RAW, see raw.c                */
#define ASF_OOBHAVE     (1UL << 23)   /* an urgent byte is waiting, oob.c   */
#define ASF_CLOSING     (1UL << 24)   /* FIN sent, parked for a late reap   */
#define ASF_RELISTENING (1UL << 25)   /* inside the listen callback's relisten */

#define ACW_RECVPKTINFO6    (1UL << 0)  /* IPV6_RECVPKTINFO                  */
#define ACW_RECVHOPLIMIT    (1UL << 1)  /* IPV6_RECVHOPLIMIT                 */
#define ACW_PKTINFO4        (1UL << 4)  /* IP_PKTINFO                        */
#define ACW_RECVDSTADDR4    (1UL << 5)  /* IP_RECVDSTADDR                    */
#define ACW_STICKY6         (1UL << 6)  /* setsockopt IPV6_PKTINFO named one */

#define ACW_RECV_ANY        (ACW_RECVPKTINFO6 | ACW_RECVHOPLIMIT | \
                             ACW_PKTINFO4 | ACW_RECVDSTADDR4)

/*
 * What a sendmsg()'s ancillary data, or the sticky options standing in for it,
 * said about one datagram. cs_Source is left at nxd_ip_version 0 when only an
 * interface was given, and the other way round: RFC 3542 6.6 lets either half
 * be unspecified and the stack fill it in.
 */
typedef struct BsdCmsgSource
{
    NXD_ADDRESS cs_Source;
    ULONG       cs_Ifindex;     /* 1-based, as if_nametoindex() counts       */
    LONG        cs_Hops;        /* 0..255, only when cs_HaveHops             */
    BOOL        cs_Have;
    BOOL        cs_HaveHops;
} BsdCmsgSource;

typedef struct AmiSocket
{
    /*
     * The NetX Duo control block. Both flavours carry a
     * nx_*_socket_reserved_ptr that we point back here, which is how the
     * receive/disconnect/listen callbacks find their AmiSocket.
     */
    union
    {
        NX_TCP_SOCKET   tcp;
        NX_UDP_SOCKET   udp;
    } as_Nx;

    struct AmiSocketBase   *as_Owner;       /* base to signal on events      */
    ULONG                   as_RefCount;    /* Dup2Socket / ObtainSocket     */
    ULONG                   as_Flags;

    UWORD                   as_Domain;
    UWORD                   as_Type;
    LONG                    as_Protocol;

    ULONG                   as_Events;      /* pending FD_* bits             */
    ULONG                   as_EventMask;   /* SO_EVENTMASK                  */
    LONG                    as_SoError;     /* SO_ERROR, cleared when read   */

    NXD_ADDRESS             as_LocalAddr;
    UINT                    as_LocalPort;
    NXD_ADDRESS             as_PeerAddr;
    UINT                    as_PeerPort;

    /*
     * sin6_scope_id is part of an endpoint, not a property of the socket.
     * bind() and connect() can name different endpoints, so their zones must
     */
    ULONG                   as_LocalScopeId;
    ULONG                   as_PeerScopeId;

    /* Partially consumed receive packet (a stream read need not drain one). */
    NX_PACKET              *as_RxPending;
    ULONG                   as_RxOffset;

#ifdef AMINETXDUO_RX_DIRECT_COMPLETE
    /*
     * Pending-receive descriptor.  Writable only by the current ThreadX thread;
     * the parked caller may only READ as_RxDState until it sees BSD_RXD_DONE.
     * ARMED -> DONE is the completer's transition, every other the caller's.
     */
    UBYTE                  *as_RxDDst;      /* the caller's buffer            */
    ULONG                   as_RxDWant;     /* its capacity                   */
    ULONG                   as_RxDFilled;   /* bytes the completer copied     */
    UINT                    as_RxDStatus;   /* last nx receive status         */
    volatile UBYTE          as_RxDState;    /* BSD_RXD_*                      */
#endif

    ULONG                   as_RcvTimeout;  /* ticks; 0 == block forever     */
    ULONG                   as_SndTimeout;
    LONG                    as_RcvBuf;      /* SO_RCVBUF, as the caller set  */
    LONG                    as_SndBuf;      /* SO_SNDBUF, as the caller set  */
    LONG                    as_LingerOn;
    LONG                    as_LingerTime;
    LONG                    as_Ttl;
    LONG                    as_Tos;

    /* TCP_USER_TIMEOUT in milliseconds, as the caller set it. The NX socket
       holds the same deadline in ticks. Kept so getsockopt answers what went
       in rather than what rounding made of it. 0 == not asked for. */
    ULONG                   as_UserTimeout;

    /*
     * IP_HDRINCL. The caller supplies its own IP header ahead of the payload
     * on a raw socket, and traceroute is the only user. See bsd_raw_hdrincl() in
     * raw.c for what survives the translation.
     */
    LONG                    as_HdrIncl;

#ifdef AMINETXDUO_MULTICAST
    /*
     * as_McastIf is a NetX interface index, or -1 for "let the route decide".
     */
    LONG                    as_McastTtl;
    LONG                    as_McastLoop;
    LONG                    as_McastIf;

#ifdef AMINETXDUO_IPV6
    /*
     * as_Mcast6If is a NetX interface index, or -1 for "let the route
     * decide". Callers name it the POSIX way, one higher.
     */
    LONG                    as_Mcast6Hops;
    LONG                    as_Mcast6If;
#endif
#endif

    /*
     * RFC 3542 (cmsg.c). as_CmsgWant is the ACW_ set above. as_CmsgSticky is
     * the source a setsockopt(IPV6_PKTINFO) named, which a per-datagram cmsg
     * on sendmsg() overrides.
     */
    ULONG                   as_CmsgWant;
    BsdCmsgSource           as_CmsgSticky;

#ifdef AMINETXDUO_IPV6
    /*
     * ICMP6_FILTER, one bit per ICMPv6 type. All ones until a caller installs
     * one, which is RFC 3542 3.2's "pass everything" default. raw.c reads it
     * on the IP thread.
     */
    ULONG                   as_Icmp6Filter[8];
#endif

    /*
     * Listening state. NetX Duo hands an incoming connection to a specific
     * socket, so a listening descriptor keeps spare sockets parked on the
     * port. accept() takes a finished one and parks a fresh one in its place.
     */
    struct AmiSocket       *as_Incoming;
    struct AmiSocket       *as_IncomingNext;
    struct AmiSocket       *as_Parent;
    UINT                    as_ListenPort;
    UINT                    as_Backlog;
    UINT                    as_IncomingCount;

    /*
     * SOCK_RAW state (raw.c). A raw socket has no NX_TCP_SOCKET and no
     * NX_UDP_SOCKET, as_Nx is unused, because NetX Duo has no raw socket
     * object: raw reception is an IP-level filter callback shared by the whole
     * stack, so the queue, the demultiplex and the wakeup are ours.
     */
    /* The urgent byte a peer sent us, held for recv(MSG_OOB), see oob.c. */
    UBYTE                   as_OobData;

    /*
     * Orderly-close list. CloseSocket() sends a FIN and returns, so the
     * connection outlives the descriptor and usually the base as well. socket.c
     */
    struct AmiSocket       *as_ClosingNext;
    ULONG                   as_ClosingAt;

    struct AmiSocket       *as_RawNext;     /* raw.c's registry link          */
    NX_PACKET              *as_RawHead;
    NX_PACKET              *as_RawTail;
    ULONG                   as_RawCount;
    ULONG                   as_RawMax;
    TX_SEMAPHORE            as_RawSem;
    BOOL                    as_RawSemOk;
} AmiSocket;

/* ------------------------------------------------------------- prototypes,
 *
 * Everything below is internal. The ABI entry points live in the generated
 * bsdsocket_vectors.h.
 */

/* library_runtime.c, what a shared library has to supply for itself. */
BOOL  bsd_runtime_open(VOID);
VOID  bsd_runtime_close(VOID);
VOID  bsd_usergroup_open(VOID);

/* netx_call.c, ThreadX context. Every call to a NetX Duo THREADS_ONLY
 * vector must be inside a successful bsd_nx_enter()/bsd_nx_leave() bracket.
 * Inside one, nothing must block on anything except ThreadX. */
LONG  bsd_nx_enter(struct AmiSocketBase *base);
VOID  bsd_nx_leave(struct AmiSocketBase *base);

/* Drop the base's cached ThreadX registration. Teardown only, no bracket open.
   See netx_call.c for what is cached and why. */
VOID  bsd_nx_release(struct AmiSocketBase *base);

#if defined(AMINETXDUO_GREEN_REALM) && defined(AMINETXDUO_RXPROBE)
ULONG ami_green_checked_wait(ULONG sigmask);
#undef Wait                     /* the NDK's inline macro, replaced whole */
#define Wait(sigmask) ami_green_checked_wait(sigmask)
#endif

/*
 * The break bits pending for this base's owner, observed without consuming.
 */
#ifdef AMINETXDUO_GREEN_REALM
ULONG bsd_break_signals(struct AmiSocketBase *base);
BOOL  bsd_nx_orphan(struct AmiSocketBase *base);
#else
static __inline ULONG bsd_break_signals(struct AmiSocketBase *base)
{
    (VOID)base;
    return SetSignal(0UL, 0UL);
}

static __inline BOOL bsd_nx_orphan(struct AmiSocketBase *base)
{
    (VOID)base;
    return TRUE;
}
#endif

struct AmiSocketBase *bsd_lib_open(register ULONG version __asm("d0"),
                                   register struct AmiSocketBase *SocketBase __asm("a6"));
APTR  bsd_lib_close(register struct AmiSocketBase *SocketBase __asm("a6"));
APTR  bsd_lib_expunge(register struct AmiSocketBase *SocketBase __asm("a6"));
APTR  bsd_lib_reserved(VOID);

/* library.c, NETCTRL_STACK_HOLD.  Make the library hold its own reference to
   the running stack so the caller's open can be closed without taking the
   network down with it.  Idempotent.  0 on success, -1 if there is no stack. */
LONG  bsd_stack_hold(struct AmiSocketBase *base);

/* Short-lived stack references for library workers that can outlive the base
   whose vector launched them. They hold no OpenCnt; the worker census keeps
   the segment loaded until the release. */
LONG  bsd_stack_transient_hold(struct AmiSocketBase *base);
VOID  bsd_stack_transient_release(struct AmiSocketBase *base);

/* library.c, the shutdown pair.  bsd_stack_unhold() gives that reference back.
   It returns 0 on success, -1 when the caller is the only one left holding the
   stack up.  bsd_stack_notify() signals every other opener and reports how
   many. */
LONG  bsd_stack_unhold(struct AmiSocketBase *base);
LONG  bsd_stack_notify(struct AmiSocketBase *base, ULONG *signalled);

/* library.c, NETSTATUS_OPENERS.  Fills up to max rows, sets *available to how
   many there were, returns how many were written. */
LONG  bsd_openers_list(struct AmiSocketBase *base, NetStatusOpener *out,
                       LONG max, LONG *available);
ULONG bsd_open_count(struct AmiSocketBase *base);

VOID  bsd_set_errno(struct AmiSocketBase *base, LONG code);
VOID  bsd_set_herrno(struct AmiSocketBase *base, LONG code);
LONG  bsd_errno_from_nx(UINT status);
/* The same, for a status from a call that was given `wait`, see errno.c. */
LONG  bsd_wait_errno(ULONG wait, UINT status);
LONG  bsd_fail(struct AmiSocketBase *base, LONG code);   /* set errno, ret -1 */

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd);
LONG       bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock);
LONG       bsd_fd_reserve(struct AmiSocketBase *base, LONG fd);
BOOL       bsd_fd_reserved(struct AmiSocketBase *base, LONG fd);
LONG       bsd_fd_free(struct AmiSocketBase *base, LONG fd);
VOID       bsd_socket_retain(AmiSocket *sock);
VOID       bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock);

/* socket.c, reclaim sockets whose orderly close has finished. Must be called
   inside a bsd_nx_enter() bracket. A no-op when the list is empty. */
VOID       bsd_closing_sweep(VOID);
VOID       bsd_closing_drain(VOID);

/* socket.c, TRUE when the socket parked on a listener holds a connection
   accept() can return, including one whose peer has already closed. Shared
   with select.c, which uses it for listener readability. */
BOOL       bsd_incoming_ready(const AmiSocket *incoming);
AmiSocket *bsd_incoming_first_ready(const AmiSocket *listener);

LONG       bsd_table_resize(struct AmiSocketBase *base, LONG size);

/* socket.c, the table size this base reports, before the table has been
   allocated as well as after. */
LONG       bsd_table_size(struct AmiSocketBase *base);

VOID       bsd_close_all(struct AmiSocketBase *base);

/* bpf.c, release the capture channels this base opened. A no-op in a build
   without AMINETXDUO_BPF. Never blocks; bsd_child_destroy() calls it. */
VOID       bsd_bpf_close_all(struct AmiSocketBase *base);

/* socket.c, the receive window this machine can afford right now. */
ULONG      ami_bsd_tcp_window(VOID);

/* handoff.c, cross-base descriptor transfer. The registry lives in the master
 * base. bsd_handoff_flush() runs from bsd_lib_close() when the last opener
 * goes, because nothing can obtain a parked socket after that. */
VOID  bsd_handoff_init(struct AmiSocketBase *master);
VOID  bsd_handoff_flush(struct AmiSocketBase *base);

/* socket.c, sockaddr helpers.
 */
LONG  bsd_sa_family(const struct sockaddr *sa, socklen_t len);
LONG  bsd_sockaddr_get(struct AmiSocketBase *base, const struct sockaddr *sa,
                       socklen_t len, NXD_ADDRESS *addr, UINT *port,
                       ULONG *scope_id);
VOID  bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                       socklen_t *len, const NXD_ADDRESS *addr, UINT port,
                       ULONG scope_id);

/* An NXD_ADDRESS holding an IPv4 address, for the many call sites that have
   nothing but a ULONG (nx_udp_source_extract, the peer of an accepted v4
   connection, ...). */
VOID  bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4);

/* options.c, as_Ttl and as_Tos onto the live NetX socket, which is what puts
   IP_TTL and IP_TOS on the wire. Caller holds the bsd_nx_enter() bracket. */
VOID  bsd_opt_apply_ip(AmiSocket *sock);

#ifdef AMINETXDUO_IPV6

/* TRUE when `addr` is ::ffff:a.b.c.d. *v4 receives a.b.c.d. */
BOOL  bsd_addr_is_v4mapped(const NXD_ADDRESS *addr, ULONG *v4);

/* ::ffff:a.b.c.d from a.b.c.d. */
VOID  bsd_addr_to_v4mapped(NXD_ADDRESS *addr, ULONG v4);

/*
 * Reduce an address to what the socket can actually use: on a socket that is
 * not V6ONLY, a v4-mapped destination becomes a plain IPv4 address, because
 * NetX Duo has no v4-mapped transmit path, it would put ::ffff:10.0.0.1 in
 * an IPv6 header and send it to a host that has no IPv6. Returns FALSE when
 * the address cannot be used on this socket at all.
 */
BOOL  bsd_addr_normalise(const AmiSocket *sock, NXD_ADDRESS *addr);

/* struct in6_addr <-> NetX Duo's four host-order ULONGs. */
VOID  bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4]);
VOID  bsd_words_to_in6(const ULONG words[4], UBYTE bytes[16]);

/* setsockopt/getsockopt for level IPPROTO_IPV6. Returns 0, or -1 with errno
   set. ENOPROTOOPT means an option this library does not implement. */
LONG  bsd_setsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                          LONG level, LONG optname, APTR optval,
                          socklen_t optlen);
LONG  bsd_getsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                          LONG level, LONG optname, APTR optval,
                          socklen_t *optlen);

/* Whether the Linux option numbering can be read on this socket. FALSE on a
   raw one, where RFC 3542 claims 26 for IPV6_CHECKSUM; the whole rationale is
   at the definition in in6.c. */
BOOL  bsd_v6_linux_numbering(const AmiSocket *sock);
#endif /* AMINETXDUO_IPV6 */

/* cmsg.c, RFC 3542 ancillary data.
 */
VOID  bsd_cmsg_reset(AmiSocket *sock);
LONG  bsd_cmsg_option(struct AmiSocketBase *base, AmiSocket *sock, LONG level,
                      LONG optname, APTR optval, socklen_t *optlen, BOOL set);
VOID  bsd_cmsg_build(AmiSocket *sock, NX_PACKET *packet, struct msghdr *msg);
LONG  bsd_cmsg_parse(struct AmiSocketBase *base, AmiSocket *sock,
                     const struct msghdr *msg, BsdCmsgSource *out);
LONG  bsd_cmsg_source_index(NX_IP *ip, const BsdCmsgSource *src, BOOL v6);

/* oob.c, TCP urgent data (MSG_OOB, SIOCATMARK, SIGURG).
 */
LONG  bsd_oob_send(struct AmiSocketBase *base, AmiSocket *sock, UBYTE byte,
                   LONG flags);
VOID  bsd_tcp_urgent_notify(NX_TCP_SOCKET *socket_ptr);
BOOL  bsd_oob_take(AmiSocket *sock, UBYTE *out);

/* raw.c, SOCK_RAW.
 *
 * Every one of these must be called inside a bsd_nx_enter() bracket, except
 * bsd_raw_available() and bsd_raw_source(), which only read.
 */
/*
 * The IP-layer MTU of the interface a datagram to `addr` would leave by, or -1
 * if the route does not resolve. Both send paths measure against it and refuse
 * an oversize datagram with EMSGSIZE rather than let transmit fragmentation
 */
LONG       bsd_route_mtu(NX_IP *ip, const NXD_ADDRESS *addr,
                         const NX_INTERFACE *source_interface);

LONG       bsd_raw_open(struct AmiSocketBase *base, AmiSocket *sock);
VOID       bsd_raw_close(AmiSocket *sock);
LONG       bsd_raw_send_packet(struct AmiSocketBase *base, AmiSocket *sock,
                               NX_PACKET *packet, const NXD_ADDRESS *addr,
                               ULONG scope, const BsdCmsgSource *src);
NX_PACKET *bsd_raw_receive(AmiSocket *sock, ULONG wait, UINT *why);
VOID       bsd_raw_source(NX_PACKET *packet, NXD_ADDRESS *addr);
ULONG      bsd_raw_available(AmiSocket *sock);
VOID       bsd_raw_revalidate_endpoint(AmiSocket *sock);

#ifdef AMINETXDUO_MULTICAST
/* mcast.c, RFC 1112 group membership and the IPPROTO_IP multicast options.
 */
LONG bsd_mcast_setopt(struct AmiSocketBase *base, AmiSocket *sock,
                      LONG optname, APTR optval, socklen_t optlen);
LONG bsd_mcast_getopt(struct AmiSocketBase *base, AmiSocket *sock,
                      LONG optname, APTR optval, socklen_t *optlen);
VOID bsd_mcast_close(AmiSocket *sock);
LONG bsd_mcast_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr);

#ifdef AMINETXDUO_IPV6
/* The RFC 3493 section 5.2 half, same file and same shape. in6.c dispatches
 * the five. bsd_mcast_close() drops both families' memberships.
 */
#define BSD_MCAST6_NO_LINK      (-2L)

LONG bsd_mcast6_setopt(struct AmiSocketBase *base, AmiSocket *sock,
                       LONG optname, APTR optval, socklen_t optlen);
LONG bsd_mcast6_getopt(struct AmiSocketBase *base, AmiSocket *sock,
                       LONG optname, APTR optval, socklen_t *optlen);
BOOL bsd_mcast6_is_option(const AmiSocket *sock, LONG optname);
LONG bsd_mcast6_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr,
                             ULONG *saved);
VOID bsd_mcast6_finish_send(ULONG saved);
#endif
#endif

/* select.c, event plumbing.
 *
 */
VOID  bsd_events_attach(AmiSocket *sock);
VOID  bsd_event_post(AmiSocket *sock, ULONG events);
VOID  bsd_listen_callback(NX_TCP_SOCKET *socket_ptr, UINT port);
VOID  bsd_tcp_disconnect_callback(NX_TCP_SOCKET *socket_ptr);
BOOL  bsd_readable(AmiSocket *sock);
BOOL  bsd_writable(AmiSocket *sock);
BOOL  bsd_exception(AmiSocket *sock);

/* Wait option for a blocking call, in ThreadX ticks. */
/* A NetX Duo call narrowed to (wait timeout) -> status, so bsd_wait_sliced()
   can drive it in slices without knowing which call it is. */
typedef UINT (*BsdSlicedCall)(VOID *arg, ULONG wait);

UINT bsd_wait_sliced(struct AmiSocketBase *base, ULONG wait,
                     BsdSlicedCall call, VOID *arg, BOOL *aborted);

ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks, LONG flags);

/*
 * The break-signal test the resolver's retransmission ladder asks between
 * queries (resolver.c). `arg` is the struct AmiSocketBase whose sb_BreakMask
 * applies. The signature is netstack's AmiNetGiveUpFn.
 */
BOOL  bsd_resolve_break(VOID *arg);

/*
 * Returns the next real tag, advancing *cursor past it, or NULL at the end.
 * TAG_MORE follows the chain, TAG_SKIP skips ti_Data further items and
 * TAG_IGNORE skips itself. None of the three is ever handed back.
 */
struct TagItem *bsd_next_tag(struct TagItem **cursor);

/* Small string/memory helpers: a shared library must not drag in newlib. */
ULONG bsd_strlen(const char *s);
VOID  bsd_strncpy(char *dst, const char *src, ULONG size);
VOID  bsd_bzero(APTR p, ULONG size);
VOID  bsd_bcopy(const APTR src, APTR dst, ULONG size);

/* Host byte order and network byte order are the same on m68k. Spelled out
   for clarity. */
#define BSD_HTONL(x)    ((ULONG)(x))
#define BSD_NTOHL(x)    ((ULONG)(x))
#define BSD_HTONS(x)    ((UWORD)(x))
#define BSD_NTOHS(x)    ((UWORD)(x))

/*
 * Would a socket bound to as_LocalAddr take traffic that arrived on `nxif`?
 * TRUE for the wildcard. socket.c owns it. transfer.c filters received
 * datagrams with it, accept() filters completed connections.
 */
BOOL bsd_bind_wants_interface(const AmiSocket *sock, const NX_INTERFACE *nxif);

/*
 * Is this the peer a connected UDP socket named? TRUE for a socket that never
 * connected. transfer.c owns it and filters received datagrams with it.
 * select.c asks it of an ICMP error's peer.
 */
BOOL bsd_udp_from_peer(const AmiSocket *sock, const NXD_ADDRESS *src,
                       UINT src_port, ULONG src_scope);

/* The complete receive-side UDP endpoint predicates: local bind plus connected
 * peer. A queued packet still starts at its UDP header; nx_udp_socket_receive()
 * has stripped that header from a received packet. transfer.c owns both;
 * select.c uses the queued form to avoid false readability. */
BOOL bsd_udp_accepts_packet(const AmiSocket *sock, const NX_PACKET *packet);
BOOL bsd_udp_accepts_received_packet(const AmiSocket *sock,
                                     const NX_PACKET *packet);
ULONG bsd_udp_available(const AmiSocket *sock);

#ifdef AMINETXDUO_RX_DIRECT_COMPLETE
/* as_RxDState.  IDLE is zero so a MEMF_CLEAR socket starts unarmed. */
#define BSD_RXD_IDLE    0
#define BSD_RXD_ARMED   1
#define BSD_RXD_DONE    2

/* The completer half of the pending-receive descriptor. transfer.c owns it;
 * select.c's receive notify calls it on the IP thread when a descriptor is
 * armed, with may_release FALSE -- rxdirect.c says why. Never suspends. */
VOID bsd_rxdirect_pump(AmiSocket *sock, BOOL may_release);
#endif

/*
 * The send direction of the same question: which source must a datagram from
 * this socket leave with? socket.c owns it. transfer.c and raw.c use it to
 */
typedef enum
{
    BSD_SOURCE_ROUTE = 0,   /* nothing pinned: NetX can route as it likes  */
    BSD_SOURCE_INDEX,       /* pinned; *index is what source_send() wants  */
    BSD_SOURCE_REFUSE,      /* pinned to something absent: EADDRNOTAVAIL   */
    BSD_SOURCE_UNREACH      /* pinned, but no route from there: ENETUNREACH */
} BsdSourceKind;

BsdSourceKind bsd_source_select(const AmiSocket *sock, const NXD_ADDRESS *dest,
                                ULONG scope, UINT *index);

#endif /* AMINETXDUO_BSDSOCKET_INTERNAL_H */
