/*
 * tcpdrill -- a packet-level conformance harness for AmiNetXDuo's TCP.
 *
 * Google's packetdrill states a test as two interleaved things: the socket
 * calls an application makes, and the exact packets that must appear on the
 * wire in response -- flags, sequence numbers, window, options, and the time
 * between them.  A failing case is then a specification of what should have
 * happened, which is the property this harness is after.  Its published
 * scripts were read as a description of correct TCP behaviour; none of its
 * code or syntax is reproduced here, and it could not be run here anyway
 * (tapdev.h has the SLIRP detail that puts the peer inside the guest).
 *
 * One file, DH0:drill.txt, holding every case.  One directive per line, first
 * word is the verb, `#` starts a comment.  Blank lines are ignored.
 *
 *   case NAME              start a case.  Everything below resets.
 *   peerport N             the fake peer's port          (default 9000)
 *   localport N            bind to this port             (default ephemeral)
 *   socket                 create a non-blocking TCP socket
 *   opt NAME VALUE         setsockopt: nodelay rcvbuf sndbuf oobinline
 *                          reuseaddr linger
 *   connect                connect(); EINPROGRESS is the expected answer
 *   listen                 bind localport, listen(4)
 *   accept                 accept(); the accepted socket becomes the subject
 *   send N [= WHAT]        send N bytes of a known pattern.  AGAIN requires
 *                          the send to be refused, as a closed window must;
 *                          SHORT allows any part of it to be taken
 *   wirebytes              the distinct sequence space the stack has sent must
 *                          equal what every send() in the case reported taking
 *   oob N                  send one byte, value N, with MSG_OOB
 *   recv MAX = WHAT        recv(); WHAT is a byte count, EOF, or AGAIN
 *   readable 0|1           WaitSelect() with a zero timeout, read set
 *   writable 0|1           the same, write set
 *   shutdown rd|wr|both
 *   close [within=MS]      CloseSocket(), optionally bounded: the call must
 *                          not wait for a peer that has stopped answering
 *   idle MS                let MS pass; frames that arrive are queued
 *
 *   tx FLAGS [key=value]   the next frame the stack sends must be this
 *   notx MS                the stack must send nothing for MS
 *   txcount MIN MAX        discard everything queued, and assert how much of
 *                          it there was -- a retransmission series is a count
 *   rx FLAGS [key=value]   inject this frame into the stack
 *
 * FLAGS is a string from FSRPAUEC, or `-` for none.  Keys:
 *
 *   seq=N   our sequence numbers are offsets from our initial sequence number,
 *           which the harness learns from the first SYN it sees; the peer's
 *           are offsets from the ISN this harness chose.  So `seq=1 ack=1`
 *           after a handshake means what it looks like, on both sides, and a
 *           script never contains a number the stack picked.
 *   ack=N   the same, the other way round.
 *   win=N   exact advertised window.  winmin/winmax bound it instead.
 *   len=N   payload bytes.
 *   urg=N   urgent pointer.
 *   mss=N   the MSS option must be present with this value (tx) or is sent
 *           with it (rx).
 *   sackok=0|1
 *           the SACK-Permitted option (RFC 2018 kind 4) must be present or
 *           absent (tx); on rx, sackok=1 puts it in the injected SYN.
 *   sack=L:R[,L:R...]
 *           the SACK option (kind 5) must carry exactly these blocks, in this
 *           order.  Sequence numbers are offsets from the ISN this harness
 *           chose, the same space `ack=` counts in, because the blocks
 *           describe data the peer sent.  `sack=-` requires no SACK option.
 *   hdrlen=N
 *           the TCP data offset, in bytes.  20 is a header with no options.
 *           A wrong data offset is a silently corrupt segment: the payload
 *           starts where it says and nowhere else, so it is worth asserting
 *           next to the options that moved it.
 *   dst=WHERE
 *           rx only: the IP destination of the injected frame, and the
 *           Ethernet destination that has to go with it.  `bcast` is
 *           255.255.255.255, `subnet` is the interface's directed broadcast,
 *           `mcast` is 224.0.0.1; the default is the interface address.
 *   within=MS / after=MS
 *           bounds on the gap between this frame and the previous event,
 *           measured from the E-Clock reading taken inside the device's
 *           BeginIO -- the instant the stack handed the frame over -- so the
 *           harness's own polling interval does not enter the measurement.
 *
 * Output goes to DH0:tcpdrill.txt: one line per directive that asserted
 * something, and a decoded expected/observed pair for every failure.  Flushed
 * line by line, because §16.9 records a diagnostic tool losing its last twenty
 * lines to a reboot.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "tapdev.h"

/* ------------------------------------------------------------ the network - */

#define LOCAL_IP        0x0A090901UL            /* 10.9.9.1, DEVS:NetInterfaces */
#define PEER_IP         0x0A090902UL            /* 10.9.9.2, this harness       */
#define SUBNET_BCAST_IP 0x0A0909FFUL            /* 10.9.9.255, NETMASK is /24   */
#define LIMITED_BCAST_IP 0xFFFFFFFFUL
#define MCAST_IP        0xE0000001UL            /* 224.0.0.1, all hosts         */
#define DEFAULT_PEER_PORT   9000

static const UBYTE local_mac[6] = { 0x02, 0x00, 0x44, 0x52, 0x4C, 0x01 };
static const UBYTE peer_mac[6]  = { 0x02, 0x00, 0x44, 0x52, 0x4C, 0x02 };

#define ETYPE_IP        0x0800
#define ETYPE_ARP       0x0806
#define ETH_HDR         14

/* --------------------------------------------------------------- sockets -- */

#define AF_INET_        2
#define SOCK_STREAM_    1

#define SOL_SOCKET_     0xFFFF
#define SO_REUSEADDR_   0x0004
#define SO_KEEPALIVE_   0x0008
#define SO_LINGER_      0x0080
#define SO_OOBINLINE_   0x0100
#define SO_SNDBUF_      0x1001
#define SO_RCVBUF_      0x1002
#define IPPROTO_TCP_    6
#define TCP_NODELAY_    1

#define FIONBIO_        0x8004667EUL
#define MSG_OOB_        1

#define E_WOULDBLOCK    35
#define E_INPROGRESS    36

typedef struct SockAddrIn
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
} SockAddrIn;

static struct Library *SockBase;

#define LVO_CALL_HEAD   register struct Library *a6 __asm("a6") = SockBase

static LONG s_socket(LONG dom, LONG type, LONG proto)
{
    LVO_CALL_HEAD;
    register LONG d0 __asm("d0") = dom;
    register LONG d1 __asm("d1") = type;
    register LONG d2 __asm("d2") = proto;
    register LONG r  __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)" : "=r"(r), "=r" (_clob_d1)
                      : "r"(a6), "r"(d0), "r"(d1), "r"(d2)
                      : "a0", "a1", "cc", "memory");
    return r;
}

static LONG s_bind(LONG s, const SockAddrIn *a)
{
    LVO_CALL_HEAD;
    register LONG        d0 __asm("d0") = s;
    register CONST_APTR  a0 __asm("a0") = (CONST_APTR)a;
    register LONG        d1 __asm("d1") = (LONG)sizeof(*a);
    register LONG        r  __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)" : "=r"(r), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r"(a6), "r"(d0), "r"(a0), "r"(d1)
                      : "a1", "cc", "memory");
    return r;
}

static LONG s_listen(LONG s, LONG backlog)
{
    LVO_CALL_HEAD;
    register LONG d0 __asm("d0") = s;
    register LONG d1 __asm("d1") = backlog;
    register LONG r  __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-42:W)" : "=r"(r), "=r" (_clob_d1)
                      : "r"(a6), "r"(d0), "r"(d1)
                      : "a0", "a1", "cc", "memory");
    return r;
}

static LONG s_accept(LONG s)
{
    LVO_CALL_HEAD;
    register LONG d0 __asm("d0") = s;
    register APTR a0 __asm("a0") = NULL;
    register APTR a1 __asm("a1") = NULL;
    register LONG r  __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-48:W)" : "=r"(r), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r"(a6), "r"(d0), "r"(a0), "r"(a1)
                      : "cc", "memory");
    return r;
}

static LONG s_connect(LONG s, const SockAddrIn *a)
{
    LVO_CALL_HEAD;
    register LONG       d0 __asm("d0") = s;
    register CONST_APTR a0 __asm("a0") = (CONST_APTR)a;
    register LONG       d1 __asm("d1") = (LONG)sizeof(*a);
    register LONG       r  __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)" : "=r"(r), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r"(a6), "r"(d0), "r"(a0), "r"(d1)
                      : "a1", "cc", "memory");
    return r;
}

static LONG s_send(LONG s, const void *buf, LONG len, LONG flags)
{
    LVO_CALL_HEAD;
    register LONG       d0 __asm("d0") = s;
    register CONST_APTR a0 __asm("a0") = (CONST_APTR)buf;
    register LONG       d1 __asm("d1") = len;
    register LONG       d2 __asm("d2") = flags;
    register LONG       r  __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)" : "=r"(r), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r"(a6), "r"(d0), "r"(a0), "r"(d1), "r"(d2)
                      : "a1", "cc", "memory");
    return r;
}

