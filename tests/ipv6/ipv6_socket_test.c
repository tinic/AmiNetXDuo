/*
 * AmiNetXDuo -- AF_INET6 through bsdsocket.library's ABI.
 *
 * The third IPv6 test, and the only one that goes through the LVO jump table:
 * ipv6_test.c drives NetX Duo directly, ipv6_link_test.c drives the netstack,
 * and this one is an ordinary AmigaOS program that does
 * OpenLibrary("bsdsocket.library") and calls vectors, as a ported Unix
 * application would.  It is linked against none of our code.
 *
 * Everything happens over ::1, which nxd_ipv6_enable() configures on the
 * internal loopback interface, so the wire is not a variable.  What is under
 * test is the socket layer -- sockaddr_in6 in and out of bind/connect/accept/
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
#include <proto/dos.h>

#include <stdarg.h>


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
 * This must match ndk-include/netinet/in.h:182 exactly -- 28 bytes, family at
 * offset 0, no length byte.
 */
struct t_in6_addr
{
    UBYTE   s6_addr[16];
};

struct t_sockaddr_in6
{
    UBYTE               sin6_family;    /* offset  0 -- not a length byte */
    UBYTE               sin6_pad;       /* offset  1 -- compiler padding  */
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

#define T_AF_INET           2
#define T_AF_INET6          23
#define T_AF_UNSPEC         0
#define T_SOCK_STREAM       1
#define T_SOCK_DGRAM        2
#define T_IPPROTO_TCP       6
#define T_IPPROTO_IPV6      41
#define T_IPV6_V6ONLY_BSD   27
#define T_IPV6_V6ONLY_LINUX 26

#define T_EAFNOSUPPORT      47

#define T_AI_PASSIVE        1
#define T_AI_NUMERICHOST    4

#define T_PORT              9099


/* ------------------------------------------------------------ LVO stubs --- */

/*
 * Every stub declares three variables it never uses because d0, d1, a0 and a1
 * are scratch on AmigaOS: a library function may destroy them without saying
 * so.  An `asm` block that lists them only as inputs tells GCC the opposite --
 * that whatever was in them survives the call -- and GCC will reuse the
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

/* gai_strerror takes its argument in a0, not d0 -- pragmas line 141. */
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
       actually chose -- ::1 for a loopback connection. */
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
                  "gai_strerror(EAI_NONAME) -- argument really is in a0", 0);

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
    }
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{
    t_log("AmiNetXDuo -- AF_INET6 through bsdsocket.library");

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
    t_test_udp_loopback();
    t_test_getaddrinfo();

    CloseLibrary(SocketBase);
    SocketBase = NULL;

    t_log("");
    t_log("%ld checks, %ld failures -- %s", t_checks, t_failures,
          (t_failures == 0UL) ? "PASS" : "FAIL");

    t_flush();

    return((t_failures == 0UL) ? 0 : 20);
}
