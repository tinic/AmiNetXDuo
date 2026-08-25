/*
 * AmiNetXDuo, which datagrams a UDP socket is allowed to receive.
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

#include "tapdev.h"


/* DEVS:NetInterfaces/tap0 gives the stack 10.9.9.1/24; 10.9.9.2 is this
   harness.  10.9.9.3 is a second host on the same wire, the one that has no
   business answering a socket connected to 10.9.9.2. */
#define LOCAL_IP        0x0A090901UL
#define PEER_IP         0x0A090902UL
#define OTHER_IP        0x0A090903UL
#define LOOPBACK_IP     0x7F000001UL

#define PEER_PORT       9000
#define OTHER_PORT      9001
#define LOCAL_PORT      4801

#define ETYPE_IP        0x0800
#define ETYPE_ARP       0x0806
#define ETH_HDR         14

#define ICMP_UNREACH        3
#define ICMP_PORT_UNREACH   3
#define ICMP_HOST_UNREACH   1

static const UBYTE local_mac[6] = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x09 };
static const UBYTE peer_mac[6]  = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x0A };


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
            t_log_buffer[t_log_used++] = (char)c;
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
        t_log_buffer[t_log_used++] = '\n';
}

static VOID t_flush(VOID)
{
BPTR out;

    out = Output();
    if (out != (BPTR)0)
        (VOID)Write(out, (APTR)t_log_buffer, (LONG)t_log_used);
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
        *b++ = 0;
}


#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <netinet/in.h>

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
register LONG _s_d1 __asm("d1");
register LONG _s_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-72:W)"
                      : "=r" (_s_d1), "=r" (_s_a0), "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (a2)
                      : "cc", "memory");
    return(res);
}

/* recv(), the vector recvfrom() is not: bsd_recv() has its own zero-length
   handling in transfer.c, so a case that only ever called recvfrom() would
   leave half of it untested. */
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
                      : BSD_SCRATCH_OUT, "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
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
register LONG _s_d1 __asm("d1");
register LONG _s_a0 __asm("a0");
register LONG _s_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (_s_d1), "=r" (_s_a0), "=r" (_s_a1), "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2),
                        "r" (a1), "r" (d3)
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
register LONG _s_d1 __asm("d1");
register LONG _s_a0 __asm("a0");
register LONG _s_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-66:W)"
                      : "=r" (_s_d1), "=r" (_s_a0), "=r" (_s_a1), "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
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
register LONG _s_d1 __asm("d1");
register LONG _s_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-96:W)"
                      : "=r" (_s_d1), "=r" (_s_a0), "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (a1)
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
register LONG _s_d1 __asm("d1");
register LONG _s_a0 __asm("a0");
register LONG _s_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (_s_d1), "=r" (_s_a0), "=r" (_s_a1), "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
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


static VOID wr16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)(v & 0xFF);
}

static VOID wr32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);
    p[3] = (UBYTE)(v & 0xFF);
}

static ULONG ones_partial(const UBYTE *p, ULONG len, ULONG sum)
{
ULONG i;

    for (i = 0; i + 1 < len; i += 2)
        sum += (ULONG)(((ULONG)p[i] << 8) | p[i + 1]);
    if (i < len)
        sum += (ULONG)((ULONG)p[i] << 8);

    return(sum);
}

static UWORD ones_fold(ULONG sum)
{
    while ((sum >> 16) != 0UL)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    return((UWORD)(~sum));
}

static BOOL t_inject(ULONG src_ip, UWORD src_port, UBYTE tag, ULONG len)
{
static UBYTE f[TAP_FRAME_MAX];
UBYTE       *ip  = &f[ETH_HDR];
UBYTE       *udp = ip + 20;
ULONG        iplen = 20UL + 8UL + len;
ULONG        i;
ULONG        sum;
UBYTE        ph[4];

    t_bzero(f, (ULONG)sizeof(f));

    for (i = 0; i < 6UL; i++)
    {
        f[i]     = local_mac[i];
        f[6 + i] = peer_mac[i];
    }
    wr16(&f[12], ETYPE_IP);

    ip[0] = 0x45;
    wr16(&ip[2], (UWORD)iplen);
    wr16(&ip[4], 0x5000);
    wr16(&ip[6], 0x4000);               /* DF */
    ip[8] = 64;
    ip[9] = 17;                         /* UDP */
    wr32(&ip[12], src_ip);
    wr32(&ip[16], LOCAL_IP);
    wr16(&ip[10], ones_fold(ones_partial(ip, 20UL, 0UL)));

    wr16(&udp[0], src_port);
    wr16(&udp[2], (UWORD)LOCAL_PORT);
    wr16(&udp[4], (UWORD)(8UL + len));

    for (i = 0; i < len; i++)
        udp[8 + i] = (UBYTE)(tag + (UBYTE)i);

    sum = ones_partial(&ip[12], 8UL, 0UL);
    ph[0] = 0;
    ph[1] = 17;
    wr16(&ph[2], (UWORD)(8UL + len));
    sum = ones_partial(ph, 4UL, sum);
    sum = ones_partial(udp, 8UL + len, sum);
    wr16(&udp[6], ones_fold(sum));

    return((BOOL)(tap_rx_put(f, ETH_HDR + iplen) == 0));
}


