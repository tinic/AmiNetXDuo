/*
 * AmiNetXDuo, AF_INET6 through bsdsocket.library's ABI.
 *
 * The third IPv6 test, and the only one that goes through the LVO jump table:
 * ipv6_test.c drives NetX Duo directly, ipv6_link_test.c drives the netstack,
 * and this one is an ordinary AmigaOS program that does
 * OpenLibrary("bsdsocket.library") and calls vectors, as a ported Unix
 * application would.  It is linked against none of our code.
 *
 * Everything happens over ::1, which nxd_ipv6_enable() configures on the
 * internal loopback interface, so the wire is not a variable.  What is under
 * test is the socket layer, sockaddr_in6 in and out of bind/connect/accept/
 * getsockname/getpeername, IPV6_V6ONLY, inet_ntop/inet_pton for AF_INET6,
 * getaddrinfo.
 *
 * The NDK's sockaddr_in6 is the Linux one (no sin6_len, family at offset 0)
 * sitting in a header whose sockaddr_in is 4.4BSD (sin_len at offset 0, family
 * at offset 1).  A stack that reads sa->sa_family generically gets the padding
 * byte.  Every sockaddr below is built the way a real application would build
 * it, so if that were wrong here, bind() would fail with EAFNOSUPPORT.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>

/*
 * The one header this program takes from us, and on purpose: RFC 3542's
 * ancillary data is struct shapes and macros, not vectors, so a caller has to
 * have them from somewhere.  Including it here is also the check that it
 * compiles standalone against the NDK, the CMSG_* macros it replaces are
 * <sys/socket.h>'s own.
 */
#include <stddef.h>
#include <sys/types.h>          /* ssize_t, which sys/socket.h uses but does
                                   not pull in for itself */
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include "aminetxduo/cmsg.h"

/* The synthetic SANA-II device this test brings the stack up on. */
#include "tapdev.h"


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

/*
 * Buffered, and flushed to stdout at the end, as ipv6_test.c does. Streaming
 * straight to RawPutChar() came back as several hundred NUL bytes in the
 * serial capture: this program logs in a tight burst with none of the pauses
 * ami_log() leaves between lines, and the emulator's serial capture does not
 * keep up. The buffer costs 8 KB of BSS.
 */
#define T_LOG_SIZE      8192

static char     t_log_buffer[T_LOG_SIZE];
static ULONG    t_log_used;

static VOID t_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
    {
        RawPutChar(c);

        if (t_log_used < (ULONG)(T_LOG_SIZE - 1))
        {
            t_log_buffer[t_log_used++] = (char)c;
        }
    }
}

static VOID t_log(const char *fmt, ...)
{

va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) t_put_char, NULL);
    va_end(args);

    RawPutChar('\n');
    if (t_log_used < (ULONG)(T_LOG_SIZE - 1))
    {
        t_log_buffer[t_log_used++] = '\n';
    }
}

static VOID t_flush(VOID)
{

BPTR    out;

    out =  Output();
    if (out != (BPTR)0)
    {
        (VOID)Write(out, (APTR)t_log_buffer, (LONG)t_log_used);
    }
}

static ULONG    t_checks;
static ULONG    t_failures;

static BOOL t_check(BOOL ok, const char *what, LONG detail)
{
    t_checks++;
    if (!ok)
    {
        t_failures++;
        t_log("  FAIL %s (%ld)", what, detail);
    }
    else
    {
        t_log("  ok   %s", what);
    }

    return(ok);
}


/* -------------------------------------------------------------- the ABI -- */

/*
 * Declared here rather than taken from the NDK's <netinet/in.h>, for the same
 * reason library_test.c declares its own LVOs: the layout is the thing under
 * test, so writing it out keeps the expectation independent of whatever
 * header is on the include path.
 *
 * This must match ndk-include/netinet/in.h:182 exactly, 28 bytes, family at
 * offset 0, no length byte.
 */
struct t_in6_addr
{
    UBYTE   s6_addr[16];
};

struct t_sockaddr_in6
{
    UBYTE               sin6_family;    /* offset  0, not a length byte */
    UBYTE               sin6_pad;       /* offset  1, compiler padding  */
    UWORD               sin6_port;      /* offset  2 */
    ULONG               sin6_flowinfo;  /* offset  4 */
    struct t_in6_addr   sin6_addr;      /* offset  8 */
    ULONG               sin6_scope_id;  /* offset 24 */
};

struct t_sockaddr_in
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

struct t_addrinfo
{
    LONG                ai_flags;
    LONG                ai_family;
    LONG                ai_socktype;
    LONG                ai_protocol;
    ULONG               ai_addrlen;
    APTR                ai_addr;
    char               *ai_canonname;
    struct t_addrinfo  *ai_next;
};

struct t_timeval
{
    ULONG tv_secs;
    ULONG tv_micro;
};

struct t_fdset
{
    ULONG bits[8];
};

#define T_AF_INET           2
#define T_AF_INET6          23
#define T_AF_UNSPEC         0
#define T_SOCK_STREAM       1
#define T_SOCK_DGRAM        2
#define T_SOCK_RAW          3
#define T_IPPROTO_TCP       6
#define T_IPPROTO_IPV6      41
#define T_RAW_PROTO         253
#define T_IPV6_V6ONLY_BSD   27
#define T_IPV6_V6ONLY_LINUX 26

#define T_ENOPROTOOPT       42
#define T_EAFNOSUPPORT      47
#define T_EWOULDBLOCK       35
#define T_ECONNREFUSED      61
#define T_EPIPE             32
#define T_EDESTADDRREQ      39
#define T_ENOTCONN          57

#define T_MSG_DONTWAIT      0x80
#define T_MSG_PEEK          0x02
#define T_MSG_OOB           0x01

#define T_FIONREAD          0x4004667FUL
#define T_SO_EVENTMASK      0x2001
#define T_FD_OOB            0x04

#define T_AI_PASSIVE        1
#define T_AI_NUMERICHOST    4

#define T_PORT              9099
#define T_TAP_ADDR          0x0A090901UL      /* tap0, 10.9.9.1 */


/* ------------------------------------------------------------ LVO stubs --- */

/*
 * Every stub declares three variables it never uses because d0, d1, a0 and a1
 * are scratch on AmigaOS: a library function may destroy them without saying
 * so.  An `asm` block that lists them only as inputs tells GCC the opposite,
 * that whatever was in them survives the call, and GCC will reuse the
 * "still valid" copy afterwards.  The first version of this file did that, and
 * `send()` returned the right value while `rc == sizeof(message)` compared
 * false immediately after, because GCC had kept `len` in d1 and the library
 * had overwritten it.
 *
 * The NDK's own idiom (see inline/bsdsocket.h, which declares
 * `register int _d1 __asm("d1"); register int _a0 __asm("a0");
 *  register int _a1 __asm("a1");` and lists them as "=r" outputs) names the
 * scratch registers and declares them written, so GCC knows their previous
 * contents are gone.  a2/a3/d2/d3 are callee-saved and need no such treatment.
 */
#define BSD_SCRATCH                                                          \
    register LONG _s_d1 __asm("d1");                                         \
    register LONG _s_a0 __asm("a0");                                         \
    register LONG _s_a1 __asm("a1")

#define BSD_SCRATCH_OUT "=r" (_s_d1), "=r" (_s_a0), "=r" (_s_a1)

static struct Library *SocketBase;

static LONG bsd_socket(LONG domain, LONG type, LONG proto)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = domain;
register LONG            d1  __asm("d1") = type;
register LONG            d2  __asm("d2") = proto;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-30:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_bind(LONG fd, APTR name, LONG len)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = name;
register LONG            d1  __asm("d1") = len;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-36:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_listen(LONG fd, LONG backlog)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register LONG            d1  __asm("d1") = backlog;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-42:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_accept(LONG fd, APTR addr, APTR addrlen)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = addr;
register APTR            a1  __asm("a1") = addrlen;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-48:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_connect(LONG fd, APTR name, LONG len)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = name;
register LONG            d1  __asm("d1") = len;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-54:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_send(LONG fd, APTR buf, LONG len, LONG flags)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = buf;
register LONG            d1  __asm("d1") = len;
register LONG            d2  __asm("d2") = flags;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-66:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (d1),
                        "r" (d2)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_recv(LONG fd, APTR buf, LONG len, LONG flags)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = buf;
register LONG            d1  __asm("d1") = len;
register LONG            d2  __asm("d2") = flags;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-78:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (d1),
                        "r" (d2)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_shutdown(LONG fd, LONG how)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register LONG            d1  __asm("d1") = how;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-84:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_sendto(LONG fd, APTR buf, LONG len, LONG flags,
                       APTR to, LONG tolen)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = buf;
register LONG            d1  __asm("d1") = len;
register LONG            d2  __asm("d2") = flags;
register APTR            a1  __asm("a1") = to;
register LONG            d3  __asm("d3") = tolen;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-60:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (d1),
                        "r" (d2), "r" (a1), "r" (d3)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_recvfrom(LONG fd, APTR buf, LONG len, LONG flags,
                         APTR from, APTR fromlen)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = buf;
register LONG            d1  __asm("d1") = len;
register LONG            d2  __asm("d2") = flags;
register APTR            a1  __asm("a1") = from;
register APTR            a2  __asm("a2") = fromlen;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-72:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (d1),
                        "r" (d2), "r" (a1), "r" (a2)
                      : "cc", "memory");
    return(res);
}

/* -0x10e and -0x114: the RFC 3542 options ride these two and nothing else. */
static LONG bsd_sendmsg(LONG fd, APTR msg, LONG flags)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = msg;
register LONG            d1  __asm("d1") = flags;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-270:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0),
                        "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_recvmsg(LONG fd, APTR msg, LONG flags)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = msg;
register LONG            d1  __asm("d1") = flags;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-276:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0),
                        "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_setsockopt(LONG fd, LONG level, LONG name, APTR val, LONG len)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register LONG            d1  __asm("d1") = level;
register LONG            d2  __asm("d2") = name;
register APTR            a0  __asm("a0") = val;
register LONG            d3  __asm("d3") = len;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-90:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (d1), "r" (d2),
                        "r" (a0), "r" (d3)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_getsockopt(LONG fd, LONG level, LONG name, APTR val, APTR len)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register LONG            d1  __asm("d1") = level;
register LONG            d2  __asm("d2") = name;
register APTR            a0  __asm("a0") = val;
register APTR            a1  __asm("a1") = len;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-96:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (d1), "r" (d2),
                        "r" (a0), "r" (a1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_getsockname(LONG fd, APTR name, APTR len)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = name;
register APTR            a1  __asm("a1") = len;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-102:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_getpeername(LONG fd, APTR name, APTR len)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register APTR            a0  __asm("a0") = name;
register APTR            a1  __asm("a1") = len;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-108:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_CloseSocket(LONG fd)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-120:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_IoctlSocket(LONG fd, ULONG req, APTR argp)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register ULONG           d1  __asm("d1") = req;
register APTR            a0  __asm("a0") = argp;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-114:W)"
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0)
                      : "cc", "memory");
    return(res);
}

/* RFC 3493 section 4, revision 3 vectors. Here because an ifindex is only
   useful if something can turn it back into a name. */
static ULONG bsd_if_nametoindex(const char *name)
{
register struct Library *a6  __asm("a6") = SocketBase;
register const char     *a0  __asm("a0") = name;
register ULONG           res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-882:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (a0)
                      : "cc", "memory");
    return(res);
}

static char *bsd_if_indextoname(ULONG index, char *name)
{
register struct Library *a6  __asm("a6") = SocketBase;
register ULONG           d0  __asm("d0") = index;
register char           *a0  __asm("a0") = name;
register char           *res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-888:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_Errno(VOID)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-162:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_WaitSelect(LONG nfds, APTR readfds, APTR timeout)
{
register struct Library *a6 __asm("a6") = SocketBase;
register LONG            d0 __asm("d0") = nfds;
register APTR            a0 __asm("a0") = readfds;
register APTR            a1 __asm("a1") = NULL;
register APTR            a2 __asm("a2") = NULL;
register APTR            a3 __asm("a3") = timeout;
register APTR            d1 __asm("d1") = NULL;
register LONG            res __asm("d0");
register LONG _clob_d1 __asm("d1");
register LONG _clob_a0 __asm("a0");
register LONG _clob_a1 __asm("a1");
register LONG _clob_a2 __asm("a2");
register LONG _clob_a3 __asm("a3");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2), "=r" (_clob_a3)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_WaitSelectExcept(LONG nfds, APTR exceptfds, APTR timeout)
{
register struct Library *a6 __asm("a6") = SocketBase;
register LONG            d0 __asm("d0") = nfds;
register APTR            a0 __asm("a0") = NULL;
register APTR            a1 __asm("a1") = NULL;
register APTR            a2 __asm("a2") = exceptfds;
register APTR            a3 __asm("a3") = timeout;
register APTR            d1 __asm("d1") = NULL;
register LONG            res __asm("d0");
register LONG _clob_d1 __asm("d1");
register LONG _clob_a0 __asm("a0");
register LONG _clob_a1 __asm("a1");
register LONG _clob_a2 __asm("a2");
register LONG _clob_a3 __asm("a3");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2), "=r" (_clob_a3)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_WaitSelectAll(LONG nfds, APTR readfds, APTR writefds,
                              APTR exceptfds, APTR timeout)
{
register struct Library *a6 __asm("a6") = SocketBase;
register LONG            d0 __asm("d0") = nfds;
register APTR            a0 __asm("a0") = readfds;
register APTR            a1 __asm("a1") = writefds;
register APTR            a2 __asm("a2") = exceptfds;
register APTR            a3 __asm("a3") = timeout;
register APTR            d1 __asm("d1") = NULL;
register LONG            res __asm("d0");
register LONG _clob_d1 __asm("d1");
register LONG _clob_a0 __asm("a0");
register LONG _clob_a1 __asm("a1");
register LONG _clob_a2 __asm("a2");
register LONG _clob_a3 __asm("a3");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1), "=r" (_clob_a2), "=r" (_clob_a3)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_GetSocketEvents(ULONG *events)
{
register struct Library *a6  __asm("a6") = SocketBase;
register ULONG          *a0  __asm("a0") = events;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-300:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (a0)
                      : "cc", "memory");
    return(res);
}

