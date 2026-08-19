/*
 * bsdsocket.library, internal interfaces.
 *
 * AmiTCP/Roadshow-compatible socket library implemented directly on the
 * native NetX Duo APIs (nx_tcp_socket_*, nx_udp_socket_*, nx_packet_*), not
 * on addons/BSD/nxd_bsd.c, see docs/RESEARCH.md S6.4 for why.
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
 * ThreadX and NetX Duo first, always. tx_port.h typedefs VOID, CHAR, UINT and
 * others. <exec/types.h> guards its own definitions with #ifndef, so it
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
/* NetStatusOpener, for the openers walk library.c owns. */
#include "aminetxduo/netstatus.h"

/*
 * The AF_INET6 names the NDK lacks. It is published and ships in the archive's
 * Developer drawer, so the numbers below are derived from it rather than
 * copied. Two copies of an ABI constant is one copy too many.
 */
#include "aminetxduo/in6.h"

/* ------------------------------------------------------------------ errno,
 *
 * AmiTCP/Roadshow errno values are 4.4BSD's, not what the newlib headers
 * in this toolchain define (they use the Linux numbering: EWOULDBLOCK
 * 11, ENOTSOCK 108, ...). Applications compare against the AmiTCP netinclude
 * values, so those are the numbers that go into the caller's errno. They are
 * ABI constants. The prefixed names keep them clear of <errno.h>.
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

/* ------------------------------------------------------------------ IPv6,
 *
 * What the roadshow NDK already defines, checked against
 * amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include:
 *
 *   sys/socket.h:196   AF_INET6 23, which collides with
 *                                              AF_IPX 23 three lines above.
 *                                              Nothing here uses AF_IPX.
 *   netinet/in.h:178   struct in6_addr, unsigned char s6_addr[16]
 *   netinet/in.h:182   struct sockaddr_in6 , sin6_family, sin6_port,
 *                                              sin6_flowinfo, sin6_addr,
 *                                              sin6_scope_id
 *
 * What it does not define is in aminetxduo/in6.h, included above: IPPROTO_IPV6,
 * every IPV6_* socket option, INET6_ADDRSTRLEN, IN6ADDR_*_INIT, the
 * IN6_IS_ADDR_* macros, sockaddr_storage, PF_INET6 and AI_ADDRCONFIG. That
 * header is published, it ships in the archive's Developer drawer, and the BSD
 * numbers below are aliases of it rather than second copies. An application
 * built against this NDK cannot name any of them, so it spells them out
 * itself, which is why in6.h #ifndef-guards every one.
 *
 * The BSD/Linux pairs are here rather than there because accepting both
 * numberings is behaviour, not ABI. in6.h publishes the one number a caller
 * uses, and these are what setsockopt() matches against.
 *
 * struct sockaddr_in6 is a hazard, and in6.h carries the warning in full for
 * the callers who need it. `struct sockaddr_in` here is 4.4BSD's, sin_len at
 * offset 0 and sin_family at 1. `struct sockaddr_in6` right below it is the
 * Linux one, pasted in verbatim, comment about "Scope ID (new in 2.4)" and
 * all, with sin6_family at offset 0 and no sin6_len. Reading sa->sa_family out
 * of a sockaddr_in6 reads its padding byte. (Same class of hazard as
 * ndk-include/pwd.h being newlib's 10-field struct passwd rather than the
 * Amiga's 7-field one.) So bsd_sa_family() below decides the family from the
 * bytes and the length rather than from a struct member, and in6.c pins every
 * offset with _Static_assert.
 */