static BOOL t_inject_proto(ULONG src_ip, UBYTE proto, UBYTE tag, ULONG len)
{
static UBYTE f[TAP_FRAME_MAX];
UBYTE       *ip  = &f[ETH_HDR];
ULONG        iplen = 20UL + len;
ULONG        i;

    t_bzero(f, (ULONG)sizeof(f));

    for (i = 0; i < 6UL; i++)
    {
        f[i]     = local_mac[i];
        f[6 + i] = peer_mac[i];
    }
    wr16(&f[12], ETYPE_IP);

    ip[0] = 0x45;
    wr16(&ip[2], (UWORD)iplen);
    wr16(&ip[4], 0x7000);
    wr16(&ip[6], 0x4000);               /* DF */
    ip[8] = 64;
    ip[9] = proto;
    wr32(&ip[12], src_ip);
    wr32(&ip[16], LOCAL_IP);
    wr16(&ip[10], ones_fold(ones_partial(ip, 20UL, 0UL)));

    for (i = 0; i < len; i++)
        ip[20 + i] = (UBYTE)(tag + (UBYTE)i);

    return((BOOL)(tap_rx_put(f, ETH_HDR + iplen) == 0));
}


static UWORD rd16(const UBYTE *p)
{
    return((UWORD)(((UWORD)p[0] << 8) | p[1]));
}