static APTR bsd_inet_ntop(LONG af, APTR src, APTR dst, LONG size)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = af;
register APTR            a0  __asm("a0") = src;
register APTR            a1  __asm("a1") = dst;
register LONG            d1  __asm("d1") = size;
register APTR            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-600:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (a1),
                        "r" (d1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_inet_pton(LONG af, APTR src, APTR dst)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = af;
register APTR            a0  __asm("a0") = src;
register APTR            a1  __asm("a1") = dst;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-606:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_getaddrinfo(APTR node, APTR serv, APTR hints, APTR res_out)
{
register struct Library *a6  __asm("a6") = SocketBase;
register APTR            a0  __asm("a0") = node;
register APTR            a1  __asm("a1") = serv;
register APTR            a2  __asm("a2") = hints;
register APTR            a3  __asm("a3") = res_out;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-810:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3)
                      : "cc", "memory");
    return(res);
}

static VOID bsd_freeaddrinfo(APTR ai)
{
register struct Library *a6 __asm("a6") = SocketBase;
register APTR            a0 __asm("a0") = ai;
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-804:W)"
                      : BSD_SCRATCH_OUT
                      : "r" (a6), "r" (a0)
                      : "cc", "memory");
}

/* gai_strerror takes its argument in a0, not d0, pragmas line 141. */
static APTR bsd_gai_strerror(LONG code)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            a0  __asm("a0") = code;
register APTR            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-816:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (a0)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_getnameinfo(APTR sa, ULONG salen, APTR host, ULONG hostlen,
                            APTR serv, ULONG servlen, ULONG flags)
{
register struct Library *a6  __asm("a6") = SocketBase;
register APTR            a0  __asm("a0") = sa;
register ULONG           d0  __asm("d0") = salen;
register APTR            a1  __asm("a1") = host;
register ULONG           d1  __asm("d1") = hostlen;
register APTR            a2  __asm("a2") = serv;
register ULONG           d2  __asm("d2") = servlen;
register ULONG           d3  __asm("d3") = flags;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-822:W)"
                      : BSD_SCRATCH_OUT, "=r" (res) : "r" (a6), "r" (a0), "r" (d0), "r" (a1),
                        "r" (d1), "r" (a2), "r" (d2), "r" (d3)
                      : "cc", "memory");
    return(res);
}


/* ------------------------------------------------------------- helpers --- */

static VOID t_bzero(APTR p, ULONG n)
{
UBYTE  *b = (UBYTE *)p;
ULONG   i;

    for (i = 0; i < n; i++)
    {
        b[i] = 0;
    }
}

static BOOL t_streq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0' && *a == *b)
    {
        a++;
        b++;
    }

    return (BOOL)(*a == *b);
}

/* ::1 */
static VOID t_make_loopback6(struct t_sockaddr_in6 *sa, UWORD port)
{
    t_bzero(sa, sizeof(*sa));
    sa->sin6_family        = T_AF_INET6;
    sa->sin6_port          = port;      /* m68k is network order */
    sa->sin6_addr.s6_addr[15] = 1;
}

/* :: with a port */
static VOID t_make_any6(struct t_sockaddr_in6 *sa, UWORD port)
{
    t_bzero(sa, sizeof(*sa));
    sa->sin6_family = T_AF_INET6;
    sa->sin6_port   = port;
}

static LONG t_udp_readable(LONG fd)
{
struct t_fdset   set;
struct t_timeval tv;

    t_bzero(&set, sizeof(set));
    set.bits[(ULONG)fd >> 5] = 1UL << ((ULONG)fd & 31UL);
    tv.tv_secs  = 0;
    tv.tv_micro = 0;

    return bsd_WaitSelect(fd + 1, &set, &tv);
}

static LONG t_socket_exception(LONG fd)
{
struct t_fdset   set;
struct t_timeval tv;

    t_bzero(&set, sizeof(set));
    set.bits[(ULONG)fd >> 5] = 1UL << ((ULONG)fd & 31UL);
    tv.tv_secs  = 0;
    tv.tv_micro = 0;

    return bsd_WaitSelectExcept(fd + 1, &set, &tv);
}


/* ---------------------------------------------------------------- tests --- */

static VOID t_test_conversions(VOID)
{
/* Both zeroed, so a call that returns success without writing its output
   fails the check below instead of comparing whatever was on the stack. */
struct t_in6_addr   addr = { { 0 } };
char                text[64] = { 0 };
APTR                p;
LONG                rc;

    t_log("address conversions");

    /* pton then ntop must round-trip through the canonical RFC 5952 form. */
    rc = bsd_inet_pton(T_AF_INET6, (APTR)"2001:0db8:0000:0000:0000:0000:0000:0001",
                       &addr);
    (VOID)t_check((BOOL)(rc == 1), "inet_pton AF_INET6 accepts the long form", rc);
    (VOID)t_check((BOOL)(addr.s6_addr[0] == 0x20 && addr.s6_addr[1] == 0x01 &&
                         addr.s6_addr[15] == 0x01),
                  "inet_pton produced the right 16 bytes", (LONG)addr.s6_addr[0]);

    p = bsd_inet_ntop(T_AF_INET6, &addr, text, sizeof(text));
    (VOID)t_check((BOOL)(p != NULL), "inet_ntop AF_INET6 returned the buffer", 0);
    if (p != NULL)
    {
        t_log("  2001:db8::1 prints as \"%s\"", text);
        (VOID)t_check(t_streq(text, "2001:db8::1"),
                      "inet_ntop wrote the RFC 5952 canonical form", 0);
    }

    /* v4-mapped uses the dotted tail, as every other stack prints it. */
    rc = bsd_inet_pton(T_AF_INET6, (APTR)"::ffff:10.0.2.15", &addr);
    (VOID)t_check((BOOL)(rc == 1), "inet_pton accepts a v4-mapped address", rc);
    p = bsd_inet_ntop(T_AF_INET6, &addr, text, sizeof(text));
    if (p != NULL)
    {
        (VOID)t_check(t_streq(text, "::ffff:10.0.2.15"),
                      "v4-mapped prints with the dotted tail", 0);
    }

    /* Malformed input is 0, not -1: -1 means "family I do not know". */
    rc = bsd_inet_pton(T_AF_INET6, (APTR)"2001:db8::1::2", &addr);
    (VOID)t_check((BOOL)(rc == 0), "inet_pton rejects two :: runs", rc);

    rc = bsd_inet_pton(T_AF_INET6, (APTR)"fe80::1/64", &addr);
    (VOID)t_check((BOOL)(rc == 0), "inet_pton rejects a prefix suffix", rc);

    /* A buffer one byte too small must fail rather than truncate. */
    p = bsd_inet_ntop(T_AF_INET6, &addr, text, 4);
    (VOID)t_check((BOOL)(p == NULL), "inet_ntop refuses a short buffer", 0);
}

static VOID t_test_socket_basics(VOID)
{
LONG                fd;
LONG                rc;
LONG                value;
ULONG               len;
struct t_sockaddr_in6 sa;
struct t_sockaddr_in  sa4;

    t_log("AF_INET6 socket basics");

    fd = bsd_socket(T_AF_INET6, T_SOCK_STREAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(AF_INET6, SOCK_STREAM)", bsd_Errno()))
    {
        return;
    }

    /* IPV6_V6ONLY defaults to off, and answers to both numberings. */
    value = -1;
    len   = sizeof(value);
    rc = bsd_getsockopt(fd, T_IPPROTO_IPV6, T_IPV6_V6ONLY_BSD, &value, &len);
    (VOID)t_check((BOOL)(rc == 0 && value == 0),
                  "IPV6_V6ONLY (BSD numbering) defaults to 0", value);

    value = -1;
    len   = sizeof(value);
    rc = bsd_getsockopt(fd, T_IPPROTO_IPV6, T_IPV6_V6ONLY_LINUX, &value, &len);
    (VOID)t_check((BOOL)(rc == 0 && value == 0),
                  "IPV6_V6ONLY (Linux numbering) answers too", value);

    value = 1;
    rc = bsd_setsockopt(fd, T_IPPROTO_IPV6, T_IPV6_V6ONLY_BSD, &value,
                        sizeof(value));
    (VOID)t_check((BOOL)(rc == 0), "setsockopt IPV6_V6ONLY=1", bsd_Errno());

    value = -1;
    len   = sizeof(value);
    (VOID)bsd_getsockopt(fd, T_IPPROTO_IPV6, T_IPV6_V6ONLY_BSD, &value, &len);
    (VOID)t_check((BOOL)(value == 1), "IPV6_V6ONLY reads back as 1", value);

    /* A sockaddr_in on an AF_INET6 socket is a programming error. */
    t_bzero(&sa4, sizeof(sa4));
    sa4.sin_len    = sizeof(sa4);
    sa4.sin_family = T_AF_INET;
    sa4.sin_port   = T_PORT;
    rc = bsd_bind(fd, &sa4, sizeof(sa4));
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_EAFNOSUPPORT),
                  "bind(AF_INET6 socket, sockaddr_in) is EAFNOSUPPORT",
                  bsd_Errno());

    /* The real thing. */
    t_make_any6(&sa, T_PORT + 1);
    rc = bsd_bind(fd, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "bind(::, port)", bsd_Errno());

    /* getsockname must hand back a sockaddr_in6, 28 bytes, family at 0. */
    t_bzero(&sa, sizeof(sa));
    len = sizeof(sa);
    rc  = bsd_getsockname(fd, &sa, &len);
    (VOID)t_check((BOOL)(rc == 0), "getsockname", bsd_Errno());
    (VOID)t_check((BOOL)(len == 28), "getsockname reports 28 bytes", (LONG)len);
    (VOID)t_check((BOOL)(sa.sin6_family == T_AF_INET6),
                  "sin6_family is at offset 0", (LONG)sa.sin6_family);
    (VOID)t_check((BOOL)(sa.sin6_port == T_PORT + 1),
                  "getsockname reports the bound port", (LONG)sa.sin6_port);

    (VOID)bsd_CloseSocket(fd);

    /* A dgram socket, so the UDP path is reached too. */
    fd = bsd_socket(T_AF_INET6, T_SOCK_DGRAM, 0);
    (VOID)t_check((BOOL)(fd >= 0), "socket(AF_INET6, SOCK_DGRAM)", bsd_Errno());
    if (fd >= 0)
    {
        (VOID)bsd_CloseSocket(fd);
    }
}

static VOID t_test_tcp_loopback(VOID)
{
LONG                    server, client, accepted;
LONG                    rc;
ULONG                   len;
struct t_sockaddr_in6   sa;
static const char       message[] = "AF_INET6 over ::1";
char                    buffer[64];

    t_log("TCP over ::1");

    server = bsd_socket(T_AF_INET6, T_SOCK_STREAM, 0);
    if (!t_check((BOOL)(server >= 0), "server socket", bsd_Errno()))
    {
        return;
    }

    t_make_any6(&sa, T_PORT);
    rc = bsd_bind(server, &sa, sizeof(sa));
    if (!t_check((BOOL)(rc == 0), "server bind(::, 9099)", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        return;
    }

    rc = bsd_listen(server, 4);
    if (!t_check((BOOL)(rc == 0), "server listen", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        return;
    }

    client = bsd_socket(T_AF_INET6, T_SOCK_STREAM, 0);
    if (!t_check((BOOL)(client >= 0), "client socket", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        return;
    }

    t_make_loopback6(&sa, T_PORT);
    rc = bsd_connect(client, &sa, sizeof(sa));
    if (!t_check((BOOL)(rc == 0), "client connect to ::1", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(client);
        (VOID)bsd_CloseSocket(server);
        return;
    }

    t_bzero(&sa, sizeof(sa));
    len = sizeof(sa);
    accepted = bsd_accept(server, &sa, &len);
    if (!t_check((BOOL)(accepted >= 0), "server accept", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(client);
        (VOID)bsd_CloseSocket(server);
        return;
    }

    (VOID)t_check((BOOL)(sa.sin6_family == T_AF_INET6),
                  "accept filled in a sockaddr_in6", (LONG)sa.sin6_family);
    (VOID)t_check((BOOL)(len == 28), "accept reports 28 bytes", (LONG)len);
    (VOID)t_check((BOOL)(sa.sin6_addr.s6_addr[15] == 1),
                  "the peer of a ::1 connection is ::1",
                  (LONG)sa.sin6_addr.s6_addr[15]);

    /* getpeername on the client must agree. */
    t_bzero(&sa, sizeof(sa));
    len = sizeof(sa);
    rc  = bsd_getpeername(client, &sa, &len);
    (VOID)t_check((BOOL)(rc == 0 && sa.sin6_family == T_AF_INET6 &&
                         sa.sin6_port == T_PORT),
                  "getpeername reports ::1 port 9099", bsd_Errno());

    /* And getsockname on a socket bound to :: reports the source the stack
       actually chose, ::1 for a loopback connection. */
    t_bzero(&sa, sizeof(sa));
    len = sizeof(sa);
    rc  = bsd_getsockname(client, &sa, &len);
    (VOID)t_check((BOOL)(rc == 0 && sa.sin6_addr.s6_addr[15] == 1),
                  "getsockname on a ::1 connection reports ::1",
                  (LONG)sa.sin6_addr.s6_addr[15]);

    rc = bsd_send(client, (APTR)message, sizeof(message), 0);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(message)), "client send", rc);

    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(accepted, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(message)), "server recv", rc);
    (VOID)t_check(t_streq(buffer, message), "payload survived the round trip", 0);
    t_log("  server got \"%s\"", buffer);

    /* Echo it back so the reverse direction is exercised too. */
    rc = bsd_send(accepted, buffer, sizeof(message), 0);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(message)), "server echo", rc);

    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(client, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(message) &&
                         t_streq(buffer, message)),
                  "client received the echo", rc);

    (VOID)bsd_CloseSocket(accepted);
    (VOID)bsd_CloseSocket(client);
    (VOID)bsd_CloseSocket(server);
}

