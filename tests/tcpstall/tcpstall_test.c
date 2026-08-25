/*
 * AmiNetXDuo, whether a stalled TCP connection can be seen from outside the
 * stack, and whether an application can decline to wait out the ladder.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "aminetxduo/netstatus.h"
#include "aminetxduo/tcp.h"

static const char version_tag[] __attribute__((used)) =
    "$VER: tcpstall_test 1.0 (12.8.2026)";

#define T_AF_INET       2
#define T_SOCK_STREAM   1
#define T_SOCK_DGRAM    2
#define T_IPPROTO_TCP   6
#define T_FIONBIO       0x8004667EUL

#define T_EWOULDBLOCK   35
#define T_ENOPROTOOPT   42

typedef struct TAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} TAddr;

static LONG t_socket(struct Library *base, LONG dom, LONG type, LONG proto)
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

static LONG t_connect(struct Library *base, LONG s, TAddr *sa)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = sa;
    register LONG            d1  __asm("d1") = (LONG)sizeof(TAddr);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG t_send(struct Library *base, LONG s, APTR buf, LONG len)
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

static LONG t_setsockopt(struct Library *base, LONG s, LONG level, LONG name,
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

static LONG t_getsockopt(struct Library *base, LONG s, LONG level, LONG name,
                         APTR val, LONG *len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            d1  __asm("d1") = level;
    register LONG            d2  __asm("d2") = name;
    register APTR            a0  __asm("a0") = val;
    register APTR            a1  __asm("a1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-96:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG t_ioctl(struct Library *base, LONG s, ULONG req, APTR argp)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register ULONG           d1  __asm("d1") = req;
    register APTR            a0  __asm("a0") = argp;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-114:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG t_close(struct Library *base, LONG s)
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

static LONG t_errno(struct Library *base)
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

static LONG t_control(struct Library *base, ULONG op, NetStatusControl *ctl)
{
    register struct Library *a6  __asm("a6") = base;
    register ULONG           d0  __asm("d0") = AMI_NETSTATUS_MAGIC;
    register ULONG           d1  __asm("d1") = op;
    register APTR            a0  __asm("a0") = ctl;
    register ULONG           d2  __asm("d2") = sizeof(NetStatusControl);
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-876:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (a0), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static ULONG t_checks;
static ULONG t_failures;

static VOID t_say(const char *fmt, LONG a, LONG b, LONG c)
{
    LONG args[3];

    args[0] = a;
    args[1] = b;
    args[2] = c;

    VPrintf((CONST_STRPTR)fmt, args);
    Flush(Output());
}

static VOID t_check(LONG ok, const char *what)
{
    t_checks++;

    if (!ok)
    {
        t_failures++;
        t_say("FAIL %s\n", (LONG)what, 0, 0);
    }
}

/* Hundredths of a second since the first call, from the system clock. */
static ULONG t_elapsed(VOID)
{
    static struct DateStamp base;
    static LONG             started;
    struct DateStamp        now;
    LONG                    minutes;
    LONG                    ticks;

    DateStamp(&now);

    if (!started)
    {
        base    = now;
        started = 1;
        return 0;
    }

    minutes = (now.ds_Days - base.ds_Days) * 1440 +
              (now.ds_Minute - base.ds_Minute);
    ticks   = now.ds_Tick - base.ds_Tick;

    return (ULONG)(minutes * 6000L + ticks * 2L);
}

#define T_SECS(hundredths)  ((LONG)((hundredths) / 100UL))

static LONG t_blackhole(struct Library *base, ULONG peer, LONG on)
{
    NetStatusControl ctl;
    UBYTE           *m = ctl.nsc_HwAddress;
    ULONG            i;

    for (i = 0; i < sizeof(ctl); i++)
        ((UBYTE *)&ctl)[i] = 0;

    ctl.nsc_Magic       = AMI_NETSTATUS_MAGIC;
    ctl.nsc_Version     = (UWORD)AMI_NETSTATUS_VERSION;
    ctl.nsc_Destination = peer;

    if (t_control(base, NETCTRL_ARP_DELETE, &ctl) < 0 && on)
    {
        /* Nothing cached yet is not a failure to blackhole. */
    }

    if (!on)
        return 0;

    m[0] = 0x02; m[1] = 0x00; m[2] = 0x00;
    m[3] = 0x00; m[4] = 0x00; m[5] = 0xFE;

    return t_control(base, NETCTRL_ARP_ADD, &ctl);
}