static ULONG rd32(const UBYTE *p)
{
    return(((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3]);
}

static VOID t_arp_reply(const UBYTE *req)
{
UBYTE        f[42];
const UBYTE *a = &req[ETH_HDR];
ULONG        i;

    t_bzero(f, (ULONG)sizeof(f));

    for (i = 0; i < 6UL; i++)
    {
        f[i]     = req[6 + i];
        f[6 + i] = peer_mac[i];
    }
    wr16(&f[12], ETYPE_ARP);

    wr16(&f[14], 1);                    /* Ethernet */
    wr16(&f[16], ETYPE_IP);
    f[18] = 6;
    f[19] = 4;
    wr16(&f[20], 2);                    /* reply    */
    for (i = 0; i < 6UL; i++)
        f[22 + i] = peer_mac[i];
    wr32(&f[28], PEER_IP);
    for (i = 0; i < 6UL; i++)
        f[32 + i] = a[8 + i];           /* their MAC */
    for (i = 0; i < 4UL; i++)
        f[38 + i] = a[14 + i];          /* their IP  */

    (VOID)tap_rx_put(f, (ULONG)sizeof(f));
}

static ULONG t_pump(UBYTE *out, ULONG max)
{
static UBYTE scratch[TAP_FRAME_MAX];
ULONG        stamp;
ULONG        len;
ULONG        got = 0;
ULONG        i;

    while ((len = tap_tx_get(scratch, (ULONG)sizeof(scratch), &stamp)) != 0)
    {
        if (rd16(&scratch[12]) == ETYPE_ARP)
        {
            if (len >= 42UL && rd16(&scratch[20]) == 1 &&
                rd32(&scratch[38]) == PEER_IP)
            {
                t_arp_reply(scratch);
            }
            continue;
        }

        if (got != 0UL || rd16(&scratch[12]) != ETYPE_IP)
            continue;

        if (len < ETH_HDR + 28UL || scratch[ETH_HDR + 9] != 17)
            continue;

        if (rd32(&scratch[ETH_HDR + 16]) != PEER_IP ||
            rd16(&scratch[ETH_HDR + 20]) != (UWORD)LOCAL_PORT)
            continue;

        got = (len > max) ? max : len;
        for (i = 0; i < got; i++)
            out[i] = scratch[i];
    }

    return(got);
}

static ULONG t_wait_tx(UBYTE *out, ULONG max, ULONG ms)
{
ULONG spent = 0;
ULONG len;

    for (;;)
    {
        len = t_pump(out, max);
        if (len != 0UL)
            return(len);
        if (spent >= ms)
            return(0);
        Delay(1);                       /* 20 ms */
        spent += 20UL;
    }
}

static BOOL t_inject_icmp(ULONG src_ip, UBYTE type, UBYTE code,
                          const UBYTE *frame, ULONG frame_len)
{
static UBYTE f[TAP_FRAME_MAX];
UBYTE       *ip   = &f[ETH_HDR];
UBYTE       *icmp = ip + 20;
const UBYTE *q    = &frame[ETH_HDR];
ULONG        qhdr;
ULONG        quoted;
ULONG        iplen;
ULONG        i;

    if (frame_len < ETH_HDR + 28UL)
        return(FALSE);

    qhdr = (ULONG)(q[0] & 0x0F) * 4UL;
    if (qhdr < 20UL || frame_len < ETH_HDR + qhdr + 8UL)
        return(FALSE);

    quoted = qhdr + 8UL;
    iplen  = 20UL + 8UL + quoted;

    t_bzero(f, (ULONG)sizeof(f));

    for (i = 0; i < 6UL; i++)
    {
        f[i]     = local_mac[i];
        f[6 + i] = peer_mac[i];
    }
    wr16(&f[12], ETYPE_IP);

    ip[0] = 0x45;
    wr16(&ip[2], (UWORD)iplen);
    wr16(&ip[4], 0x6000);
    wr16(&ip[6], 0x0000);
    ip[8] = 64;
    ip[9] = 1;                          /* ICMP */
    wr32(&ip[12], src_ip);
    wr32(&ip[16], LOCAL_IP);
    wr16(&ip[10], ones_fold(ones_partial(ip, 20UL, 0UL)));

    icmp[0] = type;
    icmp[1] = code;
    wr16(&icmp[2], 0);
    wr32(&icmp[4], 0);                  /* unused, and no next-hop MTU */

    for (i = 0; i < quoted; i++)
        icmp[8 + i] = q[i];

    wr16(&icmp[2], ones_fold(ones_partial(icmp, 8UL + quoted, 0UL)));

    return((BOOL)(tap_rx_put(f, ETH_HDR + iplen) == 0));
}


typedef struct sockaddr_in SockAddrIn;

static VOID t_addr(SockAddrIn *a, ULONG ip, UWORD port)
{
    t_bzero(a, (ULONG)sizeof(*a));
    a->sin_len    = (UBYTE)sizeof(*a);
    a->sin_family = AF_INET;
    a->sin_port   = port;
    a->sin_addr.s_addr = ip;
}

static VOID t_settle(VOID)
{
    Delay(5);
}

static LONG t_drain(LONG fd, UBYTE *tag_out)
{
static UBYTE buf[64];
LONG         n;

    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    if (n > 0 && tag_out != NULL)
        *tag_out = buf[0];

    return(n);
}

static VOID t_case_unconnected(VOID)
{
LONG        fd;
SockAddrIn  a;
LONG        one = 1;
UBYTE       tag = 0;

    t_log("");
    t_log("u01. an unconnected socket takes a datagram from anybody");

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return;

    (VOID)bsd_IoctlSocket(fd, FIONBIO, &one);

    t_addr(&a, 0UL, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0), "bind",
                 bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    t_settle();
    (VOID)t_check((BOOL)t_inject(OTHER_IP, OTHER_PORT, 0xA0, 8UL),
                  "a datagram from an unrelated host was injected", 0);
    t_settle();

    (VOID)t_check((BOOL)(t_drain(fd, &tag) == 8),
                  "recvfrom() returned it, no peer was named", bsd_Errno());
    (VOID)t_check((BOOL)(tag == 0xA0), "and it is the datagram that was sent",
                  (LONG)tag);

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_case_connected(VOID)
{
LONG        fd;
SockAddrIn  a;
LONG        one = 1;
UBYTE       tag = 0;
LONG        n;

    t_log("");
    t_log("u02. a connected socket takes datagrams from its peer only "
          "(RFC 1122 4.1.3.5)");

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return;

    (VOID)bsd_IoctlSocket(fd, FIONBIO, &one);

    t_addr(&a, 0UL, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0), "bind",
                 bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    t_addr(&a, PEER_IP, (UWORD)PEER_PORT);
    if (!t_check((BOOL)(bsd_connect(fd, &a, (LONG)sizeof(a)) == 0),
                 "connect() to 10.9.9.2:9000", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xB0, 8UL),
                  "the peer sent one", 0);
    t_settle();

    (VOID)t_check((BOOL)(t_drain(fd, &tag) == 8),
                  "recvfrom() returned the peer's datagram", bsd_Errno());
    (VOID)t_check((BOOL)(tag == 0xB0), "and it is the one the peer sent",
                  (LONG)tag);

    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, OTHER_PORT, 0xC0, 8UL),
                  "10.9.9.2:9001 sent one", 0);
    t_settle();

    n = t_drain(fd, &tag);
    (VOID)t_check((BOOL)(n < 0),
                  "recvfrom() did not return a datagram from the wrong port",
                  n);

    t_settle();
    (VOID)t_check((BOOL)t_inject(OTHER_IP, PEER_PORT, 0xD0, 8UL),
                  "10.9.9.3:9000 sent one", 0);
    t_settle();

    n = t_drain(fd, &tag);
    (VOID)t_check((BOOL)(n < 0),
                  "recvfrom() did not return a datagram from the wrong host",
                  n);

    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xE0, 8UL),
                  "the peer sent another", 0);
    t_settle();

    (VOID)t_check((BOOL)(t_drain(fd, &tag) == 8),
                  "recvfrom() still returns the peer's datagram", bsd_Errno());
    (VOID)t_check((BOOL)(tag == 0xE0),
                  "and it is the peer's, not one of the impostors'",
                  (LONG)tag);

    (VOID)bsd_CloseSocket(fd);
}