static VOID t_test_tcp_oob_event_consumption(VOID)
{
LONG                  server, client, accepted;
LONG                  rc;
LONG                  event_mask = T_FD_OOB;
ULONG                 events = 0;
struct t_sockaddr_in  sa;
UBYTE                 sent = 0xA5;
UBYTE                 received = 0;

    t_log("TCP OOB select after GetSocketEvents");

    server = bsd_socket(T_AF_INET, T_SOCK_STREAM, 0);
    client = bsd_socket(T_AF_INET, T_SOCK_STREAM, 0);
    if (!t_check((BOOL)(server >= 0 && client >= 0), "TCP OOB sockets",
                 bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = T_PORT + 41;
    sa.sin_addr   = 0x7F000001UL;

    rc = bsd_bind(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "TCP OOB listener bind", bsd_Errno());
    rc = bsd_listen(server, 1);
    (VOID)t_check((BOOL)(rc == 0), "TCP OOB listener listen", bsd_Errno());
    rc = bsd_connect(client, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "TCP OOB client connect", bsd_Errno());

    accepted = bsd_accept(server, NULL, NULL);
    if (!t_check((BOOL)(accepted >= 0), "TCP OOB accept", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(client);
        (VOID)bsd_CloseSocket(server);
        return;
    }

    rc = bsd_setsockopt(accepted, SOL_SOCKET, T_SO_EVENTMASK, &event_mask,
                        sizeof(event_mask));
    (VOID)t_check((BOOL)(rc == 0), "enable TCP OOB event mask", bsd_Errno());

    rc = bsd_send(client, &sent, 1, T_MSG_OOB);
    (VOID)t_check((BOOL)(rc == 1), "send TCP urgent byte", rc);

    Delay(2);
    rc = t_socket_exception(accepted);
    (VOID)t_check((BOOL)(rc == 1), "urgent byte sets exceptfds", rc);

    rc = bsd_GetSocketEvents(&events);
    (VOID)t_check((BOOL)(rc == accepted && (events & T_FD_OOB) != 0),
                  "GetSocketEvents consumes its OOB latch", rc);

    rc = t_socket_exception(accepted);
    (VOID)t_check((BOOL)(rc == 1),
                  "unread urgent byte remains in exceptfds", rc);

    rc = bsd_recv(accepted, &received, 1, T_MSG_OOB);
    (VOID)t_check((BOOL)(rc == 1 && received == sent),
                  "recv(MSG_OOB) consumes urgent byte", rc);

    rc = t_socket_exception(accepted);
    (VOID)t_check((BOOL)(rc == 0), "consumed urgent byte clears exceptfds", rc);

    (VOID)bsd_CloseSocket(accepted);
    (VOID)bsd_CloseSocket(client);
    (VOID)bsd_CloseSocket(server);
}

/* NetX listens by port, not address family.  Reject an IPv6 handshake which
   lands on an AF_INET listener, then make sure returning that connection did
   not lose the listener's parked socket by accepting the IPv4 peer it wants. */
static VOID t_test_tcp_listener_family(VOID)
{
LONG                    server, client, accepted;
LONG                    rc;
struct t_sockaddr_in     sa4;
struct t_sockaddr_in6    sa6;

    t_log("TCP listener address family");

    server = bsd_socket(T_AF_INET, T_SOCK_STREAM, 0);
    if (!t_check((BOOL)(server >= 0), "AF_INET listener socket", bsd_Errno()))
        return;

    t_bzero(&sa4, sizeof(sa4));
    sa4.sin_len    = sizeof(sa4);
    sa4.sin_family = T_AF_INET;
    sa4.sin_port   = T_PORT + 6;

    rc = bsd_bind(server, &sa4, sizeof(sa4));
    if (!t_check((BOOL)(rc == 0), "AF_INET listener bind", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        return;
    }

    rc = bsd_listen(server, 2);
    if (!t_check((BOOL)(rc == 0), "AF_INET listen", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        return;
    }

    client = bsd_socket(T_AF_INET6, T_SOCK_STREAM, 0);
    if (t_check((BOOL)(client >= 0), "wrong-family IPv6 client", bsd_Errno()))
    {
        t_make_loopback6(&sa6, T_PORT + 6);
        rc = bsd_connect(client, &sa6, sizeof(sa6));
        if (t_check((BOOL)(rc == 0), "IPv6 handshake reached the port", rc))
        {
            accepted = bsd_accept(server, NULL, NULL);
            (VOID)t_check((BOOL)(accepted < 0 &&
                                 bsd_Errno() == T_EWOULDBLOCK),
                          "AF_INET listener rejects IPv6", bsd_Errno());
            if (accepted >= 0)
                (VOID)bsd_CloseSocket(accepted);
        }
        (VOID)bsd_CloseSocket(client);
    }

    client = bsd_socket(T_AF_INET, T_SOCK_STREAM, 0);
    if (t_check((BOOL)(client >= 0), "right-family IPv4 client", bsd_Errno()))
    {
        sa4.sin_addr = 0x7F000001UL;
        rc = bsd_connect(client, &sa4, sizeof(sa4));
        if (t_check((BOOL)(rc == 0), "IPv4 client connects after refusal", rc))
        {
            accepted = bsd_accept(server, NULL, NULL);
            (VOID)t_check((BOOL)(accepted >= 0),
                          "AF_INET listener still accepts IPv4", bsd_Errno());
            if (accepted >= 0)
                (VOID)bsd_CloseSocket(accepted);
        }
        (VOID)bsd_CloseSocket(client);
    }

    (VOID)bsd_CloseSocket(server);
}

/* A parked server socket is born with an unspecified local address.  Once a
   dual-stack listener accepts IPv4, getsockname() must report the mapped
   address that completed the connection rather than leaving that wildcard. */
static VOID t_test_tcp_accepted_local(VOID)
{
LONG                    server, client, accepted;
LONG                    rc;
ULONG                   len;
struct t_sockaddr_in     sa4;
struct t_sockaddr_in6    sa6;

    t_log("accepted TCP local endpoint");

    server = bsd_socket(T_AF_INET6, T_SOCK_STREAM, 0);
    if (!t_check((BOOL)(server >= 0), "dual-stack listener socket",
                 bsd_Errno()))
        return;

    t_make_any6(&sa6, T_PORT + 7);
    rc = bsd_bind(server, &sa6, sizeof(sa6));
    if (!t_check((BOOL)(rc == 0), "dual-stack listener bind", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        return;
    }

    rc = bsd_listen(server, 1);
    if (!t_check((BOOL)(rc == 0), "dual-stack listen", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        return;
    }

    client = bsd_socket(T_AF_INET, T_SOCK_STREAM, 0);
    if (!t_check((BOOL)(client >= 0), "IPv4 client socket", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        return;
    }

    t_bzero(&sa4, sizeof(sa4));
    sa4.sin_len    = sizeof(sa4);
    sa4.sin_family = T_AF_INET;
    sa4.sin_port   = T_PORT + 7;
    sa4.sin_addr   = 0x7F000001UL;

    rc = bsd_connect(client, &sa4, sizeof(sa4));
    if (!t_check((BOOL)(rc == 0), "IPv4 connects to dual-stack listener", rc))
    {
        (VOID)bsd_CloseSocket(client);
        (VOID)bsd_CloseSocket(server);
        return;
    }

    accepted = bsd_accept(server, NULL, NULL);
    if (t_check((BOOL)(accepted >= 0), "dual-stack accept IPv4", bsd_Errno()))
    {
        t_bzero(&sa6, sizeof(sa6));
        len = sizeof(sa6);
        rc = bsd_getsockname(accepted, &sa6, &len);
        (VOID)t_check(
            (BOOL)(rc == 0 && len == 28 &&
                   sa6.sin6_family == T_AF_INET6 &&
                   sa6.sin6_addr.s6_addr[10] == 0xff &&
                   sa6.sin6_addr.s6_addr[11] == 0xff &&
                   sa6.sin6_addr.s6_addr[12] == 127 &&
                   sa6.sin6_addr.s6_addr[13] == 0 &&
                   sa6.sin6_addr.s6_addr[14] == 0 &&
                   sa6.sin6_addr.s6_addr[15] == 1),
            "accepted getsockname reports ::ffff:127.0.0.1", rc);

        (VOID)bsd_CloseSocket(accepted);
    }

    (VOID)bsd_CloseSocket(client);
    (VOID)bsd_CloseSocket(server);
}

static VOID t_test_udp_loopback(VOID)
{
LONG                    server, client;
LONG                    rc;
ULONG                   len;
struct t_sockaddr_in6   sa;
static const char       datagram[] = "one AF_INET6 datagram";
char                    buffer[64];

    t_log("UDP over ::1");

    server = bsd_socket(T_AF_INET6, T_SOCK_DGRAM, 0);
    client = bsd_socket(T_AF_INET6, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && client >= 0), "udp sockets",
                 bsd_Errno()))
    {
        return;
    }

    t_make_any6(&sa, T_PORT + 2);
    rc = bsd_bind(server, &sa, sizeof(sa));
    if (!t_check((BOOL)(rc == 0), "udp server bind", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    t_make_loopback6(&sa, T_PORT + 2);
    rc = bsd_sendto(client, (APTR)datagram, sizeof(datagram), 0, &sa,
                    sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(datagram)), "sendto ::1", rc);

    t_bzero(buffer, sizeof(buffer));
    t_bzero(&sa, sizeof(sa));
    len = sizeof(sa);
    rc  = bsd_recvfrom(server, buffer, sizeof(buffer), 0, &sa, &len);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(datagram)), "recvfrom", rc);
    (VOID)t_check(t_streq(buffer, datagram), "datagram survived", 0);
    (VOID)t_check((BOOL)(sa.sin6_family == T_AF_INET6 && len == 28 &&
                         sa.sin6_addr.s6_addr[15] == 1),
                  "recvfrom reported the ::1 source as sockaddr_in6",
                  (LONG)sa.sin6_family);

    (VOID)bsd_CloseSocket(server);
    (VOID)bsd_CloseSocket(client);
}

/* NetX queues UDP by port.  A socket bound to one address must still reject
   a datagram for another address on the same interface; 127/8 makes that
   distinction testable without configuring a second interface address. */
static VOID t_test_udp_bound_address(VOID)
{
LONG                  server, client;
LONG                  rc;
ULONG                 len;
struct t_sockaddr_in  sa;
static const char     wrong[] = "for 127.0.0.1";
static const char     right[] = "for 127.0.0.2";
char                  buffer[64];

    t_log("UDP exact local-address bind");

    server = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    client = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && client >= 0), "udp4 alias sockets",
                 bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len         = sizeof(sa);
    sa.sin_family      = T_AF_INET;
    sa.sin_port        = T_PORT + 5;
    sa.sin_addr        = 0x7F000002UL;          /* 127.0.0.2 */

    rc = bsd_bind(server, &sa, sizeof(sa));
    if (!t_check((BOOL)(rc == 0), "bind 127.0.0.2", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    sa.sin_addr = 0x7F000001UL;                 /* 127.0.0.1 */
    rc = bsd_sendto(client, (APTR)wrong, sizeof(wrong), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(wrong)), "sendto wrong alias", rc);

    Delay(2);
    rc = t_udp_readable(server);
    (VOID)t_check((BOOL)(rc == 0),
                  "WaitSelect ignores the wrong local alias", rc);

    t_bzero(buffer, sizeof(buffer));
    len = sizeof(sa);
    rc = bsd_recvfrom(server, buffer, sizeof(buffer), T_MSG_DONTWAIT,
                      &sa, &len);
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "bound socket rejects the wrong alias", bsd_Errno());

    sa.sin_addr = 0x7F000002UL;
    rc = bsd_sendto(client, (APTR)right, sizeof(right), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(right)), "sendto bound alias", rc);

    Delay(2);
    rc = t_udp_readable(server);
    (VOID)t_check((BOOL)(rc == 1),
                  "WaitSelect sees the bound local alias", rc);

    t_bzero(buffer, sizeof(buffer));
    len = sizeof(sa);
    rc = bsd_recvfrom(server, buffer, sizeof(buffer), T_MSG_DONTWAIT,
                      &sa, &len);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(right) && t_streq(buffer, right)),
                  "bound socket receives its own alias", rc);

    (VOID)bsd_CloseSocket(server);
    (VOID)bsd_CloseSocket(client);
}

static VOID t_test_udp_connected_readiness(VOID)
{
LONG                  server, good, bad;
LONG                  rc;
LONG                  available;
struct t_sockaddr_in  sa;
static const char     wrong[] = "wrong UDP peer";
static const char     right[] = "right UDP peer";
char                  buffer[64];

    t_log("connected UDP readiness");

    server = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    good   = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    bad    = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && good >= 0 && bad >= 0),
                 "connected UDP sockets", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_addr   = 0x7F000001UL;

    sa.sin_port = T_PORT + 8;
    rc = bsd_bind(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connected UDP server bind", bsd_Errno());

    sa.sin_port = T_PORT + 9;
    rc = bsd_bind(good, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "good UDP peer bind", bsd_Errno());

    sa.sin_port = T_PORT + 10;
    rc = bsd_bind(bad, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "bad UDP peer bind", bsd_Errno());

    sa.sin_port = T_PORT + 9;
    rc = bsd_connect(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect UDP server to good peer", bsd_Errno());

    sa.sin_port = T_PORT + 8;
    rc = bsd_sendto(bad, (APTR)wrong, sizeof(wrong), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(wrong)), "send from wrong peer", rc);

    Delay(2);
    rc = t_udp_readable(server);
    (VOID)t_check((BOOL)(rc == 0), "WaitSelect ignores wrong UDP peer", rc);

    available = -1;
    rc = bsd_IoctlSocket(server, T_FIONREAD, &available);
    (VOID)t_check((BOOL)(rc == 0 && available == 0),
                  "FIONREAD ignores wrong UDP peer", available);

    rc = bsd_sendto(good, (APTR)right, sizeof(right), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(right)), "send from right peer", rc);

    Delay(2);
    rc = t_udp_readable(server);
    (VOID)t_check((BOOL)(rc == 1), "WaitSelect sees connected UDP peer", rc);

    available = -1;
    rc = bsd_IoctlSocket(server, T_FIONREAD, &available);
    (VOID)t_check((BOOL)(rc == 0 && available == (LONG)sizeof(right)),
                  "FIONREAD scans to the connected UDP peer", available);

    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(right) && t_streq(buffer, right)),
                  "recv skips wrong peer and returns right one", rc);

    (VOID)bsd_CloseSocket(bad);
    (VOID)bsd_CloseSocket(good);
    (VOID)bsd_CloseSocket(server);
}