static LONG s_recv(LONG s, void *buf, LONG len, LONG flags)
{
    LVO_CALL_HEAD;
    register LONG d0 __asm("d0") = s;
    register APTR a0 __asm("a0") = buf;
    register LONG d1 __asm("d1") = len;
    register LONG d2 __asm("d2") = flags;
    register LONG r  __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-78:W)" : "=r"(r), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r"(a6), "r"(d0), "r"(a0), "r"(d1), "r"(d2)
                      : "a1", "cc", "memory");
    return r;
}

static LONG s_shutdown(LONG s, LONG how)
{
    LVO_CALL_HEAD;
    register LONG d0 __asm("d0") = s;
    register LONG d1 __asm("d1") = how;
    register LONG r  __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-84:W)" : "=r"(r), "=r" (_clob_d1)
                      : "r"(a6), "r"(d0), "r"(d1)
                      : "a0", "a1", "cc", "memory");
    return r;
}

static LONG s_setsockopt(LONG s, LONG level, LONG name, const void *v, LONG n)
{
    LVO_CALL_HEAD;
    register LONG       d0 __asm("d0") = s;
    register LONG       d1 __asm("d1") = level;
    register LONG       d2 __asm("d2") = name;
    register CONST_APTR a0 __asm("a0") = (CONST_APTR)v;
    register LONG       d3 __asm("d3") = n;
    register LONG       r  __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-90:W)" : "=r"(r), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r"(a6), "r"(d0), "r"(d1), "r"(d2), "r"(a0), "r"(d3)
                      : "a1", "cc", "memory");
    return r;
}

static LONG s_ioctl(LONG s, ULONG req, void *arg)
{
    LVO_CALL_HEAD;
    register LONG  d0 __asm("d0") = s;
    register ULONG d1 __asm("d1") = req;
    register APTR  a0 __asm("a0") = arg;
    register LONG  r  __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-114:W)" : "=r"(r), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r"(a6), "r"(d0), "r"(d1), "r"(a0)
                      : "a1", "cc", "memory");
    return r;
}

static LONG s_close(LONG s)
{
    LVO_CALL_HEAD;
    register LONG d0 __asm("d0") = s;
    register LONG r  __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)" : "=r"(r)
                      : "r"(a6), "r"(d0)
                      : "a0", "a1", "cc", "memory");
    return r;
}

typedef struct DrillTimeval { LONG tv_sec; LONG tv_usec; } DrillTimeval;
typedef struct DrillFdSet   { ULONG bits[8]; } DrillFdSet;

static LONG s_waitselect(LONG nfds, DrillFdSet *rd, DrillFdSet *wr,
                         DrillFdSet *ex, DrillTimeval *tv)
{
    LVO_CALL_HEAD;
    register LONG  d0 __asm("d0") = nfds;
    register APTR  a0 __asm("a0") = rd;
    register APTR  a1 __asm("a1") = wr;
    register APTR  a2 __asm("a2") = ex;
    register APTR  a3 __asm("a3") = tv;
    register ULONG d1 __asm("d1") = 0;
    register LONG  r  __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)" : "=r"(r), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r"(a6), "r"(d0), "r"(a0), "r"(a1), "r"(a2),
                        "r"(a3), "r"(d1)
                      : "cc", "memory");
    return r;
}

static LONG s_errno(VOID)
{
    LVO_CALL_HEAD;
    register LONG r __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)" : "=r"(r)
                      : "r"(a6)
                      : "a0", "a1", "d1", "cc", "memory");
    return r;
}

/* ------------------------------------------------------------- reporting -- */

static BPTR  out_file;
static ULONG n_pass;
static ULONG n_fail;
static ULONG n_cases;
static ULONG n_cases_failed;

static ULONG d_len(const char *s)
{
    ULONG n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static VOID emit(const char *s)
{
    ULONG n = d_len(s);

    if (out_file != (BPTR)0)
    {
        Write(out_file, (APTR)s, (LONG)n);
        /* Flush on every line: see the header. */
        Flush(out_file);
    }
    Write(Output(), (APTR)s, (LONG)n);
}

/* A tiny formatter. %s %ld %lu %02x %04x are all this needs, and pulling in
   newlib's printf would drag mathieeedoubbas.library onto a bare boot disk
   (tests/conformance/build.sh records the same trap). */
static VOID fmt_num(char **p, ULONG v, UWORD base, UWORD width, BOOL sign)
{
    char  tmp[12];
    UWORD n = 0;
    char *o = *p;

    if (sign && (LONG)v < 0)
    {
        *o++ = '-';
        v = (ULONG)(-(LONG)v);
    }

    do
    {
        UWORD d = (UWORD)(v % base);
        tmp[n++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
        v /= base;
    } while (v != 0);

    while (n < width)
        tmp[n++] = '0';

    while (n-- != 0)
        *o++ = tmp[n];

    *p = o;
}

static VOID say(const char *fmt, ...)
{
    static char line[320];              /* 4 KB of Shell stack; see the header */
    char       *o = line;
    const char *f = fmt;
    va_list     ap;

    va_start(ap, fmt);

    while (*f != '\0' && (ULONG)(o - line) < sizeof(line) - 16)
    {
        if (*f != '%')
        {
            *o++ = *f++;
            continue;
        }
        f++;
        if (*f == 's')
        {
            const char *s = va_arg(ap, const char *);
            f++;
            while (*s != '\0' && (ULONG)(o - line) < sizeof(line) - 8)
                *o++ = *s++;
        }
        else if (*f == 'd')
        {
            f++;
            fmt_num(&o, (ULONG)va_arg(ap, LONG), 10, 0, TRUE);
        }
        else if (*f == 'u')
        {
            f++;
            fmt_num(&o, va_arg(ap, ULONG), 10, 0, FALSE);
        }
        else if (*f == 'x')
        {
            f++;
            fmt_num(&o, va_arg(ap, ULONG), 16, 0, FALSE);
        }
        else
        {
            *o++ = *f++;
        }
    }

    va_end(ap);

    *o++ = '\n';
    *o   = '\0';
    emit(line);
}

/* ---------------------------------------------------------- packet bytes -- */

static ULONG rd32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
                                           ((ULONG)p[2] << 8) | (ULONG)p[3]; }
static UWORD rd16(const UBYTE *p) { return (UWORD)(((UWORD)p[0] << 8) | p[1]); }
static VOID  wr32(UBYTE *p, ULONG v) { p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16);
                                       p[2] = (UBYTE)(v >> 8);  p[3] = (UBYTE)v; }
static VOID  wr16(UBYTE *p, UWORD v) { p[0] = (UBYTE)(v >> 8); p[1] = (UBYTE)v; }

static VOID cp(UBYTE *to, const UBYTE *from, ULONG n)
{
    while (n-- != 0)
        *to++ = *from++;
}

static VOID zero(UBYTE *p, ULONG n)
{
    while (n-- != 0)
        *p++ = 0;
}

static UWORD ones_sum(const UBYTE *p, ULONG n, ULONG start)
{
    ULONG sum = start;

    while (n > 1)
    {
        sum += rd16(p);
        p += 2;
        n -= 2;
    }
    if (n != 0)
        sum += ((ULONG)*p) << 8;

    while ((sum >> 16) != 0)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    return (UWORD)(~sum);
}

static ULONG ones_partial(const UBYTE *p, ULONG n, ULONG sum)
{
    while (n > 1)
    {
        sum += rd16(p);
        p += 2;
        n -= 2;
    }
    if (n != 0)
        sum += ((ULONG)*p) << 8;
    return sum;
}

/* -------------------------------------------------------------- decoding -- */

#define TF_FIN  0x01
#define TF_SYN  0x02
#define TF_RST  0x04
#define TF_PSH  0x08
#define TF_ACK  0x10
#define TF_URG  0x20
#define TF_ECE  0x40
#define TF_CWR  0x80

typedef struct Seg
{
    ULONG   stamp;
    UWORD   ether;
    BOOL    is_tcp;
    ULONG   src_ip;
    ULONG   dst_ip;
    UWORD   src_port;
    UWORD   dst_port;
    ULONG   seq;
    ULONG   ack;
    UWORD   flags;
    UWORD   win;
    UWORD   urg;
    ULONG   dlen;
    LONG    mss;                /* -1 when the option is absent */
    BOOL    sackok;             /* SACK-Permitted option present */
    UWORD   sack_n;             /* SACK blocks carried, 0 for none */
    ULONG   sack_l[4];
    ULONG   sack_r[4];
    UWORD   thl;                /* TCP header length in bytes    */
    BOOL    ip_ok;              /* IP header checksum verified  */
    BOOL    tcp_ok;             /* TCP checksum verified        */

    /* Enough of the frame to say what a frame the decoder rejected actually
       was: "non-TCP frame ether=0x0800" on its own does not distinguish, for
       instance, an ICMP echo reply. */
    ULONG   frame_len;
    UBYTE   head[34];
} Seg;

