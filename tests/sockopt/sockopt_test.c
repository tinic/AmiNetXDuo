/*
 * AmiNetXDuo, the socket option surface, through bsdsocket.library's ABI.
 *
 * setsockopt() and getsockopt() are the part of the library a caller cannot
 * check for itself: an option that is stored, answered back and applied to
 * nothing looks exactly like one that works.  Nothing else in the tree touched
 * SO_RCVBUF, SO_SNDBUF, SO_LINGER, SO_OOBINLINE, TCP_MAXSEG, TCP_NODELAY,
 * IP_TTL, IP_TOS, SIOCATMARK, FIOASYNC, the SIOCGIF* queries or the multicast
 * width paths, and that is where every finding of the audit landed.
 *
 * So each option below has a case, including the ones this library
 * deliberately does not honour, SO_REUSEADDR and SO_BROADCAST are stored and
 * answered and change nothing, and a test that says so is what stops that
 * being rediscovered as a defect.
 *
 * An ordinary AmigaOS program: OpenLibrary("bsdsocket.library") and calls
 * through the LVO table, linked against none of our code, exactly as
 * tests/ipv6/ipv6_socket_test.c and tests/libraries/library_test.c are.  The
 * interface comes from tests/tcpdrill/tapdev.c, a SANA-II device made at run
 * time, so this needs nobody's driver and runs wherever tier 2 runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>

#include "tapdev.h"


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

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
    RawDoFmt((STRPTR)fmt, args, (VOID (*)())t_put_char, NULL);
    va_end(args);

    RawPutChar('\n');
    if (t_log_used < (ULONG)(T_LOG_SIZE - 1))
    {
        t_log_buffer[t_log_used++] = '\n';
    }
}

static VOID t_flush(VOID)
{
BPTR out;

    out = Output();
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

static VOID t_bzero(APTR p, ULONG len)
{
UBYTE *b = (UBYTE *)p;

    while (len-- > 0UL)
    {
        *b++ = 0;
    }
}


/* -------------------------------------------------------------- the ABI -- */

/*
 * Taken from the NDK rather than written out.  ipv6_socket_test.c restates its
 * structures because their LAYOUT is what it checks; here the numbers are not
 * under test, the behaviour behind them is, and a hand-copied IP_TTL that is
 * really IP_TOS would test the wrong option and pass.
 */
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <net/if.h>

#define T_EFAULT                14
#define T_EINVAL                22
#define T_ENOTTY                25
#define T_ENOPROTOOPT           42

/* NX_IP_TIME_TO_LIVE, which socket.c gives every socket. */
#define T_DEFAULT_TTL           128

/* ------------------------------------------------------------ LVO stubs --- */

/*
 * d0, d1, a0 and a1 are scratch across a library call and GCC has to be told
 * so, or it keeps a "still valid" copy and reads the library's leftovers.  The
 * NDK's own inline headers do exactly this.
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
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
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
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
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
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1)
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
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
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
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
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
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (a1)
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

static LONG bsd_CloseSocket(LONG fd)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            d0  __asm("d0") = fd;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-120:W)"
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_Errno(VOID)
{
register struct Library *a6  __asm("a6") = SocketBase;
register LONG            res __asm("d0");
BSD_SCRATCH;

    __asm __volatile ("jsr a6@(-162:W)"
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6)
                      : "cc", "memory");
    return(res);
}


/* --------------------------------------------------------------- helpers -- */

static LONG t_set_int(LONG fd, LONG level, LONG name, LONG value)
{
    return(bsd_setsockopt(fd, level, name, &value, (LONG)sizeof(value)));
}

/* -1 when getsockopt failed, so a caller can tell it apart from a zero. */
static LONG t_get_int(LONG fd, LONG level, LONG name, LONG *out)
{
LONG        value = 0;
socklen_t   len   = (socklen_t)sizeof(value);
LONG        rc;

    rc = bsd_getsockopt(fd, level, name, &value, &len);
    if (out != NULL)
    {
        *out = value;
    }

    return(rc);
}

/* setsockopt failed with this errno. */
static BOOL t_set_fails(LONG fd, LONG level, LONG name, LONG value, LONG err)
{
LONG rc = t_set_int(fd, level, name, value);

    return((BOOL)(rc < 0 && bsd_Errno() == err));
}