static VOID t_test_udp_peek_fionread(VOID)
{
LONG                  server, client;
LONG                  rc;
LONG                  available = -1;
struct t_sockaddr_in  sa;
static const char     first[] = "first";
static const char     second[] = "a longer second datagram";
char                  buffer[64];

    t_log("UDP FIONREAD after MSG_PEEK");

    server = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    client = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && client >= 0),
                 "UDP peek FIONREAD sockets", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = T_PORT + 39;
    sa.sin_addr   = 0x7F000001UL;

    rc = bsd_bind(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "UDP peek FIONREAD bind", bsd_Errno());

    rc = bsd_sendto(client, (APTR)first, sizeof(first), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(first)),
                  "send first FIONREAD datagram", rc);
    rc = bsd_sendto(client, (APTR)second, sizeof(second), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(second)),
                  "send second FIONREAD datagram", rc);

    Delay(2);
    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer),
                  T_MSG_DONTWAIT | T_MSG_PEEK);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(first) && t_streq(buffer, first)),
                  "peek first FIONREAD datagram", rc);

    rc = bsd_IoctlSocket(server, T_FIONREAD, &available);
    (VOID)t_check((BOOL)(rc == 0 && available == (LONG)sizeof(first)),
                  "FIONREAD stops at parked datagram boundary", available);

    rc = bsd_recv(server, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(first)),
                  "consume parked FIONREAD datagram", rc);

    available = -1;
    rc = bsd_IoctlSocket(server, T_FIONREAD, &available);
    (VOID)t_check((BOOL)(rc == 0 && available == (LONG)sizeof(second)),
                  "FIONREAD advances to second datagram", available);

    (VOID)bsd_CloseSocket(client);
    (VOID)bsd_CloseSocket(server);
}

static VOID t_test_udp_reconnect_after_peek(VOID)
{
LONG                  server, old_peer, new_peer;
LONG                  rc;
struct t_sockaddr_in  sa;
static const char     old_data[] = "old UDP peer";
static const char     new_data[] = "new UDP peer";
char                  buffer[64];

    t_log("UDP reconnect after MSG_PEEK");

    server   = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    old_peer = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    new_peer = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && old_peer >= 0 && new_peer >= 0),
                 "UDP reconnect sockets", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_addr   = 0x7F000001UL;

    sa.sin_port = T_PORT + 11;
    rc = bsd_bind(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "UDP reconnect server bind", bsd_Errno());

    sa.sin_port = T_PORT + 12;
    rc = bsd_bind(old_peer, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "old UDP peer bind", bsd_Errno());

    sa.sin_port = T_PORT + 13;
    rc = bsd_bind(new_peer, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "new UDP peer bind", bsd_Errno());

    sa.sin_port = T_PORT + 11;
    rc = bsd_sendto(old_peer, (APTR)old_data, sizeof(old_data), 0,
                    &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(old_data)),
                  "send before UDP peek", rc);

    Delay(2);
    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer),
                  T_MSG_DONTWAIT | T_MSG_PEEK);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(old_data) &&
                         t_streq(buffer, old_data)),
                  "peek packet from old UDP peer", rc);

    sa.sin_port = T_PORT + 13;
    rc = bsd_connect(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "reconnect UDP to new peer", bsd_Errno());

    sa.sin_port = T_PORT + 11;
    rc = bsd_sendto(new_peer, (APTR)new_data, sizeof(new_data), 0,
                    &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(new_data)),
                  "send from new UDP peer", rc);

    Delay(2);
    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(new_data) &&
                         t_streq(buffer, new_data)),
                  "reconnect drops peeked packet from old UDP peer", rc);

    (VOID)bsd_CloseSocket(new_peer);
    (VOID)bsd_CloseSocket(old_peer);
    (VOID)bsd_CloseSocket(server);
}

static VOID t_test_udp_disconnect(VOID)
{
LONG                  server, good, bad;
LONG                  rc;
LONG                  len;
struct t_sockaddr_in  sa;
struct t_sockaddr_in  unspec;
static const char     data[] = "queued before UDP disconnect";
char                  buffer[64];

    t_log("UDP AF_UNSPEC disconnect");

    server = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    good   = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    bad    = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && good >= 0 && bad >= 0),
                 "UDP disconnect sockets", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_addr   = 0x7F000001UL;

    sa.sin_port = T_PORT + 36;
    rc = bsd_bind(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "UDP disconnect server bind", bsd_Errno());
    sa.sin_port = T_PORT + 37;
    rc = bsd_bind(good, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "UDP disconnect good-peer bind",
                  bsd_Errno());
    sa.sin_port = T_PORT + 38;
    rc = bsd_bind(bad, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "UDP disconnect bad-peer bind",
                  bsd_Errno());

    sa.sin_port = T_PORT + 37;
    rc = bsd_connect(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect before UDP disconnect",
                  bsd_Errno());

    sa.sin_port = T_PORT + 36;
    rc = bsd_sendto(bad, (APTR)data, sizeof(data), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(data)),
                  "queue packet from filtered UDP peer", rc);

    Delay(2);
    rc = t_udp_readable(server);
    (VOID)t_check((BOOL)(rc == 0),
                  "connected UDP hides the other peer", rc);

    t_bzero(&unspec, sizeof(unspec));
    rc = bsd_connect(server, &unspec, sizeof(unspec));
    (VOID)t_check((BOOL)(rc == 0), "disconnect UDP with AF_UNSPEC",
                  bsd_Errno());

    len = sizeof(sa);
    rc = bsd_getpeername(server, &sa, &len);
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_ENOTCONN),
                  "disconnected UDP has no peer", bsd_Errno());

    rc = t_udp_readable(server);
    (VOID)t_check((BOOL)(rc == 1),
                  "UDP disconnect broadens queued receive peers", rc);

    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(data) && t_streq(buffer, data)),
                  "disconnected UDP receives formerly filtered peer", rc);

    rc = bsd_send(server, (APTR)data, sizeof(data), 0);
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_EDESTADDRREQ),
                  "disconnected UDP send needs a destination", bsd_Errno());

    (VOID)bsd_CloseSocket(bad);
    (VOID)bsd_CloseSocket(good);
    (VOID)bsd_CloseSocket(server);
}

static VOID t_test_udp_icmp_readiness(VOID)
{
LONG                  fd;
LONG                  rc;
struct t_sockaddr_in  sa;
static const char     probe[] = "closed UDP port";
char                  buffer[16];

    t_log("connected UDP ICMP readiness");

    fd = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "UDP ICMP socket", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = T_PORT + 31;
    /* NetX, following RFC 1122, does not originate an ICMP error in response
       to a loopback-source datagram. Use the harness's real interface address
       so this actually exercises the asynchronous UDP error path. */
    sa.sin_addr   = T_TAP_ADDR;

    rc = bsd_connect(fd, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect UDP to unused port", bsd_Errno());

    rc = bsd_send(fd, (APTR)probe, sizeof(probe), 0);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(probe)),
                  "send UDP unused-port probe", rc);

    Delay(3);
    rc = t_udp_readable(fd);
    (VOID)t_check((BOOL)(rc == 1), "ICMP error makes UDP read-ready", rc);

    rc = bsd_recv(fd, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_ECONNREFUSED),
                  "read-ready UDP reports ICMP error", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_test_udp_so_error_consumes_icmp(VOID)
{
LONG                  fd;
LONG                  rc;
LONG                  error = 0;
LONG                  error_len = sizeof(error);
struct t_sockaddr_in  sa;
static const char     probe[] = "consume UDP error";
char                  buffer[16];

    t_log("UDP SO_ERROR consumes ICMP error");

    fd = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "UDP SO_ERROR socket", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = T_PORT + 32;
    sa.sin_addr   = T_TAP_ADDR;

    rc = bsd_connect(fd, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect UDP SO_ERROR probe",
                  bsd_Errno());

    rc = bsd_send(fd, (APTR)probe, sizeof(probe), 0);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(probe)),
                  "send UDP SO_ERROR probe", rc);

    Delay(3);
    rc = t_udp_readable(fd);
    (VOID)t_check((BOOL)(rc == 1), "UDP SO_ERROR probe becomes ready", rc);

    rc = bsd_getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len);
    (VOID)t_check((BOOL)(rc == 0 && error == T_ECONNREFUSED),
                  "SO_ERROR returns UDP ICMP error", error);

    rc = bsd_recv(fd, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "recv does not repeat consumed UDP ICMP error", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_test_udp_so_error_clears_exception(VOID)
{
LONG                  fd;
LONG                  rc;
LONG                  error = 0;
LONG                  error_len = sizeof(error);
struct t_sockaddr_in  sa;
static const char     probe[] = "UDP exception probe";

    t_log("UDP SO_ERROR clears select exception");

    fd = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "UDP exception socket", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = T_PORT + 40;
    sa.sin_addr   = T_TAP_ADDR;

    rc = bsd_connect(fd, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect UDP exception probe",
                  bsd_Errno());
    rc = bsd_send(fd, (APTR)probe, sizeof(probe), 0);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(probe)),
                  "send UDP exception probe", rc);

    Delay(3);
    rc = t_socket_exception(fd);
    (VOID)t_check((BOOL)(rc == 1), "UDP ICMP error sets exceptfds", rc);

    rc = bsd_getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len);
    (VOID)t_check((BOOL)(rc == 0 && error == T_ECONNREFUSED),
                  "consume UDP exception through SO_ERROR", error);

    rc = t_socket_exception(fd);
    (VOID)t_check((BOOL)(rc == 0),
                  "consumed SO_ERROR clears exceptfds", rc);

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_test_waitselect_counts_ready_bits(VOID)
{
LONG                  fd;
LONG                  rc;
LONG                  error = 0;
LONG                  error_len = sizeof(error);
ULONG                 word;
ULONG                 mask;
struct t_fdset        read_set, write_set, except_set;
struct t_timeval      tv;
struct t_sockaddr_in  sa;
static const char     probe[] = "multi-set UDP error";

    t_log("WaitSelect counts ready bits, not descriptors");

    fd = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "multi-set UDP socket", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = T_PORT + 42;
    sa.sin_addr   = T_TAP_ADDR;

    rc = bsd_connect(fd, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect multi-set UDP probe",
                  bsd_Errno());
    rc = bsd_send(fd, (APTR)probe, sizeof(probe), 0);
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(probe)),
                  "send multi-set UDP probe", rc);

    Delay(3);
    t_bzero(&read_set, sizeof(read_set));
    t_bzero(&write_set, sizeof(write_set));
    t_bzero(&except_set, sizeof(except_set));
    word = (ULONG)fd >> 5;
    mask = 1UL << ((ULONG)fd & 31UL);
    read_set.bits[word] = mask;
    write_set.bits[word] = mask;
    except_set.bits[word] = mask;
    tv.tv_secs  = 0;
    tv.tv_micro = 0;

    rc = bsd_WaitSelectAll(fd + 1, &read_set, &write_set, &except_set, &tv);
    /* POSIX returns "the total number of bits set in the bit masks", and
       AmiTCP's selscan() increments once per set per descriptor. Callers
       decrement the count once per FD_ISSET they act on, so one descriptor
       ready in three sets must answer 3 or the loop stops early. */
    (VOID)t_check((BOOL)(rc == 3),
                  "one descriptor ready in three sets counts three", rc);
    (VOID)t_check((BOOL)((read_set.bits[word] & mask) != 0 &&
                         (write_set.bits[word] & mask) != 0 &&
                         (except_set.bits[word] & mask) != 0),
                  "all three ready bits are preserved", rc);

    rc = bsd_getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len);
    (VOID)t_check((BOOL)(rc == 0 && error == T_ECONNREFUSED),
                  "consume multi-set UDP error", error);

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_test_datagram_shutdown(VOID)
{
LONG                  udp, raw;
LONG                  rc;
struct t_sockaddr_in  sa;
static const char     data[] = "after shutdown";
char                  buffer[16];

    t_log("UDP and raw shutdown semantics");

    udp = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    raw = bsd_socket(T_AF_INET, T_SOCK_RAW, T_RAW_PROTO);
    if (!t_check((BOOL)(udp >= 0 && raw >= 0), "datagram shutdown sockets",
                 bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = T_PORT + 33;
    sa.sin_addr   = 0x7F000001UL;

    rc = bsd_connect(udp, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect UDP before shutdown", bsd_Errno());
    rc = bsd_connect(raw, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect raw before shutdown", bsd_Errno());

    rc = bsd_shutdown(udp, 2);
    (VOID)t_check((BOOL)(rc == 0), "shutdown connected UDP", bsd_Errno());
    rc = bsd_shutdown(raw, 2);
    (VOID)t_check((BOOL)(rc == 0), "shutdown connected raw", bsd_Errno());

    rc = t_udp_readable(udp);
    (VOID)t_check((BOOL)(rc == 1), "shutdown UDP is read-ready", rc);
    rc = t_udp_readable(raw);
    (VOID)t_check((BOOL)(rc == 1), "shutdown raw is read-ready", rc);

    rc = bsd_recv(udp, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc == 0), "shutdown UDP receive returns EOF", rc);
    rc = bsd_recv(raw, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc == 0), "shutdown raw receive returns EOF", rc);

    rc = bsd_send(udp, (APTR)data, sizeof(data), 0);
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_EPIPE),
                  "shutdown UDP send returns EPIPE", bsd_Errno());
    rc = bsd_send(raw, (APTR)data, sizeof(data), 0);
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_EPIPE),
                  "shutdown raw send returns EPIPE", bsd_Errno());

    (VOID)bsd_CloseSocket(raw);
    (VOID)bsd_CloseSocket(udp);
}