static BOOL decode(Seg *s, const UBYTE *f, ULONG len, ULONG stamp)
{
    const UBYTE *ip;
    const UBYTE *tcp;
    UWORD        ihl;
    UWORD        thl;
    ULONG        iplen;

    zero((UBYTE *)s, (ULONG)sizeof(*s));
    s->stamp = stamp;
    s->ether = rd16(&f[12]);
    s->mss   = -1;

    s->frame_len = len;
    {
        ULONG n = (len < (ULONG)sizeof(s->head)) ? len : (ULONG)sizeof(s->head);
        ULONG i;

        for (i = 0; i < n; i++)
            s->head[i] = f[i];
    }

    if (s->ether != ETYPE_IP || len < ETH_HDR + 20)
        return FALSE;

    ip  = &f[ETH_HDR];
    ihl = (UWORD)((ip[0] & 0x0F) * 4);
    if (ihl < 20 || len < (ULONG)(ETH_HDR + ihl))
        return FALSE;

    s->ip_ok  = (ones_sum(ip, ihl, 0) == 0) ? TRUE : FALSE;
    iplen     = rd16(&ip[2]);
    s->src_ip = rd32(&ip[12]);
    s->dst_ip = rd32(&ip[16]);

    if (ip[9] != 6 || iplen < ihl + 20UL)
        return FALSE;

    tcp = ip + ihl;
    thl = (UWORD)(((tcp[12] >> 4) & 0x0F) * 4);
    if (thl < 20 || iplen < ihl + (ULONG)thl)
        return FALSE;

    s->is_tcp   = TRUE;
    s->src_port = rd16(&tcp[0]);
    s->dst_port = rd16(&tcp[2]);
    s->seq      = rd32(&tcp[4]);
    s->ack      = rd32(&tcp[8]);
    s->flags    = tcp[13];
    s->win      = rd16(&tcp[14]);
    s->urg      = rd16(&tcp[18]);
    s->dlen     = iplen - ihl - thl;
    s->thl      = thl;

    /* The TCP checksum is verified on every frame the stack sends: it costs
       nothing here and no script would think to assert on it. */
    {
        ULONG sum = 0;
        UBYTE ph[4];

        sum = ones_partial(&ip[12], 8, sum);
        ph[0] = 0; ph[1] = 6;
        wr16(&ph[2], (UWORD)(iplen - ihl));
        sum = ones_partial(ph, 4, sum);
        sum = ones_partial(tcp, iplen - ihl, sum);
        while ((sum >> 16) != 0)
            sum = (sum & 0xFFFFUL) + (sum >> 16);
        s->tcp_ok = ((UWORD)(~sum) == 0) ? TRUE : FALSE;
    }

    /* Options.  MSS on the SYN, SACK-Permitted on the SYN, SACK blocks on an
       acknowledgement that leaves a hole.  Walk properly regardless: a wrong
       data offset is exactly the fault an option-length walk catches. */
    {
        const UBYTE *o   = tcp + 20;
        const UBYTE *end = tcp + thl;

        while (o < end)
        {
            if (*o == 0)
                break;
            if (*o == 1) { o++; continue; }
            if (o + 1 >= end || o[1] < 2)
                break;
            if (*o == 2 && o[1] == 4 && o + 4 <= end)
                s->mss = (LONG)rd16(&o[2]);
            if (*o == 4 && o[1] == 2)
                s->sackok = TRUE;
            if (*o == 5 && o[1] >= 10 && ((o[1] - 2) % 8) == 0 && o + o[1] <= end)
            {
                UWORD n = (UWORD)((o[1] - 2) / 8);
                UWORD i;

                if (n > 4)
                    n = 4;
                for (i = 0; i < n; i++)
                {
                    s->sack_l[i] = rd32(&o[2 + i * 8]);
                    s->sack_r[i] = rd32(&o[6 + i * 8]);
                }
                s->sack_n = n;
            }
            o += o[1];
        }
    }

    return TRUE;
}

static VOID flag_string(UWORD f, char *out)
{
    char *o = out;

    if ((f & TF_FIN) != 0) *o++ = 'F';
    if ((f & TF_SYN) != 0) *o++ = 'S';
    if ((f & TF_RST) != 0) *o++ = 'R';
    if ((f & TF_PSH) != 0) *o++ = 'P';
    if ((f & TF_ACK) != 0) *o++ = 'A';
    if ((f & TF_URG) != 0) *o++ = 'U';
    if ((f & TF_ECE) != 0) *o++ = 'E';
    if ((f & TF_CWR) != 0) *o++ = 'C';
    if (o == out) *o++ = '-';
    *o = '\0';
}

/* ---------------------------------------------------------------- clocks -- */

static ULONG eclock_per_ms;

static ULONG ticks_to_ms(ULONG a, ULONG b)     /* b - a, in milliseconds */
{
    ULONG d = b - a;                            /* unsigned wrap is correct */

    if (eclock_per_ms == 0)
        return 0;
    return d / eclock_per_ms;
}

/* ------------------------------------------------------------ case state -- */

typedef struct Case
{
    char    name[48];
    LONG    sock;
    LONG    lsock;
    UWORD   local_port;
    UWORD   peer_port;
    ULONG   u_isn;
    BOOL    u_isn_known;
    ULONG   p_isn;
    ULONG   t_last;             /* E-Clock of the previous event */
    ULONG   fails;
    ULONG   line;

    /* What `wirebytes` compares: what send() said it took, against how far
       into our own sequence space anything ever reached. */
    ULONG   accepted;
    ULONG   wire_end;           /* highest seq + len seen, absolute */
    BOOL    wire_seen;
} Case;

static Case cs;

/* A queue of frames the stack sent that no directive has consumed yet. */
#define PEND_MAX    32
static Seg   pend[PEND_MAX];
static UWORD pend_head, pend_tail, pend_count;

static UBYTE scratch[TAP_FRAME_MAX];
static UBYTE payload[4096];

/* ------------------------------------------------------------------- ARP -- */

static VOID arp_reply(const UBYTE *req)
{
    UBYTE f[42];
    const UBYTE *a = &req[ETH_HDR];

    zero(f, (ULONG)sizeof(f));
    cp(&f[0], &req[6], 6);              /* back to whoever asked */
    cp(&f[6], peer_mac, 6);
    wr16(&f[12], ETYPE_ARP);

    wr16(&f[14], 1);                    /* Ethernet   */
    wr16(&f[16], ETYPE_IP);
    f[18] = 6;
    f[19] = 4;
    wr16(&f[20], 2);                    /* reply      */
    cp(&f[22], peer_mac, 6);
    wr32(&f[28], PEER_IP);
    cp(&f[32], &a[8], 6);               /* their MAC  */
    cp(&f[38], &a[14], 4);              /* their IP   */

    (VOID)tap_rx_put(f, (ULONG)sizeof(f));
}

/*
 * Drain the device.  ARP is answered here and never queued: it is not part of
 * any script and happens whenever the stack has to resolve the peer.
 */
static ULONG n_background;      /* frames dropped by the filter in pump() */

/*
 * Note a data segment against the case's sequence space.
 *
 * A HIGH-WATER MARK, not a sum: a retransmission carries bytes the peer has
 * already been sent, and counting it twice would make every case with one in
 * it look like the fault this measures.  What is left is the distinct sequence
 * space the stack has committed to -- the number a capture is summed for.
 *
 * Called from pump(), so it sees every frame including the ones a `tx`
 * directive is about to consume and the ones nothing ever asks for.
 */
static VOID wire_note(const Seg *s)
{
    ULONG end;

    if (!s->is_tcp || s->dlen == 0 || s->dst_ip != PEER_IP)
        return;

    end = s->seq + s->dlen;
    if (!cs.wire_seen || (LONG)(end - cs.wire_end) > 0)
    {
        cs.wire_end  = end;
        cs.wire_seen = TRUE;
    }
}

static VOID pump(VOID)
{
    ULONG stamp;
    ULONG len;

    while ((len = tap_tx_get(scratch, (ULONG)sizeof(scratch), &stamp)) != 0)
    {
        UWORD ether = rd16(&scratch[12]);

        if (ether == ETYPE_ARP)
        {
            if (len >= 42 && rd16(&scratch[20]) == 1 &&
                rd32(&scratch[38]) == PEER_IP)
            {
                arp_reply(scratch);
            }
            continue;
        }

        /*
         * Traffic that is not part of any case.  The stack under test is a
         * whole stack: it answers ARP (above), and anything else in the tree
         * that opens a UDP socket -- mDNS, a DHCP renewal, an IGMP report --
         * puts frames on this wire that no script mentions.  They used to be
         * queued like everything else, so the next `tx` in whatever case was
         * running failed with "non-TCP frame ether=0x0800" and every assertion
         * after it was one frame out of step; that is how c04, c05 and a01
         * failed in one run of an unchanged stack and passed in the next.
         *
         * Anything IPv4 that is not TCP to the peer is therefore counted and
         * dropped.  A malformed TCP segment, or one aimed at the peer, still
         * reaches the queue -- those are results.
         */
        if (ether != ETYPE_IP)
        {
            /* Anything that is not IPv4 is not part of any case either, and
               the test above used to be written as one the wrong way round:
               it only ever reached IPv4 frames, so an IPv6 router
               solicitation or multicast-listener report went into the queue
               and put every later assertion in the case one frame out. */
            n_background++;
            continue;
        }

        if (len >= ETH_HDR + 20 &&
            (scratch[ETH_HDR + 9] != 6 || rd32(&scratch[ETH_HDR + 16]) != PEER_IP))
        {
            n_background++;
            continue;
        }

        if (pend_count >= PEND_MAX)
        {
            /* Dropping here would leave every later assertion one frame out
               of step. */
            say("  !! frame queue overflow -- a case sent more than %u frames "
                "between directives", (ULONG)PEND_MAX);
            pend_tail = (UWORD)((pend_tail + 1) % PEND_MAX);
            pend_count--;
        }

        (VOID)decode(&pend[pend_head], scratch, len, stamp);
        wire_note(&pend[pend_head]);
        pend_head = (UWORD)((pend_head + 1) % PEND_MAX);
        pend_count++;
    }
}