typedef struct TConn
{
    const char *name;
    LONG        fd;
    ULONG       deadline_ms;        /* TCP_USER_TIMEOUT, 0 = default        */
    LONG        failed_at;          /* seconds, -1 while alive              */
    LONG        failed_errno;
    ULONG       peak_stall;         /* ms, the last reading before it died  */
    ULONG       peak_retx;
} TConn;

static LONG t_open(struct Library *base, TConn *c, TAddr *sa)
{
    LONG one = 1;
    LONG len;
    LONG value;

    c->failed_at = -1;

    c->fd = t_socket(base, T_AF_INET, T_SOCK_STREAM, 0);
    if (c->fd < 0)
        return -1;

    if (c->deadline_ms != 0)
    {
        value = (LONG)c->deadline_ms;
        if (t_setsockopt(base, c->fd, T_IPPROTO_TCP, TCP_USER_TIMEOUT,
                         &value, sizeof(value)) < 0)
            return -1;
    }

    /* Read it back before the connection exists: an option that is stored and
       answered proves nothing on its own, but one that is not stored has
       already failed. */
    value = -1;
    len   = sizeof(value);
    t_check(t_getsockopt(base, c->fd, T_IPPROTO_TCP, TCP_USER_TIMEOUT,
                         &value, &len) == 0 &&
            value == (LONG)c->deadline_ms,
            "TCP_USER_TIMEOUT did not read back as it was set");

    if (t_connect(base, c->fd, sa) < 0)
        return -1;

    /* Non-blocking, so a send into a queue that cannot drain reports rather
       than parks this program for the length of the stall it is measuring. */
    (VOID)t_ioctl(base, c->fd, T_FIONBIO, &one);

    return 0;
}

/* One second's worth of work on a connection: offer data, read the stall. */
static VOID t_poll(struct Library *base, TConn *c, LONG now, UBYTE *chunk,
                   LONG chunk_len, LONG verbose)
{
    /* Zeroed because the reads below are guarded by a getsockopt this file
       cannot prove fills it. */
    struct TcpStallInfo info = { 0 };
    LONG                len = sizeof(info);
    LONG                n;

    if (c->failed_at >= 0)
        return;

    n = t_send(base, c->fd, chunk, chunk_len);

    if (n < 0)
    {
        LONG e = t_errno(base);

        if (e != T_EWOULDBLOCK)
        {
            c->failed_at    = now;
            c->failed_errno = e;
            t_say("  %s failed at %lds, errno %ld\n",
                  (LONG)c->name, now, e);
            return;
        }
    }

    if (t_getsockopt(base, c->fd, T_IPPROTO_TCP, TCP_STALLINFO,
                     &info, &len) < 0)
        return;

    c->peak_stall = info.tsi_Stalled;
    c->peak_retx  = info.tsi_Retransmits;

    if (verbose)
    {
        t_say("  %s stalled %lums", (LONG)c->name, (LONG)info.tsi_Stalled, 0);
        t_say(" retx %lu rto %lums deadline %lums\n",
              (LONG)info.tsi_Retransmits, (LONG)info.tsi_Rto,
              (LONG)info.tsi_UserTimeout);
    }
}

