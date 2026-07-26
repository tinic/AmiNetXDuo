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

/* ------------------------------------------------------------------ errno --
 *
 * AmiTCP/Roadshow errno values are 4.4BSD's, which is NOT what the newlib
 * headers in this toolchain define (they use the Linux numbering: EWOULDBLOCK
 * 11, ENOTSOCK 108, ...). Applications compare against the AmiTCP netinclude
 * values, so those are the numbers that go into the caller's errno. They are
 * ABI constants; the prefixed names keep them clear of <errno.h>.
 */
#define AMI_EPERM               1
#define AMI_ENOENT              2
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
 * WHAT THE ROADSHOW NDK ALREADY DEFINES, verified against
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
 * WHAT IT DOES NOT DEFINE, and therefore what is defined here: IPPROTO_IPV6,
 * every IPV6_* socket option, INET6_ADDRSTRLEN, in6addr_any, IN6ADDR_*_INIT,
 * the IN6_IS_ADDR_* macros, sockaddr_storage, PF_INET6, AI_V4MAPPED and
 * AI_ADDRCONFIG. An application built against this NDK cannot name any of
 * them, so every one of the numbers below is something the application will
 * have spelled out itself -- which is exactly why they have to be the numbers
 * everyone else uses.
 *
 * THE TRAP IN struct sockaddr_in6, and it is a real one:
 *
 * `struct sockaddr_in` in this header is 4.4BSD's, with sin_len at offset 0
 * and sin_family at offset 1. `struct sockaddr_in6` right below it is the
 * LINUX one -- pasted in verbatim, comment about "Scope ID (new in 2.4)" and
 * all -- with sin6_family at offset 0 and NO sin6_len. The two are therefore
 * NOT interchangeable through `struct sockaddr *`: reading sa->sa_family out
 * of a sockaddr_in6 reads its padding byte.
 *
 * That is the same class of hazard as ndk-include/pwd.h turning out to be
 * newlib's 10-field struct passwd rather than the Amiga's 7-field one, so it
 * gets the same treatment: bsd_sa_family() below decides the family from the
 * bytes and the length rather than from a struct member, and in6.c pins every
 * offset with _Static_assert.
 */

/* IPPROTO_IPV6. The IANA number; the NDK stops at IPPROTO_RAW 255. */
#define AMI_IPPROTO_IPV6            41

/*
 * IPV6_V6ONLY has two numberings in the wild and the NDK picks neither:
 * 27 in KAME and the BSDs (netinet6/in6.h), 26 in Linux. This header set is 4.4BSD
 * everywhere except the pasted-in sockaddr_in6, which is Linux -- so there is
 * no lineage to defer to, and guessing wrong means silently ignoring the one
 * option a dual-stack application is most likely to set.
 *
 * Both are accepted, and getsockopt answers to both. The collision risk is
 * nil: 26 is IPV6_CHECKSUM in BSD (raw sockets, which this library does not
 * offer) and 27 is IPV6_JOIN_ANYCAST in Linux (likewise).
 */
#define AMI_IPV6_V6ONLY_BSD         27
#define AMI_IPV6_V6ONLY_LINUX       26

/* IPV6_UNICAST_HOPS: 4 in BSD, 16 in Linux. Same argument, same treatment. */
#define AMI_IPV6_UNICAST_HOPS_BSD    4
#define AMI_IPV6_UNICAST_HOPS_LINUX 16

/* INET6_ADDRSTRLEN: "0:0:0:0:0:ffff:255.255.255.255" plus NUL. */
#define AMI_INET6_ADDRSTRLEN        46

/* --------------------------------------------------------------- library -- */

#define BSD_LIB_NAME        "bsdsocket.library"
#define BSD_LIB_VERSION     4
#define BSD_LIB_REVISION    0

#define BSD_DEFAULT_DTABLESIZE  64
#define BSD_MAX_DTABLESIZE     256

/* NetX Duo's listen queue depth for a bound port. */
#define BSD_MAX_BACKLOG          8

/*
 * TCP receive window.
 *
 * This is the number a bulk transfer is actually limited by, and it was
 * measured rather than reasoned about -- see tests/trace/. On loopback the
 * sender held exactly one 4096-byte segment in flight against a 4096-byte
 * advertised window, 100% of it, and waited 14.9 ms (median) between
 * segments; over the wire it reached 7200 bytes against 8192, 88%. In neither
 * case was the CPU, the link or the periodic tick the thing in the way.
 *
 * -D it to experiment; ami_bsd_tcp_window() is what sockets actually get, and
 * it takes this as the FLOOR rather than the answer.
 */