/*
 * IPPROTO_IPV6 comes from aminetxduo/in6.h and IPPROTO_ICMPV6 from
 * aminetxduo/cmsg.h, because a caller has to be able to name a level as well
 * as an option. The AMI_IPPROTO_IPV6 alias is gone rather than kept pointing
 * at the published name. Nothing used it once options.c stopped.
 *
 * IPV6_V6ONLY has two numberings in the wild and the NDK picks neither:
 * 27 in KAME and the BSDs (netinet6/in6.h), 26 in Linux. This header set is
 * 4.4BSD everywhere except the pasted-in sockaddr_in6, which is Linux, so
 * there is no lineage to defer to. Both are accepted, and getsockopt answers
 * to both.
 *
 * 26 is a collision, on one socket. It is IPV6_CHECKSUM in BSD, and this
 * library does offer raw IPv6 sockets: socket(AF_INET6, SOCK_RAW, ...) is what
 * traceroute and ping open, and raw.c carries them. So the Linux numbering is
 * withdrawn on a raw socket. bsd_v6_linux_numbering() in in6.c has the whole
 * reasoning. (27 is IPV6_JOIN_ANYCAST in Linux, which this library does not
 * offer, so the BSD number needs no such guard.)
 */
#define AMI_IPV6_V6ONLY_BSD         IPV6_V6ONLY
#define AMI_IPV6_V6ONLY_LINUX       26

/* IPV6_UNICAST_HOPS: 4 in BSD, 16 in Linux. Same argument, same treatment. */
#define AMI_IPV6_UNICAST_HOPS_BSD   IPV6_UNICAST_HOPS
#define AMI_IPV6_UNICAST_HOPS_LINUX 16

/*
 * IPV6_TCLASS: 61 in BSD, 67 in Linux. Darwin's 36 is deliberately not
 * accepted, Apple kept it for binary compatibility, and 36 is
 * IPV6_RECVPKTINFO in BSD and IPV6_HDRINCL in Linux, so it names something
 * else in both lineages this header set can belong to. 61 is Linux's
 * IPV6_PATHMTU, which this library does not offer.
 */
#define AMI_IPV6_TCLASS_BSD         IPV6_TCLASS
#define AMI_IPV6_TCLASS_LINUX       67

/*
 * RFC 3493 section 5.2: BSD 9..13, Linux 17..21. Same treatment again. The
 * Linux names those BSD numbers displace are IPV6_NEXTHOP, IPV6_AUTHHDR,
 * IPV6_FLOWINFO and two IPV6_2292 spellings, none of which this library
 * offers.
 */
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

/* --------------------------------------------------------------- library, */

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
 *      at -0x372..-0x384, the first slots past the end of the NDK's table.
 *      AMI_NETSTATUS_VERSION also went 6 -> 7 here (NETCTRL_INTERFACE_ADD,
 *      NETCTRL_STACK_HOLD) and this list was not updated. A command built
 *      against 7 can meet a revision-3 library that answers 6 and be told
 *      nothing useful.
 *   4  AMI_NETSTATUS_VERSION 8, NETCTRL_INTERFACE_CONFIGURE
 *   5  AMI_NETSTATUS_VERSION 9, NETCTRL_INTERFACE_MDNS
 *   6  AMI_NETSTATUS_VERSION 10, NETCTRL_STACK_NOTIFY/_RELEASE,
 *      NETSTATUS_OPENERS
 */
#define BSD_LIB_REVISION    6

/* SBTC_LOGFACILITY's documented default. The NDK's <sys/syslog.h> ships the
   priority codes only, so the BSD facility value is spelled out here. */
#define BSD_LOG_USER        (1L << 3)

/*
 * 256, matching Roadshow's documented default. <sys/types.h> makes FD_SETSIZE
 * 256, so `WaitSelect(FD_SETSIZE, ...)`, common in ported code, failed
 * with EINVAL on the 64 we used to ship.
 *
 * The table is a lazily allocated array of pointers, so this costs 1,024
 * bytes instead of 256 for an opener that makes a socket, and nothing for one
 * that does not. SBTC_DTABLESIZE can raise or lower it.
 *
 * The ceiling is the largest table SBTC_DTABLESIZE hands out. It also
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
 * in tests/trace/. On loopback the sender held exactly one 4096-byte segment
 * in flight against a 4096-byte advertised window, 100% of it, and waited
 * 14.9 ms (median) between segments. Over the wire it reached 7200 bytes
 * against 8192, 88%. Neither the CPU, the link nor the periodic tick was the
 * limit.
 *
 * ami_bsd_tcp_window() is what sockets actually get. This is only its floor,
 * which is what shipped before it, and what forty concurrent transfers were
 * measured passing on. Setting this and the ceiling to the same value pins the
 * window, which is how a fixed window is measured against the derived one.
 */