/* --------------------------------------------------------- SOL_SOCKET --- */

/*
 * The three that are stored, answered and honoured by nothing.  Pinned rather
 * than left implicit: each has a comment in options.c saying why it cannot be
 * implemented, and a caller reading back what it set has to keep working.
 */
static VOID t_test_accepted_and_ignored(VOID)
{
LONG fd;
LONG value;

    t_log("options accepted and deliberately not honoured");

    fd = bsd_socket(AF_INET, SOCK_STREAM, 0);
    if (!t_check((BOOL)(fd >= 0), "tcp socket", bsd_Errno()))
        return;

    /* SO_REUSEADDR: NetX Duo's port table is exclusive and has no override. */
    (VOID)t_check((BOOL)(t_set_int(fd, SOL_SOCKET, SO_REUSEADDR, 1) == 0),
                  "setsockopt SO_REUSEADDR=1", bsd_Errno());
    (VOID)t_get_int(fd, SOL_SOCKET, SO_REUSEADDR, &value);
    (VOID)t_check((BOOL)(value == 1), "SO_REUSEADDR reads back 1", value);

    (VOID)t_set_int(fd, SOL_SOCKET, SO_REUSEADDR, 0);
    (VOID)t_get_int(fd, SOL_SOCKET, SO_REUSEADDR, &value);
    (VOID)t_check((BOOL)(value == 0), "and clears again", value);

    /* SO_REUSEPORT is the same option word here. */
    (VOID)t_set_int(fd, SOL_SOCKET, SO_REUSEPORT, 1);
    (VOID)t_get_int(fd, SOL_SOCKET, SO_REUSEADDR, &value);
    (VOID)t_check((BOOL)(value == 1),
                  "SO_REUSEPORT is SO_REUSEADDR under another name", value);

    /* SO_BROADCAST: this stack never asks permission to broadcast. */
    (VOID)t_check((BOOL)(t_set_int(fd, SOL_SOCKET, SO_BROADCAST, 1) == 0),
                  "setsockopt SO_BROADCAST=1", bsd_Errno());
    (VOID)t_get_int(fd, SOL_SOCKET, SO_BROADCAST, &value);
    (VOID)t_check((BOOL)(value == 1), "SO_BROADCAST reads back 1", value);

    /* SO_KEEPALIVE: stored, and applied only in a keepalive build. */
    (VOID)t_check((BOOL)(t_set_int(fd, SOL_SOCKET, SO_KEEPALIVE, 1) == 0),
                  "setsockopt SO_KEEPALIVE=1", bsd_Errno());
    (VOID)t_get_int(fd, SOL_SOCKET, SO_KEEPALIVE, &value);
    (VOID)t_check((BOOL)(value == 1), "SO_KEEPALIVE reads back 1", value);

    /*
     * SO_OOBINLINE is the one of the four that answers the truth instead:
     * oob.c delivers the urgent byte in the stream whatever this says, so a 0
     * that was set reads back as 1.
     */
    (VOID)t_check((BOOL)(t_set_int(fd, SOL_SOCKET, SO_OOBINLINE, 0) == 0),
                  "setsockopt SO_OOBINLINE=0", bsd_Errno());
    (VOID)t_get_int(fd, SOL_SOCKET, SO_OOBINLINE, &value);
    (VOID)t_check((BOOL)(value == 1),
                  "SO_OOBINLINE reads back 1 -- always in force", value);

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_test_buffers(VOID)
{
LONG fd;
LONG value;

    t_log("SO_RCVBUF / SO_SNDBUF");

    fd = bsd_socket(AF_INET, SOCK_STREAM, 0);
    if (!t_check((BOOL)(fd >= 0), "tcp socket", bsd_Errno()))
        return;

    (VOID)t_check((BOOL)(t_set_int(fd, SOL_SOCKET, SO_RCVBUF,
                                   32768) == 0),
                  "setsockopt SO_RCVBUF=32768", bsd_Errno());
    (VOID)t_get_int(fd, SOL_SOCKET, SO_RCVBUF, &value);
    (VOID)t_check((BOOL)(value >= 32768), "SO_RCVBUF answers at least that",
                  value);

    (VOID)t_check((BOOL)(t_set_int(fd, SOL_SOCKET, SO_SNDBUF,
                                   32768) == 0),
                  "setsockopt SO_SNDBUF=32768", bsd_Errno());
    (VOID)t_get_int(fd, SOL_SOCKET, SO_SNDBUF, &value);
    (VOID)t_check((BOOL)(value >= 32768), "SO_SNDBUF answers at least that",
                  value);

    (VOID)t_check(t_set_fails(fd, SOL_SOCKET, SO_RCVBUF, -1, T_EINVAL),
                  "a negative SO_RCVBUF is EINVAL", bsd_Errno());
    (VOID)t_check(t_set_fails(fd, SOL_SOCKET, SO_SNDBUF, -1, T_EINVAL),
                  "a negative SO_SNDBUF is EINVAL", bsd_Errno());

    /* An unset socket answers with the window it actually got. */
    (VOID)bsd_CloseSocket(fd);

    fd = bsd_socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0)
    {
        (VOID)t_get_int(fd, SOL_SOCKET, SO_RCVBUF, &value);
        (VOID)t_check((BOOL)(value > 0),
                      "an untouched SO_RCVBUF reports the socket's window",
                      value);
        (VOID)bsd_CloseSocket(fd);
    }
}

