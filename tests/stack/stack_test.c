/*
 * AmiNetXDuo, the API from a 4 KB stack, and how much of it gets touched.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* NETCTRL_DHCP_RENEW and NetStatusControl. The only aminetxduo header this
   file uses: everything else here is a hand-written LVO, because a program
   that linked the stack would measure a second copy of it. */
#include <aminetxduo/netstatus.h>

#ifndef ST_STACK
#define ST_STACK        4096UL
#endif

/* Enough passes that a per-call leak of stack shows up as a deeper mark, few
   enough that the emulator tier is not waiting on it. */
#ifndef ST_ROUNDS
#define ST_ROUNDS       32UL
#endif

#define ST_PORT         7460
#define ST_CHUNK        512UL
#define ST_PATTERN      0xA5C3A5C3UL
#define ST_PHASES       13

#define ST_TIMEOUT_TICKS (600 * 50)     /* 600 s at 50 ticks/s */

#define ST_DONE_SIGNAL  SIGBREAKB_CTRL_F
#define ST_DONE_MASK    SIGBREAKF_CTRL_F

#define S_AF_INET       2
#define S_SOCK_STREAM   1
#define S_SOL_SOCKET    0xFFFF
#define S_SO_REUSEADDR  0x0004

typedef struct StAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} StAddr;

typedef struct StTimeval
{
    LONG    tv_secs;
    LONG    tv_micro;
} StTimeval;

static LONG s_socket(struct Library *base, LONG dom, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = dom;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG s_bind(struct Library *base, LONG s, StAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(StAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG s_listen(struct Library *base, LONG s, LONG backlog)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = backlog;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-42:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG s_accept(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = (APTR)0;
    register APTR            a1  __asm("a1") = (APTR)0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG s_connect(struct Library *base, LONG s, StAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(StAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG s_send(struct Library *base, LONG s, APTR buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG s_recv(struct Library *base, LONG s, APTR buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-78:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG s_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
                         APTR val, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register APTR            a0  __asm("a0") = val;
    register LONG            d3  __asm("d3") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
                      : "a1", "cc", "memory");
    return res;
}

static LONG s_getsockname(struct Library *base, LONG s, StAddr *sa, LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register APTR            a1  __asm("a1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-102:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG s_closesocket(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG s_waitselect(struct Library *base, LONG nfds, APTR rd, APTR wr,
                         APTR ex, StTimeval *tv, ULONG *sigs)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = nfds;
    register APTR            a0  __asm("a0") = rd;
    register APTR            a1  __asm("a1") = wr;
    register APTR            a2  __asm("a2") = ex;
    register APTR            a3  __asm("a3") = tv;
    register ULONG          *d1  __asm("d1") = sigs;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

static LONG s_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static APTR s_gethostbyname(struct Library *base, const char *name)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)name;
    register APTR            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static APTR s_gethostbyaddr(struct Library *base, APTR addr, LONG len,
                            LONG type)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = addr;
    register LONG            d0  __asm("d0") = len;
    register LONG            d1  __asm("d1") = type;
    register APTR            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-216:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static APTR s_getservbyname(struct Library *base, const char *name,
                            const char *proto)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)name;
    register APTR            a1  __asm("a1") = (APTR)proto;
    register APTR            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-234:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG s_getaddrinfo(struct Library *base, const char *node,
                          const char *service, APTR hints, APTR *res_out)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)node;
    register APTR            a1  __asm("a1") = (APTR)service;
    register APTR            a2  __asm("a2") = hints;
    register APTR            a3  __asm("a3") = res_out;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-810:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (a2), "r" (a3)
                      : "cc", "memory");
    return res;
}

static LONG s_getnameinfo(struct Library *base, StAddr *sa, LONG salen,
                          char *host, LONG hostlen, char *serv, LONG servlen,
                          LONG flags)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d0  __asm("d0") = salen;
    register APTR            a1  __asm("a1") = host;
    register LONG            d1  __asm("d1") = hostlen;
    register APTR            a2  __asm("a2") = serv;
    register LONG            d2  __asm("d2") = servlen;
    register LONG            d3  __asm("d3") = flags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-822:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (a0), "r" (d0), "r" (a1), "r" (d1),
                        "r" (a2), "r" (d2), "r" (d3)
                      : "cc", "memory");
    return res;
}