static BOOL pend_pop(Seg *out)
{
    if (pend_count == 0)
        return FALSE;

    *out = pend[pend_tail];
    pend_tail = (UWORD)((pend_tail + 1) % PEND_MAX);
    pend_count--;
    return TRUE;
}

/* Wait up to `ms` for one frame, servicing ARP throughout. */
static BOOL wait_frame(Seg *out, ULONG ms)
{
    ULONG spent = 0;

    for (;;)
    {
        pump();
        if (pend_pop(out))
            return TRUE;
        if (spent >= ms)
            return FALSE;
        Delay(1);                        /* 20 ms; see the timing note */
        spent += 20;
    }
}

/* ------------------------------------------------------------- injection -- */

typedef struct Inject
{
    UWORD   flags;
    ULONG   seq;                /* already absolute */
    ULONG   ack;
    UWORD   win;
    UWORD   urg;
    ULONG   dlen;
    LONG    mss;
    BOOL    sackok;
    ULONG   dst_ip;             /* IP destination; LOCAL_IP unless dst= said */
} Inject;

static VOID build_and_inject(const Inject *in)
{
    static UBYTE f[TAP_FRAME_MAX];
    UBYTE *ip  = &f[ETH_HDR];
    UBYTE *tcp;
    UWORD  opt = (UWORD)((in->mss >= 0) ? 4 : 0);
    UWORD  thl;
    ULONG  i;
    ULONG  iplen;

    if (in->sackok)
        opt = (UWORD)(opt + 4);
    thl = (UWORD)(20 + opt);

    zero(f, (ULONG)sizeof(f));

    /* The Ethernet destination has to agree with the IP one, or tap_rx_put()
       hands the frame up without SANA2IOF_BCAST and the stack sees a unicast
       frame carrying a broadcast address -- which is a different case. */
    if (in->dst_ip == LIMITED_BCAST_IP || in->dst_ip == SUBNET_BCAST_IP)
    {
        for (i = 0; i < 6; i++)
            f[i] = 0xFF;
    }
    else if ((in->dst_ip & 0xF0000000UL) == 0xE0000000UL)
    {
        f[0] = 0x01; f[1] = 0x00; f[2] = 0x5E;
        f[3] = (UBYTE)((in->dst_ip >> 16) & 0x7F);
        f[4] = (UBYTE)((in->dst_ip >> 8) & 0xFF);
        f[5] = (UBYTE)(in->dst_ip & 0xFF);
    }
    else
    {
        cp(&f[0], local_mac, 6);
    }
    cp(&f[6], peer_mac, 6);
    wr16(&f[12], ETYPE_IP);

    tcp   = ip + 20;
    iplen = 20UL + thl + in->dlen;

    ip[0] = 0x45;
    ip[1] = 0;
    wr16(&ip[2], (UWORD)iplen);
    wr16(&ip[4], 0x4000);               /* id 0x4000, so it is recognisable */
    wr16(&ip[6], 0x4000);               /* DF */
    ip[8] = 64;
    ip[9] = 6;
    wr32(&ip[12], PEER_IP);
    wr32(&ip[16], in->dst_ip);
    wr16(&ip[10], ones_sum(ip, 20, 0));

    wr16(&tcp[0], cs.peer_port);
    wr16(&tcp[2], cs.local_port);
    wr32(&tcp[4], in->seq);
    wr32(&tcp[8], in->ack);
    tcp[12] = (UBYTE)((thl / 4) << 4);
    tcp[13] = (UBYTE)in->flags;
    wr16(&tcp[14], in->win);
    wr16(&tcp[18], in->urg);

    if (in->mss >= 0)
    {
        tcp[20] = 2;
        tcp[21] = 4;
        wr16(&tcp[22], (UWORD)in->mss);
    }
    if (in->sackok)
    {
        /* NOP, NOP, kind 4, length 2 -- a whole option word, so the data
           offset stays a whole number of words with or without the MSS. */
        UWORD at = (UWORD)(20 + (in->mss >= 0 ? 4 : 0));

        tcp[at]     = 1;
        tcp[at + 1] = 1;
        tcp[at + 2] = 4;
        tcp[at + 3] = 2;
    }

    for (i = 0; i < in->dlen; i++)
        tcp[thl + i] = (UBYTE)('a' + (i % 26));

    {
        ULONG sum = 0;
        UBYTE ph[4];

        sum = ones_partial(&ip[12], 8, sum);
        ph[0] = 0; ph[1] = 6;
        wr16(&ph[2], (UWORD)(thl + in->dlen));
        sum = ones_partial(ph, 4, sum);
        sum = ones_partial(tcp, thl + in->dlen, sum);
        while ((sum >> 16) != 0)
            sum = (sum & 0xFFFFUL) + (sum >> 16);
        wr16(&tcp[16], (UWORD)(~sum));
    }

    if (tap_rx_put(f, ETH_HDR + iplen) != 0)
    {
        say("  !! injection dropped -- no CMD_READ outstanding for 0x0800");
        cs.fails++;
    }
}

/* ---------------------------------------------------------- the parser ---- */

static BOOL is_space(char c) { return (c == ' ' || c == '\t' || c == '\r') ? TRUE : FALSE; }

static const char *skip_ws(const char *p)
{
    while (*p != '\0' && is_space(*p))
        p++;
    return p;
}

static const char *token(const char *p, char *out, ULONG max)
{
    ULONG n = 0;

    p = skip_ws(p);
    while (*p != '\0' && !is_space(*p) && n + 1 < max)
        out[n++] = *p++;
    out[n] = '\0';
    while (*p != '\0' && !is_space(*p))
        p++;
    return p;
}

static BOOL streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0') ? TRUE : FALSE;
}