static VOID t_test_linger(VOID)
{
LONG            fd;
struct linger lin;
socklen_t       len;

    t_log("SO_LINGER");

    fd = bsd_socket(AF_INET, SOCK_STREAM, 0);
    if (!t_check((BOOL)(fd >= 0), "tcp socket", bsd_Errno()))
        return;

    lin.l_onoff  = 1;
    lin.l_linger = 5;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin,
                                        (LONG)sizeof(lin)) == 0),
                  "setsockopt SO_LINGER on, 5s", bsd_Errno());

    t_bzero(&lin, sizeof(lin));
    len = (socklen_t)sizeof(lin);
    (VOID)bsd_getsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, &len);
    (VOID)t_check((BOOL)(lin.l_onoff != 0 && lin.l_linger == 5 &&
                         len == (socklen_t)sizeof(lin)),
                  "SO_LINGER reads back", lin.l_linger);

    /*
     * A negative l_linger became a tick count of about 497 days in
     * bsd_socket_close(), so CloseSocket() would never return.
     */
    lin.l_onoff  = 1;
    lin.l_linger = -1;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin,
                                        (LONG)sizeof(lin)) < 0 &&
                         bsd_Errno() == T_EINVAL),
                  "a negative l_linger is EINVAL", bsd_Errno());

    lin.l_onoff  = 1;
    lin.l_linger = 100000;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin,
                                        (LONG)sizeof(lin)) < 0 &&
                         bsd_Errno() == T_EINVAL),
                  "and so is one past the 4.4BSD bound", bsd_Errno());

    /* The refused calls must not have replaced the accepted one. */
    t_bzero(&lin, sizeof(lin));
    len = (socklen_t)sizeof(lin);
    (VOID)bsd_getsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, &len);
    (VOID)t_check((BOOL)(lin.l_linger == 5),
                  "a refused SO_LINGER leaves the old one", lin.l_linger);

    /* Too short a buffer is EINVAL in both directions. */
    len = 2;
    (VOID)t_check((BOOL)(bsd_getsockopt(fd, SOL_SOCKET, SO_LINGER, &lin,
                                        &len) < 0 &&
                         bsd_Errno() == T_EINVAL),
                  "getsockopt SO_LINGER into two bytes is EINVAL", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);
}

/*
 * SO_ERROR clears on read, and used to clear on a read that failed: the zero
 * went in before the copy-out, so a bad optval returned EFAULT and destroyed
 * the pending error.  A non-blocking connect() caller has no other way to
 * learn why it failed.
 */