static LONG s_netstackcontrol(struct Library *base, ULONG op,
                              NetStatusControl *ctl)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = op;
    register APTR            a0  __asm("a0") = ctl;
    register ULONG           d2  __asm("d2") = (ULONG)sizeof(*ctl);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-876:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG s_getdtablesize(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-138:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6)
                      : "a0", "a1", "cc", "memory");
    return res;
}

typedef struct StResult
{
    ULONG   sr_Asked;               /* NP_StackSize we passed             */
    ULONG   sr_Stack;               /* tc_SPUpper - tc_SPLower, measured   */
    ULONG   sr_Deepest;             /* bytes below tc_SPUpper ever used   */
    ULONG   sr_Untouched;           /* what the pattern still holds       */
    LONG    sr_Checks;
    LONG    sr_Failures;
    LONG    sr_Finished;
    LONG    sr_Bytes;               /* echoed over loopback               */
    LONG    sr_Phase;               /* how far the worker got             */
    LONG    sr_Renewed;             /* interfaces NETCTRL_DHCP_RENEW took */
    ULONG   sr_Mark[ST_PHASES];     /* the mark as each phase ended       */
} StResult;

static const char * const st_phase_name[ST_PHASES] =
{
    "before OpenLibrary",
    "OpenLibrary + getdtablesize",
    "socket/connect/getsockname",
    "loopback send/WaitSelect/recv rounds",
    "NETCTRL_DHCP_RENEW, which arms the absorb",
    "gethostbyname .invalid WITH A LEASE PENDING (the absorb)",
    "gethostbyname localhost (hosts file, shallow)",
    "gethostbyname .invalid (DNS)",
    "gethostbyname .local (mDNS)",
    "getservbyname",
    "getaddrinfo .invalid",
    "getnameinfo 192.0.2.1 (reverse DNS)",
    "CloseLibrary"
};

static StResult st_result;
static struct Task *st_parent;

static VOID st_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (VOID)VPrintf((STRPTR)fmt, (LONG *)ap);
    va_end(ap);
    (VOID)Flush(Output());
}

static VOID st_check(LONG ok, const char *what, LONG detail)
{
    st_result.sr_Checks++;

    if (ok)
    {
        st_log("  ok   %s\n", (LONG)what);
        return;
    }

    st_result.sr_Failures++;
    st_log("  FAIL %s (%ld)\n", (LONG)what, detail);
}

#define ST_PAINT_MARGIN     256

static VOID st_paint(VOID)
{
    struct Task *me    = FindTask(NULL);
    ULONG        here;
    ULONG       *lo    = (ULONG *)me->tc_SPLower;
    ULONG       *stop  = (ULONG *)(((ULONG)&here) - ST_PAINT_MARGIN);
    ULONG       *p;

    if (lo == NULL || stop <= lo)
        return;

    for (p = lo; p < stop; p++)
        *p = ST_PATTERN;
}

static ULONG st_mark(VOID)
{
    struct Task *me = FindTask(NULL);
    ULONG       *lo = (ULONG *)me->tc_SPLower;
    ULONG       *hi = (ULONG *)me->tc_SPUpper;
    ULONG       *p;

    if (lo == NULL || hi == NULL)
        return 0;

    st_result.sr_Stack = (ULONG)((UBYTE *)hi - (UBYTE *)lo);

    for (p = lo; p < hi; p++)
        if (*p != ST_PATTERN)
            break;

    st_result.sr_Untouched = (ULONG)((UBYTE *)p - (UBYTE *)lo);

    return (ULONG)((UBYTE *)hi - (UBYTE *)p);
}

static VOID st_phase(LONG which)
{
    st_result.sr_Phase = which;

    if (which >= 0 && which < ST_PHASES)
        st_result.sr_Mark[which] = st_mark();
}

#define ST_PEER_STACK   16384UL

static volatile LONG   st_peer_port;
static volatile LONG   st_peer_stop;

