/*
 * bsdsocket.library -- internal interfaces.
 *
 * AmiTCP/Roadshow-compatible socket library implemented directly on the
 * native NetX Duo APIs (nx_tcp_socket_*, nx_udp_socket_*, nx_packet_*), not
 * on addons/BSD/nxd_bsd.c -- see docs/RESEARCH.md S6.4 for why.
 *
 * Structure:
 *   library.c    romtag, Open/Close/Expunge, per-opener child bases
 *   socket.c     socket/bind/listen/accept/connect/shutdown/CloseSocket
 *   transfer.c   send/sendto/recv/recvfrom
 *   options.c    set/getsockopt, IoctlSocket, get{sock,peer}name, Dup2Socket
 *   select.c     WaitSelect, SetSocketSignals, GetSocketEvents, NX callbacks
 *   errno.c      Errno, SetErrnoPtr, SocketBaseTagList, status mapping
 *   inet.c       the inet_* address conversions
 *   resolver.c   gethostby*, gethostname, gethostid
 *   netx_call.c  the ThreadX context bracket every NetX Duo call needs
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_BSDSOCKET_INTERNAL_H
#define AMINETXDUO_BSDSOCKET_INTERNAL_H

/*
 * ThreadX and NetX Duo first, always. tx_port.h *typedefs* VOID, CHAR, UINT
 * and friends; <exec/types.h> guards its own definitions with #ifndef, so it
 * tolerates being second but not first.
 */
#include "tx_api.h"
#include "nx_api.h"

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
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>

/*
 * <sys/socket.h> uses size_t/ssize_t but does not pull them in itself, and we
 * cannot rely on the ThreadX port header having included <stdlib.h> first.
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

/*
 * The AF_INET6 names the NDK lacks. Published -- it ships in the archive's
 * Developer drawer -- so the numbers below are derived from it rather than
 * restated: two copies of an ABI constant is one copy too many.
 */
#include "aminetxduo/in6.h"

/* ------------------------------------------------------------------ errno --
 *
 * AmiTCP/Roadshow errno values are 4.4BSD's, not what the newlib headers
 * in this toolchain define (they use the Linux numbering: EWOULDBLOCK
 * 11, ENOTSOCK 108, ...). Applications compare against the AmiTCP netinclude
 * values, so those are the numbers that go into the caller's errno. They are
 * ABI constants; the prefixed names keep them clear of <errno.h>.
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

/* ------------------------------------------------------------------ IPv6 --
 *
 * What the roadshow NDK already defines, verified against
 * amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include:
 *
 *   sys/socket.h:196   AF_INET6 23          -- and note it collides with
 *                                              AF_IPX 23 three lines above.
 *                                              Nothing here uses AF_IPX.
 *   netinet/in.h:178   struct in6_addr      -- unsigned char s6_addr[16]
 *   netinet/in.h:182   struct sockaddr_in6  -- sin6_family, sin6_port,
 *                                              sin6_flowinfo, sin6_addr,
 *                                              sin6_scope_id
 *
 * What it does not define is in aminetxduo/in6.h, included above: IPPROTO_IPV6,
 * every IPV6_* socket option, INET6_ADDRSTRLEN, IN6ADDR_*_INIT, the
 * IN6_IS_ADDR_* macros, sockaddr_storage, PF_INET6 and AI_ADDRCONFIG. That
 * header is PUBLISHED -- it ships in the archive's Developer drawer -- and the
 * BSD numbers below are aliases of it, not second copies. An application built
 * against this NDK cannot name any of them, so it will have spelled them out
 * itself, which is why in6.h #ifndef-guards every one.
 *
 * The BSD/Linux pairs are here rather than there because accepting both
 * numberings is behaviour, not ABI: in6.h publishes the one number a caller
 * should use, and these are what setsockopt() matches against.
 *
 * struct sockaddr_in6 is a trap, and in6.h carries the warning in full for
 * the callers who need it: `struct sockaddr_in` here is 4.4BSD's, sin_len at
 * offset 0 and sin_family at 1, while `struct sockaddr_in6` right below it is
 * the Linux one -- pasted in verbatim, comment about "Scope ID (new in 2.4)"
 * and all -- with sin6_family at offset 0 and no sin6_len. Reading
 * sa->sa_family out of a sockaddr_in6 reads its padding byte. (Same class of
 * hazard as ndk-include/pwd.h being newlib's 10-field struct passwd rather
 * than the Amiga's 7-field one.) So bsd_sa_family() below decides the family
 * from the bytes and the length rather than from a struct member, and in6.c
 * pins every offset with _Static_assert.
 */