#ifndef BSD_TCP_WINDOW
#define BSD_TCP_WINDOW        8192
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

    /* Cross-base descriptor hand-off (handoff.c). The one place two tasks'
     * sockets can meet, so it lives in the master and nowhere else. */
    struct MinList          sb_Handoffs;
    LONG                    sb_NextHandoffId;

    /* ---- per opener ----------------------------------------------------- */
    struct Task            *sb_Task;        /* the task that opened us       */

    /*
     * ThreadX context for this opener's task (netx_call.c). NetX Duo's
     * THREADS_ONLY vectors reject a caller that is not a TX_THREAD, so every
     * NetX-touching entry point brackets itself with bsd_nx_enter()/leave(),
     * which is the shared ami_netstack_enter()/leave() bracket plus a nesting
     * counter. The control block lives here rather than on the stack: it is a
     * few hundred bytes, a base belongs to exactly one task, and that task is
     * inside at most one vector at a time.
     */
    AmiNetCaller            sb_NxCaller;
    LONG                    sb_NxNest;      /* bracket depth, 0 == outside   */

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

    STRPTR                  sb_LogTag;      /* SBTC_LOGTAGPTR                */
    LONG                    sb_LogStat;
    LONG                    sb_LogFacility;
    LONG                    sb_LogMask;
    LONG                  (*sb_FDCallback)(LONG fd, LONG action);

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
     * Addresses are NXD_ADDRESS, not ULONG, in BOTH build configurations.
     *
     * NetX Duo's own IPv4 entry points are thin wrappers that build one of
     * these and call the nxd_* function underneath (nx_tcp_client_socket_
     * connect.c is three lines and a delegation), so carrying the version tag
     * here costs the floor build four bytes per address and no code at all --
     * while removing every "which family is this" branch from the call sites.
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
     * bind()/connect() were given. It is NOT used to select an outgoing
     * interface: NetX Duo's dual-stack connect and send entry points take no
     * interface parameter, and pick the source themselves through
     * _nxd_ipv6_interface_find(). On a machine with one Ethernet interface --
     * which is every machine this runs on today -- the two answers are the
     * same. See the milestone report for the honest version.
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
     * Listening state. NetX Duo hands an incoming connection to a *specific*
     * socket, so a listening descriptor keeps a spare socket parked on the
     * port; accept() takes it and relistens a fresh one. docs/RESEARCH.md
     * S6.4 -- this is the bit nxd_bsd.c hides and we have to own.
     */
    struct AmiSocket       *as_Incoming;
    struct AmiSocket       *as_Parent;
    UINT                    as_ListenPort;
    UINT                    as_Backlog;

    /*
     * SOCK_RAW state (raw.c). A raw socket has no NX_TCP_SOCKET and no
     * NX_UDP_SOCKET -- as_Nx is unused -- because NetX Duo has no raw socket
     * object at all: raw reception is an IP-level filter callback shared by
     * the whole stack, so the queue, the demultiplex and the wakeup are ours.
     *
     * as_RawSem is what a blocking recv() suspends on. It cannot be the Exec
     * signal the rest of the library wakes on: the filter runs on the NetX Duo
     * IP thread, which is a ThreadX thread and must not touch Exec, and the
     * receiver is already inside a bsd_nx_enter() bracket where ThreadX
     * suspension is the correct way to wait (it is what nx_tcp_socket_receive
     * does). bsd_event_post() still fires as well, so WaitSelect() sees it.
     */
    /* The urgent byte a peer sent us, held for recv(MSG_OOB) -- see oob.c. */
    UBYTE                   as_OobData;

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
LONG  bsd_fail(struct AmiSocketBase *base, LONG code);   /* set errno, ret -1 */

/* socket.c -- descriptor table */
AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd);
LONG       bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock);
VOID       bsd_fd_free(struct AmiSocketBase *base, LONG fd);
VOID       bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock);
LONG       bsd_table_resize(struct AmiSocketBase *base, LONG size);
VOID       bsd_close_all(struct AmiSocketBase *base);

/* handoff.c -- cross-base descriptor transfer. The registry lives in the
 * master base; bsd_handoff_flush() runs from bsd_lib_close() when the last
 * opener goes, because nothing can obtain a parked socket after that. */
VOID  bsd_handoff_init(struct AmiSocketBase *master);
VOID  bsd_handoff_flush(struct AmiSocketBase *base);

/* socket.c -- sockaddr helpers.
 *
 * bsd_sa_family() is the single place that decides what a caller's
 * `struct sockaddr *` actually is; see the sockaddr_in6 note above for why
 * that cannot be a struct member read. It returns AF_INET, AF_INET6,
 * AF_UNSPEC, or -1 for something it will not touch.
 *
 * bsd_sockaddr_get() parses one into an NXD_ADDRESS + port (+ scope id, which
 * may be NULL); bsd_sockaddr_put() writes one back in the shape the SOCKET's
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
NX_PACKET *bsd_raw_receive(AmiSocket *sock, ULONG wait);
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
ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks);

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

#endif /* AMINETXDUO_BSDSOCKET_INTERNAL_H */