static VOID t_arm_options(struct Library *base)
{
    struct TcpStallInfo info = { 0 };
    LONG                len;
    LONG                value;
    LONG                s;

    t_say("-- arm 1: the options, with no network under them\n", 0, 0, 0);

    s = t_socket(base, T_AF_INET, T_SOCK_STREAM, 0);
    t_check(s >= 0, "could not make a TCP socket");
    if (s < 0)
        return;

    value = -1;
    len   = sizeof(value);
    t_check(t_getsockopt(base, s, T_IPPROTO_TCP, TCP_USER_TIMEOUT,
                         &value, &len) == 0 && value == 0,
            "a socket that asked for nothing did not answer a zero deadline");

    len = sizeof(info);
    t_check(t_getsockopt(base, s, T_IPPROTO_TCP, TCP_STALLINFO,
                         &info, &len) == 0,
            "TCP_STALLINFO would not answer");
    t_check(len == (LONG)sizeof(info),
            "TCP_STALLINFO answered the wrong length");
    t_check(info.tsi_Stalled == 0 && info.tsi_Retransmits == 0,
            "an unconnected socket reported a stall");

    value = 30000;
    t_check(t_setsockopt(base, s, T_IPPROTO_TCP, TCP_USER_TIMEOUT,
                         &value, sizeof(value)) == 0,
            "TCP_USER_TIMEOUT would not be set");

    value = -1;
    len   = sizeof(value);
    t_check(t_getsockopt(base, s, T_IPPROTO_TCP, TCP_USER_TIMEOUT,
                         &value, &len) == 0 && value == 30000,
            "TCP_USER_TIMEOUT did not answer what was set");

    len = sizeof(info);
    t_check(t_getsockopt(base, s, T_IPPROTO_TCP, TCP_STALLINFO,
                         &info, &len) == 0 && info.tsi_UserTimeout == 30000,
            "TCP_STALLINFO does not report the deadline");

    value = -1;
    t_check(t_setsockopt(base, s, T_IPPROTO_TCP, TCP_USER_TIMEOUT,
                         &value, sizeof(value)) < 0,
            "a negative deadline was accepted");

    (VOID)t_close(base, s);

    /* TCP options on a UDP socket. */
    s = t_socket(base, T_AF_INET, T_SOCK_DGRAM, 0);
    if (s >= 0)
    {
        value = 1000;
        t_check(t_setsockopt(base, s, T_IPPROTO_TCP, TCP_USER_TIMEOUT,
                             &value, sizeof(value)) < 0 &&
                t_errno(base) == T_ENOPROTOOPT,
                "a UDP socket accepted TCP_USER_TIMEOUT");

        len = sizeof(info);
        t_check(t_getsockopt(base, s, T_IPPROTO_TCP, TCP_STALLINFO,
                             &info, &len) < 0,
                "a UDP socket answered TCP_STALLINFO");

        (VOID)t_close(base, s);
    }
}

#define T_CHUNK     512
#define T_LIMIT     150     /* seconds; the default ladder ends at 127       */

static VOID t_arm_stall(struct Library *base, ULONG peer, UWORD port)
{
    static UBYTE chunk[T_CHUNK];
    TConn        a;
    TAddr        sa;
    TConn        b;
    LONG         netstat_done = 0;
    LONG         i;

    t_say("-- arm 2: a peer that stops answering\n", 0, 0, 0);

    for (i = 0; i < T_CHUNK; i++)
        chunk[i] = (UBYTE)'x';

    for (i = 0; i < (LONG)sizeof(sa); i++)
        ((UBYTE *)&sa)[i] = 0;

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = T_AF_INET;
    sa.sin_port   = port;
    sa.sin_addr   = peer;

    a.name = "default   ";
    a.deadline_ms = 0;
    b.name = "deadline15";
    b.deadline_ms = 15000;

    t_check(t_open(base, &a, &sa) == 0, "the default socket would not connect");
    t_check(t_open(base, &b, &sa) == 0, "the deadline socket would not connect");

    if (a.failed_at != -1 || b.failed_at != -1 || a.fd < 0 || b.fd < 0)
    {
        t_say("  no peer at the address given; nothing to stall\n", 0, 0, 0);
        t_failures++;
        return;
    }

    /* Enough in flight that the retransmit timer is armed on both. */
    (VOID)t_send(base, a.fd, chunk, T_CHUNK);
    (VOID)t_send(base, b.fd, chunk, T_CHUNK);

    Delay(50);

    t_check(t_blackhole(base, peer, 1) == 0,
            "the static ARP entry that induces the stall would not go in");

    t_say("  peer blackholed at the ARP cache\n", 0, 0, 0);

    (VOID)t_elapsed();

    for (i = 0; i < T_LIMIT; i++)
    {
        LONG now;
        LONG verbose;

        Delay(50);

        now     = T_SECS(t_elapsed());
        verbose = (now < 6 || (now % 15) == 0) ? 1 : 0;

        t_poll(base, &a, now, chunk, T_CHUNK, verbose);
        t_poll(base, &b, now, chunk, T_CHUNK, verbose);

        /* What a user sees, from the shipped command, while it is happening.
           Once, well inside the deadline socket's life. */
        if (!netstat_done && now >= 8)
        {
            netstat_done = 1;
            t_say("  ---- netstat -a, 8 s into the stall ----\n", 0, 0, 0);
            Flush(Output());
            (VOID)SystemTagList((CONST_STRPTR)"SYS:netstat -a", NULL);
            t_say("  ---- end netstat ----\n", 0, 0, 0);
        }

        if (a.failed_at >= 0 && b.failed_at >= 0)
            break;
    }

    (VOID)t_blackhole(base, peer, 0);
    t_say("  blackhole removed\n", 0, 0, 0);

    (VOID)t_close(base, a.fd);
    (VOID)t_close(base, b.fd);

    t_check(b.failed_at >= 0, "the socket with a 15 s deadline never failed");
    t_check(b.failed_at >= 13 && b.failed_at <= 22,
            "a 15 s deadline was not served within a few seconds of 15");
    t_check(a.failed_at < 0 || a.failed_at > 100,
            "the default socket gave up early: behaviour changed for a caller"
            " that asked for nothing");
    t_check(b.peak_retx > 0,
            "the retransmit count stayed at zero through a stall");
    t_check(b.peak_stall >= 5000,
            "the stall clock never reported a stall");

    t_say("  default   failed_at=%lds errno=%ld\n",
          (LONG)a.failed_at, (LONG)a.failed_errno, 0);
    t_say("  deadline15 failed_at=%lds errno=%ld\n",
          (LONG)b.failed_at, (LONG)b.failed_errno, 0);
}