static VOID t_test_so_error(VOID)
{
LONG                 fd;
LONG                 value;
socklen_t            len;
struct sockaddr_in sa;
LONG                 one = 1;
LONG                 pending;

    t_log("SO_ERROR");

    fd = bsd_socket(AF_INET, SOCK_STREAM, 0);
    if (!t_check((BOOL)(fd >= 0), "tcp socket", bsd_Errno()))
        return;

    (VOID)t_get_int(fd, SOL_SOCKET, SO_ERROR, &value);
    (VOID)t_check((BOOL)(value == 0), "a fresh socket has no pending error",
                  value);

    /* Nothing is listening on 127.0.0.1:1, so the dial fails. */
    (VOID)bsd_IoctlSocket(fd, FIONBIO, &one);

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = 1;
    sa.sin_addr.s_addr = 0x7F000001UL;

    (VOID)bsd_connect(fd, &sa, (LONG)sizeof(sa));

    /* Give the stack a moment to refuse it. */
    Delay(25);

    /* A copy-out that cannot fit must fail without taking the error with it. */
    len = 1;
    (VOID)t_check((BOOL)(bsd_getsockopt(fd, SOL_SOCKET, SO_ERROR, &value,
                                        &len) < 0),
                  "getsockopt SO_ERROR into one byte fails", bsd_Errno());

    (VOID)t_check((BOOL)(bsd_getsockopt(fd, SOL_SOCKET, SO_ERROR, NULL,
                                        NULL) < 0 &&
                         bsd_Errno() == T_EFAULT),
                  "and a NULL optval is EFAULT", bsd_Errno());

    pending = -1;
    (VOID)t_get_int(fd, SOL_SOCKET, SO_ERROR, &pending);
    (VOID)t_check((BOOL)(pending != 0),
                  "the pending error survived both failed reads", pending);

    /* And the successful read is the one that clears it. */
    (VOID)t_get_int(fd, SOL_SOCKET, SO_ERROR, &value);
    (VOID)t_check((BOOL)(value == 0), "SO_ERROR clears on a read that worked",
                  value);

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_test_type_and_timeouts(VOID)
{
LONG              fd;
LONG              value;
struct timeval  tv;
socklen_t         len;
struct sockaddr_in sa;

    t_log("SO_TYPE, SO_ACCEPTCONN, SO_RCVTIMEO, SO_SNDTIMEO, SO_EVENTMASK");

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "udp socket", bsd_Errno()))
        return;

    (VOID)t_get_int(fd, SOL_SOCKET, SO_TYPE, &value);
    (VOID)t_check((BOOL)(value == SOCK_DGRAM), "SO_TYPE says SOCK_DGRAM",
                  value);

    tv.tv_secs  = 3;
    tv.tv_micro = 500000;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
                                        (LONG)sizeof(tv)) == 0),
                  "setsockopt SO_RCVTIMEO 3.5s", bsd_Errno());

    t_bzero(&tv, sizeof(tv));
    len = (socklen_t)sizeof(tv);
    (VOID)bsd_getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, &len);
    (VOID)t_check((BOOL)(tv.tv_secs == 3), "SO_RCVTIMEO reads back 3s",
                  (LONG)tv.tv_secs);

    tv.tv_secs  = 1;
    tv.tv_micro = 0;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
                                        (LONG)sizeof(tv)) == 0),
                  "setsockopt SO_SNDTIMEO 1s", bsd_Errno());

    (VOID)t_check((BOOL)(t_set_int(fd, SOL_SOCKET, SO_EVENTMASK,
                                   0x0001) == 0),
                  "setsockopt SO_EVENTMASK", bsd_Errno());
    (VOID)t_get_int(fd, SOL_SOCKET, SO_EVENTMASK, &value);
    (VOID)t_check((BOOL)(value == 0x0001), "SO_EVENTMASK reads back", value);

    (VOID)t_check((BOOL)(t_get_int(fd, SOL_SOCKET, 0x7FFF, &value) < 0 &&
                         bsd_Errno() == T_ENOPROTOOPT),
                  "an unknown SOL_SOCKET option is ENOPROTOOPT", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);

    /* SO_ACCEPTCONN before and after listen(). */
    fd = bsd_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;

    (VOID)t_get_int(fd, SOL_SOCKET, SO_ACCEPTCONN, &value);
    (VOID)t_check((BOOL)(value == 0), "SO_ACCEPTCONN is 0 before listen()",
                  value);

    t_bzero(&sa, sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = 9401;

    if (bsd_bind(fd, &sa, (LONG)sizeof(sa)) == 0 &&
        bsd_listen(fd, 1) == 0)
    {
        (VOID)t_get_int(fd, SOL_SOCKET, SO_ACCEPTCONN, &value);
        (VOID)t_check((BOOL)(value == 1), "and 1 after it", value);
    }

    (VOID)bsd_CloseSocket(fd);
}