static VOID st_peer_entry(VOID)
{
    struct Library *base;
    LONG            fd  = -1;
    LONG            one = 1;
    StAddr          sa;
    ULONG           i;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (base == NULL)
    {
        st_peer_port = -1;
        Signal(st_parent, ST_DONE_MASK);
        return;
    }

    fd = s_socket(base, S_AF_INET, S_SOCK_STREAM, 0);
    if (fd >= 0)
    {
        (VOID)s_setsockopt(base, fd, S_SOL_SOCKET, S_SO_REUSEADDR, &one,
                           (LONG)sizeof(one));

        for (i = 0; i < sizeof(sa); i++)
            ((UBYTE *)&sa)[i] = 0;
        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = S_AF_INET;
        sa.sin_port   = (UWORD)ST_PORT;
        sa.sin_addr   = 0x7F000001UL;

        if (s_bind(base, fd, &sa) != 0 || s_listen(base, fd, 4) != 0)
        {
            (VOID)s_closesocket(base, fd);
            fd = -1;
        }
    }

    st_peer_port = (fd >= 0) ? ST_PORT : -1;
    Signal(st_parent, ST_DONE_MASK);

    while (fd >= 0 && !st_peer_stop)
    {
        ULONG     rd[1];
        StTimeval tv;
        LONG      conn;

        rd[0] = 1UL << fd;
        tv.tv_secs  = 1;
        tv.tv_micro = 0;

        if (s_waitselect(base, fd + 1, rd, NULL, NULL, &tv, NULL) <= 0)
            continue;

        conn = s_accept(base, fd);
        if (conn < 0)
            continue;

        for (;;)
        {
            UBYTE buf[ST_CHUNK];
            LONG  got = s_recv(base, conn, buf, (LONG)sizeof(buf));
            LONG  done = 0;

            if (got <= 0)
                break;

            while (done < got)
            {
                LONG put = s_send(base, conn, buf + done, got - done);

                if (put <= 0)
                {
                    done = got;
                    break;
                }
                done += put;
            }
        }

        (VOID)s_closesocket(base, conn);
    }

    if (fd >= 0)
        (VOID)s_closesocket(base, fd);

    CloseLibrary(base);
}