static ULONG t_parse_ip(const char **p)
{
    ULONG addr = 0;
    LONG  part = 0;
    LONG  seen = 0;
    const char *s = *p;

    for (;;)
    {
        if (*s >= '0' && *s <= '9')
        {
            part = part * 10 + (*s - '0');
            seen = 1;
            s++;
        }
        else if (*s == '.' && seen)
        {
            addr = (addr << 8) | (ULONG)(part & 0xFF);
            part = 0;
            seen = 0;
            s++;
        }
        else
        {
            break;
        }
    }

    addr = (addr << 8) | (ULONG)(part & 0xFF);
    *p   = s;

    return addr;
}

int main(VOID)
{
    struct Library *base;
    const char     *args = (const char *)GetArgStr();
    ULONG           peer = 0;
    ULONG           port = 5001;

    t_say("tcpstall: is a stalled connection visible, and can it be cut short\n",
          0, 0, 0);

    if (args != NULL)
    {
        while (*args == ' ' || *args == '\t')
            args++;

        if (*args >= '0' && *args <= '9')
        {
            peer = t_parse_ip(&args);

            while (*args == ' ' || *args == '\t')
                args++;

            if (*args >= '0' && *args <= '9')
            {
                port = 0;
                while (*args >= '0' && *args <= '9')
                    port = port * 10 + (ULONG)(*args++ - '0');
            }
        }
    }

    if (peer == 0)
    {
        t_say("tcpstall: usage: tcpstall_test <peer-address> [port]\n", 0, 0, 0);
        return RETURN_ERROR;
    }

    t_say("  peer %lu.%lu.", (LONG)((peer >> 24) & 0xFF),
          (LONG)((peer >> 16) & 0xFF), 0);
    t_say("%lu.%lu port %lu\n", (LONG)((peer >> 8) & 0xFF),
          (LONG)(peer & 0xFF), (LONG)port);

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        t_say("tcpstall: no bsdsocket.library\n", 0, 0, 0);
        return RETURN_FAIL;
    }

    t_arm_options(base);
    t_arm_stall(base, peer, (UWORD)port);

    CloseLibrary(base);

    t_say("tcpstall: %lu checks, %lu failures, %s\n",
          (LONG)t_checks, (LONG)t_failures,
          (LONG)((t_failures == 0UL) ? "PASS" : "FAIL"));

    return (t_failures == 0UL) ? RETURN_OK : RETURN_ERROR;
}