/* -------------------------------------------------------- IPPROTO_TCP --- */

static VOID t_test_tcp_options(VOID)
{
LONG fd;
LONG value;

    t_log("TCP_NODELAY, TCP_MAXSEG");

    fd = bsd_socket(AF_INET, SOCK_STREAM, 0);
    if (!t_check((BOOL)(fd >= 0), "tcp socket", bsd_Errno()))
        return;

    /* NetX Duo has no Nagle, so this is on and cannot be turned off. */
    (VOID)t_check((BOOL)(t_set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1) == 0),
                  "setsockopt TCP_NODELAY=1", bsd_Errno());
    (VOID)t_get_int(fd, IPPROTO_TCP, TCP_NODELAY, &value);
    (VOID)t_check((BOOL)(value == 1), "TCP_NODELAY reads back 1", value);

    (VOID)t_set_int(fd, IPPROTO_TCP, TCP_NODELAY, 0);
    (VOID)t_get_int(fd, IPPROTO_TCP, TCP_NODELAY, &value);
    (VOID)t_check((BOOL)(value == 1),
                  "and still 1 after asking for Nagle -- there is none",
                  value);

    (VOID)t_check((BOOL)(bsd_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                                        NULL, 4) < 0 &&
                         bsd_Errno() == T_EFAULT),
                  "TCP_NODELAY with a NULL optval is EFAULT", bsd_Errno());

    /* TCP_MAXSEG before the connection: NetX Duo takes it. */
    (VOID)t_check((BOOL)(t_set_int(fd, IPPROTO_TCP, TCP_MAXSEG,
                                   1200) == 0),
                  "setsockopt TCP_MAXSEG=1200", bsd_Errno());
    (VOID)t_get_int(fd, IPPROTO_TCP, TCP_MAXSEG, &value);
    (VOID)t_check((BOOL)(value == 1200), "TCP_MAXSEG reads back 1200", value);

    /* A negative went in as a four-billion MSS and was stored. */
    (VOID)t_check(t_set_fails(fd, IPPROTO_TCP, TCP_MAXSEG, -1, T_EINVAL),
                  "a negative TCP_MAXSEG is EINVAL", bsd_Errno());
    (VOID)t_check(t_set_fails(fd, IPPROTO_TCP, TCP_MAXSEG, 0, T_EINVAL),
                  "and zero is EINVAL", bsd_Errno());
    (VOID)t_check(t_set_fails(fd, IPPROTO_TCP, TCP_MAXSEG, 70000,
                              T_EINVAL),
                  "and one past a maximum datagram is EINVAL", bsd_Errno());

    (VOID)t_get_int(fd, IPPROTO_TCP, TCP_MAXSEG, &value);
    (VOID)t_check((BOOL)(value == 1200),
                  "a refused TCP_MAXSEG leaves the old one", value);

    (VOID)t_check((BOOL)(t_get_int(fd, IPPROTO_TCP, 0x7FFF, &value) < 0 &&
                         bsd_Errno() == T_ENOPROTOOPT),
                  "an unknown IPPROTO_TCP option is ENOPROTOOPT", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);

    /* Level IPPROTO_TCP on a UDP socket answers for neither option. */
    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    (VOID)t_check(t_set_fails(fd, IPPROTO_TCP, TCP_NODELAY, 1,
                              T_ENOPROTOOPT),
                  "TCP_NODELAY on a UDP socket is ENOPROTOOPT", bsd_Errno());
    (VOID)t_check(t_set_fails(fd, IPPROTO_TCP, TCP_MAXSEG, 1200,
                              T_ENOPROTOOPT),
                  "TCP_MAXSEG on a UDP socket is ENOPROTOOPT", bsd_Errno());

    /* And reading them there, which used to answer 1 and 0 with success. */
    (VOID)t_check((BOOL)(t_get_int(fd, IPPROTO_TCP, TCP_NODELAY, &value) < 0 &&
                         bsd_Errno() == T_ENOPROTOOPT),
                  "getsockopt TCP_NODELAY on a UDP socket is ENOPROTOOPT",
                  bsd_Errno());
    (VOID)t_check((BOOL)(t_get_int(fd, IPPROTO_TCP, TCP_MAXSEG, &value) < 0 &&
                         bsd_Errno() == T_ENOPROTOOPT),
                  "getsockopt TCP_MAXSEG on a UDP socket is ENOPROTOOPT",
                  bsd_Errno());

    /* IP_HDRINCL is a raw-socket option and is refused in both directions. */
    (VOID)t_check(t_set_fails(fd, IPPROTO_IP, IP_HDRINCL, 1, T_ENOPROTOOPT),
                  "setsockopt IP_HDRINCL off a raw socket is ENOPROTOOPT",
                  bsd_Errno());
    (VOID)t_check((BOOL)(t_get_int(fd, IPPROTO_IP, IP_HDRINCL, &value) < 0 &&
                         bsd_Errno() == T_ENOPROTOOPT),
                  "and so is reading it", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);
}