#define AMI_IPPROTO_IPV6            IPPROTO_IPV6

/*
 * IPV6_V6ONLY has two numberings in the wild and the NDK picks neither:
 * 27 in KAME and the BSDs (netinet6/in6.h), 26 in Linux. This header set is
 * 4.4BSD everywhere except the pasted-in sockaddr_in6, which is Linux, so
 * there is no lineage to defer to. Both are accepted, and getsockopt answers
 * to both. No collision risk: 26 is IPV6_CHECKSUM in BSD (raw sockets, which
 * this library does not offer) and 27 is IPV6_JOIN_ANYCAST in Linux (likewise).
 */
#define AMI_IPV6_V6ONLY_BSD         IPV6_V6ONLY
#define AMI_IPV6_V6ONLY_LINUX       26

/* IPV6_UNICAST_HOPS: 4 in BSD, 16 in Linux. Same argument, same treatment. */
#define AMI_IPV6_UNICAST_HOPS_BSD   IPV6_UNICAST_HOPS
#define AMI_IPV6_UNICAST_HOPS_LINUX 16

/*
 * IPV6_TCLASS: 61 in BSD, 67 in Linux. Darwin's 36 is deliberately not
 * accepted -- Apple kept it for binary compatibility, and 36 is
 * IPV6_RECVPKTINFO in BSD and IPV6_HDRINCL in Linux, so it names something
 * else in both lineages this header set could belong to. 61 is Linux's
 * IPV6_PATHMTU, which this library does not offer.
 */
#define AMI_IPV6_TCLASS_BSD         IPV6_TCLASS
#define AMI_IPV6_TCLASS_LINUX       67

#define AMI_INET6_ADDRSTRLEN        INET6_ADDRSTRLEN

/* --------------------------------------------------------------- library -- */

#define BSD_LIB_NAME        "bsdsocket.library"
#define BSD_LIB_VERSION     4
/*
 * lib_Version is the ABI number callers pass to OpenLibrary() and never
 * changes. lib_Revision identifies which of our builds this is, so a command
 * can tell a library that has the netstatus vectors from one that does not.
 * include/aminetxduo/netstatus.h has the other half.
 *
 *   0  v0.2.0 and earlier
 *   1  NetStackQuery/NetStackControl at -0x366/-0x36c
 *   2  NetStatusControl grew nsc_Name (AMI_NETSTATUS_VERSION 6)
 *   3  RFC 3493 if_nametoindex/if_indextoname/if_nameindex/if_freenameindex
 *      at -0x372..-0x384, the first slots past the end of the NDK's table
 */
#define BSD_LIB_REVISION    3

/* SBTC_LOGFACILITY's documented default. The NDK's <sys/syslog.h> ships the
   priority codes only, so the BSD facility value is spelled out here. */
#define BSD_LOG_USER        (1L << 3)

/*
 * 256, matching Roadshow's documented default. <sys/types.h> makes FD_SETSIZE
 * 256, so `WaitSelect(FD_SETSIZE, ...)` -- common in ported code -- failed
 * with EINVAL on the 64 we used to ship.
 *
 * The table is a lazily allocated array of pointers, so this costs 1,024
 * bytes instead of 256 for an opener that makes a socket, and nothing for one
 * that does not. SBTC_DTABLESIZE can raise or lower it.
 *
 * The ceiling is the largest table SBTC_DTABLESIZE will hand out. It also
 * sizes the fd_set scratch in WaitSelect() and tcp_handler.c, 4 bytes per 32
 * descriptors per set. A caller asking for more than FD_SETSIZE descriptors
 * has to build its own wider fd_set to name them.
 */
#define BSD_DEFAULT_DTABLESIZE 256
#define BSD_MAX_DTABLESIZE     1024

/* Moved here from select.c: sb_SelIn below needs the type. */
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