#define T_SOL_SOCKET    0xFFFF
#define T_SO_ERROR      0x1007
#define T_SO_RCVTIMEO   0x1006

#define T_ECONNREFUSED  61
#define T_EWOULDBLOCK   35
#define T_EMSGSIZE      40

typedef struct { LONG tv_secs; LONG tv_micro; } TTimeval;

static VOID t_rcvtimeo(LONG fd, LONG secs)
{
TTimeval tv;

    tv.tv_secs  = secs;
    tv.tv_micro = 0;
    (VOID)bsd_setsockopt(fd, T_SOL_SOCKET, T_SO_RCVTIMEO, &tv, (LONG)sizeof(tv));
}

static VOID t_rcvtimeo_ms(LONG fd, LONG ms)
{
TTimeval tv;

    tv.tv_secs  = ms / 1000;
    tv.tv_micro = (ms % 1000) * 1000;
    (VOID)bsd_setsockopt(fd, T_SOL_SOCKET, T_SO_RCVTIMEO, &tv, (LONG)sizeof(tv));
}

static ULONG t_ms_since(ULONG t0)
{
ULONG rate = tap_eclock_rate();

    return((rate != 0UL) ? ((tap_eclock_now() - t0) / (rate / 1000UL)) : 0UL);
}

static LONG t_so_error(LONG fd)
{
LONG value = -1;
LONG len   = (LONG)sizeof(value);

    if (bsd_getsockopt(fd, T_SOL_SOCKET, T_SO_ERROR, &value, &len) != 0)
        return(-1);

    return(value);
}