#ifndef BSD_TCP_WINDOW
#define BSD_TCP_WINDOW        8192
#endif

/*
 * The per-socket receive queue is capped in packets, not only in window bytes:
 * the pool is spent one whole packet per peer segment whatever its size, so a
 * sub-MSS peer would otherwise pin one packet each while the byte window barely
 * moves (see nx_user.h NX_ENABLE_LOW_WATERMARK).  bsd_tcp_rx_queue_cap() sets
 * the cap to window/BSD_TCP_RX_MSS_REF + slack: a full-MSS peer is already held
 * to window/MSS packets by the byte window and never reaches it, so the flood
 * is bounded without touching the clean path.
 */
#define BSD_TCP_RX_MSS_REF      1460
#define BSD_TCP_RX_QUEUE_SLACK  4

/* The UDP receive queue, in datagrams. bsd_udp_queue_max() in socket.c derives
   the per-socket default from the pool between these two. options.c bounds a
   SO_RCVBUF request by the ceiling. */
#define BSD_UDP_QUEUE_MIN       8
#define BSD_UDP_QUEUE_CEILING   64
#define BSD_UDP_POOL_SHARE      4       /* 1/N of the pool per socket       */

/*
 * One in this many pool packets is the budget the whole stack's TCP receive
 * windows can claim above the floor. See ami_bsd_tcp_window().
 *
 * 8. A quarter was tried and refused. (AMI_POOL_MAX_PACKETS / share) *
 * AMI_POOL_PAYLOAD is 50,176 at 8 and 100,352 at 4, so a quarter is what it
 * takes to give one socket a window the 16-bit field cannot hold and make RFC
 * 1323's scale exponent non-zero. It was built and measured, A1200 bridged to
 * a real peer, 1 MB, 2 reps, 3 boots an arm, arms interleaved
 * (tests/perf/run-fitzbench.sh -a -B ens18 -H <peer> -m A1200 -k 1024 -r 2 -w):
 *
 *   window   scaling  raw loss   effective loss        read KB/s
 *   33580    off      0.927 %    0.000 0.000 0.000     392 391 393
 *   50176    on       0.801 %    0.000 0.000 0.000     391 395 390
 *   100352   on, s=1  1.176 %    0.252 0.294 0.169     392 396 393
 *
 * The read does not move at any window -- the peer reports app_limited on
 * every one of 2604 `ss -tim` samples and rwnd_limited on none, so the receive
 * window was never what bound the transfer -- and the quarter is the only arm
 * where retransmissions stop being spurious and start filling real holes.
 * 0.000 to 0.25 % effective loss on all three pairs, for no throughput, is the
 * trade refused. docs/RESEARCH.md's own rule is that the retransmission rate
 * is the number to gate on and the throughput is downstream of it.
 *
 * At an eighth ami_bsd_tcp_window() divides the budget by the connected socket
 * count as before, so a lone socket can claim an eighth of the pool, all
 * sockets together never more than one budget, and above five sockets the
 * BSD_TCP_WINDOW floor governs instead.
 */
#ifndef BSD_TCP_WINDOW_POOL_SHARE
#define BSD_TCP_WINDOW_POOL_SHARE   8
#endif