/*
 * TCP receive window. This is what limits a bulk transfer, and it was measured
 * -- see tests/trace/. On loopback the sender held exactly one 4096-byte
 * segment in flight against a 4096-byte advertised window, 100% of it, and
 * waited 14.9 ms (median) between segments; over the wire it reached 7200
 * bytes against 8192, 88%. Neither the CPU, the link nor the periodic tick was
 * the limit.
 *
 * ami_bsd_tcp_window() is what sockets actually get; this is only its floor.
 * The floor is what shipped before it, and what forty concurrent transfers
 * were measured passing on. Setting this and the ceiling to the same value
 * pins the window, which is how a fixed window is measured against the derived
 * one.
 */
#ifndef BSD_TCP_WINDOW
#define BSD_TCP_WINDOW        8192
#endif

/*
 * Ceiling. 65535 is the architectural cap: NX_ENABLE_TCP_WINDOW_SCALING is not
 * defined here, so the window is whatever fits in the 16-bit header field and
 * nothing negotiates past it in either direction -- not offering the option
 * also stops the peer scaling. 32768 is the largest value that has been
 * measured (docs/RESEARCH.md S16.5, S24). Two reasons not to go to the cap
 * untested: there is no SACK in the vendored tree, so a burst loss inside a
 * larger window costs a full go-back-N, and every byte of window is a byte of
 * packet pool somebody else cannot have.
 */
#ifndef BSD_TCP_WINDOW_CEILING
#define BSD_TCP_WINDOW_CEILING  32768
#endif

/*
 * One in this many pool packets is the budget the whole stack's TCP receive
 * windows may claim ABOVE the floor. See ami_bsd_tcp_window().
 */
#ifndef BSD_TCP_WINDOW_POOL_SHARE
#define BSD_TCP_WINDOW_POOL_SHARE   8
#endif

/* Room for one dotted quad plus terminator, used by Inet_NtoA(). */
#define BSD_NTOA_BUFLEN         16

struct AmiSocket;

/*
 * One of these per OpenLibrary(). The master base (sb_Master == NULL) owns
 * the segment list and the child list; every opener gets a byte-for-byte
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

    /* ---- master only ---------------------------------------------------- */
    struct SignalSemaphore  sb_Lock;        /* guards the child list + stack */
    struct MinList          sb_Children;
    ULONG                   sb_StackRefs;   /* netstack_startup() references */

    /* Cross-base descriptor hand-off (handoff.c). Two tasks' sockets meet
     * only here, so it lives in the master base. */
    struct MinList          sb_Handoffs;
    LONG                    sb_NextHandoffId;

    /* ---- per opener ----------------------------------------------------- */
    struct Task            *sb_Task;        /* the task that opened us       */

    /*
     * ThreadX context for this opener's task (netx_call.c). NetX Duo's
     * THREADS_ONLY vectors reject a caller that is not a TX_THREAD, so every
     * NetX-touching entry point brackets itself with bsd_nx_enter()/leave() --
     * the shared ami_netstack_enter()/leave() bracket plus a nesting counter.
     * The control block lives here rather than on the stack: it is a few
     * hundred bytes, a base belongs to one task, and that task is inside at
     * most one vector at a time.
     */
    AmiNetCaller            sb_NxCaller;

    /*
     * WaitSelect()'s input sets, here for the reason above and then some: they
     * are sized to BSD_MAX_DTABLESIZE rather than to the caller's nfds, so on
     * the stack they cost 384 bytes of the CALLER's stack on every call
     * whether it is watching two descriptors or a thousand. An Amiga caller may
     * have 4 KB and no guard page, and a program that sits in WaitSelect() in a
     * loop is the normal shape of a network client -- measured at 864 bytes for
     * the whole frame before this moved (-fstack-usage), which real hardware
     * reported as freezes under sustained TCP.
     */
    BsdFdSets               sb_SelIn;
    BsdFdSets               sb_SelReady;   /* and its result sets    */
    LONG                    sb_NxNest;      /* bracket depth, 0 == outside   */