static ULONG t_connected_send(LONG *fd_out, UBYTE *frame, ULONG max)
{
LONG        fd;
SockAddrIn  a;
UBYTE       msg[8];
ULONG       len;
ULONG       i;

    *fd_out = -1;

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return(0);

    t_addr(&a, 0UL, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0), "bind",
                 bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return(0);
    }

    t_addr(&a, PEER_IP, (UWORD)PEER_PORT);
    if (!t_check((BOOL)(bsd_connect(fd, &a, (LONG)sizeof(a)) == 0),
                 "connect() to 10.9.9.2:9000", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return(0);
    }

    for (i = 0; i < (ULONG)sizeof(msg); i++)
        msg[i] = (UBYTE)(0x10 + i);

    if (!t_check((BOOL)(bsd_send(fd, msg, (LONG)sizeof(msg), 0) == 8),
                 "send() 8 bytes to the peer", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return(0);
    }

    len = t_wait_tx(frame, max, 2000UL);
    if (!t_check((BOOL)(len != 0UL), "the datagram reached the wire",
                 (LONG)len))
    {
        (VOID)bsd_CloseSocket(fd);
        return(0);
    }

    *fd_out = fd;

    return(len);
}

static VOID t_case_icmp_refused(VOID)
{
static UBYTE frame[TAP_FRAME_MAX];
LONG         fd;
ULONG        len;
ULONG        rate, t0, t1, ms;
LONG         n;

    t_log("");
    t_log("u03. a port unreachable for a connected socket is ECONNREFUSED "
          "(RFC 1122 3.2.2.1, 4.1.3.3)");

    len = t_connected_send(&fd, frame, (ULONG)sizeof(frame));
    if (len == 0UL)
        return;

    (VOID)t_check((BOOL)t_inject_icmp(PEER_IP, ICMP_UNREACH, ICMP_PORT_UNREACH,
                                      frame, len),
                  "the peer answered port unreachable", 0);
    t_settle();

    t_rcvtimeo(fd, 4);

    rate = tap_eclock_rate();
    t0   = tap_eclock_now();
    n    = t_drain(fd, NULL);
    t1   = tap_eclock_now();

    ms = (rate != 0UL) ? (((t1 - t0) / (rate / 1000UL))) : 0UL;

    (VOID)t_check((BOOL)(n < 0), "recvfrom() failed rather than waiting", n);
    (VOID)t_check((BOOL)(bsd_Errno() == T_ECONNREFUSED),
                  "and the reason is ECONNREFUSED", bsd_Errno());
    (VOID)t_check((BOOL)(ms < 1000UL),
                  "it came back at once, not at the four-second timeout",
                  (LONG)ms);

    /* BSD reports an error once.  The recv consumed it, so SO_ERROR is clear
       and the next call is an ordinary empty-queue answer. */
    (VOID)t_check((BOOL)(t_so_error(fd) == 0),
                  "getsockopt(SO_ERROR) is clear -- the error was consumed",
                  t_so_error(fd));

    t_rcvtimeo(fd, 1);
    n = t_drain(fd, NULL);
    (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "and the next receive is an ordinary timeout",
                  bsd_Errno());

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_case_icmp_other_peer(VOID)
{
static UBYTE frame[TAP_FRAME_MAX];
LONG         fd;
ULONG        len;
LONG         n;

    t_log("");
    t_log("u04. an error naming a datagram sent to somebody else is not this "
          "socket's");

    len = t_connected_send(&fd, frame, (ULONG)sizeof(frame));
    if (len == 0UL)
        return;

    /* Rewrite the quoted datagram's destination to a host this socket never
       connected to.  The quoted header's own checksum is left alone: RFC 792
       does not ask a receiver to verify it, and nothing in the path does. */
    wr32(&frame[ETH_HDR + 16], OTHER_IP);

    (VOID)t_check((BOOL)t_inject_icmp(PEER_IP, ICMP_UNREACH, ICMP_PORT_UNREACH,
                                      frame, len),
                  "a port unreachable quoting a datagram to 10.9.9.3 arrived",
                  0);
    t_settle();

    t_rcvtimeo(fd, 1);
    n = t_drain(fd, NULL);

    (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "recvfrom() timed out rather than reporting it",
                  bsd_Errno());
    (VOID)t_check((BOOL)(t_so_error(fd) == 0),
                  "and SO_ERROR is clear", t_so_error(fd));

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_case_icmp_unconnected(VOID)
{
static UBYTE frame[TAP_FRAME_MAX];
LONG         fd;
SockAddrIn   a;
UBYTE        msg[8];
ULONG        len;
LONG         n;

    t_log("");
    t_log("u05. an unconnected socket has no peer to attribute an error to");

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return;

    t_addr(&a, 0UL, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0), "bind",
                 bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    t_bzero(msg, (ULONG)sizeof(msg));
    t_addr(&a, PEER_IP, (UWORD)PEER_PORT);
    if (!t_check((BOOL)(bsd_sendto(fd, msg, (LONG)sizeof(msg), 0, &a,
                                   (LONG)sizeof(a)) == 8),
                 "sendto() 8 bytes to the peer", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    len = t_wait_tx(frame, (ULONG)sizeof(frame), 2000UL);
    if (!t_check((BOOL)(len != 0UL), "the datagram reached the wire",
                 (LONG)len))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    (VOID)t_check((BOOL)t_inject_icmp(PEER_IP, ICMP_UNREACH, ICMP_PORT_UNREACH,
                                      frame, len),
                  "the peer answered port unreachable", 0);
    t_settle();

    t_rcvtimeo(fd, 1);
    n = t_drain(fd, NULL);

    (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "recvfrom() timed out -- the socket named no peer",
                  bsd_Errno());
    (VOID)t_check((BOOL)(t_so_error(fd) == 0),
                  "and SO_ERROR is clear", t_so_error(fd));

    (VOID)bsd_CloseSocket(fd);
}


static VOID t_case_bind_address(VOID)
{
LONG        fd;
SockAddrIn  a;
UBYTE       tag = 0;

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return;

    t_addr(&a, LOCAL_IP, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0),
                 "bind(10.9.9.1:4801)", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }
    t_rcvtimeo(fd, 1);

    t_settle();
    (VOID)t_check((BOOL)t_inject(OTHER_IP, OTHER_PORT, 0xF0, 8UL),
                  "inject a datagram addressed to 10.9.9.1", 0);
    t_settle();

    (VOID)t_check((BOOL)(t_drain(fd, &tag) == 8),
                  "the socket bound to the wire's address took it", bsd_Errno());
    (VOID)t_check((BOOL)(tag == 0xF0), "and it is the datagram that was sent",
                  (LONG)tag);

    (VOID)bsd_CloseSocket(fd);

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return;

    t_addr(&a, LOOPBACK_IP, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0),
                 "bind(127.0.0.1:4801)", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }
    t_rcvtimeo(fd, 1);

    t_settle();
    (VOID)t_check((BOOL)t_inject(OTHER_IP, OTHER_PORT, 0xF1, 8UL),
                  "inject the same datagram again", 0);
    t_settle();

    {
        LONG n = t_drain(fd, &tag);

        (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EWOULDBLOCK),
                      "a socket bound to 127.0.0.1 does not take it",
                      (n < 0) ? bsd_Errno() : n);
    }

    (VOID)bsd_CloseSocket(fd);
}

static VOID t_case_maxdgram(VOID)
{
static UBYTE big[1473];
static UBYTE frame[TAP_FRAME_MAX];
LONG         fd;
SockAddrIn   a;
ULONG        len;

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return;

    t_addr(&a, 0UL, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0), "bind",
                 bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    t_addr(&a, PEER_IP, (UWORD)PEER_PORT);
    if (!t_check((BOOL)(bsd_connect(fd, &a, (LONG)sizeof(a)) == 0),
                 "connect() to 10.9.9.2:9000", bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    /* 1500 - 20 - 8.  The first ARP is answered inside t_wait_tx(). */
    (VOID)t_check((BOOL)(bsd_send(fd, big, 1472, 0) == 1472),
                  "send(1472) to the 1500-byte interface is accepted",
                  bsd_Errno());

    len = t_wait_tx(frame, (ULONG)sizeof(frame), 2000UL);
    (VOID)t_check((BOOL)(len == (ULONG)(ETH_HDR + 20 + 8 + 1472)),
                  "and one whole frame carried it", (LONG)len);

    {
        LONG n = bsd_send(fd, big, 1473, 0);

        (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EMSGSIZE),
                      "send(1473) is EMSGSIZE", (n < 0) ? bsd_Errno() : n);
    }

    len = t_wait_tx(frame, (ULONG)sizeof(frame), 300UL);
    (VOID)t_check((BOOL)(len == 0UL),
                  "and the refused datagram reached no wire", (LONG)len);

    t_addr(&a, LOOPBACK_IP, (UWORD)PEER_PORT);
    {
        LONG n = bsd_sendto(fd, big, 1473, 0, &a, (LONG)sizeof(a));

        (VOID)t_check((BOOL)(n == 1473),
                      "sendto(1473) to 127.0.0.1 is not EMSGSIZE",
                      (n < 0) ? bsd_Errno() : n);
    }

    (VOID)bsd_CloseSocket(fd);
}

#define T_MSG_PEEK      0x02

static VOID t_case_datagram_boundary(VOID)
{
static UBYTE buf[256];
LONG         fd;
SockAddrIn   a;
LONG         n;

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return;

    t_addr(&a, 0UL, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0), "bind",
                 bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }
    t_rcvtimeo(fd, 1);

    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xA5, 200UL),
                  "inject a 200-byte datagram", 0);
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0x5A, 8UL),
                  "inject an 8-byte one behind it", 0);
    t_settle();

    n = bsd_recvfrom(fd, buf, 50, 0, NULL, NULL);
    (VOID)t_check((BOOL)(n == 50), "a 50-byte read of the 200 returns 50", n);
    (VOID)t_check((BOOL)(buf[0] == 0xA5), "and it is the first datagram",
                  (LONG)buf[0]);

    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n == 8), "the next read is the NEXT datagram, not the "
                                  "other 150 bytes", n);
    (VOID)t_check((BOOL)(buf[0] == 0x5A), "and it is the one behind it",
                  (LONG)buf[0]);

    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xC3, 16UL),
                  "inject a 16-byte datagram", 0);
    t_settle();

    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), T_MSG_PEEK, NULL, NULL);
    (VOID)t_check((BOOL)(n == 16 && buf[0] == 0xC3), "MSG_PEEK reads it", n);

    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n == 16 && buf[0] == 0xC3),
                  "and the read after it is the same datagram", n);

    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "which the peek did not duplicate", (n < 0) ? bsd_Errno() : n);

    (VOID)bsd_CloseSocket(fd);
}