/* --------------------------------------------------------- IPPROTO_IP --- */

/*
 * IP_TTL 256 read back as 256 and went on the wire as zero, so every packet
 * was dropped at the first hop.  Both options are also applied to the NX
 * socket now, which getsockopt cannot see, what it can see is that the
 * out-of-range value is refused and the in-range one survives.
 */
static VOID t_test_ip_options(LONG type, const char *what)
{
LONG fd;
LONG value;

    t_log("IP_TTL, IP_TOS on a %s socket", what);

    fd = bsd_socket(AF_INET, type, 0);
    if (!t_check((BOOL)(fd >= 0), "socket", bsd_Errno()))
        return;

    (VOID)t_get_int(fd, IPPROTO_IP, IP_TTL, &value);
    (VOID)t_check((BOOL)(value == T_DEFAULT_TTL),
                  "IP_TTL starts at the stack default", value);

    (VOID)t_check((BOOL)(t_set_int(fd, IPPROTO_IP, IP_TTL, 64) == 0),
                  "setsockopt IP_TTL=64", bsd_Errno());
    (VOID)t_get_int(fd, IPPROTO_IP, IP_TTL, &value);
    (VOID)t_check((BOOL)(value == 64), "IP_TTL reads back 64", value);

    (VOID)t_check(t_set_fails(fd, IPPROTO_IP, IP_TTL, 256, T_EINVAL),
                  "IP_TTL 256 is EINVAL", bsd_Errno());
    (VOID)t_check(t_set_fails(fd, IPPROTO_IP, IP_TTL, -2, T_EINVAL),
                  "IP_TTL -2 is EINVAL", bsd_Errno());

    (VOID)t_get_int(fd, IPPROTO_IP, IP_TTL, &value);
    (VOID)t_check((BOOL)(value == 64), "a refused IP_TTL leaves the old one",
                  value);

    /* -1 is "the default", the same spelling IPV6_UNICAST_HOPS takes. */
    (VOID)t_check((BOOL)(t_set_int(fd, IPPROTO_IP, IP_TTL, -1) == 0),
                  "IP_TTL -1 is accepted", bsd_Errno());
    (VOID)t_get_int(fd, IPPROTO_IP, IP_TTL, &value);
    (VOID)t_check((BOOL)(value == T_DEFAULT_TTL),
                  "and puts the default back", value);

    (VOID)t_check((BOOL)(t_set_int(fd, IPPROTO_IP, IP_TOS, 0x10) == 0),
                  "setsockopt IP_TOS=0x10", bsd_Errno());
    (VOID)t_get_int(fd, IPPROTO_IP, IP_TOS, &value);
    (VOID)t_check((BOOL)(value == 0x10), "IP_TOS reads back", value);

    (VOID)t_check(t_set_fails(fd, IPPROTO_IP, IP_TOS, 256, T_EINVAL),
                  "IP_TOS 256 is EINVAL", bsd_Errno());

    (VOID)t_check((BOOL)(t_get_int(fd, IPPROTO_IP, 0x7FFF, &value) < 0 &&
                         bsd_Errno() == T_ENOPROTOOPT),
                  "an unknown IPPROTO_IP option is ENOPROTOOPT", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);
}


/* ---------------------------------------------------------- multicast --- */