#ifdef AMINETXDUO_NXCENSUS
    /* Measurement build only -- see the census note in netx_call.c. */
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
    APTR                    sb_ErrnoPtr;    /* caller-supplied, may be NULL  */
    LONG                    sb_ErrnoSize;   /* 1, 2 or 4                     */
    LONG                    sb_HErrno;
    LONG                   *sb_HErrnoPtr;

    ULONG                   sb_BreakMask;   /* SBTC_BREAKMASK, Ctrl-C default*/
    ULONG                   sb_SigIOMask;   /* SBTC_SIGIOMASK                */
    ULONG                   sb_SigUrgMask;  /* SBTC_SIGURGMASK               */
    ULONG                   sb_SigEventMask;/* SBTC_SIGEVENTMASK             */

    /* SBTC_SIG_ADDRESS_CHANGE_MASK. Stored, not yet delivered: the change
       point is ami_ns_address_changed() in src/netstack/. Default 0 = no
       notification, so an opener that never sets it sees no difference. */
    ULONG                   sb_SigAddressChangeMask;

    /* SBTC_CAN_SHARE_LIBRARY_BASES. Recorded only -- this library never
       restricts a base to its opening task, so sharing already works. */
    ULONG                   sb_CanShareBases;

    STRPTR                  sb_LogTag;      /* SBTC_LOGTAGPTR                */
    LONG                    sb_LogStat;
    LONG                    sb_LogFacility; /* SBTC_LOGFACILITY, BSD_LOG_USER*/
    LONG                    sb_LogMask;     /* SBTC_LOGMASK, 0xFF            */
    LONG                  (*sb_FDCallback)(LONG fd, LONG action);

    /* SBTC_ERROR_HOOK: called on every errno/h_errno change. */
    struct Hook            *sb_ErrorHook;

    /*
     * Wakeup plumbing. sb_EventSignal is allocated by the opening task in
     * bsd_lib_open() -- Exec signals belong to a task, and a base belongs to
     * exactly one task, so this is safe. Every NetX Duo receive/connect/
     * disconnect callback ends up doing Signal(sb_Task, sb_EventSigMask).
     */
    BYTE                    sb_EventSignal;
    ULONG                   sb_EventSigMask;

    /* Lazily opened by WaitSelect() when a caller passes a timeout. */
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
};