static LONG to_num(const char *s)
{
    LONG v    = 0;
    BOOL neg  = FALSE;

    if (*s == '-') { neg = TRUE; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* --------------------------------------------------- expectation records -- */

typedef struct Expect
{
    UWORD   flags;
    BOOL    have_seq;   LONG seq;
    BOOL    have_ack;   LONG ack;
    BOOL    have_win;   LONG win;
    BOOL    have_winmin; LONG winmin;
    BOOL    have_winmax; LONG winmax;
    BOOL    have_len;   LONG len;
    BOOL    have_urg;   LONG urg;
    BOOL    have_mss;   LONG mss;
    BOOL    have_sackok; LONG sackok;
    BOOL    have_sack;  UWORD sack_n; ULONG sack_l[4]; ULONG sack_r[4];
    BOOL    have_hdrlen; LONG hdrlen;
    BOOL    have_within; LONG within;
    BOOL    have_after;  LONG after;
    BOOL    have_dst;   ULONG dst;
} Expect;

static UWORD parse_flags(const char *s)
{
    UWORD f = 0;

    while (*s != '\0')
    {
        switch (*s)
        {
        case 'F': f |= TF_FIN; break;
        case 'S': f |= TF_SYN; break;
        case 'R': f |= TF_RST; break;
        case 'P': f |= TF_PSH; break;
        case 'A': f |= TF_ACK; break;
        case 'U': f |= TF_URG; break;
        case 'E': f |= TF_ECE; break;
        case 'C': f |= TF_CWR; break;
        default: break;
        }
        s++;
    }
    return f;
}

static const char *parse_keys(const char *p, Expect *e)
{
    char tok[48];

    for (;;)
    {
        const char *next = token(p, tok, sizeof(tok));
        const char *eq   = tok;

        if (tok[0] == '\0' || tok[0] == '#')
            return next;

        while (*eq != '\0' && *eq != '=')
            eq++;
        if (*eq != '=')
        {
            p = next;
            continue;
        }
        {
            char  key[24];
            ULONG n = (ULONG)(eq - tok);
            ULONG i;

            if (n > sizeof(key) - 1)
                n = sizeof(key) - 1;
            for (i = 0; i < n; i++)
                key[i] = tok[i];
            key[n] = '\0';

            if (streq(key, "seq"))        { e->have_seq = TRUE;    e->seq = to_num(eq + 1); }
            else if (streq(key, "ack"))   { e->have_ack = TRUE;    e->ack = to_num(eq + 1); }
            else if (streq(key, "win"))   { e->have_win = TRUE;    e->win = to_num(eq + 1); }
            else if (streq(key, "winmin")){ e->have_winmin = TRUE; e->winmin = to_num(eq + 1); }
            else if (streq(key, "winmax")){ e->have_winmax = TRUE; e->winmax = to_num(eq + 1); }
            else if (streq(key, "len"))   { e->have_len = TRUE;    e->len = to_num(eq + 1); }
            else if (streq(key, "urg"))   { e->have_urg = TRUE;    e->urg = to_num(eq + 1); }
            else if (streq(key, "mss"))   { e->have_mss = TRUE;    e->mss = to_num(eq + 1); }
            else if (streq(key, "sackok")){ e->have_sackok = TRUE; e->sackok = to_num(eq + 1); }
            else if (streq(key, "hdrlen")){ e->have_hdrlen = TRUE; e->hdrlen = to_num(eq + 1); }
            else if (streq(key, "sack"))
            {
                /* L:R,L:R... in the peer's sequence space, or `-` for none. */
                const char *v = eq + 1;

                e->have_sack = TRUE;
                e->sack_n    = 0;
                while (*v != '\0' && *v != '-' && e->sack_n < 4)
                {
                    ULONG l = 0, r = 0;

                    while (*v >= '0' && *v <= '9') l = l * 10 + (ULONG)(*v++ - '0');
                    if (*v == ':') v++;
                    while (*v >= '0' && *v <= '9') r = r * 10 + (ULONG)(*v++ - '0');
                    e->sack_l[e->sack_n] = l;
                    e->sack_r[e->sack_n] = r;
                    e->sack_n++;
                    if (*v == ',') v++;
                    else break;
                }
            }
            else if (streq(key, "within")){ e->have_within = TRUE; e->within = to_num(eq + 1); }
            else if (streq(key, "after")) { e->have_after = TRUE;  e->after = to_num(eq + 1); }
            else if (streq(key, "dst"))
            {
                const char *v = eq + 1;

                e->have_dst = TRUE;
                if (streq(v, "bcast"))       e->dst = LIMITED_BCAST_IP;
                else if (streq(v, "subnet")) e->dst = SUBNET_BCAST_IP;
                else if (streq(v, "mcast"))  e->dst = MCAST_IP;
                else                         e->dst = LOCAL_IP;
            }
        }
        p = next;
    }
}

/* ---------------------------------------------------------- the reporter -- */

static VOID describe(const Seg *s, char *out, ULONG max)
{
    char *o = out;
    char  fl[10];

    (VOID)max;

    if (!s->is_tcp)
    {
        const char *t = "non-TCP frame ether=0x";
        ULONG       n;
        ULONG       i;

        while (*t != '\0') *o++ = *t++;
        fmt_num(&o, s->ether, 16, 4, FALSE);

        t = " framelen="; while (*t != '\0') *o++ = *t++;
        fmt_num(&o, s->frame_len, 10, 0, FALSE);

        if (s->ether == ETYPE_IP && s->frame_len >= ETH_HDR + 20)
        {
            t = " ipproto="; while (*t != '\0') *o++ = *t++;
            fmt_num(&o, s->head[ETH_HDR + 9], 10, 0, FALSE);
            t = " iplen="; while (*t != '\0') *o++ = *t++;
            fmt_num(&o, ((ULONG)s->head[ETH_HDR + 2] << 8) |
                        (ULONG)s->head[ETH_HDR + 3], 10, 0, FALSE);
        }

        t = " ["; while (*t != '\0') *o++ = *t++;
        n = (s->frame_len < (ULONG)sizeof(s->head)) ? s->frame_len
                                                    : (ULONG)sizeof(s->head);
        for (i = 0; i < n; i++)
        {
            fmt_num(&o, s->head[i], 16, 2, FALSE);
            if (i + 1 < n)
                *o++ = ' ';
        }
        *o++ = ']';

        *o = '\0';
        return;
    }

    flag_string(s->flags, fl);
    { const char *t = fl; while (*t != '\0') *o++ = *t++; }
    { const char *t = " seq="; while (*t != '\0') *o++ = *t++; }
    fmt_num(&o, cs.u_isn_known ? (s->seq - cs.u_isn) : s->seq, 10, 0, FALSE);
    { const char *t = " ack="; while (*t != '\0') *o++ = *t++; }
    fmt_num(&o, (s->flags & TF_ACK) ? (s->ack - cs.p_isn) : 0UL, 10, 0, FALSE);
    { const char *t = " win="; while (*t != '\0') *o++ = *t++; }
    fmt_num(&o, s->win, 10, 0, FALSE);
    { const char *t = " len="; while (*t != '\0') *o++ = *t++; }
    fmt_num(&o, s->dlen, 10, 0, FALSE);
    { const char *t = " hdrlen="; while (*t != '\0') *o++ = *t++; }
    fmt_num(&o, s->thl, 10, 0, FALSE);
    if (s->mss >= 0)
    {
        const char *t = " mss="; while (*t != '\0') *o++ = *t++;
        fmt_num(&o, (ULONG)s->mss, 10, 0, FALSE);
    }
    if (s->sackok)
    {
        const char *t = " sackok"; while (*t != '\0') *o++ = *t++;
    }
    if (s->sack_n != 0)
    {
        const char *t = " sack="; UWORD i;

        while (*t != '\0') *o++ = *t++;
        for (i = 0; i < s->sack_n; i++)
        {
            if (i != 0) *o++ = ',';
            fmt_num(&o, s->sack_l[i] - cs.p_isn, 10, 0, FALSE);
            *o++ = ':';
            fmt_num(&o, s->sack_r[i] - cs.p_isn, 10, 0, FALSE);
        }
    }
    if ((s->flags & TF_URG) != 0)
    {
        const char *t = " urg="; while (*t != '\0') *o++ = *t++;
        fmt_num(&o, s->urg, 10, 0, FALSE);
    }
    if (!s->tcp_ok)
    {
        const char *t = " BAD-TCP-CHECKSUM"; while (*t != '\0') *o++ = *t++;
    }
    if (!s->ip_ok)
    {
        const char *t = " BAD-IP-CHECKSUM"; while (*t != '\0') *o++ = *t++;
    }
    *o = '\0';
}

static VOID pass(const char *what)
{
    n_pass++;
    say("  ok   %s", what);
}

static VOID fail(const char *what, const char *why)
{
    n_fail++;
    cs.fails++;
    say("  FAIL %s", what);
    say("       %s", why);
}

/* ------------------------------------------------------------- directives - */

static VOID do_tx(const char *args, const char *raw)
{
    char    tok[48];
    Expect  e;
    Seg     got;
    ULONG   limit;
    char    desc[360];
    char    why[280];
    char   *w;

    zero((UBYTE *)&e, (ULONG)sizeof(e));
    args = token(args, tok, sizeof(tok));
    e.flags = parse_flags(tok);
    (VOID)parse_keys(args, &e);

    limit = e.have_within ? (ULONG)e.within + 60UL : 2000UL;

    if (!wait_frame(&got, limit))
    {
        fail(raw, "nothing was sent at all");
        return;
    }

    if (!got.is_tcp)
    {
        describe(&got, desc, sizeof(desc));
        fail(raw, desc);
        return;
    }

    /* The first SYN we ever see fixes our initial sequence number.  Nothing
       in a script may name it. */
    if (!cs.u_isn_known && (got.flags & TF_SYN) != 0)
    {
        cs.u_isn       = got.seq;
        cs.u_isn_known = TRUE;
    }
    if (cs.local_port == 0)
        cs.local_port = got.src_port;

    describe(&got, desc, sizeof(desc));
    w = why;

    if (got.flags != e.flags)
    {
        char want[10], have[10];
        const char *t;

        flag_string(e.flags, want);
        flag_string(got.flags, have);
        t = "flags: wanted "; while (*t) *w++ = *t++;
        t = want;             while (*t) *w++ = *t++;
        t = ", got ";         while (*t) *w++ = *t++;
        t = have;             while (*t) *w++ = *t++;
        *w = '\0';
        fail(raw, why);
        say("       observed  %s", desc);
        cs.t_last = got.stamp;
        return;
    }

#define CHECK(cond, label, wanted, actual)                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            char *q = why; const char *t = label;                             \
            while (*t) *q++ = *t++;                                           \
            t = ": wanted "; while (*t) *q++ = *t++;                          \
            fmt_num(&q, (ULONG)(wanted), 10, 0, TRUE);                        \
            t = ", got ";    while (*t) *q++ = *t++;                          \
            fmt_num(&q, (ULONG)(actual), 10, 0, TRUE);                        \
            *q = '\0';                                                        \
            fail(raw, why);                                                   \
            say("       observed  %s", desc);                                 \
            cs.t_last = got.stamp;                                            \
            return;                                                           \
        }                                                                     \
    } while (0)

    if (e.have_seq)
        CHECK(got.seq == cs.u_isn + (ULONG)e.seq, "seq",
              e.seq, (LONG)(got.seq - cs.u_isn));
    if (e.have_ack)
        CHECK(got.ack == cs.p_isn + (ULONG)e.ack, "ack",
              e.ack, (LONG)(got.ack - cs.p_isn));
    if (e.have_win)
        CHECK((LONG)got.win == e.win, "win", e.win, got.win);
    if (e.have_winmin)
        CHECK((LONG)got.win >= e.winmin, "win below minimum", e.winmin, got.win);
    if (e.have_winmax)
        CHECK((LONG)got.win <= e.winmax, "win above maximum", e.winmax, got.win);
    if (e.have_len)
        CHECK((LONG)got.dlen == e.len, "len", e.len, got.dlen);
    if (e.have_urg)
        CHECK((LONG)got.urg == e.urg, "urgent pointer", e.urg, got.urg);
    if (e.have_mss)
        CHECK(got.mss == e.mss, "mss option", e.mss, got.mss);
    if (e.have_sackok)
        CHECK((got.sackok ? 1 : 0) == e.sackok, "SACK-Permitted option",
              e.sackok, got.sackok ? 1 : 0);
    if (e.have_hdrlen)
        CHECK((LONG)got.thl == e.hdrlen, "TCP header length", e.hdrlen, got.thl);
    if (e.have_sack)
    {
        UWORD i;

        CHECK((LONG)got.sack_n == (LONG)e.sack_n, "SACK block count",
              e.sack_n, got.sack_n);
        for (i = 0; i < e.sack_n; i++)
        {
            CHECK(got.sack_l[i] == cs.p_isn + e.sack_l[i], "SACK block left edge",
                  e.sack_l[i], (LONG)(got.sack_l[i] - cs.p_isn));
            CHECK(got.sack_r[i] == cs.p_isn + e.sack_r[i], "SACK block right edge",
                  e.sack_r[i], (LONG)(got.sack_r[i] - cs.p_isn));
        }
    }

    CHECK(got.tcp_ok, "TCP checksum (0 = valid)", 0, 1);
    CHECK(got.ip_ok,  "IP checksum (0 = valid)", 0, 1);

    if (e.have_within || e.have_after)
    {
        ULONG gap = ticks_to_ms(cs.t_last, got.stamp);

        if (e.have_after)
            CHECK((LONG)gap >= e.after, "gap too short, ms", e.after, gap);
        if (e.have_within)
            CHECK((LONG)gap <= e.within, "gap too long, ms", e.within, gap);
    }
#undef CHECK

    {
        ULONG gap = ticks_to_ms(cs.t_last, got.stamp);

        cs.t_last = got.stamp;
        n_pass++;
        say("  ok   %s   [+%ums]", raw, gap);
    }
}