#define T_ZERO_TIMEOUT_MS   2000
#define T_ZERO_PROMPT_MS    500

static VOID t_case_zero_read_udp(VOID)
{
static UBYTE buf[256];
LONG         fd;
SockAddrIn   a;
SockAddrIn   from;
LONG         fromlen;
LONG         n;
ULONG        t0;
ULONG        ms;

    t_log("");
    t_log("u09. a zero-capacity read of a UDP socket consumes one queued "
          "datagram");

    fd = bsd_socket(AF_INET, SOCK_DGRAM, 0);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_DGRAM)", bsd_Errno()))
        return;

    t_addr(&a, 0UL, (UWORD)LOCAL_PORT);
    if (!t_check((BOOL)(bsd_bind(fd, &a, (LONG)sizeof(a)) == 0), "bind",
                 bsd_Errno()))
    {
        (VOID)bsd_CloseSocket(fd);
        return;
    }

    t_rcvtimeo_ms(fd, T_ZERO_TIMEOUT_MS);

    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xE1, 32UL),
                  "inject a 32-byte datagram", 0);
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xE2, 32UL),
                  "inject a second one behind it", 0);
    t_settle();

    t0 = tap_eclock_now();
    n  = bsd_recv(fd, buf, 0, 0);
    ms = t_ms_since(t0);
    (VOID)t_check((BOOL)(n == 0), "recv(fd, buf, 0) returns 0", n);
    (VOID)t_check((BOOL)(ms < T_ZERO_PROMPT_MS),
                  "and it returned at once, not at the receive timeout",
                  (LONG)ms);

    buf[0] = 0;
    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n == 32), "the next read gets a whole datagram", n);
    (VOID)t_check((BOOL)(buf[0] == 0xE2),
                  "and it is the SECOND one -- the zero read consumed the "
                  "first", (LONG)buf[0]);

    t_rcvtimeo_ms(fd, 200);
    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "the queue is empty afterwards, not one datagram deep",
                  (n < 0) ? bsd_Errno() : n);

    t_rcvtimeo_ms(fd, T_ZERO_TIMEOUT_MS);
    t0 = tap_eclock_now();
    n  = bsd_recv(fd, buf, 0, 0);
    ms = t_ms_since(t0);
    (VOID)t_check((BOOL)(n == 0), "recv(fd, buf, 0) on an empty queue is 0", n);
    (VOID)t_check((BOOL)(ms < T_ZERO_PROMPT_MS),
                  "and it did not wait for a datagram to arrive", (LONG)ms);

    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xF1, 32UL),
                  "inject a 32-byte datagram", 0);
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xF2, 32UL),
                  "inject a second one behind it", 0);
    t_settle();

    t_bzero(&from, (ULONG)sizeof(from));
    fromlen = (LONG)sizeof(from);
    n = bsd_recvfrom(fd, buf, 0, 0, &from, &fromlen);
    (VOID)t_check((BOOL)(n == 0), "recvfrom(..., 0, ...) returns 0", n);

    buf[0] = 0;
    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n == 32 && buf[0] == 0xF2),
                  "and the read after it is the SECOND datagram",
                  (n == 32) ? (LONG)buf[0] : n);

    t_rcvtimeo_ms(fd, 200);
    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "with nothing left behind it",
                  (n < 0) ? bsd_Errno() : n);

    t_rcvtimeo_ms(fd, T_ZERO_TIMEOUT_MS);
    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0x00, 0UL),
                  "inject a datagram carrying no payload", 0);
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0x99, 16UL),
                  "inject a 16-byte one behind it", 0);
    t_settle();

    t_bzero(&from, (ULONG)sizeof(from));
    fromlen = (LONG)sizeof(from);
    t0 = tap_eclock_now();
    n  = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, &from, &fromlen);
    ms = t_ms_since(t0);
    (VOID)t_check((BOOL)(n == 0),
                  "a full-size read of the empty datagram returns 0", n);
    (VOID)t_check((BOOL)(ms < T_ZERO_PROMPT_MS),
                  "at once -- it was queued, not awaited", (LONG)ms);
    (VOID)t_check((BOOL)(from.sin_addr.s_addr == PEER_IP &&
                         from.sin_port == (UWORD)PEER_PORT),
                  "and the source address proves a record was delivered",
                  (LONG)from.sin_port);

    buf[0] = 0;
    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n == 16 && buf[0] == 0x99),
                  "and the one behind it is still there, so the empty "
                  "datagram was its own record",
                  (n == 16) ? (LONG)buf[0] : n);

    (VOID)bsd_CloseSocket(fd);
}