/* ---------------------------------------------------------------- socket -- */

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
#define ASF_INCOMING    (1UL << 10)   /* listen slave; has no descriptor yet */
#define ASF_ACCEPTPEND  (1UL << 11)   /* listen callback fired              */
#define ASF_REUSEADDR   (1UL << 12)
#define ASF_BROADCAST   (1UL << 13)
#define ASF_KEEPALIVE   (1UL << 14)
#define ASF_OOBINLINE   (1UL << 15)
#define ASF_DELETED     (1UL << 16)   /* NX socket already torn down        */
#define ASF_NXBOUND     (1UL << 17)   /* NetX Duo holds the port            */
#define ASF_SERVER      (1UL << 18)   /* came off a listen port: unaccept   */
#define ASF_ORPHANED    (1UL << 19)   /* NX would not delete it; leaked     */
#define ASF_INET6       (1UL << 20)   /* created with domain AF_INET6       */
#define ASF_V6ONLY      (1UL << 21)   /* IPV6_V6ONLY; see options.c         */
#define ASF_RAW         (1UL << 22)   /* SOCK_RAW; see raw.c                */
#define ASF_OOBHAVE     (1UL << 23)   /* an urgent byte is waiting; oob.c   */
#define ASF_CLOSING     (1UL << 24)   /* FIN sent, parked for a late reap   */

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

    /*
     * Addresses are NXD_ADDRESS, not ULONG, in both build configurations.
     * NetX Duo's own IPv4 entry points are thin wrappers that build one of
     * these and call the nxd_* function underneath (nx_tcp_client_socket_
     * connect.c is three lines and a delegation), so carrying the version tag
     * here costs the IPv4-only build four bytes per address and no code, and
     * removes the "which family is this" branch from the call sites.
     *
     * nxd_ip_address.v6 only exists when FEATURE_NX_IPV6 is on, so anything
     * touching it is still under #ifdef AMINETXDUO_IPV6.
     */
    NXD_ADDRESS             as_LocalAddr;
    UINT                    as_LocalPort;
    NXD_ADDRESS             as_PeerAddr;
    UINT                    as_PeerPort;

    /*
     * sin6_scope_id, kept so that getsockname()/getpeername() report back what
     * bind()/connect() were given. It does not select an outgoing interface:
     * NetX Duo's dual-stack connect and send entry points take no interface
     * parameter and pick the source themselves through
     * _nxd_ipv6_interface_find(). With one Ethernet interface, which is all
     * this runs on today, the two answers are the same.
     */
    ULONG                   as_ScopeId;

    /* Partially consumed receive packet (a stream read need not drain one). */
    NX_PACKET              *as_RxPending;
    ULONG                   as_RxOffset;

    ULONG                   as_RcvTimeout;  /* ticks; 0 == block forever     */
    ULONG                   as_SndTimeout;
    LONG                    as_RcvBuf;      /* SO_RCVBUF, as the caller set  */
    LONG                    as_SndBuf;      /* SO_SNDBUF, as the caller set  */
    LONG                    as_LingerOn;
    LONG                    as_LingerTime;
    LONG                    as_Ttl;
    LONG                    as_Tos;

    /*
     * IP_HDRINCL. The caller supplies its own IP header ahead of the payload
     * on a raw socket; traceroute is the only user. See bsd_raw_hdrincl() in
     * raw.c for what survives the translation.
     */
    LONG                    as_HdrIncl;

    /*
     * Listening state. NetX Duo hands an incoming connection to a specific
     * socket, so a listening descriptor keeps a spare socket parked on the
     * port; accept() takes it and relistens a fresh one. docs/RESEARCH.md S6.4.
     */
    struct AmiSocket       *as_Incoming;
    struct AmiSocket       *as_Parent;
    UINT                    as_ListenPort;
    UINT                    as_Backlog;

    /*
     * SOCK_RAW state (raw.c). A raw socket has no NX_TCP_SOCKET and no
     * NX_UDP_SOCKET -- as_Nx is unused -- because NetX Duo has no raw socket
     * object: raw reception is an IP-level filter callback shared by the whole
     * stack, so the queue, the demultiplex and the wakeup are ours.
     *
     * as_RawSem is what a blocking recv() suspends on. It cannot be the Exec
     * signal the rest of the library wakes on: the filter runs on the NetX Duo
     * IP thread, which is a ThreadX thread and must not touch Exec, and the
     * receiver is already inside a bsd_nx_enter() bracket where ThreadX
     * suspension is the right way to wait (as nx_tcp_socket_receive does).
     * bsd_event_post() still fires as well, so WaitSelect() sees it.
     */
    /* The urgent byte a peer sent us, held for recv(MSG_OOB) -- see oob.c. */
    UBYTE                   as_OobData;

    /*
     * Orderly-close list. CloseSocket() sends a FIN and returns, so the
     * connection outlives the descriptor and usually the base as well; socket.c
     * keeps the block here until TCP has finished with it. as_ClosingAt is a
     * tx_time_get() deadline, so a peer that never answers cannot pin the block
     * for ever.
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

/* ------------------------------------------------------------- prototypes --
 *
 * Everything below is internal; the ABI entry points live in the generated
 * bsdsocket_vectors.h.
 */

/* library_runtime.c -- what a shared library has to supply for itself. */
BOOL  bsd_runtime_open(VOID);
VOID  bsd_runtime_close(VOID);

/* netx_call.c -- ThreadX context. Nothing may call a NetX Duo THREADS_ONLY
 * vector outside a successful bsd_nx_enter()/bsd_nx_leave() bracket, and
 * nothing inside one may block on anything except ThreadX. */
LONG  bsd_nx_enter(struct AmiSocketBase *base);
VOID  bsd_nx_leave(struct AmiSocketBase *base);

/* Drop the base's cached ThreadX registration. Teardown only, no bracket open;
   see netx_call.c for what is cached and why. */
VOID  bsd_nx_release(struct AmiSocketBase *base);

/* library.c */
struct AmiSocketBase *bsd_lib_open(register ULONG version __asm("d0"),
                                   register struct AmiSocketBase *SocketBase __asm("a6"));
APTR  bsd_lib_close(register struct AmiSocketBase *SocketBase __asm("a6"));
APTR  bsd_lib_expunge(register struct AmiSocketBase *SocketBase __asm("a6"));
APTR  bsd_lib_reserved(VOID);

/* errno.c */
VOID  bsd_set_errno(struct AmiSocketBase *base, LONG code);
VOID  bsd_set_herrno(struct AmiSocketBase *base, LONG code);
LONG  bsd_errno_from_nx(UINT status);
/* The same, for a status from a call that was given `wait` -- see errno.c. */
LONG  bsd_wait_errno(ULONG wait, UINT status);
LONG  bsd_fail(struct AmiSocketBase *base, LONG code);   /* set errno, ret -1 */