static VOID do_notx(const char *args, const char *raw)
{
    char  tok[24];
    ULONG ms;
    Seg   got;
    char  desc[360];

    (VOID)token(args, tok, sizeof(tok));
    ms = (ULONG)to_num(tok);

    if (wait_frame(&got, ms))
    {
        describe(&got, desc, sizeof(desc));
        fail(raw, desc);
        return;
    }
    pass(raw);
}

static VOID do_rx(const char *args, const char *raw)
{
    char   tok[48];
    Expect e;
    Inject in;

    zero((UBYTE *)&e, (ULONG)sizeof(e));
    args = token(args, tok, sizeof(tok));
    e.flags = parse_flags(tok);
    (VOID)parse_keys(args, &e);

    zero((UBYTE *)&in, (ULONG)sizeof(in));
    in.flags = e.flags;
    in.seq   = cs.p_isn + (ULONG)(e.have_seq ? e.seq : 0);
    in.ack   = cs.u_isn + (ULONG)(e.have_ack ? e.ack : 0);
    in.win   = (UWORD)(e.have_win ? e.win : 8192);
    in.urg   = (UWORD)(e.have_urg ? e.urg : 0);
    in.dlen  = (ULONG)(e.have_len ? e.len : 0);
    in.mss   = e.have_mss ? e.mss : -1;
    in.sackok = (e.have_sackok && e.sackok) ? TRUE : FALSE;
    in.dst_ip = e.have_dst ? e.dst : LOCAL_IP;

    /* Give the stack a moment to post its reads before the very first
       injection of a case; after that they are always outstanding. */
    pump();

    /*
     * The clock is read before the injection, not after.  tap_rx_put() ends in
     * ReplyMsg(), which signals the reader thread, and the reader runs at a
     * higher priority, so the stack's answer can be transmitted and stamped
     * before tap_rx_put() has returned here.  Reading the clock afterwards
     * made every `after`/`within` on an answering frame come out as an
     * unsigned underflow of about 6,057,780 ms.
     */
    cs.t_last = tap_eclock_now();
    build_and_inject(&in);

    /* One tick, so the reader thread has run before the next directive
       asserts on what the injection caused. */
    Delay(1);
    pump();

    pass(raw);
}

/* --------------------------------------------------------- socket verbs --- */

static VOID sock_nonblocking(LONG s)
{
    LONG one = 1;

    (VOID)s_ioctl(s, FIONBIO_, &one);
}

static VOID do_socket(const char *raw)
{
    cs.sock = s_socket(AF_INET_, SOCK_STREAM_, 0);
    if (cs.sock < 0)
    {
        fail(raw, "socket() failed");
        return;
    }
    sock_nonblocking(cs.sock);
    pass(raw);
}

static VOID do_connect(const char *raw)
{
    SockAddrIn a;
    LONG       rc;

    zero((UBYTE *)&a, (ULONG)sizeof(a));
    a.sin_len    = (UBYTE)sizeof(a);
    a.sin_family = AF_INET_;
    a.sin_port   = cs.peer_port;
    a.sin_addr   = PEER_IP;

    cs.t_last = tap_eclock_now();
    rc = s_connect(cs.sock, &a);

    if (rc == 0)
    {
        pass(raw);
        return;
    }
    if (s_errno() == E_INPROGRESS || s_errno() == E_WOULDBLOCK)
    {
        pass(raw);
        return;
    }
    {
        char  why[80];
        char *w = why;
        const char *t = "connect() failed, errno ";

        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
    }
}

static VOID do_listen(const char *raw)
{
    SockAddrIn a;
    LONG       one = 1;

    cs.lsock = s_socket(AF_INET_, SOCK_STREAM_, 0);
    if (cs.lsock < 0)
    {
        fail(raw, "socket() failed");
        return;
    }
    (VOID)s_setsockopt(cs.lsock, SOL_SOCKET_, SO_REUSEADDR_, &one, (LONG)sizeof(one));
    sock_nonblocking(cs.lsock);

    zero((UBYTE *)&a, (ULONG)sizeof(a));
    a.sin_len    = (UBYTE)sizeof(a);
    a.sin_family = AF_INET_;
    a.sin_port   = cs.local_port;
    a.sin_addr   = 0;

    if (s_bind(cs.lsock, &a) != 0)
    {
        char  why[64];
        char *w = why;
        const char *t = "bind() failed, errno ";

        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
        return;
    }
    if (s_listen(cs.lsock, 4) != 0)
    {
        fail(raw, "listen() failed");
        return;
    }
    pass(raw);
}

static VOID do_accept(const char *raw)
{
    UWORD tries;

    for (tries = 0; tries < 50; tries++)
    {
        LONG s = s_accept(cs.lsock);

        if (s >= 0)
        {
            cs.sock = s;
            sock_nonblocking(s);
            pass(raw);
            return;
        }
        pump();
        Delay(1);
    }
    fail(raw, "accept() never produced a socket");
}

static VOID do_send(const char *args, const char *raw)
{
    char  tok[24];
    LONG  want;
    LONG  rc;
    ULONG i;
    BOOL  want_again  = FALSE;
    BOOL  allow_short = FALSE;

    args = token(args, tok, sizeof(tok));
    want = to_num(tok);
    if (want > (LONG)sizeof(payload))
        want = (LONG)sizeof(payload);

    /*
     * `send N = AGAIN` -- the send is expected to be refused, which is what a
     * non-blocking socket must do against a closed window.
     *
     * `send N = SHORT` -- the send may take any part of it, including none.
     * How much is not the assertion; `wirebytes` is, and it needs a directive
     * that does not decide the answer in advance.
     */
    args = token(args, tok, sizeof(tok));
    if (tok[0] == '=')
    {
        (VOID)token(args, tok, sizeof(tok));
        want_again  = streq(tok, "AGAIN");
        allow_short = streq(tok, "SHORT");
    }

    for (i = 0; i < (ULONG)want; i++)
        payload[i] = (UBYTE)('A' + (i % 26));

    cs.t_last = tap_eclock_now();
    rc = s_send(cs.sock, payload, want, 0);

    if (rc > 0)
        cs.accepted += (ULONG)rc;

    if (allow_short)
    {
        if (rc < 0 && s_errno() != E_WOULDBLOCK)
        {
            char  why[64];
            char *w = why;
            const char *t = "send() failed, errno ";

            while (*t) *w++ = *t++;
            fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
            *w = '\0';
            fail(raw, why);
            return;
        }

        n_pass++;
        say("  ok   %s   [accepted %u]", raw, (rc > 0) ? (ULONG)rc : 0UL);
        return;
    }

    if (want_again)
    {
        if (rc < 0 && s_errno() == E_WOULDBLOCK)
        {
            pass(raw);
            return;
        }
    }
    else if (rc != want)
    {
        char  why[96];
        char *w = why;
        const char *t = "send() returned ";

        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)rc, 10, 0, TRUE);
        t = " of "; while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)want, 10, 0, TRUE);
        t = ", errno "; while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
        return;
    }
    if (want_again)
    {
        fail(raw, "send() was accepted where it had to be refused");
        return;
    }
    pass(raw);
}