/*
 * Ceiling, derived rather than chosen.
 *
 * The largest window the pool can ever produce is the budget at the pool's own
 * maximum size, which is what this is: a lone socket on a machine with enough
 * free memory to reach AMI_POOL_MAX_PACKETS. Anything below it caps a window
 * the pool was willing to give. Anything above it can never be reached. So it
 * is not a free number and not a constant. It moves with the pool sizing in
 * include/aminetxduo/netstack.h and with the share above.
 *
 * 33580 used to be here: 23 whole segments at the Ethernet MSS, which is what
 * a 32 KB payload behind a header needs to arrive in one round trip. 32768 is
 * twelve bytes short of that, so a 32,780-byte response did not complete in
 * one whatever else was true.
 *
 * The measurements behind that number are kept below. They come from a
 * request/response workload and tests/perf/run-fitzbench.sh is a bulk one, and
 * neither answers the other.
 *
 * An early sweep read 32768, 33580 and 48180 as flat and kept 32768, with a
 * congestion window of 12 segments that bound the transfer before the receive
 * window did. SACK and D-SACK take it to 24 on a clean link, which puts the
 * receive window back in charge: over 512 exchanges of a 32,780-byte response,
 * 32768 -> 33580 moved round trips a chunk from 2.05 to 1.18 and the read from
 * 985 to 1714 KB/s clean, and from 2.32 to 1.94 and 835 to 963 KB/s under 1%
 * injected loss. 48180 measured no better than 33580 and was refused for that.
 *
 * It pays only where the guest is fast enough for the round trip to have been
 * the cost. Across profiles, 2 boots an arm: 68020 moved round trips a chunk
 * 2.00 -> 1.02 and the read did not move at all, because time per chunk stayed
 * ~77 ms in both arms and the remaining gap widened to absorb what was saved.
 * The peer went from 3-4% app-limited to 93-94%: at a 40 ms round trip the
 * 68020 was never waiting on the network. Same signature on 68000, app-limited
 * 2-4% -> 66-83%. On 68000 it costs a little: clean reads 179.2 -> 174.1 KB/s
 * and 181.4 -> 178.7 for the minimal build, spreads under 1 KB/s.
 *
 * So a number above 33580 has been refused once on that workload. It stopped
 * being a ceiling the day the pool budget arrived, and became a second,
 * unrelated bound that happened to be the tighter one on any machine with more
 * than about 2.3 MB free. Derived, it was 50,176 at the share above, and that
 * window is the middle row of the table there: raw loss and effective loss and
 * read all indistinguishable from the 33580 it replaces.
 *
 * It is 100,352 now, because AMI_POOL_MAX_PACKETS went to 512 and this is a
 * share of it. That is the same number of packets pinned as the quarter share
 * refused in the table above, out of twice the pool, so the fraction that
 * measurement was about is unchanged at an eighth and the packets left over
 * for everybody else double. It is also out of reach below about 13.6 MB free,
 * where AvailMem()/16 is what sizes the pool: the lab's 8 MB A1200 gets 368
 * packets and a 72,128-byte window, and the ceiling never applies.
 *
 * Every byte of window is a byte of packet pool no other socket can have.
 *
 * AMINETXDUO_TCP_WINDOW overrides both this and the floor, which is how a
 * fixed window is measured against the derived one.
 */
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
 * enforces it, refusing a larger window with NX_OPTION_ERROR.
 *
 * That guard is why this arm exists. At the share above the budget is 50,176
 * and fits the field on its own, so nothing reaches it today. It was reached
 * once: with the share at a quarter the budget is 100,352 on any machine with
 * about 4.2 MB of free public memory, and a tree built with
 * -DAMINETXDUO_TCP_WINDOW_SCALING=OFF and no ceiling of its own failed socket()
 * for every TCP socket on the lab's own A1200 profile. tcp.drill w01 opened
 * with "socket() failed" and "connect() failed, errno 9" and every case after
 * it followed. The two arms stop the share and the option disagreeing at all.
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
 * A sampled PC inside this library is an address in a hunk LoadSeg() put
 * wherever it liked, and nothing outside can turn it back into a function
 * without knowing where the hunks landed, which is what the seglist says.
 * Exec publishes struct Library and nothing after it, so sb_SegList sits at an
 * offset only this file knows. A profiler reading it from a hard-coded number
 * breaks silently the first time a field moves: it still finds eight bytes
 * that look like a segment header and still resolves every address to a wrong,
 * plausible name.
 *
 * So this record says where it is and identifies itself. The profiler scans
 * our positive half for the magic, checks that bst_LibBase is the base it was
 * found in and that the five longwords sum to zero, and then checks the
 * seglist it gets against the hull of our own jump table. A library that does
 * not carry it is named by module instead.
 *
 * The convention is the profiler's and is written down in
 * tools/profiler/prof.h, deliberately not included from here, because it
 * belongs to a tool meant to be liftable out of this tree whole and this
 * library must not acquire a dependency on it. Five longwords is the whole
 * of it.
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

    /* ---- master only ---------------------------------------------------- */
    struct SignalSemaphore  sb_Lock;        /* guards the child list + stack */
    struct MinList          sb_Children;
    ULONG                   sb_StackRefs;   /* netstack_startup() references */
    ULONG                   sb_TransientStackRefs; /* async workers, no base */

    /*
     * The library's own reference to the stack it is running, taken once and
     * never given back (bsd_stack_hold()).  It is what keeps the network up
     * after the command that started it exits, so that command does not have
     * to leak a base to do the same job.  A flag and not a count, so a second
     * AddNetInterface costs nothing.
     */
    BOOL                    sb_StackHeld;

    /* Cross-base descriptor hand-off (handoff.c). Two tasks' sockets meet
     * only here, so it lives in the master base. */
    struct MinList          sb_Handoffs;
    LONG                    sb_NextHandoffId;

    /* ---- per opener ----------------------------------------------------- */
    struct Task            *sb_Task;        /* the task that opened us       */

    /*
     * ThreadX context for this opener's task (netx_call.c). NetX Duo's
     * THREADS_ONLY vectors reject a caller that is not a TX_THREAD, so every
     * NetX-touching entry point brackets itself with bsd_nx_enter()/leave(),
     * the shared ami_netstack_enter()/leave() bracket plus a nesting counter.
     * The control block lives here rather than on the stack: it is a few
     * hundred bytes, a base belongs to one task, and that task is inside at
     * most one vector at a time.
     */
    AmiNetCaller            sb_NxCaller;

    /*
     * WaitSelect()'s input sets, here for the reason above and one more. They
     * are sized to BSD_MAX_DTABLESIZE rather than to the caller's nfds, so on
     * the stack they cost 384 bytes of the caller's stack on every call,
     * whether it is watching two descriptors or a thousand. An Amiga caller can
     * have 4 KB and no guard page, and a program that sits in WaitSelect() in a
     * loop is the normal shape of a network client. The whole frame measured
     * 864 bytes before this moved (-fstack-usage), which real hardware reported
     * as freezes under sustained TCP.
     */
    BsdFdSets               sb_SelIn;
    BsdFdSets               sb_SelReady;   /* and its result sets    */
    LONG                    sb_NxNest;      /* bracket depth, 0 == outside   */