static VOID t_test_shutdown_fionread(VOID)
{
LONG                  server, client;
LONG                  rc;
LONG                  available = -1;
struct t_sockaddr_in  sa;
static const char     data[] = "queued before shutdown";

    t_log("FIONREAD after read shutdown");

    server = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    client = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && client >= 0),
                 "shutdown FIONREAD sockets", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_addr   = 0x7F000001UL;

    sa.sin_port = T_PORT + 34;
    rc = bsd_bind(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "shutdown FIONREAD server bind",
                  bsd_Errno());

    sa.sin_port = T_PORT + 35;
    rc = bsd_bind(client, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "shutdown FIONREAD client bind",
                  bsd_Errno());

    rc = bsd_connect(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect shutdown FIONREAD server",
                  bsd_Errno());

    sa.sin_port = T_PORT + 34;
    rc = bsd_sendto(client, (APTR)data, sizeof(data), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(data)),
                  "queue datagram before read shutdown", rc);

    Delay(2);
    rc = bsd_IoctlSocket(server, T_FIONREAD, &available);
    (VOID)t_check((BOOL)(rc == 0 && available == (LONG)sizeof(data)),
                  "FIONREAD sees datagram before shutdown", available);

    rc = bsd_shutdown(server, 0);
    (VOID)t_check((BOOL)(rc == 0), "shutdown UDP read half", bsd_Errno());

    available = -1;
    rc = bsd_IoctlSocket(server, T_FIONREAD, &available);
    (VOID)t_check((BOOL)(rc == 0 && available == 0),
                  "FIONREAD is zero after read shutdown", available);

    (VOID)bsd_CloseSocket(client);
    (VOID)bsd_CloseSocket(server);
}

/* A custom raw protocol avoids ICMP's echo traffic while exercising the same
   global receive tee.  Receiving the first packet on the wildcard sender
   synchronizes with the IP thread before the bound socket is polled. */
static VOID t_test_raw_bound_address(VOID)
{
LONG                  server, client;
LONG                  rc;
struct t_sockaddr_in  sa;
static const char     wrong[] = "raw for 127.0.0.1";
static const char     right[] = "raw for 127.0.0.2";
char                  buffer[96];

    t_log("raw exact local-address bind");

    server = bsd_socket(T_AF_INET, T_SOCK_RAW, T_RAW_PROTO);
    client = bsd_socket(T_AF_INET, T_SOCK_RAW, T_RAW_PROTO);
    if (!t_check((BOOL)(server >= 0 && client >= 0), "raw alias sockets",
                 bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_addr   = 0x7F000002UL;

    rc = bsd_bind(server, &sa, sizeof(sa));
    if (!t_check((BOOL)(rc == 0), "raw bind 127.0.0.2", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    sa.sin_addr = 0x7F000001UL;
    rc = bsd_sendto(client, (APTR)wrong, sizeof(wrong), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(wrong)), "raw send wrong alias", rc);

    /* The wildcard sender sees its own looped-back packet.  Once this returns,
       the specifically bound socket has either queued or rejected it. */
    rc = bsd_recv(client, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc >= 20), "raw wildcard synchronization receive", rc);

    rc = bsd_recv(server, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "raw bind rejects the wrong alias", bsd_Errno());

    sa.sin_addr = 0x7F000002UL;
    rc = bsd_sendto(client, (APTR)right, sizeof(right), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(right)), "raw send bound alias", rc);

    rc = bsd_recv(server, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc >= 20 &&
                         (UBYTE)buffer[16] == 127 &&
                         (UBYTE)buffer[17] == 0 &&
                         (UBYTE)buffer[18] == 0 &&
                         (UBYTE)buffer[19] == 2),
                  "raw bind receives its own alias", rc);

    (VOID)bsd_CloseSocket(server);
    (VOID)bsd_CloseSocket(client);
}

static VOID t_test_raw_bind_after_peek(VOID)
{
LONG                  server, client;
LONG                  rc;
struct t_sockaddr_in  sa;
static const char     wrong[] = "raw before bind";
static const char     right[] = "raw after bind";
char                  buffer[96];

    t_log("raw bind after MSG_PEEK");

    server = bsd_socket(T_AF_INET, T_SOCK_RAW, T_RAW_PROTO);
    client = bsd_socket(T_AF_INET, T_SOCK_RAW, T_RAW_PROTO);
    if (!t_check((BOOL)(server >= 0 && client >= 0), "raw late-bind sockets",
                 bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_addr   = 0x7F000001UL;

    /* As in the peer-transition test, exercise both storage locations: one
       packet parked by MSG_PEEK and one still on the semaphore-backed queue. */
    rc = bsd_sendto(client, (APTR)wrong, sizeof(wrong), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(wrong)),
                  "raw pre-bind first send", rc);
    rc = bsd_recv(client, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc >= 20), "raw pre-bind first synchronization", rc);

    rc = bsd_sendto(client, (APTR)wrong, sizeof(wrong), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(wrong)),
                  "raw pre-bind second send", rc);
    rc = bsd_recv(client, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc >= 20), "raw pre-bind second synchronization", rc);

    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer),
                  T_MSG_DONTWAIT | T_MSG_PEEK);
    (VOID)t_check((BOOL)(rc >= 20 &&
                         (UBYTE)buffer[16] == 127 &&
                         (UBYTE)buffer[17] == 0 &&
                         (UBYTE)buffer[18] == 0 &&
                         (UBYTE)buffer[19] == 1),
                  "peek raw packet before bind", rc);

    sa.sin_addr = 0x7F000002UL;
    rc = bsd_bind(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "late raw bind to 127.0.0.2",
                  bsd_Errno());

    rc = bsd_sendto(client, (APTR)right, sizeof(right), 0, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(right)),
                  "raw post-bind send", rc);
    rc = bsd_recv(client, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc >= 20), "raw post-bind synchronization", rc);

    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc >= 20 &&
                         (UBYTE)buffer[16] == 127 &&
                         (UBYTE)buffer[17] == 0 &&
                         (UBYTE)buffer[18] == 0 &&
                         (UBYTE)buffer[19] == 2),
                  "raw bind removes packets for old local endpoint", rc);

    (VOID)bsd_CloseSocket(client);
    (VOID)bsd_CloseSocket(server);
}

static VOID t_test_raw_connect_after_peek(VOID)
{
LONG                  server, old_peer, new_peer;
LONG                  rc;
struct t_sockaddr_in  sa;
static const char     old_data[] = "raw old peer";
static const char     new_data[] = "raw new peer";
char                  buffer[96];

    t_log("raw connect after MSG_PEEK");

    server   = bsd_socket(T_AF_INET, T_SOCK_RAW, T_RAW_PROTO);
    old_peer = bsd_socket(T_AF_INET, T_SOCK_RAW, T_RAW_PROTO);
    new_peer = bsd_socket(T_AF_INET, T_SOCK_RAW, T_RAW_PROTO);
    if (!t_check((BOOL)(server >= 0 && old_peer >= 0 && new_peer >= 0),
                 "raw connect sockets", bsd_Errno()))
        return;

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    /* Use two addresses which the stack actually owns.  bind(127.0.0.2)
       is useful for exercising exact local-destination filtering, but NetX
       has one address on its loopback interface and therefore transmits any
       127/8 socket with 127.0.0.1 as its source.  That cannot distinguish the
       peers this test needs. */
    sa.sin_addr   = T_TAP_ADDR;
    rc = bsd_bind(old_peer, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "raw old peer bind", bsd_Errno());

    sa.sin_addr = 0x7F000001UL;
    rc = bsd_bind(new_peer, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "raw new peer bind", bsd_Errno());

    /* Leave one old-peer packet parked by MSG_PEEK and another on the raw
       queue. The new peer's receives synchronize with the global IP hook. */
    sa.sin_addr = T_TAP_ADDR;
    rc = bsd_sendto(old_peer, (APTR)old_data, sizeof(old_data), 0,
                    &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(old_data)),
                  "raw old peer first send", rc);
    rc = bsd_recv(old_peer, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc >= 20), "raw first send synchronization", rc);

    rc = bsd_sendto(old_peer, (APTR)old_data, sizeof(old_data), 0,
                    &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(old_data)),
                  "raw old peer second send", rc);
    rc = bsd_recv(old_peer, buffer, sizeof(buffer), 0);
    (VOID)t_check((BOOL)(rc >= 20), "raw second send synchronization", rc);

    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer),
                  T_MSG_DONTWAIT | T_MSG_PEEK);
    (VOID)t_check((BOOL)(rc >= 20 &&
                         (UBYTE)buffer[12] == 10 &&
                         (UBYTE)buffer[13] == 9 &&
                         (UBYTE)buffer[14] == 9 &&
                         (UBYTE)buffer[15] == 1),
                  "peek packet from raw old peer", rc);

    sa.sin_addr = 0x7F000001UL;
    rc = bsd_connect(server, &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == 0), "connect raw socket to new peer",
                  bsd_Errno());

    rc = bsd_sendto(new_peer, (APTR)new_data, sizeof(new_data), 0,
                    &sa, sizeof(sa));
    (VOID)t_check((BOOL)(rc == (LONG)sizeof(new_data)),
                  "raw new peer send", rc);

    Delay(2);
    t_bzero(buffer, sizeof(buffer));
    rc = bsd_recv(server, buffer, sizeof(buffer), T_MSG_DONTWAIT);
    (VOID)t_check((BOOL)(rc >= 20 &&
                         (UBYTE)buffer[12] == 127 &&
                         (UBYTE)buffer[13] == 0 &&
                         (UBYTE)buffer[14] == 0 &&
                         (UBYTE)buffer[15] == 1),
                  "raw connect removes packets from old peer", rc);

    (VOID)bsd_CloseSocket(new_peer);
    (VOID)bsd_CloseSocket(old_peer);
    (VOID)bsd_CloseSocket(server);
}

/* --------------------------------------------------------- RFC 3542 ------ */

/*
 * A control buffer has to be aligned for a socklen_t, and a bare char array is
 * not: on a 68000 an odd cmsg_len would be an address error rather than a
 * wrong answer.  CMSG_BUFFER() is the declaration that cannot be wrong, and
 * this is the program that has to demonstrate it, it is a caller of the
 * published header and nothing else.
 */
#define T_CBUF_BYTES    (CMSG_SPACE(sizeof(struct in6_pktinfo)) + \
                         CMSG_SPACE(sizeof(LONG)))

/* The macros on their own, no library involved, so a failure here is the
   header being wrong rather than the stack. */
static VOID t_test_cmsg_macros(VOID)
{
CMSG_BUFFER(cbuf, T_CBUF_BYTES);
struct msghdr       msg;
struct cmsghdr     *c;
struct icmp6_filter filt;
ULONG               seen = 0;

    t_log("RFC 3542 macros");

    (VOID)t_check((BOOL)(sizeof(struct cmsghdr) == 12),
                  "struct cmsghdr is 12 bytes", (LONG)sizeof(struct cmsghdr));
    (VOID)t_check((BOOL)(CMSG_ALIGN(1) == 4 && CMSG_ALIGN(4) == 4 &&
                         CMSG_ALIGN(5) == 8),
                  "CMSG_ALIGN rounds to 4", (LONG)CMSG_ALIGN(5));
    (VOID)t_check((BOOL)(CMSG_LEN(4) == 16 && CMSG_SPACE(1) == 16),
                  "CMSG_LEN and CMSG_SPACE", (LONG)CMSG_SPACE(1));
    (VOID)t_check((BOOL)(sizeof(struct in6_pktinfo) == 20),
                  "in6_pktinfo is 20 bytes",
                  (LONG)sizeof(struct in6_pktinfo));
    (VOID)t_check((BOOL)(sizeof(struct in_pktinfo) == 12),
                  "in_pktinfo is 12 bytes", (LONG)sizeof(struct in_pktinfo));

    /* CMSG_FIRSTHDR must answer NULL for the "no ancillary data" report. */
    t_bzero(&msg, sizeof(msg));
    msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
    msg.msg_controllen = 0;
    (VOID)t_check((BOOL)(CMSG_FIRSTHDR(&msg) == NULL),
                  "CMSG_FIRSTHDR is NULL when msg_controllen is 0", 0);

    /* Two objects, walked back out. */
    t_bzero(&cbuf, sizeof(cbuf));
    msg.msg_controllen = CMSG_BUFFER_LEN(cbuf);

    c = CMSG_FIRSTHDR(&msg);
    if (!t_check((BOOL)(c != NULL), "CMSG_FIRSTHDR found the first object", 0))
        return;

    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type  = IPV6_PKTINFO;
    c->cmsg_len   = CMSG_LEN(sizeof(struct in6_pktinfo));

    c = CMSG_NXTHDR(&msg, c);
    if (!t_check((BOOL)(c != NULL), "CMSG_NXTHDR found the second object", 0))
        return;

    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type  = IPV6_HOPLIMIT;
    c->cmsg_len   = CMSG_LEN(sizeof(LONG));

    (VOID)t_check((BOOL)(CMSG_NXTHDR(&msg, c) == NULL),
                  "CMSG_NXTHDR stops at the end of the buffer", 0);

    for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c))
        seen++;
    (VOID)t_check((BOOL)(seen == 2), "the loop walks exactly two objects",
                  (LONG)seen);

    /* RFC 3542 3.2's six macros. */
    ICMP6_FILTER_SETBLOCKALL(&filt);
    (VOID)t_check((BOOL)(ICMP6_FILTER_WILLBLOCK(128, &filt) &&
                         !ICMP6_FILTER_WILLPASS(128, &filt)),
                  "SETBLOCKALL blocks everything", 0);

    ICMP6_FILTER_SETPASS(129, &filt);
    (VOID)t_check((BOOL)(ICMP6_FILTER_WILLPASS(129, &filt) &&
                         ICMP6_FILTER_WILLBLOCK(128, &filt)),
                  "SETPASS passes one type and no other", 0);

    ICMP6_FILTER_SETPASSALL(&filt);
    ICMP6_FILTER_SETBLOCK(1, &filt);
    (VOID)t_check((BOOL)(ICMP6_FILTER_WILLBLOCK(1, &filt) &&
                         ICMP6_FILTER_WILLPASS(0, &filt) &&
                         ICMP6_FILTER_WILLPASS(255, &filt)),
                  "SETPASSALL then SETBLOCK blocks one type and no other", 0);
}