static VOID st_worker_entry(VOID)
{
    struct Library *base;
    LONG            fd = -1;
    ULONG           round;
    StAddr          sa;
    ULONG           i;

    st_paint();
    st_phase(0);

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    st_check(base != NULL, "OpenLibrary on a 4 KB stack", 0);
    if (base == NULL)
    {
        st_result.sr_Deepest  = st_mark();
        st_result.sr_Finished = 0;
        Signal(st_parent, ST_DONE_MASK);
        return;
    }

    st_check(s_getdtablesize(base) > 0, "getdtablesize", 0);
    st_phase(1);

    for (round = 0; round < ST_ROUNDS; round++)
    {
        UBYTE     buf[64];
        ULONG     rd[1];
        StTimeval tv;
        LONG      n;

        fd = s_socket(base, S_AF_INET, S_SOCK_STREAM, 0);
        if (fd < 0)
        {
            st_check(0, "socket", s_errno(base));
            break;
        }

        for (i = 0; i < sizeof(sa); i++)
            ((UBYTE *)&sa)[i] = 0;
        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = S_AF_INET;
        sa.sin_port   = (UWORD)ST_PORT;
        sa.sin_addr   = 0x7F000001UL;

        if (s_connect(base, fd, &sa) != 0)
        {
            st_check(0, "connect to the loopback peer", s_errno(base));
            (VOID)s_closesocket(base, fd);
            break;
        }

        {
            LONG len = (LONG)sizeof(sa);

            (VOID)s_getsockname(base, fd, &sa, &len);
        }

        if (round == 0)
            st_phase(2);

        for (i = 0; i < sizeof(buf); i++)
            buf[i] = (UBYTE)(round + i);

        n = s_send(base, fd, buf, (LONG)sizeof(buf));
        if (n != (LONG)sizeof(buf))
        {
            st_check(0, "send", s_errno(base));
            (VOID)s_closesocket(base, fd);
            break;
        }

        rd[0] = 1UL << fd;
        tv.tv_secs  = 5;
        tv.tv_micro = 0;

        if (s_waitselect(base, fd + 1, rd, NULL, NULL, &tv, NULL) <= 0)
        {
            st_check(0, "WaitSelect on the echo", s_errno(base));
            (VOID)s_closesocket(base, fd);
            break;
        }

        n = s_recv(base, fd, buf, (LONG)sizeof(buf));
        if (n <= 0)
        {
            st_check(0, "recv the echo", s_errno(base));
            (VOID)s_closesocket(base, fd);
            break;
        }

        st_result.sr_Bytes += n;
        (VOID)s_closesocket(base, fd);
    }

    st_check(st_result.sr_Bytes > 0, "loopback round trips completed",
             st_result.sr_Bytes);

    st_phase(3);

    {
        static NetStatusControl ctl;    /* static: this is the 4 KB worker */
        LONG index;

        for (index = 0; index < 4; index++)
        {
            ULONG i;

            for (i = 0; i < sizeof(ctl); i++)
                ((UBYTE *)&ctl)[i] = 0;
            ctl.nsc_Magic   = AMI_NETSTATUS_MAGIC;
            ctl.nsc_Version = (UWORD)AMI_NETSTATUS_VERSION;
            ctl.nsc_Index   = (UWORD)index;

            if (s_netstackcontrol(base, NETCTRL_DHCP_RENEW, &ctl) == 0)
                st_result.sr_Renewed++;
        }
    }
    st_phase(4);

    Delay(50 * 5);

    (VOID)s_gethostbyname(base, "no-such-host.invalid");
    st_phase(5);

    (VOID)s_gethostbyname(base, "localhost");
    st_phase(6);
    (VOID)s_gethostbyname(base, "no-such-host.invalid");
    st_phase(7);
    (VOID)s_gethostbyname(base, "no-such-host.local");
    st_phase(8);
    st_check(1, "gethostbyname reached the resolver three ways", 0);

    {
        UBYTE quad[4];

        quad[0] = 127; quad[1] = 0; quad[2] = 0; quad[3] = 1;
        (VOID)s_gethostbyaddr(base, quad, 4, S_AF_INET);
        st_check(1, "gethostbyaddr returned", 0);
    }

    (VOID)s_getservbyname(base, "telnet", "tcp");
    st_phase(9);
    st_check(1, "getservbyname returned", 0);

    {
        APTR list = NULL;

        (VOID)s_getaddrinfo(base, "no-such-host.invalid", NULL, NULL, &list);
        st_phase(10);
        st_check(1, "getaddrinfo returned", 0);
    }

    {
        char   host[64];
        char   serv[16];

        for (i = 0; i < sizeof(sa); i++)
            ((UBYTE *)&sa)[i] = 0;
        sa.sin_len    = (UBYTE)sizeof(sa);
        sa.sin_family = S_AF_INET;
        sa.sin_port   = (UWORD)23;
        sa.sin_addr   = 0xC0000201UL;           /* 192.0.2.1 */

        (VOID)s_getnameinfo(base, &sa, (LONG)sizeof(sa), host,
                            (LONG)sizeof(host), serv, (LONG)sizeof(serv), 0);
        st_phase(11);
        st_check(1, "getnameinfo returned", 0);
    }

    CloseLibrary(base);
    st_phase(12);

    st_result.sr_Deepest  = st_mark();
    st_result.sr_Finished = 1;

    Signal(st_parent, ST_DONE_MASK);
}