#ifdef AMINETXDUO_NXCENSUS
    /* Measurement build only, see the census note in netx_call.c. */
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

    /* SBTC_SIG_ADDRESS_CHANGE_MASK. Stored, not yet delivered: the change
       point is ami_ns_address_changed() in src/netstack/. Default 0 = no
       notification, so an opener that never sets it sees no difference. */
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

    /*
     * Wakeup plumbing. sb_EventSignal is allocated by the opening task in
     * bsd_lib_open(), Exec signals belong to a task, and a base belongs to
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

    /* Last, so nothing above it moves. Anywhere in the positive half will do:
       the profiler scans for it rather than being told an offset. */
    struct BsdProfSegTag    sb_ProfSegTag;
};

/* ---------------------------------------------------------------- socket, */

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

/*
 * as_CmsgWant, which RFC 3542 ancillary objects this socket asked for. Its
 * own word rather than more ASF_ bits: as_Flags is a hot field every path
 * tests, and these are read once per datagram.
 */
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
 *
 * cs_Hops is IPV6_HOPLIMIT (RFC 3542 6.3), which is ancillary-only, there is
 * no sticky spelling of it, so cs_HaveHops is FALSE in as_CmsgSticky always.
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
     * IP_MULTICAST_TTL / _LOOP / _IF, kept per socket because BSD keeps them
     * there and NetX Duo does not: the TTL is a field on the NX_UDP_SOCKET
     * shared with unicast, and the loopback flag is one switch on the whole
     * NX_IP. mcast.c is where the two are reconciled.
     *
     * as_McastIf is a NetX interface index, or -1 for "let the route decide".
     */
    LONG                    as_McastTtl;
    LONG                    as_McastLoop;
    LONG                    as_McastIf;