/* The options, through the ABI: set, read back, and refuse where they must. */
static VOID t_test_cmsg_options(VOID)
{
LONG                fd, raw;
LONG                rc;
LONG                value;
ULONG               len;
struct in6_pktinfo  info;
struct icmp6_filter filt;

    t_log("RFC 3542 socket options");

    fd = bsd_socket(T_AF_INET6, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "udp6 socket", bsd_Errno()))
        return;

    value = 0;
    len   = sizeof(value);
    rc = bsd_getsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &value, &len);
    (VOID)t_check((BOOL)(rc == 0 && value == 0),
                  "IPV6_RECVPKTINFO defaults to off", value);

    value = 1;
    rc = bsd_setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &value,
                        sizeof(value));
    (VOID)t_check((BOOL)(rc == 0), "setsockopt IPV6_RECVPKTINFO=1",
                  bsd_Errno());

    value = 0;
    len   = sizeof(value);
    (VOID)bsd_getsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &value, &len);
    (VOID)t_check((BOOL)(value == 1), "IPV6_RECVPKTINFO reads back as 1",
                  value);

    value = 1;
    rc = bsd_setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &value,
                        sizeof(value));
    (VOID)t_check((BOOL)(rc == 0), "setsockopt IPV6_RECVHOPLIMIT", bsd_Errno());

    value = 0;
    len   = sizeof(value);
    (VOID)bsd_getsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &value, &len);
    (VOID)t_check((BOOL)(value == 1), "IPV6_RECVHOPLIMIT reads back as 1",
                  value);

    /*
     * 49, 50, 51 and 52 are IPV6_HOPOPTS, IPV6_DSTOPTS, IPV6_RTHDR and
     * IPV6_PKTOPTIONS in the BSD numbering this header set follows, and this
     * library implements none of them.  They used to be taken as the Linux
     * spellings of the four options above, so a caller passing an option
     * buffer to IPV6_DSTOPTS had it read as a struct in6_pktinfo and the
     * socket's sticky source set from it.
     */
    {
        LONG optnum;

        for (optnum = 49; optnum <= 52; optnum++)
        {
            value = 1;
            rc = bsd_setsockopt(fd, IPPROTO_IPV6, optnum, &value,
                                sizeof(value));
            (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_ENOPROTOOPT),
                          "the Linux cmsg alias is refused", optnum);
        }

        t_bzero(&info, sizeof(info));
        len = sizeof(info);
        rc  = bsd_getsockopt(fd, IPPROTO_IPV6, 50, &info, &len);
        (VOID)t_check((BOOL)(rc < 0 && bsd_Errno() == T_ENOPROTOOPT),
                      "and reading 50 does not answer with a pktinfo",
                      bsd_Errno());
    }

    /* Sticky IPV6_PKTINFO: what goes in comes out. */
    t_bzero(&info, sizeof(info));
    info.ipi6_ifindex = 1;
    rc = bsd_setsockopt(fd, IPPROTO_IPV6, IPV6_PKTINFO, &info, sizeof(info));
    (VOID)t_check((BOOL)(rc == 0), "setsockopt IPV6_PKTINFO (sticky)",
                  bsd_Errno());

    t_bzero(&info, sizeof(info));
    len = sizeof(info);
    rc  = bsd_getsockopt(fd, IPPROTO_IPV6, IPV6_PKTINFO, &info, &len);
    (VOID)t_check((BOOL)(rc == 0 && len == sizeof(info) &&
                         info.ipi6_ifindex == 1),
                  "the sticky source reads back",
                  (LONG)info.ipi6_ifindex);

    /* All-zero clears it, per RFC 3542 6.6. */
    t_bzero(&info, sizeof(info));
    (VOID)bsd_setsockopt(fd, IPPROTO_IPV6, IPV6_PKTINFO, &info, sizeof(info));
    len = sizeof(info);
    (VOID)bsd_getsockopt(fd, IPPROTO_IPV6, IPV6_PKTINFO, &info, &len);
    (VOID)t_check((BOOL)(info.ipi6_ifindex == 0),
                  "an all-zero in6_pktinfo clears the sticky source",
                  (LONG)info.ipi6_ifindex);

    /* IPV6_HOPLIMIT is ancillary-only: it is not a sticky option. */
    value = 8;
    rc = bsd_setsockopt(fd, IPPROTO_IPV6, IPV6_HOPLIMIT, &value,
                        sizeof(value));
    (VOID)t_check((BOOL)(rc < 0), "setsockopt IPV6_HOPLIMIT is refused",
                  bsd_Errno());

    /* ICMP6_FILTER needs a raw ICMPv6 socket and nothing else. */
    len = sizeof(filt);
    rc  = bsd_getsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filt, &len);
    (VOID)t_check((BOOL)(rc < 0),
                  "ICMP6_FILTER is refused on a UDP socket", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);

    raw = bsd_socket(T_AF_INET6, T_SOCK_RAW, IPPROTO_ICMPV6);
    if (t_check((BOOL)(raw >= 0), "raw ICMPv6 socket", bsd_Errno()))
    {
        t_bzero(&filt, sizeof(filt));
        len = sizeof(filt);
        rc  = bsd_getsockopt(raw, IPPROTO_ICMPV6, ICMP6_FILTER, &filt, &len);
        (VOID)t_check((BOOL)(rc == 0 && len == sizeof(filt) &&
                             ICMP6_FILTER_WILLPASS(128, &filt) &&
                             ICMP6_FILTER_WILLPASS(1, &filt)),
                      "a new raw ICMPv6 socket passes every type", rc);

        ICMP6_FILTER_SETBLOCKALL(&filt);
        ICMP6_FILTER_SETPASS(129, &filt);
        rc = bsd_setsockopt(raw, IPPROTO_ICMPV6, ICMP6_FILTER, &filt,
                            sizeof(filt));
        (VOID)t_check((BOOL)(rc == 0), "setsockopt ICMP6_FILTER", bsd_Errno());

        t_bzero(&filt, sizeof(filt));
        len = sizeof(filt);
        (VOID)bsd_getsockopt(raw, IPPROTO_ICMPV6, ICMP6_FILTER, &filt, &len);
        (VOID)t_check((BOOL)(ICMP6_FILTER_WILLPASS(129, &filt) &&
                             ICMP6_FILTER_WILLBLOCK(128, &filt)),
                      "the installed filter reads back", 0);

        /* "In order to clear an installed filter the application can issue a
           setsockopt for ICMP6_FILTER with a zero length." */
        rc = bsd_setsockopt(raw, IPPROTO_ICMPV6, ICMP6_FILTER, &filt, 0);
        (VOID)t_check((BOOL)(rc == 0), "a zero-length setsockopt clears it",
                      bsd_Errno());

        len = sizeof(filt);
        (VOID)bsd_getsockopt(raw, IPPROTO_ICMPV6, ICMP6_FILTER, &filt, &len);
        (VOID)t_check((BOOL)(ICMP6_FILTER_WILLPASS(128, &filt)),
                      "and the default is back", 0);

        (VOID)bsd_CloseSocket(raw);
    }

    /* The IPv4 half, on an AF_INET socket. */
    fd = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (t_check((BOOL)(fd >= 0), "udp4 socket", bsd_Errno()))
    {
        value = 1;
        rc = bsd_setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &value, sizeof(value));
        (VOID)t_check((BOOL)(rc == 0), "setsockopt IP_PKTINFO=1", bsd_Errno());

        value = 1;
        rc = bsd_setsockopt(fd, IPPROTO_IP, IP_RECVDSTADDR, &value,
                            sizeof(value));
        (VOID)t_check((BOOL)(rc == 0), "setsockopt IP_RECVDSTADDR=1",
                      bsd_Errno());

        value = 0;
        len   = sizeof(value);
        (VOID)bsd_getsockopt(fd, IPPROTO_IP, IP_PKTINFO, &value, &len);
        (VOID)t_check((BOOL)(value == 1), "IP_PKTINFO reads back as 1", value);

        /* They are separate options, not two spellings of one. */
        value = 0;
        (VOID)bsd_setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &value,
                             sizeof(value));
        value = 0;
        len   = sizeof(value);
        (VOID)bsd_getsockopt(fd, IPPROTO_IP, IP_RECVDSTADDR, &value, &len);
        (VOID)t_check((BOOL)(value == 1),
                      "clearing IP_PKTINFO leaves IP_RECVDSTADDR alone",
                      value);

        (VOID)bsd_CloseSocket(fd);
    }
}

/*
 * The IPV6_HOPLIMIT of the next datagram on `fd`, or -1.  The socket must have
 * IPV6_RECVHOPLIMIT set; there is no timeout, so only call it when the send
 * that fills it said it went out.
 */
static LONG t_recv_hoplimit(LONG fd, char *buffer, LONG buflen)
{
CMSG_BUFFER(cbuf, T_CBUF_BYTES);
struct msghdr   msg;
struct iovec    iov;
struct cmsghdr *c;

    t_bzero(&cbuf, sizeof(cbuf));
    t_bzero(&msg, sizeof(msg));

    iov.iov_base       = buffer;
    iov.iov_len        = (ULONG)buflen;
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
    msg.msg_controllen = CMSG_BUFFER_LEN(cbuf);

    if (bsd_recvmsg(fd, &msg, 0) < 0)
        return -1;

    for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c))
    {
        LONG         hops = 0;
        const UBYTE *src  = CMSG_DATA(c);
        ULONG        i;

        if (c->cmsg_level != IPPROTO_IPV6 || c->cmsg_type != IPV6_HOPLIMIT)
            continue;

        for (i = 0; i < sizeof(hops); i++)
            ((UBYTE *)&hops)[i] = src[i];

        return hops;
    }

    return -1;
}