static VOID do_oob(const char *args, const char *raw)
{
    char tok[24];
    UBYTE b;
    LONG  rc;

    (VOID)token(args, tok, sizeof(tok));
    b = (UBYTE)to_num(tok);

    cs.t_last = tap_eclock_now();
    rc = s_send(cs.sock, &b, 1, MSG_OOB_);

    if (rc != 1)
    {
        fail(raw, "send(MSG_OOB) did not send one byte");
        return;
    }
    cs.accepted += 1;
    pass(raw);
}

static VOID do_recv(const char *args, const char *raw)
{
    char  tok[24];
    LONG  max;
    LONG  want;
    BOOL  want_again = FALSE;
    LONG  rc;

    args = token(args, tok, sizeof(tok));
    max  = to_num(tok);
    if (max > (LONG)sizeof(payload))
        max = (LONG)sizeof(payload);

    args = token(args, tok, sizeof(tok));       /* '=' */
    (VOID)token(args, tok, sizeof(tok));
    if (streq(tok, "EOF"))
        want = 0;
    else if (streq(tok, "AGAIN"))
    {
        want = -1;
        want_again = TRUE;
    }
    else
        want = to_num(tok);

    rc = s_recv(cs.sock, payload, max, 0);

    if (want_again)
    {
        if (rc < 0 && s_errno() == E_WOULDBLOCK)
        {
            pass(raw);
            return;
        }
    }
    else if (rc == want)
    {
        pass(raw);
        return;
    }

    {
        char  why[110];
        char *w = why;
        const char *t = "recv() returned ";

        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)rc, 10, 0, TRUE);
        t = ", errno "; while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
    }
}

static VOID do_select(const char *args, const char *raw, BOOL write_set)
{
    char         tok[24];
    LONG         want;
    DrillFdSet   set;
    DrillTimeval tv;
    LONG         rc;

    (VOID)token(args, tok, sizeof(tok));
    want = to_num(tok);

    zero((UBYTE *)&set, (ULONG)sizeof(set));
    set.bits[cs.sock / 32] = 1UL << (cs.sock % 32);
    tv.tv_sec  = 0;
    tv.tv_usec = 0;

    if (write_set)
        rc = s_waitselect(cs.sock + 1, NULL, &set, NULL, &tv);
    else
        rc = s_waitselect(cs.sock + 1, &set, NULL, NULL, &tv);

    if ((rc > 0 ? 1 : 0) == want)
    {
        pass(raw);
        return;
    }

    {
        char  why[110];
        char *w = why;
        const char *t = "WaitSelect() reported ";

        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)rc, 10, 0, TRUE);
        t = " ready, wanted "; while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)want, 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
    }
}

static VOID do_shutdown(const char *args, const char *raw)
{
    char tok[24];
    LONG how;

    (VOID)token(args, tok, sizeof(tok));
    if (streq(tok, "rd"))        how = 0;
    else if (streq(tok, "wr"))   how = 1;
    else                         how = 2;

    cs.t_last = tap_eclock_now();

    if (s_shutdown(cs.sock, how) != 0)
    {
        char  why[80];
        char *w = why;
        const char *t = "shutdown() failed, errno ";

        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
        return;
    }
    pass(raw);
}

static VOID do_opt(const char *args, const char *raw)
{
    char tok[32];
    char val[24];
    LONG v;
    LONG rc;

    args = token(args, tok, sizeof(tok));
    (VOID)token(args, val, sizeof(val));
    v = to_num(val);

    if (streq(tok, "nodelay"))
        rc = s_setsockopt(cs.sock, IPPROTO_TCP_, TCP_NODELAY_, &v, (LONG)sizeof(v));
    else if (streq(tok, "rcvbuf"))
        rc = s_setsockopt(cs.sock, SOL_SOCKET_, SO_RCVBUF_, &v, (LONG)sizeof(v));
    else if (streq(tok, "sndbuf"))
        rc = s_setsockopt(cs.sock, SOL_SOCKET_, SO_SNDBUF_, &v, (LONG)sizeof(v));
    else if (streq(tok, "oobinline"))
        rc = s_setsockopt(cs.sock, SOL_SOCKET_, SO_OOBINLINE_, &v, (LONG)sizeof(v));
    else if (streq(tok, "reuseaddr"))
        rc = s_setsockopt(cs.sock, SOL_SOCKET_, SO_REUSEADDR_, &v, (LONG)sizeof(v));
    else if (streq(tok, "keepalive"))
        rc = s_setsockopt(cs.sock, SOL_SOCKET_, SO_KEEPALIVE_, &v, (LONG)sizeof(v));
    else if (streq(tok, "linger"))
    {
        LONG lin[2];
        lin[0] = (v >= 0) ? 1 : 0;
        lin[1] = (v >= 0) ? v : 0;
        rc = s_setsockopt(cs.sock, SOL_SOCKET_, SO_LINGER_, lin, (LONG)sizeof(lin));
    }
    else
    {
        fail(raw, "unknown option name");
        return;
    }

    if (rc != 0)
    {
        fail(raw, "setsockopt() failed");
        return;
    }
    pass(raw);
}

/*
 * `close [within=MS]`.
 *
 * CloseSocket() sends a FIN and the connection outlives the descriptor, so the
 * call must never wait for a peer that has stopped answering -- a program that
 * closes and exits would hang on the way out.  The optional bound asserts
 * that.  The number in the transcript is the whole CloseSocket(), measured
 * across the call rather than off the frame it produced.
 */
static VOID do_close(const char *args, const char *raw)
{
    Expect e;
    ULONG  before;
    ULONG  after;
    ULONG  ms;

    zero((UBYTE *)&e, (ULONG)sizeof(e));
    (VOID)parse_keys(args, &e);

    before    = tap_eclock_now();
    cs.t_last = before;

    if (cs.sock >= 0)
    {
        (VOID)s_close(cs.sock);
        cs.sock = -1;
    }

    after = tap_eclock_now();
    ms    = ticks_to_ms(before, after);

    if (e.have_within && (LONG)ms > e.within)
    {
        char  why[96];
        char *w = why;
        const char *t = "CloseSocket() took too long, ms: wanted ";

        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)e.within, 10, 0, TRUE);
        t = ", got ";
        while (*t) *w++ = *t++;
        fmt_num(&w, ms, 10, 0, FALSE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    n_pass++;
    say("  ok   %s   [%ums]", raw, ms);
}

/*
 * `txcount MIN MAX` -- how many frames the stack sent while we were not
 * looking, discarded.
 *
 * A retransmission series is asserted as a count: ten separate `tx` lines
 * would assert the intervals too, and the intervals belong to another
 * workstream.  This checks only that the stack kept retrying and then stopped.
 */
static VOID do_txcount(const char *args, const char *raw)
{
    char  tok[24];
    LONG  lo;
    LONG  hi;
    ULONG n = 0;
    Seg   junk;
    char  why[96];
    char *w = why;

    args = token(args, tok, sizeof(tok));
    lo   = to_num(tok);
    (VOID)token(args, tok, sizeof(tok));
    hi   = to_num(tok);

    pump();
    while (pend_pop(&junk))
        n++;

    if ((LONG)n < lo || (LONG)n > hi)
    {
        const char *t = "frames sent: wanted ";
        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)lo, 10, 0, TRUE);
        t = "..";
        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)hi, 10, 0, TRUE);
        t = ", got ";
        while (*t) *w++ = *t++;
        fmt_num(&w, n, 10, 0, FALSE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    cs.t_last = tap_eclock_now();
    n_pass++;
    say("  ok   %s   [%u frame(s)]", raw, n);
}

/*
 * `wirebytes` -- what left must equal what send() said it took.
 *
 * The one invariant a stream cannot state any other way.  A transfer that
 * merely completes proves nothing: duplicated bytes inside a stream still
 * arrive as a plausible file, and the 512 KB the fault was found on differed
 * from its source by 3888 bytes without ever failing to transfer.  So the
 * assertion is arithmetic on both sides -- the sum of the send() return values
 * against the distinct sequence space the stack committed to -- which is the
 * same sum a capture is put through, done here with no capture at all.
 */
static VOID do_wirebytes(const char *raw)
{
    ULONG wire;
    char  why[110];
    char *w = why;
    const char *t;

    pump();

    if (!cs.u_isn_known)
    {
        fail(raw, "no initial sequence number learned yet");
        return;
    }

    /* Data starts one past the SYN. */
    wire = cs.wire_seen ? (cs.wire_end - cs.u_isn - 1UL) : 0UL;

    if (wire != cs.accepted)
    {
        t = "wire bytes "; while (*t) *w++ = *t++;
        fmt_num(&w, wire, 10, 0, FALSE);
        t = ", send() accepted "; while (*t) *w++ = *t++;
        fmt_num(&w, cs.accepted, 10, 0, FALSE);
        t = " -- difference "; while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)((LONG)wire - (LONG)cs.accepted), 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    n_pass++;
    say("  ok   %s   [%u byte(s), both sides]", raw, wire);
}