int main(int argc, char **argv)
{
    struct Process *peer;
    struct Process *worker;
    ULONG           waited = 0;

    (VOID)argc;
    (VOID)argv;

    st_parent = FindTask(NULL);
    st_result.sr_Asked = ST_STACK;
    st_result.sr_Phase = -1;            /* not even phase 0 yet */

    st_log("stack: the API from %lu bytes, which is the Shell default\n",
           (LONG)ST_STACK);

    SetSignal(0, ST_DONE_MASK);

    peer = CreateNewProcTags(NP_Entry,      (ULONG)st_peer_entry,
                             NP_Name,       (ULONG)"aminetxduo stack peer",
                             NP_StackSize,  ST_PEER_STACK,
                             NP_Priority,   0,
                             TAG_DONE);
    st_check(peer != NULL, "the loopback peer starts", 0);
    if (peer == NULL)
        return RETURN_FAIL;

    while ((SetSignal(0UL, 0UL) & ST_DONE_MASK) == 0)
    {
        Delay(10);
        waited += 10;

        if (waited >= ST_TIMEOUT_TICKS)
            break;
    }
    waited = 0;

    st_check(st_peer_port > 0, "the peer is listening", st_peer_port);
    if (st_peer_port <= 0)
    {
        st_peer_stop = 1;
        Delay(100);
        return RETURN_FAIL;
    }

    SetSignal(0, ST_DONE_MASK);

    worker = CreateNewProcTags(NP_Entry,     (ULONG)st_worker_entry,
                               NP_Name,      (ULONG)"aminetxduo stack worker",
                               NP_StackSize, ST_STACK,
                               NP_Priority,  0,
                               TAG_DONE);
    st_check(worker != NULL, "the 4 KB worker starts", 0);
    if (worker == NULL)
    {
        st_peer_stop = 1;
        Delay(100);
        return RETURN_FAIL;
    }

    while ((SetSignal(0UL, 0UL) & ST_DONE_MASK) == 0)
    {
        Delay(10);
        waited += 10;

        if (waited >= ST_TIMEOUT_TICKS)
            break;
    }

    st_peer_stop = 1;

    st_check((SetSignal(0UL, 0UL) & ST_DONE_MASK) != 0,
             "the worker came back", (LONG)waited);
    st_check(st_result.sr_Finished != 0, "the worker ran to the end", 0);

    st_log("\nmark after each phase (bytes of stack used):\n");
    {
        LONG ph;

        for (ph = 0; ph < ST_PHASES; ph++)
        {
            if (ph > st_result.sr_Phase)
            {
                st_log("  ----  %s   NOT REACHED\n", (LONG)st_phase_name[ph]);
                continue;
            }
            st_log("  %4lu  %s\n", (LONG)st_result.sr_Mark[ph],
                   (LONG)st_phase_name[ph]);
        }
    }

    Delay(150);

    st_log("\n");
    st_log("stack asked for %lu bytes\n", (LONG)st_result.sr_Asked);
    st_log("stack granted   %lu bytes\n", (LONG)st_result.sr_Stack);
    st_log("deepest touched %lu bytes\n", (LONG)st_result.sr_Deepest);
    st_log("still unpainted %lu bytes of headroom\n",
           (LONG)st_result.sr_Untouched);
    st_log("bytes echoed    %ld\n", st_result.sr_Bytes);
    st_log("dhcp renewed    %ld interface(s)\n", st_result.sr_Renewed);

    if (st_result.sr_Stack > st_result.sr_Asked)
        st_log("note: this ROM rounded the request up, so the headroom below "
               "is against %lu, not %lu\n",
               (LONG)st_result.sr_Stack, (LONG)st_result.sr_Asked);

    st_check(st_result.sr_Deepest < st_result.sr_Stack,
             "the mark fits the stack that was granted",
             (LONG)st_result.sr_Deepest);

    st_check(st_result.sr_Deepest < ST_STACK,
             "the mark fits 4 KB, which is what a Shell command has",
             (LONG)st_result.sr_Deepest);

    {
        ULONG spare = (st_result.sr_Deepest < ST_STACK)
                          ? (ST_STACK - st_result.sr_Deepest) : 0;

        st_check(spare >= (ST_STACK / 8),
                 "an eighth of a Shell stack was still spare", (LONG)spare);
    }

    if (st_result.sr_Renewed == 0)
    {
        st_log("\nnote: no interface accepted NETCTRL_DHCP_RENEW, so the "
               "absorb was not provoked and phase 5 is not a measurement of "
               "it\n");
    }
    else if (st_result.sr_Phase >= 5)
    {
        ULONG mark  = st_result.sr_Mark[5];
        ULONG spare = (mark < ST_STACK) ? (ST_STACK - mark) : 0;

        st_log("\nthe lookup that absorbed a pending lease touched %lu "
               "bytes\n", (LONG)mark);
        st_check(spare >= (ST_STACK / 8),
                 "an eighth of a Shell stack was spare with a lease pending",
                 (LONG)spare);
    }

    st_log("\nstack: %ld checks, %ld failures\n",
           st_result.sr_Checks, st_result.sr_Failures);

    return (st_result.sr_Failures == 0) ? RETURN_OK : RETURN_FAIL;
}