/* One datagram over ::1, with the ancillary data attached to it. */
static VOID t_test_cmsg_receive(VOID)
{
LONG                    server, client;
LONG                    rc;
LONG                    value;
struct t_sockaddr_in6   sa;
static const char       datagram[] = "with ancillary data";
char                    buffer[64];
CMSG_BUFFER(cbuf, T_CBUF_BYTES);
struct msghdr           msg;
struct iovec            iov;
struct cmsghdr         *c;
struct t_sockaddr_in6   peer;
BOOL                    saw_pktinfo = FALSE;
BOOL                    saw_hoplimit = FALSE;
ULONG                   arrived_on = 0;

    t_log("recvmsg with IPV6_RECVPKTINFO and IPV6_RECVHOPLIMIT");

    server = bsd_socket(T_AF_INET6, T_SOCK_DGRAM, 0);
    client = bsd_socket(T_AF_INET6, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && client >= 0), "udp sockets",
                 bsd_Errno()))
    {
        return;
    }

    t_make_any6(&sa, T_PORT + 3);
    rc = bsd_bind(server, &sa, sizeof(sa));
    if (!t_check((BOOL)(rc == 0), "udp server bind", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    value = 1;
    (VOID)bsd_setsockopt(server, IPPROTO_IPV6, IPV6_RECVPKTINFO, &value,
                         sizeof(value));
    value = 1;
    (VOID)bsd_setsockopt(server, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &value,
                         sizeof(value));

    t_make_loopback6(&sa, T_PORT + 3);
    rc = bsd_sendto(client, (APTR)datagram, sizeof(datagram), 0, &sa,
                    sizeof(sa));
    if (!t_check((BOOL)(rc == (LONG)sizeof(datagram)), "sendto ::1", rc))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    t_bzero(buffer, sizeof(buffer));
    t_bzero(&cbuf, sizeof(cbuf));
    t_bzero(&msg, sizeof(msg));
    t_bzero(&peer, sizeof(peer));

    iov.iov_base = buffer;
    iov.iov_len  = sizeof(buffer);

    msg.msg_name       = &peer;
    msg.msg_namelen    = sizeof(peer);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
    msg.msg_controllen = CMSG_BUFFER_LEN(cbuf);

    rc = bsd_recvmsg(server, &msg, 0);
    if (!t_check((BOOL)(rc == (LONG)sizeof(datagram)), "recvmsg", rc))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    (VOID)t_check(t_streq(buffer, datagram), "datagram survived", 0);
    (VOID)t_check((BOOL)((msg.msg_flags & MSG_CTRUNC) == 0),
                  "MSG_CTRUNC is clear, the buffer was big enough",
                  msg.msg_flags);
    (VOID)t_check((BOOL)(msg.msg_controllen > 0),
                  "msg_controllen is no longer always zero",
                  (LONG)msg.msg_controllen);

    for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c))
    {
        if (c->cmsg_level != IPPROTO_IPV6)
            continue;

        if (c->cmsg_type == IPV6_PKTINFO)
        {
            struct in6_pktinfo info;

            saw_pktinfo = TRUE;

            (VOID)t_check((BOOL)(c->cmsg_len ==
                                 CMSG_LEN(sizeof(struct in6_pktinfo))),
                          "IPV6_PKTINFO cmsg_len is CMSG_LEN(in6_pktinfo)",
                          (LONG)c->cmsg_len);

            /* Copied out rather than read in place: CMSG_DATA is only
               4-aligned and ipi6_ifindex is a ULONG. */
            {
                UBYTE       *dst = (UBYTE *)&info;
                const UBYTE *src = CMSG_DATA(c);
                ULONG        i;

                for (i = 0; i < sizeof(info); i++)
                    dst[i] = src[i];
            }

            (VOID)t_check((BOOL)(info.ipi6_addr.s6_addr[15] == 1),
                          "ipi6_addr is ::1, the address it was sent to",
                          info.ipi6_addr.s6_addr[15]);
            /*
             * Loopback has an index like anything else: NetX Duo parks it one
             * past the physical interfaces and this library numbers a slot by
             * slot + 1, so it is the last one and if_indextoname() names it.
             * A datagram off a real interface reports 1 or 2.
             */
            (VOID)t_check((BOOL)(info.ipi6_ifindex != 0),
                          "ipi6_ifindex names the arrival interface, loopback "
                          "included", (LONG)info.ipi6_ifindex);

            {
                char name[16];

                name[0] = '\0';
                (VOID)t_check((BOOL)(bsd_if_indextoname(info.ipi6_ifindex,
                                                        name) != NULL &&
                                     name[0] != '\0'),
                              "if_indextoname() resolves the index a PKTINFO "
                              "reported", (LONG)info.ipi6_ifindex);
                (VOID)t_check((BOOL)(bsd_if_nametoindex(name) ==
                                     info.ipi6_ifindex),
                              "and if_nametoindex() takes the name back",
                              (LONG)bsd_if_nametoindex(name));
            }

            arrived_on = info.ipi6_ifindex;
        }
        else if (c->cmsg_type == IPV6_HOPLIMIT)
        {
            LONG hops = 0;
            UBYTE       *dst = (UBYTE *)&hops;
            const UBYTE *src = CMSG_DATA(c);
            ULONG        i;

            saw_hoplimit = TRUE;

            for (i = 0; i < sizeof(hops); i++)
                dst[i] = src[i];

            (VOID)t_check((BOOL)(hops > 0 && hops <= 255),
                          "IPV6_HOPLIMIT is the arriving hop limit", hops);
        }
    }

    (VOID)t_check(saw_pktinfo, "an IPV6_PKTINFO object arrived", 0);
    (VOID)t_check(saw_hoplimit, "an IPV6_HOPLIMIT object arrived", 0);

    /*
     * The other half, and the reason the option exists: answer from the
     * address the query was sent to.  Over ::1 that is the address half of the
     * in6_pktinfo rather than the index half, which loopback does not have,
     * on a machine with two interfaces the index works the same way.
     */
    if (saw_pktinfo && peer.sin6_port != 0)
    {
        struct in6_pktinfo  reply;
        static const char   answer[] = "answered on the arrival interface";

        t_bzero(&cbuf, sizeof(cbuf));
        t_bzero(&msg, sizeof(msg));
        t_bzero(&reply, sizeof(reply));

        reply.ipi6_ifindex     = arrived_on;
        reply.ipi6_addr.s6_addr[15] = 1;        /* ::1 */

        iov.iov_base = (APTR)answer;
        iov.iov_len  = sizeof(answer);

        msg.msg_name       = &peer;
        msg.msg_namelen    = sizeof(peer);
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
        msg.msg_controllen = CMSG_SPACE(sizeof(reply));

        c = CMSG_FIRSTHDR(&msg);
        if (c != NULL)
        {
            UBYTE       *dst = CMSG_DATA(c);
            const UBYTE *src = (const UBYTE *)&reply;
            ULONG        i;

            c->cmsg_level = IPPROTO_IPV6;
            c->cmsg_type  = IPV6_PKTINFO;
            c->cmsg_len   = CMSG_LEN(sizeof(reply));

            for (i = 0; i < sizeof(reply); i++)
                dst[i] = src[i];

            rc = bsd_sendmsg(server, &msg, 0);

            /* The receive below has no timeout, so it is only reached when
               the send said the datagram went out. */
            if (t_check((BOOL)(rc == (LONG)sizeof(answer)),
                        "sendmsg with an IPV6_PKTINFO source", rc))
            {
                t_bzero(buffer, sizeof(buffer));
                rc = bsd_recv(client, buffer, sizeof(buffer), 0);
                (VOID)t_check((BOOL)(rc == (LONG)sizeof(answer) &&
                                     t_streq(buffer, answer)),
                              "the answer arrived", rc);
            }
        }

        /* An interface that does not exist is refused, not silently routed. */
        t_bzero(&cbuf, sizeof(cbuf));
        t_bzero(&msg, sizeof(msg));
        t_bzero(&reply, sizeof(reply));
        reply.ipi6_ifindex = 250;

        iov.iov_base       = (APTR)answer;
        iov.iov_len        = sizeof(answer);
        msg.msg_name       = &peer;
        msg.msg_namelen    = sizeof(peer);
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
        msg.msg_controllen = CMSG_SPACE(sizeof(reply));

        c = CMSG_FIRSTHDR(&msg);
        if (c != NULL)
        {
            UBYTE       *dst = CMSG_DATA(c);
            const UBYTE *src = (const UBYTE *)&reply;
            ULONG        i;

            c->cmsg_level = IPPROTO_IPV6;
            c->cmsg_type  = IPV6_PKTINFO;
            c->cmsg_len   = CMSG_LEN(sizeof(reply));

            for (i = 0; i < sizeof(reply); i++)
                dst[i] = src[i];

            rc = bsd_sendmsg(server, &msg, 0);
            (VOID)t_check((BOOL)(rc < 0),
                          "sendmsg refuses an interface index that has no "
                          "address", bsd_Errno());
        }

        /* Ancillary data this library does not implement is refused too. */
        t_bzero(&cbuf, sizeof(cbuf));
        t_bzero(&msg, sizeof(msg));
        iov.iov_base       = (APTR)answer;
        iov.iov_len        = sizeof(answer);
        msg.msg_name       = &peer;
        msg.msg_namelen    = sizeof(peer);
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
        msg.msg_controllen = CMSG_SPACE(sizeof(LONG));

        c = CMSG_FIRSTHDR(&msg);
        if (c != NULL)
        {
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type  = SCM_RIGHTS;
            c->cmsg_len   = CMSG_LEN(sizeof(LONG));

            rc = bsd_sendmsg(server, &msg, 0);
            (VOID)t_check((BOOL)(rc < 0),
                          "sendmsg refuses SCM_RIGHTS rather than ignoring it",
                          bsd_Errno());
        }

        /*
         * RFC 3542 6.3, both halves of it: the socket's own hop limit reaches
         * the wire, and a per-datagram one overrides it.  Neither used to,
         * IPV6_UNICAST_HOPS was stored and read back and never applied.
         */
        value = 1;
        (VOID)bsd_setsockopt(client, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &value,
                             sizeof(value));

        value = 33;
        rc = bsd_setsockopt(server, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &value,
                            sizeof(value));
        (VOID)t_check((BOOL)(rc == 0), "setsockopt IPV6_UNICAST_HOPS=33",
                      bsd_Errno());

        rc = bsd_sendto(server, (APTR)answer, sizeof(answer), 0, &peer,
                        sizeof(peer));
        if (t_check((BOOL)(rc == (LONG)sizeof(answer)),
                    "sendto with IPV6_UNICAST_HOPS set", rc))
        {
            (VOID)t_check((BOOL)(t_recv_hoplimit(client, buffer,
                                                 sizeof(buffer)) == 33),
                          "the hop limit on the wire is the socket's own", 33);
        }

        t_bzero(&cbuf, sizeof(cbuf));
        t_bzero(&msg, sizeof(msg));
        iov.iov_base       = (APTR)answer;
        iov.iov_len        = sizeof(answer);
        msg.msg_name       = &peer;
        msg.msg_namelen    = sizeof(peer);
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
        msg.msg_controllen = CMSG_SPACE(sizeof(LONG));

        c = CMSG_FIRSTHDR(&msg);
        if (c != NULL)
        {
            LONG  want = 7;
            UBYTE *dst = CMSG_DATA(c);
            ULONG  i;

            c->cmsg_level = IPPROTO_IPV6;
            c->cmsg_type  = IPV6_HOPLIMIT;
            c->cmsg_len   = CMSG_LEN(sizeof(want));

            for (i = 0; i < sizeof(want); i++)
                dst[i] = ((const UBYTE *)&want)[i];

            rc = bsd_sendmsg(server, &msg, 0);
            if (t_check((BOOL)(rc == (LONG)sizeof(answer)),
                        "sendmsg with an IPV6_HOPLIMIT is no longer refused",
                        rc))
            {
                (VOID)t_check((BOOL)(t_recv_hoplimit(client, buffer,
                                                     sizeof(buffer)) == 7),
                              "and the datagram carries that hop limit", 7);
            }

            /* Out of range is EINVAL; -1 is "the socket's own", not an error. */
            want = 256;
            for (i = 0; i < sizeof(want); i++)
                dst[i] = ((const UBYTE *)&want)[i];

            rc = bsd_sendmsg(server, &msg, 0);
            (VOID)t_check((BOOL)(rc < 0),
                          "sendmsg refuses a hop limit above 255",
                          bsd_Errno());

            want = -1;
            for (i = 0; i < sizeof(want); i++)
                dst[i] = ((const UBYTE *)&want)[i];

            rc = bsd_sendmsg(server, &msg, 0);
            if (t_check((BOOL)(rc == (LONG)sizeof(answer)),
                        "sendmsg takes -1 as the socket default", rc))
            {
                (VOID)t_check((BOOL)(t_recv_hoplimit(client, buffer,
                                                     sizeof(buffer)) == 33),
                              "and the datagram is back at the socket's 33",
                              33);
            }
        }
    }

    /* A control buffer too small for both is MSG_CTRUNC, not a failure. */
    t_make_loopback6(&sa, T_PORT + 3);
    rc = bsd_sendto(client, (APTR)datagram, sizeof(datagram), 0, &sa,
                    sizeof(sa));
    if (rc == (LONG)sizeof(datagram))
    {
        t_bzero(&msg, sizeof(msg));
        iov.iov_base       = buffer;
        iov.iov_len        = sizeof(buffer);
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
        msg.msg_controllen = CMSG_SPACE(sizeof(struct in6_pktinfo));

        rc = bsd_recvmsg(server, &msg, 0);
        (VOID)t_check((BOOL)(rc == (LONG)sizeof(datagram)),
                      "recvmsg with a short control buffer still delivers", rc);
        (VOID)t_check((BOOL)((msg.msg_flags & MSG_CTRUNC) != 0),
                      "and reports MSG_CTRUNC", msg.msg_flags);
    }

    /* No control storage is the shortest possible truncated buffer. */
    rc = bsd_sendto(client, (APTR)datagram, sizeof(datagram), 0, &sa,
                    sizeof(sa));
    if (rc == (LONG)sizeof(datagram))
    {
        t_bzero(&msg, sizeof(msg));
        iov.iov_base       = buffer;
        iov.iov_len        = sizeof(buffer);
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = NULL;
        msg.msg_controllen = 0;

        rc = bsd_recvmsg(server, &msg, 0);
        (VOID)t_check((BOOL)(rc == (LONG)sizeof(datagram)),
                      "recvmsg without control storage still delivers", rc);
        (VOID)t_check((BOOL)((msg.msg_flags & MSG_CTRUNC) != 0),
                      "and reports discarded control data", msg.msg_flags);
    }

    /* A non-NULL buffer shorter than one header has the same result. */
    rc = bsd_sendto(client, (APTR)datagram, sizeof(datagram), 0, &sa,
                    sizeof(sa));
    if (rc == (LONG)sizeof(datagram))
    {
        t_bzero(&msg, sizeof(msg));
        iov.iov_base       = buffer;
        iov.iov_len        = sizeof(buffer);
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
        msg.msg_controllen = sizeof(struct cmsghdr) - 1;

        rc = bsd_recvmsg(server, &msg, 0);
        (VOID)t_check((BOOL)(rc == (LONG)sizeof(datagram)),
                      "recvmsg with a sub-header control buffer delivers", rc);
        (VOID)t_check((BOOL)((msg.msg_flags & MSG_CTRUNC) != 0),
                      "and reports the truncated header", msg.msg_flags);
    }

    (VOID)bsd_CloseSocket(server);
    (VOID)bsd_CloseSocket(client);
}

/*
 * The IPv4 half over 127.0.0.1.  This is the case the whole thing was built
 * for: a UDP server that cannot tell which of its own addresses a query was
 * sent to answers from the wrong one.
 */
