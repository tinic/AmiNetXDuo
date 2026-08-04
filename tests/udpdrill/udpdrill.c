/*
 * AmiNetXDuo, which datagrams a UDP socket is allowed to receive.
 *
 * RFC 1122 4.1.3.5: a UDP socket that has been connected is identified by the
 * full four-tuple, and a datagram whose source is not the connected peer is
 * not for it.  NetX Duo demultiplexes on the local port alone
 * (_nx_udp_packet_receive) and NX_UDP_SOCKET has no local address and no peer,
 * so nothing under bsdsocket.library can make that distinction.
 *
 * It is live for the resolver: a connected socket waiting for a DNS answer
 * will take one from any host on the wire that gets in first.  That is what
 * these cases inject.
 *
 * The peer is a synthetic SANA-II device made at run time,
 * tests/tcpdrill/tapdev.c, the same interface tests/sockopt uses, so a
 * datagram can carry any source address and port, which is the whole point
 * and is not reachable through a real driver or through SLIRP.
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

#include "tapdev.h"


/* ------------------------------------------------------------- the wire -- */

/* DEVS:NetInterfaces/tap0 gives the stack 10.9.9.1/24; 10.9.9.2 is this
   harness.  10.9.9.3 is a second host on the same wire, the one that has no
   business answering a socket connected to 10.9.9.2. */
#define LOCAL_IP        0x0A090901UL
#define PEER_IP         0x0A090902UL
#define OTHER_IP        0x0A090903UL

#define PEER_PORT       9000
#define OTHER_PORT      9001
#define LOCAL_PORT      4801

#define ETYPE_IP        0x0800
#define ETH_HDR         14

static const UBYTE local_mac[6] = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x09 };
static const UBYTE peer_mac[6]  = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x0A };


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


/* -------------------------------------------------------------- the ABI -- */

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


/* ----------------------------------------------------------- the frames -- */

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

/*
 * One UDP datagram from src_ip:src_port to this machine's address, carrying
 * `len` bytes whose first byte is `tag`, so a case can say which datagram
 * came back rather than merely that one did.
 */
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


/* ------------------------------------------------------------- the test -- */

typedef struct sockaddr_in SockAddrIn;

static VOID t_addr(SockAddrIn *a, ULONG ip, UWORD port)
{
    t_bzero(a, (ULONG)sizeof(*a));
    a->sin_len    = (UBYTE)sizeof(*a);
    a->sin_family = AF_INET;
    a->sin_port   = port;
    a->sin_addr.s_addr = ip;
}

/*
 * The stack posts its device reads from its own threads, so an injection made
 * the instant after a socket call can find no reader outstanding.  A short
 * settle before each one, and another after, so the datagram has been through
 * the receive path before the recvfrom() that asks about it.
 */
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

    /* The control: the peer's own datagram must arrive. */
    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, PEER_PORT, 0xB0, 8UL),
                  "the peer sent one", 0);
    t_settle();

    (VOID)t_check((BOOL)(t_drain(fd, &tag) == 8),
                  "recvfrom() returned the peer's datagram", bsd_Errno());
    (VOID)t_check((BOOL)(tag == 0xB0), "and it is the one the peer sent",
                  (LONG)tag);

    /* Right address, wrong port. */
    t_settle();
    (VOID)t_check((BOOL)t_inject(PEER_IP, OTHER_PORT, 0xC0, 8UL),
                  "10.9.9.2:9001 sent one", 0);
    t_settle();

    n = t_drain(fd, &tag);
    (VOID)t_check((BOOL)(n < 0),
                  "recvfrom() did not return a datagram from the wrong port",
                  n);

    /* Right port, wrong address. */
    t_settle();
    (VOID)t_check((BOOL)t_inject(OTHER_IP, PEER_PORT, 0xD0, 8UL),
                  "10.9.9.3:9000 sent one", 0);
    t_settle();

    n = t_drain(fd, &tag);
    (VOID)t_check((BOOL)(n < 0),
                  "recvfrom() did not return a datagram from the wrong host",
                  n);

    /* And the socket still works: this is the answer the resolver wanted. */
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


/* ------------------------------------------------------------------ main -- */

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

    CloseLibrary(SocketBase);
    SocketBase = NULL;

    tap_remove();

    t_log("");
    t_log("%ld checks, %ld failures, %s", t_checks, t_failures,
          (t_failures == 0UL) ? "PASS" : "FAIL");

    t_flush();

    return((t_failures == 0UL) ? 0 : 20);
}