#ifdef AMINETXDUO_IPV6
    /*
     * IPV6_MULTICAST_HOPS / _IF. Separate from the IPv4 three because BSD
     * keeps them separate and because NetX Duo puts the v6 hop limit
     * somewhere else again, on the NX_IP, not the socket. There is no
     * as_Mcast6Loop: NetX Duo has no IPv6 multicast loopback, so the option
     * reads back 0 whatever was set and has nothing to remember.
     *
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
     * docs/RESEARCH.md S6.4.
     *
     * as_Incoming is the head of that list, as_IncomingNext the link, and
     * as_IncomingCount its length, up to as_Backlog, which is what
     * listen()'s second argument promises. One entry only was the whole
     * backlog until a half-open connection was found to hold the single slot
     * for the entire SYN/ACK retransmit ladder.
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
     *
     * as_RawSem is what a blocking recv() suspends on. It cannot be the Exec
     * signal the rest of the library wakes on: the filter runs on the NetX Duo
     * IP thread, which is a ThreadX thread and must not touch Exec, and the
     * receiver is already inside a bsd_nx_enter() bracket where ThreadX
     * suspension is the right way to wait (as nx_tcp_socket_receive does).
     * bsd_event_post() still fires as well, so WaitSelect() sees it.
     */
    /* The urgent byte a peer sent us, held for recv(MSG_OOB), see oob.c. */
    UBYTE                   as_OobData;

    /*
     * Orderly-close list. CloseSocket() sends a FIN and returns, so the
     * connection outlives the descriptor and usually the base as well. socket.c
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

/* ------------------------------------------------------------- prototypes,
 *
 * Everything below is internal. The ABI entry points live in the generated
 * bsdsocket_vectors.h.
 */

/* library_runtime.c, what a shared library has to supply for itself. */
BOOL  bsd_runtime_open(VOID);
VOID  bsd_runtime_close(VOID);

/* netx_call.c, ThreadX context. Every call to a NetX Duo THREADS_ONLY
 * vector must be inside a successful bsd_nx_enter()/bsd_nx_leave() bracket.
 * Inside one, nothing must block on anything except ThreadX. */
LONG  bsd_nx_enter(struct AmiSocketBase *base);
VOID  bsd_nx_leave(struct AmiSocketBase *base);

/* Drop the base's cached ThreadX registration. Teardown only, no bracket open.
   See netx_call.c for what is cached and why. */
VOID  bsd_nx_release(struct AmiSocketBase *base);

/* library.c */
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

/* errno.c */
VOID  bsd_set_errno(struct AmiSocketBase *base, LONG code);
VOID  bsd_set_herrno(struct AmiSocketBase *base, LONG code);
LONG  bsd_errno_from_nx(UINT status);
/* The same, for a status from a call that was given `wait`, see errno.c. */
LONG  bsd_wait_errno(ULONG wait, UINT status);
LONG  bsd_fail(struct AmiSocketBase *base, LONG code);   /* set errno, ret -1 */