/* RFC 3692 "use for experimentation": no protocol handler in the stack, so a
   packet carrying it can only reach a raw socket. */
#define T_RAW_PROTO     253
#define T_RAW_PAYLOAD   24

/* Where the payload starts in what a raw IPv4 read returns: 4.4BSD hands the
   IP header over as well, and the injected header carries no options. */
#define T_RAW_IPHDR     20

static VOID t_case_zero_read_raw(VOID)
{
static UBYTE buf[256];
LONG         fd;
LONG         n;
ULONG        t0;
ULONG        ms;

    t_log("");
    t_log("u10. and the same on a raw socket");

    fd = bsd_socket(AF_INET, SOCK_RAW, T_RAW_PROTO);
    if (!t_check((BOOL)(fd >= 0), "socket(SOCK_RAW, 253)", bsd_Errno()))
        return;

    t_rcvtimeo_ms(fd, T_ZERO_TIMEOUT_MS);

    t_settle();
    (VOID)t_check((BOOL)t_inject_proto(PEER_IP, T_RAW_PROTO, 0xB1,
                                       (ULONG)T_RAW_PAYLOAD),
                  "inject a protocol-253 packet", 0);
    (VOID)t_check((BOOL)t_inject_proto(PEER_IP, T_RAW_PROTO, 0xB2,
                                       (ULONG)T_RAW_PAYLOAD),
                  "inject a second one behind it", 0);
    t_settle();

    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), T_MSG_PEEK, NULL, NULL);
    (VOID)t_check((BOOL)(n == T_RAW_IPHDR + T_RAW_PAYLOAD),
                  "MSG_PEEK sees the IP header and the payload", n);
    (VOID)t_check((BOOL)(n > T_RAW_IPHDR && buf[T_RAW_IPHDR] == 0xB1),
                  "and the head of the queue is the first packet",
                  (n > T_RAW_IPHDR) ? (LONG)buf[T_RAW_IPHDR] : n);

    t0 = tap_eclock_now();
    n  = bsd_recv(fd, buf, 0, 0);
    ms = t_ms_since(t0);
    (VOID)t_check((BOOL)(n == 0), "recv(fd, buf, 0) returns 0", n);
    (VOID)t_check((BOOL)(ms < T_ZERO_PROMPT_MS),
                  "and it returned at once", (LONG)ms);

    buf[T_RAW_IPHDR] = 0;
    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n == T_RAW_IPHDR + T_RAW_PAYLOAD),
                  "the next read gets a whole packet", n);
    (VOID)t_check((BOOL)(n > T_RAW_IPHDR && buf[T_RAW_IPHDR] == 0xB2),
                  "and it is the SECOND one -- the zero read consumed the "
                  "first",
                  (n > T_RAW_IPHDR) ? (LONG)buf[T_RAW_IPHDR] : n);

    t_rcvtimeo_ms(fd, 200);
    n = bsd_recvfrom(fd, buf, (LONG)sizeof(buf), 0, NULL, NULL);
    (VOID)t_check((BOOL)(n < 0 && bsd_Errno() == T_EWOULDBLOCK),
                  "with nothing left behind it",
                  (n < 0) ? bsd_Errno() : n);

    t_rcvtimeo_ms(fd, T_ZERO_TIMEOUT_MS);
    t0 = tap_eclock_now();
    n  = bsd_recv(fd, buf, 0, 0);
    ms = t_ms_since(t0);
    (VOID)t_check((BOOL)(n == 0), "recv(fd, buf, 0) on an empty queue is 0", n);
    (VOID)t_check((BOOL)(ms < T_ZERO_PROMPT_MS),
                  "and it did not wait for a packet to arrive", (LONG)ms);

    (VOID)bsd_CloseSocket(fd);
}

int main(void)
{
    t_log("AmiNetXDuo, which datagrams a UDP socket may receive");

    if (tap_install(local_mac) != 0)
    {
        Printf((STRPTR)"cannot install the test interface\n");
        return(20);
    }

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!t_check((BOOL)(SocketBase != NULL),
                 "OpenLibrary(bsdsocket.library, 4)", 0))
    {
        Printf((STRPTR)"bsdsocket.library not available\n");
        tap_remove();
        return(20);
    }

    t_case_unconnected();
    t_case_connected();
    t_case_icmp_refused();
    t_case_icmp_other_peer();
    t_case_icmp_unconnected();
    t_case_bind_address();
    t_case_maxdgram();
    t_case_datagram_boundary();
    t_case_zero_read_udp();
    t_case_zero_read_raw();

    CloseLibrary(SocketBase);
    SocketBase = NULL;

    tap_remove();

    t_log("");
    t_log("%ld checks, %ld failures, %s", t_checks, t_failures,
          (t_failures == 0UL) ? "PASS" : "FAIL");

    t_flush();

    return((t_failures == 0UL) ? 0 : 20);
}