/* socket.c -- descriptor table */
AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd);
LONG       bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock);
VOID       bsd_fd_free(struct AmiSocketBase *base, LONG fd);
VOID       bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock);

/* socket.c -- reclaim sockets whose orderly close has finished. Must be
   called inside a bsd_nx_enter() bracket; a no-op when the list is empty. */
VOID       bsd_closing_sweep(VOID);

/* socket.c -- TRUE when the socket parked on a listener holds a connection
   accept() can return, including one whose peer has already closed. Shared
   with select.c, which uses it for listener readability. */
BOOL       bsd_incoming_ready(const AmiSocket *incoming);

LONG       bsd_table_resize(struct AmiSocketBase *base, LONG size);

/* socket.c -- the table size this base reports, before the table has been
   allocated as well as after. */
LONG       bsd_table_size(struct AmiSocketBase *base);

VOID       bsd_close_all(struct AmiSocketBase *base);

/* bpf.c -- release the capture channels this base opened. A no-op in a build
   without AMINETXDUO_BPF. Never blocks; bsd_child_destroy() calls it. */
VOID       bsd_bpf_close_all(struct AmiSocketBase *base);

/* socket.c -- the receive window this machine can afford right now. */
ULONG      ami_bsd_tcp_window(VOID);

/* handoff.c -- cross-base descriptor transfer. The registry lives in the
 * master base; bsd_handoff_flush() runs from bsd_lib_close() when the last
 * opener goes, because nothing can obtain a parked socket after that. */
VOID  bsd_handoff_init(struct AmiSocketBase *master);
VOID  bsd_handoff_flush(struct AmiSocketBase *base);

/* socket.c -- sockaddr helpers.
 *
 * bsd_sa_family() decides what a caller's `struct sockaddr *` actually is; see
 * the sockaddr_in6 note above for why that cannot be a struct member read.
 * Returns AF_INET, AF_INET6, AF_UNSPEC, or -1 for anything it will not touch.
 *
 * bsd_sockaddr_get() parses one into an NXD_ADDRESS + port (+ scope id, which
 * may be NULL); bsd_sockaddr_put() writes one back in the shape the socket's
 * family calls for, which is not always the shape of the address -- an
 * AF_INET6 socket reports a v4 peer as ::ffff:a.b.c.d.
 */
LONG  bsd_sa_family(const struct sockaddr *sa, socklen_t len);
LONG  bsd_sockaddr_get(struct AmiSocketBase *base, const struct sockaddr *sa,
                       socklen_t len, NXD_ADDRESS *addr, UINT *port,
                       ULONG *scope_id);
VOID  bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                       socklen_t *len, const NXD_ADDRESS *addr, UINT port);

/* An NXD_ADDRESS holding an IPv4 address, for the many call sites that have
   nothing but a ULONG (nx_udp_source_extract, the peer of an accepted v4
   connection, ...). */
VOID  bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4);

#ifdef AMINETXDUO_IPV6
/* in6.c -- everything that only exists in the dual-stack build. */

/* TRUE when `addr` is ::ffff:a.b.c.d; *v4 receives a.b.c.d. */
BOOL  bsd_addr_is_v4mapped(const NXD_ADDRESS *addr, ULONG *v4);

/* ::ffff:a.b.c.d from a.b.c.d. */
VOID  bsd_addr_to_v4mapped(NXD_ADDRESS *addr, ULONG v4);

/*
 * Reduce an address to what the socket can actually use: on a socket that is
 * not V6ONLY, a v4-mapped destination becomes a plain IPv4 address, because
 * NetX Duo has no v4-mapped transmit path -- it would put ::ffff:10.0.0.1 in
 * an IPv6 header and send it to a host that has no IPv6. Returns FALSE when
 * the address cannot be used on this socket at all.
 */
BOOL  bsd_addr_normalise(const AmiSocket *sock, NXD_ADDRESS *addr);

/* struct in6_addr <-> NetX Duo's four host-order ULONGs. */
VOID  bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4]);
VOID  bsd_words_to_in6(const ULONG words[4], UBYTE bytes[16]);

/* setsockopt/getsockopt for level IPPROTO_IPV6. Returns 0, or -1 with errno
   set; ENOPROTOOPT for an option this library does not implement. */