/* socket.c, descriptor table */
AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd);
LONG       bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock);
LONG       bsd_fd_reserve(struct AmiSocketBase *base, LONG fd);
BOOL       bsd_fd_reserved(struct AmiSocketBase *base, LONG fd);
LONG       bsd_fd_free(struct AmiSocketBase *base, LONG fd);
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
 *
 * bsd_sa_family() decides what a caller's `struct sockaddr *` actually is. The
 * sockaddr_in6 note above says why that cannot be a struct member read.
 * Returns AF_INET, AF_INET6, AF_UNSPEC, or -1 for anything it will not touch.
 *
 * bsd_sockaddr_get() parses one into an NXD_ADDRESS + port (+ scope id, which
 * can be NULL). bsd_sockaddr_put() writes one back in the shape the socket's
 * family calls for, which is not always the shape of the address: an
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

/* options.c, as_Ttl and as_Tos onto the live NetX socket, which is what puts
   IP_TTL and IP_TOS on the wire. Caller holds the bsd_nx_enter() bracket. */
VOID  bsd_opt_apply_ip(AmiSocket *sock);

#ifdef AMINETXDUO_IPV6
/* in6.c, everything that only exists in the dual-stack build. */

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
 *
 * bsd_cmsg_reset() puts a fresh socket in the documented default state: no
 * ancillary data wanted, and an ICMPv6 filter that passes everything.
 *
 * bsd_cmsg_option() handles the set/get of every option that only means
 * "attach this to recvmsg()". It answers for levels IPPROTO_IP, IPPROTO_IPV6
 * and IPPROTO_ICMPV6, and returns 1 for an optname it does not own so the
 * caller can carry on looking. 0 is done, -1 is done with errno set.
 *
 * bsd_cmsg_build() fills msg_control from the packet a datagram arrived in and
 * sets msg_controllen. If the caller's buffer was too small, MSG_CTRUNC goes
 * into msg_flags. Safe with msg == NULL, which is every recv() and recvfrom().
 *
 * bsd_cmsg_parse() reads the caller's ancillary data on sendmsg(). Anything
 * this library cannot honour is refused rather than ignored.
 *
 * bsd_cmsg_source_index() turns a named source into the address index
 * nxd_udp_socket_source_send() wants, or -1 when nothing matches.
 */
VOID  bsd_cmsg_reset(AmiSocket *sock);
LONG  bsd_cmsg_option(struct AmiSocketBase *base, AmiSocket *sock, LONG level,
                      LONG optname, APTR optval, socklen_t *optlen, BOOL set);
VOID  bsd_cmsg_build(AmiSocket *sock, NX_PACKET *packet, struct msghdr *msg);
LONG  bsd_cmsg_parse(struct AmiSocketBase *base, AmiSocket *sock,
                     const struct msghdr *msg, BsdCmsgSource *out);
LONG  bsd_cmsg_source_index(NX_IP *ip, const BsdCmsgSource *src, BOOL v6);

/* oob.c, TCP urgent data (MSG_OOB, SIOCATMARK, SIGURG).
 *
 * bsd_oob_send() sends one byte with the URG bit set. It must be called inside
 * a bsd_nx_enter() bracket and returns 1, or -1 with errno set.
 *
 * bsd_tcp_urgent_notify() is what nx_tcp_socket_create() is handed as its
 * urgent-data callback, and runs on the NetX Duo IP thread.
 *
 * bsd_oob_take() hands the stored byte to recv(MSG_OOB) and clears the mark.
 * TRUE means there was one.
 */
LONG  bsd_oob_send(struct AmiSocketBase *base, AmiSocket *sock, UBYTE byte,
                   LONG flags);
VOID  bsd_tcp_urgent_notify(NX_TCP_SOCKET *socket_ptr);
BOOL  bsd_oob_take(AmiSocket *sock, UBYTE *out);

/* raw.c, SOCK_RAW.
 *
 * Every one of these must be called inside a bsd_nx_enter() bracket, except
 * bsd_raw_available() and bsd_raw_source(), which only read.
 *
 * bsd_raw_open() registers the socket with the IP-level filter, installing it
 * on the first raw socket. bsd_raw_close() unregisters it, drains its queue
 * and removes the filter again when the last one goes.
 *
 * bsd_raw_send_packet() hands a packet the caller has already filled to
 * nxd_ip_raw_packet_send(), which prepends the IP header. `scope` is the
 * sockaddr_in6 zone, 0 for none. `src` is the RFC 3542 ancillary data of a
 * sendmsg(), or NULL. The packet is consumed either way, released here on
 * failure, so the caller must not touch it again.
 *
 * bsd_raw_receive() dequeues one whole IP datagram, header included, or NULL.
 * The caller owns it and must nx_packet_release() it.
 */