static VOID t_test_cmsg_receive4(VOID)
{
LONG                server, client;
LONG                rc;
LONG                value;
struct sockaddr_in  sa;
static const char   datagram[] = "over 127.0.0.1";
char                buffer[64];
CMSG_BUFFER(cbuf, T_CBUF_BYTES);
struct msghdr       msg;
struct iovec        iov;
struct cmsghdr     *c;
BOOL                saw_pktinfo = FALSE;
BOOL                saw_dstaddr = FALSE;

    t_log("recvmsg with IP_PKTINFO and IP_RECVDSTADDR");

    server = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    client = bsd_socket(T_AF_INET, T_SOCK_DGRAM, 0);
    if (!t_check((BOOL)(server >= 0 && client >= 0), "udp4 sockets",
                 bsd_Errno()))
    {
        return;
    }

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = T_PORT + 4;

    rc = bsd_bind(server, &sa, sizeof(sa));
    if (!t_check((BOOL)(rc == 0), "udp4 server bind", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    value = 1;
    (VOID)bsd_setsockopt(server, IPPROTO_IP, IP_PKTINFO, &value,
                         sizeof(value));
    value = 1;
    (VOID)bsd_setsockopt(server, IPPROTO_IP, IP_RECVDSTADDR, &value,
                         sizeof(value));

    sa.sin_addr.s_addr = 0x7F000001UL;          /* 127.0.0.1 */
    rc = bsd_sendto(client, (APTR)datagram, sizeof(datagram), 0, &sa,
                    sizeof(sa));
    if (!t_check((BOOL)(rc == (LONG)sizeof(datagram)), "sendto 127.0.0.1", rc))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    t_bzero(buffer, sizeof(buffer));
    t_bzero(&cbuf, sizeof(cbuf));
    t_bzero(&msg, sizeof(msg));

    iov.iov_base = buffer;
    iov.iov_len  = sizeof(buffer);

    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = CMSG_BUFFER_PTR(cbuf);
    msg.msg_controllen = CMSG_BUFFER_LEN(cbuf);

    rc = bsd_recvmsg(server, &msg, 0);
    if (!t_check((BOOL)(rc == (LONG)sizeof(datagram)), "recvmsg", rc))
    {
        (VOID)bsd_CloseSocket(server);
        (VOID)bsd_CloseSocket(client);
        return;
    }

    (VOID)t_check(t_streq(buffer, datagram), "datagram survived", 0);

    for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c))
    {
        const UBYTE *src = CMSG_DATA(c);
        ULONG        i;

        if (c->cmsg_level != IPPROTO_IP)
            continue;

        if (c->cmsg_type == IP_PKTINFO)
        {
            struct in_pktinfo info;
            UBYTE            *dst = (UBYTE *)&info;

            saw_pktinfo = TRUE;

            for (i = 0; i < sizeof(info); i++)
                dst[i] = src[i];

            (VOID)t_check((BOOL)(info.ipi_addr.s_addr == 0x7F000001UL),
                          "ipi_addr is the 127.0.0.1 it was sent to",
                          (LONG)info.ipi_addr.s_addr);
            (VOID)t_check((BOOL)(info.ipi_spec_dst.s_addr != 0UL),
                          "ipi_spec_dst is a local address",
                          (LONG)info.ipi_spec_dst.s_addr);
        }
        else if (c->cmsg_type == IP_RECVDSTADDR)
        {
            struct in_addr addr;
            UBYTE         *dst = (UBYTE *)&addr;

            saw_dstaddr = TRUE;

            for (i = 0; i < sizeof(addr); i++)
                dst[i] = src[i];

            (VOID)t_check((BOOL)(addr.s_addr == 0x7F000001UL),
                          "IP_RECVDSTADDR is 127.0.0.1", (LONG)addr.s_addr);
        }
    }

    (VOID)t_check(saw_pktinfo, "an IP_PKTINFO object arrived", 0);
    (VOID)t_check(saw_dstaddr,
                  "an IP_RECVDSTADDR object arrived alongside it", 0);

    (VOID)bsd_CloseSocket(server);
    (VOID)bsd_CloseSocket(client);
}

static VOID t_test_getaddrinfo(VOID)
{
struct t_addrinfo   hints;
struct t_addrinfo  *res = NULL;
LONG                rc;
char                text[64];
APTR                p;

    t_log("getaddrinfo / getnameinfo");

    /* A numeric IPv6 literal, no DNS involved. */
    t_bzero(&hints, sizeof(hints));
    hints.ai_family   = T_AF_INET6;
    hints.ai_socktype = T_SOCK_STREAM;
    hints.ai_flags    = T_AI_NUMERICHOST;

    rc = bsd_getaddrinfo((APTR)"2001:db8::1", (APTR)"80", &hints, &res);
    if (t_check((BOOL)(rc == 0 && res != NULL),
                "getaddrinfo(2001:db8::1, 80, AF_INET6)", rc))
    {
        struct t_sockaddr_in6 *sa = (struct t_sockaddr_in6 *)res->ai_addr;

        (VOID)t_check((BOOL)(res->ai_family == T_AF_INET6),
                      "ai_family is AF_INET6", res->ai_family);
        (VOID)t_check((BOOL)(res->ai_addrlen == 28),
                      "ai_addrlen is 28", (LONG)res->ai_addrlen);
        (VOID)t_check((BOOL)(sa->sin6_family == T_AF_INET6 &&
                             sa->sin6_port == 80 &&
                             sa->sin6_addr.s6_addr[0] == 0x20),
                      "ai_addr is a filled-in sockaddr_in6",
                      (LONG)sa->sin6_port);

        bsd_freeaddrinfo(res);
        res = NULL;
    }

    /* 4294967297 is 1 modulo 2^32. It is not interface index 1 and must not
       be silently accepted as that scope on 32-bit AmigaOS. */
    rc = bsd_getaddrinfo((APTR)"fe80::1%4294967297", NULL, &hints, &res);
    (VOID)t_check((BOOL)(rc == -2 && res == NULL),
                  "getaddrinfo rejects an overflowing numeric scope", rc);

    /*
     * AF_UNSPEC with AI_PASSIVE: the documented order is IPv6 first, then
     * IPv4, and both are the wildcard address.
     */
    t_bzero(&hints, sizeof(hints));
    hints.ai_family   = T_AF_UNSPEC;
    hints.ai_socktype = T_SOCK_STREAM;
    hints.ai_flags    = T_AI_PASSIVE;

    rc = bsd_getaddrinfo(NULL, (APTR)"80", &hints, &res);
    if (t_check((BOOL)(rc == 0 && res != NULL),
                "getaddrinfo(NULL, 80, AF_UNSPEC|AI_PASSIVE)", rc))
    {
        (VOID)t_check((BOOL)(res->ai_family == T_AF_INET6),
                      "the FIRST AF_UNSPEC result is IPv6", res->ai_family);
        (VOID)t_check((BOOL)(res->ai_next != NULL &&
                             res->ai_next->ai_family == T_AF_INET),
                      "the second is IPv4",
                      (res->ai_next != NULL) ? res->ai_next->ai_family : -1);

        bsd_freeaddrinfo(res);
        res = NULL;
    }

    /* A dotted quad with an AF_INET6 hint: EAI_ADDRFAMILY (-9), because this
       NDK has no AI_V4MAPPED for the caller to have asked with. */
    t_bzero(&hints, sizeof(hints));
    hints.ai_family = T_AF_INET6;
    hints.ai_flags  = T_AI_NUMERICHOST;
    rc = bsd_getaddrinfo((APTR)"10.0.2.15", NULL, &hints, &res);
    (VOID)t_check((BOOL)(rc == -9),
                  "a dotted quad with AF_INET6 is EAI_ADDRFAMILY", rc);

    /* The mirror of it. Without AI_NUMERICHOST, so a wrong answer here means
       the literal reached the resolver and was looked up as a name. */
    t_bzero(&hints, sizeof(hints));
    hints.ai_family = T_AF_INET;
    rc = bsd_getaddrinfo((APTR)"::1", NULL, &hints, &res);
    (VOID)t_check((BOOL)(rc == -9),
                  "an IPv6 literal with AF_INET is EAI_ADDRFAMILY", rc);

    /* gai_strerror takes its argument in a0. If that were wrong this would
       return the wrong string, or garbage. */
    p = bsd_gai_strerror(-2);
    (VOID)t_check((BOOL)(p != NULL &&
                         t_streq((const char *)p,
                                 "name or service is not known")),
                  "gai_strerror(EAI_NONAME), argument really is in a0", 0);

    /* getnameinfo on a sockaddr_in6, numeric. */
    {
        struct t_sockaddr_in6 sa;

        t_make_loopback6(&sa, 80);
        t_bzero(text, sizeof(text));

        rc = bsd_getnameinfo(&sa, sizeof(sa), text, sizeof(text), NULL, 0,
                             1UL /* NI_NUMERICHOST */);
        (VOID)t_check((BOOL)(rc == 0 && t_streq(text, "::1")),
                      "getnameinfo(::1, NI_NUMERICHOST)", rc);
        t_log("  getnameinfo said \"%s\"", text);

        /* NI_NAMEREQD applies to the host half only. 65000 is absent from the
           test system's services file, and the decimal form is what every
           other stack answers. */
        t_make_loopback6(&sa, 65000);
        t_bzero(text, sizeof(text));
        rc = bsd_getnameinfo(&sa, sizeof(sa), NULL, 0,
                             text, sizeof(text), 8UL /* NI_NAMEREQD */);
        (VOID)t_check((BOOL)(rc == 0 && t_streq(text, "65000")),
                      "NI_NAMEREQD does not require a service name", rc);

        rc = bsd_getnameinfo(&sa, sizeof(sa), text, sizeof(text), NULL, 0,
                             0x80000000UL);
        (VOID)t_check((BOOL)(rc == -1),
                      "getnameinfo rejects undefined flags", rc);
    }

    /* NI_WITHSCOPEID is the NDK's opt-in for the KAME "%zone" suffix. */
    {
        struct t_sockaddr_in6 sa;
        ULONG                 scope;

        t_bzero(&sa, sizeof(sa));
        sa.sin6_family = T_AF_INET6;
        sa.sin6_addr.s6_addr[0]  = 0xfe;
        sa.sin6_addr.s6_addr[1]  = 0x80;
        sa.sin6_addr.s6_addr[15] = 1;
        scope = bsd_if_nametoindex("lo0");
        sa.sin6_scope_id = scope;

        (VOID)t_check((BOOL)(scope != 0),
                      "if_nametoindex(lo0) for getnameinfo scope", scope);

        t_bzero(text, sizeof(text));
        rc = bsd_getnameinfo(&sa, sizeof(sa), text, sizeof(text), NULL, 0,
                             1UL /* NI_NUMERICHOST */);
        (VOID)t_check((BOOL)(rc == 0 && t_streq(text, "fe80::1")),
                      "getnameinfo omits scope without NI_WITHSCOPEID", rc);

        t_bzero(text, sizeof(text));
        rc = bsd_getnameinfo(&sa, sizeof(sa), text, sizeof(text), NULL, 0,
                             1UL | 32UL /* NI_NUMERICHOST|NI_WITHSCOPEID */);
        (VOID)t_check((BOOL)(rc == 0 && t_streq(text, "fe80::1%lo0")),
                      "getnameinfo appends scope with NI_WITHSCOPEID", rc);
    }
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{
    t_log("AmiNetXDuo, AF_INET6 through bsdsocket.library");

    /*
     * An interface, without needing anyone's driver.
     *
     * Every check below talks over ::1 and nothing else, but the library will
     * not bring a stack up with no interface to put it on, so this used to
     * need Commodore's a2065.device, which is not ours to ship and which
     * therefore pinned the whole test to the one CI runner that had a copy.
     *
     * tests/tcpdrill/tapdev.c is a SANA-II device made at run time with
     * MakeLibrary()/AddDevice(), so OpenDevice() finds it in ExecBase's list
     * and never looks in DEVS:. src/sana2/ opens, configures and runs its
     * readers against it through exactly the same code as a real card.
     *
     * It has to be installed by THIS process: the device lives in the
     * installer's address space and dies with it, so a launcher cannot do it.
     * That is also why tapdev.c is linked here rather than the test staying a
     * single translation unit, it takes nothing from this tree either, only
     * NDK headers, so what the test itself uses of ours is still just
     * <aminetxduo/cmsg.h>.
     */
    {
        static const UBYTE tap_mac[6] = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x06 };

        if (tap_install(tap_mac) != 0)
        {
            Printf((STRPTR)"cannot install the test interface\n");
            return(20);
        }
    }

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!t_check((BOOL)(SocketBase != NULL), "OpenLibrary(bsdsocket.library, 4)",
                 0))
    {
        Printf((STRPTR)"bsdsocket.library not available\n");
        return(20);
    }

    t_test_conversions();
    t_test_socket_basics();
    t_test_tcp_loopback();
    t_test_tcp_oob_event_consumption();
    t_test_tcp_listener_family();
    t_test_tcp_accepted_local();
    t_test_udp_loopback();
    t_test_udp_bound_address();
    t_test_udp_connected_readiness();
    t_test_udp_peek_fionread();
    t_test_udp_reconnect_after_peek();
    t_test_udp_disconnect();
    t_test_udp_icmp_readiness();
    t_test_udp_so_error_consumes_icmp();
    t_test_udp_so_error_clears_exception();
    t_test_waitselect_counts_ready_bits();
    t_test_datagram_shutdown();
    t_test_shutdown_fionread();
    t_test_raw_bound_address();
    t_test_raw_bind_after_peek();
    t_test_raw_connect_after_peek();
    t_test_cmsg_macros();
    t_test_cmsg_options();
    t_test_cmsg_receive();
    t_test_cmsg_receive4();
    t_test_getaddrinfo();

    CloseLibrary(SocketBase);
    SocketBase = NULL;

    t_log("");
    t_log("%ld checks, %ld failures, %s", t_checks, t_failures,
          (t_failures == 0UL) ? "PASS" : "FAIL");

    t_flush();

    return((t_failures == 0UL) ? 0 : 20);
}