/*
 * `idle MS` is REAL time, off the E-Clock, not a count of Delay(1) calls.
 *
 * It used to add 20 per iteration on the grounds that Delay(1) is one 50 Hz
 * tick.  pump() between them is not free, though: measured under an emulator
 * an `idle 700` took about 1,520 ms, and a case that used it to hold an
 * acknowledgment back for less than the one second retransmission timeout
 * instead held it back for longer than one, so the segment was retransmitted
 * in the middle of the wait.  That reads as the stack sending a frame it
 * should not have, which is the opposite of what happened.
 *
 * Cases that assert on an interval are unaffected either way -- `after` and
 * `within` are measured from the tap device's own E-Clock stamps -- so this
 * changes only how long `idle` actually idles for.
 */
static VOID do_idle(const char *args, const char *raw)
{
    char  tok[24];
    ULONG ms;
    ULONG start;

    (VOID)token(args, tok, sizeof(tok));
    ms = (ULONG)to_num(tok);

    start = tap_eclock_now();

    while (ticks_to_ms(start, tap_eclock_now()) < ms)
    {
        pump();
        Delay(1);
    }
    pump();
    pass(raw);
}

/* ----------------------------------------------------------- case control - */

/*
 * Tear a case's socket down so that nothing of it appears in the next case.
 *
 * SO_LINGER {on, 0} is required. Once CloseSocket() started sending a FIN --
 * which is what RFC 793 3.5 asks for and what this harness asserts in c03 --
 * a plain close left a connection retransmitting that FIN into a peer that had
 * stopped listening, once a second, for ten seconds. Those frames turned up in
 * the next four cases, and every case that leaves unacknowledged data behind
 * (x02, x03, x04, z01) does the same with the data. The abortive close ends
 * the connection at once, which a harness whose cases share one stack needs.
 */
static VOID case_abort(LONG s)
{
    LONG lin[2];

    if (s < 0)
        return;

    lin[0] = 1;
    lin[1] = 0;
    (VOID)s_setsockopt(s, SOL_SOCKET_, SO_LINGER_, lin, (LONG)sizeof(lin));
    (VOID)s_close(s);
}

static VOID case_end(VOID)
{
    if (cs.name[0] == '\0')
        return;

    case_abort(cs.sock);
    cs.sock = -1;
    case_abort(cs.lsock);
    cs.lsock = -1;

    /* Give the close its RST/FIN and swallow whatever comes of it, so the
       next case starts on an empty queue. */
    Delay(2);
    pump();
    while (pend_count != 0)
    {
        Seg junk;
        (VOID)pend_pop(&junk);
    }

    if (cs.fails == 0)
        say("PASS %s", cs.name);
    else
    {
        n_cases_failed++;
        say("FAIL %s (%u check(s))", cs.name, cs.fails);
    }
    say("");
}

static VOID case_begin(const char *name)
{
    ULONG i;

    case_end();

    zero((UBYTE *)&cs, (ULONG)sizeof(cs));
    for (i = 0; i + 1 < sizeof(cs.name) && name[i] != '\0'; i++)
        cs.name[i] = name[i];

    cs.sock       = -1;
    cs.lsock      = -1;
    cs.peer_port  = DEFAULT_PEER_PORT;
    cs.local_port = 0;
    /* A different ISN per case, so a stale segment from the previous one
       cannot be mistaken for a live one. */
    cs.p_isn      = 0x50000000UL + (n_cases * 0x00010000UL);
    cs.t_last     = tap_eclock_now();

    n_cases++;
    say("---- %s", cs.name);
}

/* ------------------------------------------------------------------- run -- */

static VOID run_line(char *line)
{
    char        verb[24];
    const char *args;
    char        raw[200];
    ULONG       i;

    /* Keep the source line for the report, minus trailing space. */
    for (i = 0; i + 1 < sizeof(raw) && line[i] != '\0'; i++)
        raw[i] = line[i];
    raw[i] = '\0';
    while (i != 0 && is_space(raw[i - 1]))
        raw[--i] = '\0';

    args = token(line, verb, sizeof(verb));

    if (verb[0] == '\0' || verb[0] == '#')
        return;

    if (streq(verb, "case"))
    {
        char name[48];
        (VOID)token(args, name, sizeof(name));
        case_begin(name);
        return;
    }

    if (cs.name[0] == '\0')
    {
        say("!! directive before any `case`: %s", raw);
        return;
    }

    if (streq(verb, "peerport"))
    {
        char tok[16];
        (VOID)token(args, tok, sizeof(tok));
        cs.peer_port = (UWORD)to_num(tok);

        /* A new source port is a new connection with an initial sequence
           number of its own, so the one learned from the previous SYN no
           longer describes it. Forgetting it here is what lets a case drive
           two connections to the same listening port. */
        cs.u_isn_known = FALSE;
    }
    else if (streq(verb, "localport"))
    {
        char tok[16];
        (VOID)token(args, tok, sizeof(tok));
        cs.local_port = (UWORD)to_num(tok);
    }
    else if (streq(verb, "socket"))   do_socket(raw);
    else if (streq(verb, "connect"))  do_connect(raw);
    else if (streq(verb, "listen"))   do_listen(raw);
    else if (streq(verb, "accept"))   do_accept(raw);
    else if (streq(verb, "send"))     do_send(args, raw);
    else if (streq(verb, "oob"))      do_oob(args, raw);
    else if (streq(verb, "recv"))     do_recv(args, raw);
    else if (streq(verb, "readable")) do_select(args, raw, FALSE);
    else if (streq(verb, "writable")) do_select(args, raw, TRUE);
    else if (streq(verb, "shutdown")) do_shutdown(args, raw);
    else if (streq(verb, "opt"))      do_opt(args, raw);
    else if (streq(verb, "close"))    do_close(args, raw);
    else if (streq(verb, "idle"))     do_idle(args, raw);
    else if (streq(verb, "tx"))       do_tx(args, raw);
    else if (streq(verb, "notx"))     do_notx(args, raw);
    else if (streq(verb, "txcount")) do_txcount(args, raw);
    else if (streq(verb, "wirebytes")) do_wirebytes(raw);
    else if (streq(verb, "rx"))       do_rx(args, raw);
    else
        say("!! unknown directive: %s", raw);
}

static LONG run_script(const char *path)
{
    BPTR  fh;
    char  line[256];

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0)
    {
        say("!! cannot open %s", path);
        return 20;
    }

    while (FGets(fh, (STRPTR)line, (LONG)sizeof(line)) != NULL)
    {
        ULONG n = d_len(line);

        while (n != 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        run_line(line);
    }

    Close(fh);
    case_end();
    return 0;
}

/* ------------------------------------------------------------------ main -- */

int main(void)
{
    TapStats st;
    UWORD    tries;
    LONG     rc = 0;

    out_file = Open((STRPTR)"DH0:tcpdrill.txt", MODE_NEWFILE);

    say("tcpdrill -- packet-level TCP conformance");

    if (tap_install(local_mac) != 0)
    {
        say("!! cannot install %s", TAP_DEVICE_NAME);
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    eclock_per_ms = tap_eclock_rate() / 1000;
    say("E-Clock %u Hz (%u ticks/ms)", tap_eclock_rate(), eclock_per_ms);

    /*
     * Opening the library is what brings DEVS:NetInterfaces/tap0 up, and
     * bring-up is what calls OpenDevice() on the device installed above.  The
     * order matters: a stack that started first would have looked for
     * tcpdrill.device in DEVS: and not found it.
     */
    SockBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (SockBase == NULL)
    {
        say("!! bsdsocket.library would not open");
        tap_remove();
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    for (tries = 0; tries < 250 && !tap_is_online(); tries++)
        Delay(1);

    if (!tap_is_online())
    {
        say("!! the interface never came online -- DEVS:NetInterfaces/tap0?");
        CloseLibrary(SockBase);
        tap_remove();
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    /* Let the readers post before the first injection. */
    for (tries = 0; tries < 50 && tap_reads_for(ETYPE_IP) == 0; tries++)
        Delay(1);

    say("device online, %u IPv4 read(s) and %u ARP read(s) outstanding",
        tap_reads_for(ETYPE_IP), tap_reads_for(ETYPE_ARP));
    say("");

    rc = run_script("DH0:drill.txt");

    tap_get_stats(&st);
    say("");
    say("tap: tx %u  rx delivered %u  rx no-reader %u  copy-failed %u  "
        "tx-overrun %u", st.tx_frames, st.rx_delivered, st.rx_no_reader,
        st.rx_copy_failed, st.tx_overrun);
    /* Frames the stack sent that no case is about (see pump()).  Reported so a
       change in the count is visible. */
    say("background frames ignored: %u", n_background);
    say("%u case(s), %u failed; %u check(s) passed, %u failed",
        n_cases, n_cases_failed, n_pass, n_fail);

    CloseLibrary(SockBase);
    SockBase = NULL;

    tap_remove();

    if (out_file != (BPTR)0)
        Close(out_file);

    if (rc != 0)
        return (int)rc;
    return (n_fail != 0) ? 5 : 0;
}