/*
 * The IP-layer MTU of the interface a datagram to `addr` would leave by, or -1
 * if the route does not resolve. Both send paths measure against it and refuse
 * an oversize datagram with EMSGSIZE rather than let transmit fragmentation
 * handle it. bsd_udp_maxdgram() in transfer.c says why.
 */
LONG       bsd_route_mtu(NX_IP *ip, const NXD_ADDRESS *addr);

LONG       bsd_raw_open(struct AmiSocketBase *base, AmiSocket *sock);
VOID       bsd_raw_close(AmiSocket *sock);
LONG       bsd_raw_send_packet(struct AmiSocketBase *base, AmiSocket *sock,
                               NX_PACKET *packet, const NXD_ADDRESS *addr,
                               ULONG scope, const BsdCmsgSource *src);
NX_PACKET *bsd_raw_receive(AmiSocket *sock, ULONG wait, UINT *why);
VOID       bsd_raw_source(NX_PACKET *packet, NXD_ADDRESS *addr);
ULONG      bsd_raw_available(AmiSocket *sock);

#ifdef AMINETXDUO_MULTICAST
/* mcast.c, RFC 1112 group membership and the IPPROTO_IP multicast options.
 *
 * bsd_mcast_setopt()/bsd_mcast_getopt() answer IP_ADD_MEMBERSHIP,
 * IP_DROP_MEMBERSHIP, IP_MULTICAST_IF, IP_MULTICAST_TTL and
 * IP_MULTICAST_LOOP. options.c dispatches those five and nothing else here.
 * Both take their own bsd_nx_enter() bracket where they need one.
 *
 * bsd_mcast_close() drops every membership the socket still holds, as BSD
 * does on close. It runs from bsd_socket_destroy(), inside the bracket.
 *
 * bsd_mcast_prepare_send() is the send path: it puts the socket's multicast
 * TTL on the NX_UDP_SOCKET and answers with the interface index the datagram
 * must leave by, or -1 for the route's choice.
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
 *
 * bsd_mcast6_prepare_send() answers with the IPv6 ADDRESS index to send from
 * what nxd_udp_socket_source_send() wants, or -1, and stores the NX_IP
 * hop limit it overwrote in *saved. bsd_mcast6_finish_send() puts it back. The
 * pair must bracket the send, and 0 in *saved means nothing changed.
 *
 * BSD_MCAST6_NO_LINK is the third answer: IPV6_MULTICAST_HOPS is 0, "this
 * host only" (RFC 3493 5.2), so the datagram is consumed and nothing goes on
 * the link. The caller still reports the write as accepted.
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
 * bsd_events_attach() installs the NetX Duo receive/connect/disconnect
 * callbacks on a freshly created socket. Every one of them ends up in
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

ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks, LONG flags);

/*
 * The break-signal test the resolver's retransmission ladder asks between
 * queries (resolver.c). `arg` is the struct AmiSocketBase whose sb_BreakMask
 * applies. The signature is netstack's AmiNetGiveUpFn.
 */
BOOL  bsd_resolve_break(VOID *arg);

/*
 * NextTagItem(), open-coded (errno.c). utility.library is not always open when
 * this library is called, and the four control tags are three lines of
 * arithmetic. Shared by interfaces.c, routing.c and addralloc.c.
 *
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
                       UINT src_port);

/*
 * The send direction of the same question: which source must a datagram from
 * this socket leave with? socket.c owns it. transfer.c and raw.c use it to
 * pick the nxd_*_source_send() index, connect() to decide whether TCP can
 * honour the request at all.
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
