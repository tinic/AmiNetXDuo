/*
 * AmiNetXDuo, shared-library load test.
 *
 * Proves that the two images we ship are real AmigaOS shared libraries: that
 * Exec finds the romtag in the loaded segment, runs the RTF_AUTOINIT init
 * vector, and hands back a working base, and, for bsdsocket.library, that
 * OpenLibrary() brings the whole netstack up behind it and the LVO jump table
 * dispatches.
 *
 * It is a separate executable from tests/netstack because it exercises the
 * libraries through their ABI, not through the C API: nothing here is linked
 * against our code.
 *
 * Stage the libraries into LIBS: first, see tests/libraries/run-fsuae.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

static VOID t_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{

    (VOID) unused;
    if (c != '\0')
    {
        RawPutChar(c);
    }
}

static VOID t_log(const char *fmt, ...)
{

va_list args;


    va_start(args, fmt);
    RawDoFmt((STRPTR) fmt, args, (void (*)()) t_put_char, NULL);
    va_end(args);

    RawPutChar('\n');
}

static ULONG    t_checks;
static ULONG    t_failures;

static BOOL t_check(BOOL ok, const char *what, ULONG detail)
{

    t_checks++;
    if (!ok)
    {
        t_failures++;
        t_log("  FAIL %s (0x%lx)", what, detail);
    }
    else
    {
        t_log("  ok   %s", what);
    }

    return(ok);
}


/* -------------------------------------------------------------- vectors -- */

/*
 * The library bases are declared here rather than pulled from <proto/...>:
 * the inline headers expect the base to be a global with a specific name,
 * which is the ABI detail under test.
 */
struct Library *SocketBase;

/* bsdsocket.library LVOs, from the NDK's bsdsocket_lib.fd and
   <inline/bsdsocket.h>. */
#define LVO_socket          (-30)
#define LVO_bind            (-36)
#define LVO_listen          (-42)
#define LVO_accept          (-48)
#define LVO_sendto          (-60)
#define LVO_CloseSocket     (-120)
#define LVO_WaitSelect      (-126)
#define LVO_getdtablesize   (-138)
#define LVO_Errno           (-162)
#define LVO_Inet_NtoA       (-174)
#define LVO_inet_addr       (-180)
#define LVO_inet_network    (-204)
#define LVO_gethostbyname   (-210)
#define LVO_gethostname     (-282)
#define LVO_inet_pton       (-606)

/* Errno values, from the NDK's <sys/errno.h>. */
#define T_EFAULT            14
#define T_EINVAL            22
#define T_EMSGSIZE          40
#define T_EPROTONOSUPPORT   43
#define T_EOPNOTSUPP        45

static LONG bsd_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = domain;
register LONG            d1 __asm("d1") = type;
register LONG            d2 __asm("d2") = proto;
register LONG            res __asm("d0");
register LONG _clob_d1 __asm("d1");


    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_close_socket(struct Library *base, LONG sock)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = sock;
register LONG            res __asm("d0");


    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_gethostname(struct Library *base, char *name, ULONG len)
{

register struct Library *a6 __asm("a6") = base;
register char           *a0 __asm("a0") = name;
register ULONG           d0 __asm("d0") = len;
register LONG            res __asm("d0");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-282:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0)
                      : "d1", "a1", "cc", "memory");
    return(res);
}