#ifdef AMINETXDUO_MULTICAST
/*
 * 4.4BSD types IP_MULTICAST_TTL and IP_MULTICAST_LOOP as u_char and everything
 * since passes an int, so all three widths have to work.  Two bytes did not:
 * m68k is big-endian, a short of 5 is 0x00,0x05, and the one-byte read took
 * the high half, IP_MULTICAST_TTL 0, which keeps the datagram off the link.
 * The reply had the mirror-image fault and answered 5 as 1280.
 */
static VOID t_test_multicast_widths(VOID)
{
LONG        fd;
LONG        value32;
WORD        value16;
UBYTE       value8;
socklen_t   len;

    t_log("IP_MULTICAST_TTL in every optlen");

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "udp socket", bsd_Errno()))
        return;

    /* Four bytes in, four bytes out. */
    value32 = 5;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
                                        &value32, 4) == 0),
                  "set IP_MULTICAST_TTL=5 as a LONG", bsd_Errno());
    value32 = 0;
    len     = 4;
    (VOID)bsd_getsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value32, &len);
    (VOID)t_check((BOOL)(value32 == 5 && len == 4), "reads back 5 as a LONG",
                  value32);

    /* One byte in, one byte out. */
    value8 = 7;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
                                        &value8, 1) == 0),
                  "set IP_MULTICAST_TTL=7 as a UBYTE", bsd_Errno());
    value8 = 0;
    len    = 1;
    (VOID)bsd_getsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value8, &len);
    (VOID)t_check((BOOL)(value8 == 7 && len == 1), "reads back 7 as a UBYTE",
                  (LONG)value8);

    /* Two bytes in: the one that read the high half and got 0. */
    value16 = 5;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
                                        &value16, 2) == 0),
                  "set IP_MULTICAST_TTL=5 as a WORD", bsd_Errno());

    value32 = 0;
    len     = 4;
    (VOID)bsd_getsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value32, &len);
    (VOID)t_check((BOOL)(value32 == 5),
                  "a WORD of 5 is 5, not 0 -- the high byte is not the value",
                  value32);

    /* Two bytes out: the one that wrote one byte and answered 1280. */
    value16 = 0;
    len     = 2;
    (VOID)bsd_getsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &value16, &len);
    (VOID)t_check((BOOL)(value16 == 5 && len == 2),
                  "and reads back into a WORD as 5, not 1280", (LONG)value16);

    /* IP_MULTICAST_LOOP takes the same three widths. */
    value16 = 1;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                                        &value16, 2) == 0),
                  "set IP_MULTICAST_LOOP=1 as a WORD", bsd_Errno());
    value32 = 0;
    len     = 4;
    (VOID)bsd_getsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &value32,
                         &len);
    (VOID)t_check((BOOL)(value32 == 1),
                  "IP_MULTICAST_LOOP is on, not silently cleared", value32);

    /* The range, and the read-only pair. */
    value32 = 256;
    (VOID)t_check((BOOL)(bsd_setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
                                        &value32, 4) < 0 &&
                         bsd_Errno() == T_EINVAL),
                  "IP_MULTICAST_TTL 256 is EINVAL", bsd_Errno());

    (VOID)bsd_CloseSocket(fd);
}
#endif /* AMINETXDUO_MULTICAST */


/* ------------------------------------------------------------- ioctls --- */