LONG  bsd_setsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                          LONG optname, APTR optval, socklen_t optlen);
LONG  bsd_getsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                          LONG optname, APTR optval, socklen_t *optlen);
#endif /* AMINETXDUO_IPV6 */

/* oob.c -- TCP urgent data (MSG_OOB, SIOCATMARK, SIGURG).
 *
 * bsd_oob_send() sends one byte with the URG bit set; it must be called
 * inside a bsd_nx_enter() bracket and returns 1, or -1 with errno set.
 *
 * bsd_tcp_urgent_notify() is what nx_tcp_socket_create() is handed as its
 * urgent-data callback, and runs on the NetX Duo IP thread.
 *
 * bsd_oob_take() hands the stored byte to recv(MSG_OOB) and clears the mark;
 * TRUE means there was one.
 */
LONG  bsd_oob_send(struct AmiSocketBase *base, AmiSocket *sock, UBYTE byte);
VOID  bsd_tcp_urgent_notify(NX_TCP_SOCKET *socket_ptr);
BOOL  bsd_oob_take(AmiSocket *sock, UBYTE *out);

/* raw.c -- SOCK_RAW.
 *
 * Every one of these must be called inside a bsd_nx_enter() bracket, except
 * bsd_raw_available() and bsd_raw_source(), which only read.
 *
 * bsd_raw_open() registers the socket with the IP-level filter (installing it
 * on the first raw socket); bsd_raw_close() unregisters it, drains its queue
 * and removes the filter again when the last one goes.
 *
 * bsd_raw_send_packet() hands a packet the caller has already filled to
 * nxd_ip_raw_packet_send(), which prepends the IP header. The packet is
 * consumed either way -- released here on failure -- so the caller must not
 * touch it again.
 *
 * bsd_raw_receive() dequeues one whole IP datagram, header included, or NULL.
 * The caller owns it and must nx_packet_release() it.
 */
LONG       bsd_raw_open(struct AmiSocketBase *base, AmiSocket *sock);
VOID       bsd_raw_close(AmiSocket *sock);
LONG       bsd_raw_send_packet(struct AmiSocketBase *base, AmiSocket *sock,
                               NX_PACKET *packet, const NXD_ADDRESS *addr);
NX_PACKET *bsd_raw_receive(AmiSocket *sock, ULONG wait, UINT *why);
VOID       bsd_raw_source(NX_PACKET *packet, NXD_ADDRESS *addr);
ULONG      bsd_raw_available(AmiSocket *sock);

/* select.c -- event plumbing.
 *
 * bsd_events_attach() installs the NetX Duo receive/connect/disconnect
 * callbacks on a freshly created socket; every one of them ends up in
 * bsd_event_post(), which records the FD_* bits and signals the owning task.
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

ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks);

/*
 * NextTagItem(), open-coded (errno.c). utility.library may not be open when
 * this library is called, and the four control tags are three lines of
 * arithmetic. Shared by interfaces.c, routing.c and addralloc.c.
 *
 * Returns the next real tag, advancing *cursor past it, or NULL at the end.
 * TAG_MORE follows the chain, TAG_SKIP skips ti_Data further items and
 * TAG_IGNORE skips itself; none of the three is ever handed back.
 */
struct TagItem *bsd_next_tag(struct TagItem **cursor);

/* Small string/memory helpers: a shared library must not drag in newlib. */
ULONG bsd_strlen(const char *s);
VOID  bsd_strncpy(char *dst, const char *src, ULONG size);
VOID  bsd_bzero(APTR p, ULONG size);
VOID  bsd_bcopy(const APTR src, APTR dst, ULONG size);

/* Host byte order == network byte order on m68k; spelled out for clarity. */
#define BSD_HTONL(x)    ((ULONG)(x))
#define BSD_NTOHL(x)    ((ULONG)(x))
#define BSD_HTONS(x)    ((UWORD)(x))
#define BSD_NTOHS(x)    ((UWORD)(x))

/*
 * Would a socket bound to as_LocalAddr take traffic that arrived on `nxif`?
 * TRUE for the wildcard. socket.c owns it; transfer.c filters received
 * datagrams with it, accept() filters completed connections.
 */
BOOL bsd_bind_wants_interface(const AmiSocket *sock, const NX_INTERFACE *nxif);

#endif /* AMINETXDUO_BSDSOCKET_INTERNAL_H */