static char *bsd_inet_ntoa(struct Library *base, ULONG addr)
{

register struct Library *a6 __asm("a6") = base;
register ULONG           d0 __asm("d0") = addr;
register char           *res __asm("d0");


    __asm __volatile ("jsr a6@(-174:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return(res);
}

static ULONG bsd_inet_addr(struct Library *base, const char *cp)
{

register struct Library *a6 __asm("a6") = base;
register const char     *a0 __asm("a0") = cp;
register ULONG           res __asm("d0");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-180:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return(res);
}

static ULONG bsd_inet_network(struct Library *base, const char *cp)
{

register struct Library *a6 __asm("a6") = base;
register const char     *a0 __asm("a0") = cp;
register ULONG           res __asm("d0");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-204:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_inet_pton(struct Library *base, LONG af, const char *src,
                          APTR dst)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = af;
register const char     *a0 __asm("a0") = src;
register APTR            a1 __asm("a1") = dst;
register LONG            res __asm("d0");
register LONG _clob_a0 __asm("a0");
register LONG _clob_a1 __asm("a1");


    __asm __volatile ("jsr a6@(-606:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return(res);
}

static LONG bsd_errno(struct Library *base)
{

register struct Library *a6 __asm("a6") = base;
register LONG            res __asm("d0");


    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_getdtablesize(struct Library *base)
{

register struct Library *a6 __asm("a6") = base;
register LONG            res __asm("d0");


    __asm __volatile ("jsr a6@(-138:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_bind(struct Library *base, LONG sock, APTR name, LONG namelen)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = sock;
register APTR            a0 __asm("a0") = name;
register LONG            d1 __asm("d1") = namelen;
register LONG            res __asm("d0");
register LONG _clob_d1 __asm("d1");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return(res);
}

static LONG bsd_listen(struct Library *base, LONG sock, LONG backlog)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = sock;
register LONG            d1 __asm("d1") = backlog;
register LONG            res __asm("d0");
register LONG _clob_d1 __asm("d1");


    __asm __volatile ("jsr a6@(-42:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return(res);
}

static LONG bsd_accept(struct Library *base, LONG sock, APTR addr, APTR addrlen)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = sock;
register APTR            a0 __asm("a0") = addr;
register APTR            a1 __asm("a1") = addrlen;
register LONG            res __asm("d0");
register LONG _clob_a0 __asm("a0");
register LONG _clob_a1 __asm("a1");


    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return(res);
}

static LONG bsd_sendto(struct Library *base, LONG sock, APTR buf, LONG len,
                       LONG flags, APTR to, LONG tolen)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = sock;
register APTR            a0 __asm("a0") = buf;
register LONG            d1 __asm("d1") = len;
register LONG            d2 __asm("d2") = flags;
register APTR            a1 __asm("a1") = to;
register LONG            d3 __asm("d3") = tolen;
register LONG            res __asm("d0");
register LONG _clob_d1 __asm("d1");
register LONG _clob_a0 __asm("a0");
register LONG _clob_a1 __asm("a1");


    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
                      : "cc", "memory");
    return(res);
}

static LONG bsd_wait_select(struct Library *base, LONG nfds, APTR r, APTR w,
                            APTR e, APTR timeout, APTR signals)
{

register struct Library *a6 __asm("a6") = base;
register LONG            d0 __asm("d0") = nfds;
register APTR            a0 __asm("a0") = r;
register APTR            a1 __asm("a1") = w;
register APTR            a2 __asm("a2") = e;
register APTR            a3 __asm("a3") = timeout;
register APTR            d1 __asm("d1") = signals;
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

static APTR bsd_gethostbyname(struct Library *base, const char *name)
{

register struct Library *a6 __asm("a6") = base;
register const char     *a0 __asm("a0") = name;
register APTR            res __asm("d0");
register LONG _clob_a0 __asm("a0");


    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return(res);
}


/* ------------------------------------------------ documented edge cases --- */

/*
 * The socket-layer errno rules the NDK 3.2 autodoc names, each of which this
 * library used to get wrong.  Every one of them is decided before any packet
 * moves, so none of this depends on the network being up.
 *
 * The structures are spelled out rather than included: this file reaches the
 * library through its LVOs, and the layouts (sin_len, sin_family, then a
 * network-order port) are the ABI under test.
 */
struct t_sockaddr_in
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

struct t_timeval
{
    ULONG   tv_secs;
    ULONG   tv_micro;
};

#define T_AF_INET           2L
#define T_SOCK_STREAM       1L
#define T_SOCK_DGRAM        2L
#define T_SOCK_SEQPACKET    5L

#define T_DGRAM_BUF         (65536UL + 16UL)

static VOID t_addr_in(struct t_sockaddr_in *sa, ULONG host, UWORD port)
{

    ULONG i;


    for (i = 0UL; i < sizeof(*sa); i++)
    {
        ((UBYTE *) sa)[i] =  0;
    }

    sa -> sin_len    =  (UBYTE) sizeof(*sa);
    sa -> sin_family =  (UBYTE) T_AF_INET;
    sa -> sin_port   =  port;           /* m68k is already network order */
    sa -> sin_addr   =  host;
}

static VOID t_audit_edges(struct Library *sbase)
{

struct t_sockaddr_in  sa;
struct t_timeval      tv;
LONG                  fd, rc;


    t_log("");
    t_log("documented socket edge cases");

    /*
     * "[EPROTONOSUPPORT] The protocol type or the specified protocol is not
     * supported within this domain.", the only "not supported" code in
     * socket()'s ERRORS list.  SOCK_SEQPACKET is a defined type this library
     * does not implement.
     */
    fd =  bsd_socket(sbase, T_AF_INET, T_SOCK_SEQPACKET, 0L);
    rc =  bsd_errno(sbase);
    if (fd >= 0)
    {
        (VOID) bsd_close_socket(sbase, fd);
    }
    (VOID) t_check((BOOL) (fd == -1L && rc == T_EPROTONOSUPPORT),
                   "socket(SOCK_SEQPACKET) is EPROTONOSUPPORT", (ULONG) rc);

    /* "[EOPNOTSUPP] The referenced socket is not of type SOCK_STREAM." */
    fd =  bsd_socket(sbase, T_AF_INET, T_SOCK_DGRAM, 0L);
    if (t_check((BOOL) (fd >= 0), "socket(AF_INET, SOCK_DGRAM)", (ULONG) fd))
    {
        rc =  bsd_accept(sbase, fd, NULL, NULL);
        rc =  (rc == -1L) ? bsd_errno(sbase) : 0L;
        (VOID) t_check((BOOL) (rc == T_EOPNOTSUPP),
                       "accept(datagram socket) is EOPNOTSUPP", (ULONG) rc);

        (VOID) bsd_close_socket(sbase, fd);
    }

    /*
     * The other half of that pair, which the library used to merge into it: a
     * stream socket that never listened is EINVAL, as in BSD.
     */
    fd =  bsd_socket(sbase, T_AF_INET, T_SOCK_STREAM, 0L);
    if (fd >= 0)
    {
        rc =  bsd_accept(sbase, fd, NULL, NULL);
        rc =  (rc == -1L) ? bsd_errno(sbase) : 0L;
        (VOID) t_check((BOOL) (rc == T_EINVAL),
                       "accept(stream, never listened) is EINVAL", (ULONG) rc);

        /*
         * "[EFAULT] The addr parameter is not in a writable part of the user
         * address space."  addrlen is the value-result saying how much room
         * addr has; without it the address cannot be written.  Checked on a
         * listening socket, so nothing else can be the reason.
         */
        t_addr_in(&sa, 0x7F000001UL, 7710);
        if (bsd_bind(sbase, fd, &sa, (LONG) sizeof(sa)) == 0
            && bsd_listen(sbase, fd, 1L) == 0)
        {
            rc =  bsd_accept(sbase, fd, &sa, NULL);
            rc =  (rc == -1L) ? bsd_errno(sbase) : 0L;
            (VOID) t_check((BOOL) (rc == T_EFAULT),
                           "accept(addr, addrlen NULL) is EFAULT", (ULONG) rc);
        }
        else
        {
            (VOID) t_check(FALSE, "bind+listen for the EFAULT case", 0UL);
        }

        (VOID) bsd_close_socket(sbase, fd);
    }

    /*
     * "The 'nfds' parameter may be truncated if it covers more sockets than
     * are currently in use."  Truncated, not refused: a zero timeout over an
     * over-large nfds polls nothing and returns 0.
     */
    tv.tv_secs  =  0UL;
    tv.tv_micro =  0UL;
    rc =  bsd_wait_select(sbase, bsd_getdtablesize(sbase) + 64L,
                          NULL, NULL, NULL, &tv, NULL);
    (VOID) t_check((BOOL) (rc == 0L), "WaitSelect(nfds > table) truncates",
                   (ULONG) ((rc == -1L) ? bsd_errno(sbase) : rc));

    /*
     * "If the message is too long to pass atomically through the underlying
     * protocol, the error EMSGSIZE is returned, and the message is not
     * transmitted."
     *
     * The ceiling is the egress interface's MTU less the IP and UDP headers,
     * so it is not one number: NetX Duo's loopback interface carries a
     * 65535-byte MTU, making 65507 the largest datagram it will take.  Both
     * sides of that boundary are checked, because a blanket 1472, the
     * Ethernet figure, would refuse a datagram loopback can carry.
     */
    {
        UBYTE *big =  (UBYTE *) AllocMem(T_DGRAM_BUF, MEMF_ANY | MEMF_CLEAR);

        if (big != NULL)
        {
            fd =  bsd_socket(sbase, T_AF_INET, T_SOCK_DGRAM, 0L);
            if (fd >= 0)
            {
                t_addr_in(&sa, 0x7F000001UL, 7711);

                rc =  bsd_sendto(sbase, fd, big, 65508L, 0L, &sa,
                                 (LONG) sizeof(sa));
                rc =  (rc == -1L) ? bsd_errno(sbase) : 0L;
                (VOID) t_check((BOOL) (rc == T_EMSGSIZE),
                               "sendto(65508 over loopback) is EMSGSIZE",
                               (ULONG) rc);

                rc =  bsd_sendto(sbase, fd, big, 65507L, 0L, &sa,
                                 (LONG) sizeof(sa));
                rc =  (rc == -1L) ? bsd_errno(sbase) : 0L;
                (VOID) t_check((BOOL) (rc != T_EMSGSIZE),
                               "sendto(65507 over loopback) is not EMSGSIZE",
                               (ULONG) rc);

                (VOID) bsd_close_socket(sbase, fd);
            }

            FreeMem(big, T_DGRAM_BUF);
        }
        else
        {
            t_log("  skip sendto EMSGSIZE: no %ld bytes free", T_DGRAM_BUF);
        }
    }
}


/* --------------------------------------------------- the bsdsocket phase -- */

/*
 * Run in a Process of its own so main() can put a clock on it, specifically
 * on the CloseLibrary() at the end, which drops the last netstack reference and
 * tears the stack down.  That close hung for ever: netstack_shutdown() stopped
 * the ARexx host by waiting on a MsgPort created during bring-up, on a Task
 * that no longer existed, so the signal went nowhere (netstack_rexx.c).  It is
 * invisible in normal use because AddNetInterface keeps a reference; any
 * program that opens the library, uses it and exits hits it.
 *
 * A wedged close cannot be unwedged, so a failing run leaves this Process
 * parked in Exec while main() reports and exits.  It is parked in Wait(), not
 * in this hunk, and the run is over either way.
 */

#define T_BRINGUP_LIMIT     900UL       /* 200 ms units: 180 s to come up   */
#define T_CLOSE_LIMIT       300UL       /* 200 ms units: 60 s to close      */
#define T_POLL_TICKS        10UL        /* Delay() units: 200 ms            */
#define T_CHILD_STACK       65536UL

static volatile ULONG   t_closing;
static volatile ULONG   t_child_done;

static VOID t_bsd_main(VOID)
{

struct Library  *sbase;
char             hostname[64];
LONG             sock;


    /*
     * This is the whole stack: OpenLibrary() runs the romtag init, clones a
     * per-opener base and calls netstack_startup(), which opens the SANA-II
     * device, starts ThreadX and blocks for a DHCP lease.
     */
    t_log("opening bsdsocket.library (this brings the stack up)");

    sbase =  OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4UL);
    if (!t_check((BOOL) (sbase != NULL), "OpenLibrary(bsdsocket.library)", 0UL))
    {
        t_child_done =  1UL;
        return;
    }

    SocketBase =  sbase;

    t_log("  bsdsocket.library %ld.%ld: %s",
          (ULONG) sbase -> lib_Version, (ULONG) sbase -> lib_Revision,
          (sbase -> lib_IdString != NULL) ? sbase -> lib_IdString
                                          : (STRPTR) "(no id)");

    hostname[0] =  '\0';
    (VOID) t_check((BOOL) (bsd_gethostname(sbase, hostname,
                                           (ULONG) sizeof(hostname)) == 0),
                   "gethostname()", 0UL);
    t_log("  hostname '%s'", hostname);

    {
        char *dotted =  bsd_inet_ntoa(sbase, 0x0A00020FUL);   /* 10.0.2.15 */

        t_log("  Inet_NtoA(10.0.2.15) = '%s'",
              (dotted != NULL) ? dotted : "(null)");
        (VOID) t_check((BOOL) (dotted != NULL), "Inet_NtoA()", 0UL);
    }

    /*
     * The address conversions, against the NDK 3.2 autodoc's own rules: the
     * short dotted forms, INADDR_NONE for anything malformed, and
     * inet_network()'s byte pack, "192.168.1" is the network number
     * 0x00C0A801, not an address with a zero host part.
     */
    {
        ULONG net;

        (VOID) t_check((BOOL) (bsd_inet_addr(sbase, "192.168.1.1")
                               == 0xC0A80101UL), "inet_addr(dotted quad)", 0UL);
        (VOID) t_check((BOOL) (bsd_inet_addr(sbase, "10.1") == 0x0A000001UL),
                       "inet_addr(two-part form)", 0UL);
        (VOID) t_check((BOOL) (bsd_inet_addr(sbase, "") == 0xFFFFFFFFUL),
                       "inet_addr(\"\") is INADDR_NONE", 0UL);
        (VOID) t_check((BOOL) (bsd_inet_addr(sbase, "1.2.3.4x")
                               == 0xFFFFFFFFUL),
                       "inet_addr(trailing garbage) is INADDR_NONE", 0UL);

        net =  bsd_inet_network(sbase, "192.168.1");
        (VOID) t_check((BOOL) (net == 0x00C0A801UL),
                       "inet_network(\"192.168.1\")", net);
        (VOID) t_check((BOOL) (bsd_inet_network(sbase, "127") == 127UL),
                       "inet_network(\"127\")", 0UL);
    }

    {
        ULONG addr =  0UL;

        (VOID) t_check((BOOL) (bsd_inet_pton(sbase, 2L, "1.2.3.4", &addr) == 1L
                               && addr == 0x01020304UL),
                       "inet_pton(AF_INET, dotted quad)", addr);
        /* Only the full a.b.c.d form; a short form is 0, not 1. */
        (VOID) t_check((BOOL) (bsd_inet_pton(sbase, 2L, "1.2.3", &addr) == 0L),
                       "inet_pton(AF_INET, short form) is 0", 0UL);
        /* An unknown family is -1, which is not the same answer as invalid. */
        (VOID) t_check((BOOL) (bsd_inet_pton(sbase, 99L, "1.2.3.4", &addr)
                               == -1L),
                       "inet_pton(bad family) is -1", 0UL);
    }

    sock =  bsd_socket(sbase, 2L /* AF_INET */, 1L /* SOCK_STREAM */, 0L);
    if (t_check((BOOL) (sock >= 0), "socket(AF_INET, SOCK_STREAM)", (ULONG) sock))
    {
        (VOID) t_check((BOOL) (bsd_close_socket(sbase, sock) == 0),
                       "CloseSocket()", 0UL);
    }

    t_audit_edges(sbase);

    {
        APTR he =  bsd_gethostbyname(sbase, "localhost");

        (VOID) t_check((BOOL) (he != NULL), "gethostbyname(\"localhost\")", 0UL);
    }

    t_log("");
    t_log("closing bsdsocket.library (tears the stack down)");

    SocketBase =  NULL;
    t_closing  =  1UL;

    CloseLibrary(sbase);

    t_log("  closed bsdsocket.library");
    t_child_done =  1UL;
}


/* ------------------------------------------------------------------ main -- */

int main(void)
{

struct Library  *ugbase;
struct Process  *child;
struct TagItem   tags[6];
ULONG            waited;
ULONG            closed_after;


    t_log("AmiNetXDuo, shared library load test");

    /* ---- usergroup.library ---------------------------------------------- */

    ugbase =  OpenLibrary((CONST_STRPTR) "usergroup.library", 0UL);
    if (t_check((BOOL) (ugbase != NULL), "OpenLibrary(usergroup.library)", 0UL))
    {

        t_log("  usergroup.library %ld.%ld: %s",
              (ULONG) ugbase -> lib_Version, (ULONG) ugbase -> lib_Revision,
              (ugbase -> lib_IdString != NULL) ? ugbase -> lib_IdString
                                               : (STRPTR) "(no id)");

        (VOID) t_check((BOOL) (ugbase -> lib_Version >= 4),
                       "usergroup.library version >= 4",
                       (ULONG) ugbase -> lib_Version);

        CloseLibrary(ugbase);
    }

    /* ---- bsdsocket.library, on a watched Process ------------------------- */

    tags[0].ti_Tag  =  NP_Entry;      tags[0].ti_Data =  (ULONG) t_bsd_main;
    tags[1].ti_Tag  =  NP_Name;       tags[1].ti_Data =  (ULONG) "library_test bsd";
    tags[2].ti_Tag  =  NP_StackSize;  tags[2].ti_Data =  T_CHILD_STACK;
    tags[3].ti_Tag  =  NP_Cli;        tags[3].ti_Data =  (ULONG) FALSE;
    tags[4].ti_Tag  =  TAG_DONE;      tags[4].ti_Data =  0;

    child =  CreateNewProc(tags);
    if (!t_check((BOOL) (child != NULL), "CreateNewProc(bsdsocket phase)", 0UL))
    {
        t_log("");
        t_log("%ld checks, %ld failures, FAIL", t_checks, t_failures);
        return(20);
    }

    closed_after =  0UL;
    for (waited = 0UL; t_child_done == 0UL; waited++)
    {
        if (t_closing != 0UL)
        {
            closed_after++;
            if (closed_after > T_CLOSE_LIMIT)
            {
                break;
            }
        }
        else if (waited > T_BRINGUP_LIMIT)
        {
            break;
        }

        Delay(T_POLL_TICKS);
    }

    t_log("");
    if (t_closing == 0UL)
    {
        (VOID) t_check(FALSE, "bsdsocket phase reached the close", waited / 5UL);
    }
    else
    {
        (VOID) t_check((BOOL) (t_child_done != 0UL),
                       "CloseLibrary(bsdsocket.library) returned",
                       closed_after / 5UL);
        if (t_child_done == 0UL)
        {
            t_log("  the last close did not return in %ld s, the teardown is "
                  "stuck", T_CLOSE_LIMIT / 5UL);
        }
    }

    t_log("");
    t_log("%ld checks, %ld failures, %s",
          t_checks, t_failures, (t_failures == 0UL) ? "PASS" : "FAIL");

    return((t_failures == 0UL) ? 0 : 20);
}