static VOID t_test_ioctls(VOID)
{
LONG            fd;
LONG            value;
struct ifreq  ifr;
struct ifconf ifc;
UBYTE           buf[512];

    t_log("IoctlSocket");

    fd = bsd_socket(AF_INET, SOCK_STREAM, 0);
    if (!t_check((BOOL)(fd >= 0), "tcp socket", bsd_Errno()))
        return;

    /*
     * SIOCATMARK: "is the next byte the urgent one?".  This implementation is
     * always OOBINLINE, so the mark means an urgent byte has arrived and
     * recv(MSG_OOB) has not taken it; on a socket that never saw one it is 0.
     */
    value = -1;
    (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, SIOCATMARK, &value) == 0 &&
                         value == 0),
                  "SIOCATMARK is 0 with no urgent byte", value);

    (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, SIOCATMARK, NULL) < 0 &&
                         bsd_Errno() == T_EFAULT),
                  "SIOCATMARK with a NULL argp is EFAULT", bsd_Errno());

    /* FIOASYNC turns the whole FD_* set on, which SO_EVENTMASK then reports. */
    value = 1;
    (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, FIOASYNC, &value) == 0),
                  "IoctlSocket(FIOASYNC, 1)", bsd_Errno());
    (VOID)t_get_int(fd, SOL_SOCKET, SO_EVENTMASK, &value);
    (VOID)t_check((BOOL)(value != 0),
                  "FIOASYNC set the event mask", value);

    value = 0;
    (VOID)bsd_IoctlSocket(fd, FIOASYNC, &value);
    (VOID)t_get_int(fd, SOL_SOCKET, SO_EVENTMASK, &value);
    (VOID)t_check((BOOL)(value == 0), "and clearing it clears the mask", value);

    /* FIONBIO and FIONREAD, which the conformance suite also reaches. */
    value = 1;
    (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, FIONBIO, &value) == 0),
                  "IoctlSocket(FIONBIO, 1)", bsd_Errno());

    value = -1;
    (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, FIONREAD, &value) == 0 &&
                         value == 0),
                  "FIONREAD is 0 on an idle socket", value);

    /* A request this object does not answer is ENOTTY, per the autodoc. */
    (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, 0xDEADBEEFUL, &value) < 0 &&
                         bsd_Errno() == T_ENOTTY),
                  "an unknown request is ENOTTY", bsd_Errno());

    /*
     * The interface queries.  They ignore the socket, BSD requires one to be
     * passed and says nothing about which, and answer out of interfaces.c.
     */
    t_bzero(&ifc, sizeof(ifc));
    ifc.ifc_len = (LONG)sizeof(buf);
    ifc.ifc_buf = buf;
    (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, SIOCGIFCONF, &ifc) == 0 &&
                         ifc.ifc_len > 0),
                  "SIOCGIFCONF names at least one interface", ifc.ifc_len);

    if (ifc.ifc_len > 0)
    {
        UBYTE *name = buf;
        UBYTE  i;

        t_bzero(&ifr, sizeof(ifr));
        for (i = 0; i < 15 && name[i] != 0; i++)
            ifr.ifr_name[i] = (char)name[i];

        t_log("  first interface is \"%s\"", ifr.ifr_name);

        (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, SIOCGIFFLAGS, &ifr) == 0),
                      "SIOCGIFFLAGS answers for it", bsd_Errno());
        (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, SIOCGIFADDR, &ifr) == 0),
                      "SIOCGIFADDR answers for it", bsd_Errno());
        (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, SIOCGIFNETMASK, &ifr) == 0),
                      "SIOCGIFNETMASK answers for it", bsd_Errno());
        (VOID)t_check((BOOL)(bsd_IoctlSocket(fd, SIOCGIFBRDADDR, &ifr) == 0),
                      "SIOCGIFBRDADDR answers for it", bsd_Errno());
    }

    (VOID)bsd_CloseSocket(fd);
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{
    t_log("AmiNetXDuo -- socket options through bsdsocket.library");

    /*
     * The stack needs an interface to come up on, and this one is made at run
     * time rather than opened out of DEVS:, see tests/tcpdrill/tapdev.c.  It
     * has to be installed by this process, because the device lives in the
     * installer's address space.
     */
    {
        static const UBYTE tap_mac[6] = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x08 };

        if (tap_install(tap_mac) != 0)
        {
            Printf((STRPTR)"cannot install the test interface\n");
            return(20);
        }
    }

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!t_check((BOOL)(SocketBase != NULL),
                 "OpenLibrary(bsdsocket.library, 4)", 0))
    {
        Printf((STRPTR)"bsdsocket.library not available\n");
        return(20);
    }

    t_test_accepted_and_ignored();
    t_test_buffers();
    t_test_linger();
    t_test_so_error();
    t_test_type_and_timeouts();
    t_test_tcp_options();
    t_test_ip_options(SOCK_STREAM, "tcp");
    t_test_ip_options(SOCK_DGRAM, "udp");
#ifdef AMINETXDUO_MULTICAST
    t_test_multicast_widths();
#endif
    t_test_ioctls();

    CloseLibrary(SocketBase);
    SocketBase = NULL;

    t_log("");
    t_log("%ld checks, %ld failures -- %s", t_checks, t_failures,
          (t_failures == 0UL) ? "PASS" : "FAIL");

    t_flush();

    return((t_failures == 0UL) ? 0 : 20);
}
