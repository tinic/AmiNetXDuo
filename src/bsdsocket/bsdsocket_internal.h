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

/* --------------------------------------------------------------- library -- */

#define BSD_LIB_NAME        "bsdsocket.library"
#define BSD_LIB_VERSION     4
#define BSD_LIB_REVISION    0

#define BSD_DEFAULT_DTABLESIZE  64
#define BSD_MAX_DTABLESIZE     256

/* NetX Duo's listen queue depth for a bound port. */
#define BSD_MAX_BACKLOG          8

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

    /* ---- per opener ----------------------------------------------------- */
    struct Task            *sb_Task;        /* the task that opened us       */

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

    /* Scratch returned to callers by the non-reentrant entry points. */
    char                    sb_NtoABuf[BSD_NTOA_BUFLEN];
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

    ULONG                   as_LocalAddr;
    UINT                    as_LocalPort;
    ULONG                   as_PeerAddr;
    UINT                    as_PeerPort;

    /* Partially consumed receive packet (a stream read need not drain one). */
    NX_PACKET              *as_RxPending;
    ULONG                   as_RxOffset;

    ULONG                   as_RcvTimeout;  /* ticks; 0 == block forever     */
    ULONG                   as_SndTimeout;
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
} AmiSocket;

/* ------------------------------------------------------------- prototypes --
 *
 * Everything below is internal; the ABI entry points live in the generated
 * bsdsocket_vectors.h.
 */

/* library_runtime.c -- what a shared library has to supply for itself. */
BOOL  bsd_runtime_open(VOID);
VOID  bsd_runtime_close(VOID);

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

/* socket.c -- sockaddr helpers */
LONG  bsd_sockaddr_in(struct AmiSocketBase *base, const struct sockaddr *sa,
                      socklen_t len, ULONG *addr, UINT *port);
VOID  bsd_sockaddr_out(struct sockaddr *sa, socklen_t *len,
                       ULONG addr, UINT port);

/* select.c -- event plumbing.
 *
 * bsd_events_attach() installs the NetX Duo receive/connect/disconnect
 * callbacks on a freshly created socket; every one of them ends up in
 * bsd_event_post(), which records the FD_* bits and signals the owning task.
 */
VOID  bsd_events_attach(AmiSocket *sock);
VOID  bsd_event_post(AmiSocket *sock, ULONG events);
VOID  bsd_listen_callback(NX_TCP_SOCKET *socket_ptr, UINT port);
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
