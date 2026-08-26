/*
 * tcpdrill -- a packet-level conformance harness for AmiNetXDuo's TCP.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "tapdev.h"

/* The window bounds come from the same header the library compiles against,
   read by tests/tcpdrill/CMakeLists.txt.  Not defaulted here: a second copy of
   33580 is exactly the thing this replaces. */
#if !defined(DRILL_TCP_WINDOW) || !defined(DRILL_TCP_WINDOW_CEILING)
#error "DRILL_TCP_WINDOW / DRILL_TCP_WINDOW_CEILING come from CMake"
#endif

/* Which options the library was built with.  add_compile_definitions puts
   these on every target, so the drill reads the same switches the stack did
   and `hdrlen=auto` needs no script to enumerate them. */
#ifdef AMINETXDUO_TCP_WINDOW_SCALING
#define DRILL_HAS_WSCALE    1
#else
#define DRILL_HAS_WSCALE    0
#endif
#ifdef AMINETXDUO_TCP_SACK_OFF
#define DRILL_HAS_SACK      0
#else
#define DRILL_HAS_SACK      1
#endif
#ifdef AMINETXDUO_TCP_TIMESTAMP_OFF
#define DRILL_HAS_TS        0
#else
#define DRILL_HAS_TS        1
#endif

#define LOCAL_IP        0x0A090901UL            /* 10.9.9.1, DEVS:NetInterfaces */
#define PEER_IP         0x0A090902UL            /* 10.9.9.2, this harness       */

#define LOOPBACK_IP     0x7F000001UL            /* 127.0.0.1 */
#define FOREIGN_IP      0x0A09094DUL            /* 10.9.9.77 */

#define DST_UNICAST     0
#define DST_BCAST       1                       /* 255.255.255.255 */
#define DST_SUBNET      2                       /* 10.9.9.255      */
#define DST_MCAST       3                       /* 224.0.0.1       */
#define DEFAULT_PEER_PORT   9000

static const UBYTE local_mac[6] = { 0x02, 0x00, 0x44, 0x52, 0x4C, 0x01 };
static const UBYTE peer_mac[6]  = { 0x02, 0x00, 0x44, 0x52, 0x4C, 0x02 };

#define ETYPE_IP        0x0800
#define ETYPE_ARP       0x0806
#define ETYPE_IPV6      0x86DD
#define ETH_HDR         14

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

#define SO_SNDTIMEO_    0x1005
#define SO_RCVTIMEO_    0x1006

#define FIONBIO_        0x8004667EUL
#define MSG_OOB_        1
#define MSG_WAITALL_    0x40

#define E_WOULDBLOCK    35
#define E_INPROGRESS    36
#define E_ADDRNOTAVAIL  49
#define E_TIMEDOUT      60
#define E_NOTCONN       57

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

static VOID s_set_socket_signals(ULONG break_mask)
{
    LVO_CALL_HEAD;
    register ULONG d0 __asm("d0") = break_mask;
    register ULONG d1 __asm("d1") = 0;
    register ULONG d2 __asm("d2") = 0;

    __asm __volatile ("jsr a6@(-132:W)"
                      : "+r"(d0), "+r"(d1), "+r"(d2)
                      : "r"(a6)
                      : "a0", "a1", "cc", "memory");
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
        Flush(out_file);
    }
    Write(Output(), (APTR)s, (LONG)n);
}

/* A tiny formatter. %s %d %u %x are all this needs, and pulling in
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

typedef enum SayArgKind
{
    SAY_ARG_STRING,
    SAY_ARG_SIGNED,
    SAY_ARG_UNSIGNED
} SayArgKind;

typedef struct SayArg
{
    SayArgKind  kind;
    const char *string;
    ULONG       number;
} SayArg;

static VOID say_bad_arg(char **out, const char *limit)
{
    const char *s = "<?>";

    while (*s != '\0' && *out < limit)
        *(*out)++ = *s++;
}

static VOID say_args(const char *fmt, const SayArg *args, UWORD count)
{
    static char line[320];              /* 4 KB of Shell stack; see the header */
    char       *o = line;
    const char *f = fmt;
    UWORD       used = 0;

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
            const SayArg *arg = (used < count) ? &args[used] : NULL;
            f++;
            used++;
            if (arg != NULL && arg->kind == SAY_ARG_STRING &&
                arg->string != NULL)
            {
                const char *s = arg->string;

                while (*s != '\0' && (ULONG)(o - line) < sizeof(line) - 8)
                    *o++ = *s++;
            }
            else
                say_bad_arg(&o, line + sizeof(line) - 8);
        }
        else if (*f == 'd')
        {
            const SayArg *arg = (used < count) ? &args[used] : NULL;
            f++;
            used++;
            if (arg != NULL && arg->kind == SAY_ARG_SIGNED)
                fmt_num(&o, arg->number, 10, 0, TRUE);
            else
                say_bad_arg(&o, line + sizeof(line) - 8);
        }
        else if (*f == 'u')
        {
            const SayArg *arg = (used < count) ? &args[used] : NULL;
            f++;
            used++;
            if (arg != NULL && arg->kind == SAY_ARG_UNSIGNED)
                fmt_num(&o, arg->number, 10, 0, FALSE);
            else
                say_bad_arg(&o, line + sizeof(line) - 8);
        }
        else if (*f == 'x')
        {
            const SayArg *arg = (used < count) ? &args[used] : NULL;
            f++;
            used++;
            if (arg != NULL && arg->kind == SAY_ARG_UNSIGNED)
                fmt_num(&o, arg->number, 16, 0, FALSE);
            else
                say_bad_arg(&o, line + sizeof(line) - 8);
        }
        else
        {
            *o++ = *f++;
        }
    }

    *o++ = '\n';
    *o   = '\0';
    emit(line);
}

#define SAY_S(value) ((SayArg){ SAY_ARG_STRING, (value), 0 })
#define SAY_D(value) ((SayArg){ SAY_ARG_SIGNED, NULL, (ULONG)(LONG)(value) })
#define SAY_U(value) ((SayArg){ SAY_ARG_UNSIGNED, NULL, (ULONG)(value) })
#define SAY0(format) say_args((format), NULL, 0)
#define SAY(format, ...)                                                   \
    do {                                                                   \
        const SayArg say_arguments[] = { __VA_ARGS__ };                    \
        say_args((format), say_arguments,                                  \
                 (UWORD)(sizeof(say_arguments) / sizeof(say_arguments[0]))); \
    } while (0)

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
    UWORD   doff;               /* data offset, in words        */
    BOOL    ts;                 /* RFC 7323 timestamps present  */
    ULONG   tsval;
    ULONG   tsecr;
    LONG    wscale;             /* RFC 7323 shift, -1 when absent */
    BOOL    sackok;             /* RFC 2018 SACK-Permitted present */
    BOOL    has_sack;           /* an RFC 2018 SACK option was present */
    UWORD   n_sack;             /* blocks in it */
    ULONG   sack_lo[4];
    ULONG   sack_hi[4];
    BOOL    ip_ok;              /* IP header checksum verified  */
    BOOL    tcp_ok;             /* TCP checksum verified        */

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
    s->stamp  = stamp;
    s->ether  = rd16(&f[12]);
    s->mss    = -1;
    s->wscale = -1;

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
    s->doff = (UWORD)((tcp[12] >> 4) & 0x0F);
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
            if (*o == 3 && o[1] == 3 && o + 3 <= end)
                s->wscale = (LONG)o[2];
            if (*o == 4 && o[1] == 2)
                s->sackok = TRUE;
            if (*o == 5 && o[1] >= 10 && o + o[1] <= end)
            {
                UWORD n = (UWORD)((o[1] - 2) / 8);
                UWORD b;

                s->has_sack = TRUE;
                if (n > 4)
                    n = 4;
                s->n_sack = n;
                for (b = 0; b < n; b++)
                {
                    s->sack_lo[b] = rd32(&o[2 + (b * 8)]);
                    s->sack_hi[b] = rd32(&o[6 + (b * 8)]);
                }
            }
            if (*o == 8 && o[1] == 10 && o + 10 <= end)
            {
                s->ts    = TRUE;
                s->tsval = rd32(&o[2]);
                s->tsecr = rd32(&o[6]);
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

static ULONG eclock_per_ms;

static ULONG ticks_to_ms(ULONG a, ULONG b)     /* b - a, in milliseconds */
{
    ULONG d = b - a;                            /* unsigned wrap is correct */

    if (eclock_per_ms == 0)
        return 0;
    return d / eclock_per_ms;
}

#define GAP_BACKWARDS_MS    300000UL

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
    LONG    send_rc;            /* what the last send() returned */
    ULONG   wire_bytes;         /* tx payload accepted since that send */
    LONG    rwnd;
    LONG    our_wscale;         /* shift our SYN offered, -1 if none   */
    LONG    peer_wscale;        /* shift an injected SYN offered, -1   */
    BOOL    peer_sackok;        /* an injected SYN offered SACK        */
    BOOL    peer_ts;            /* an injected frame carried a timestamp */
    ULONG   local_addr;         /* what `listen` binds to, 0 = wildcard */
} Case;

static Case cs;

#define PEND_MAX    32
static Seg   pend[PEND_MAX];
static UWORD pend_head, pend_tail, pend_count;

static UBYTE scratch[TAP_FRAME_MAX];
static UBYTE payload[4096];

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

static ULONG n_background;      /* frames dropped by the filter in pump() */

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

        if (ether == ETYPE_IP && len >= ETH_HDR + 20 &&
            (scratch[ETH_HDR + 9] != 6 || rd32(&scratch[ETH_HDR + 16]) != PEER_IP))
        {
            n_background++;
            continue;
        }

        if (ether == ETYPE_IPV6)
        {
            n_background++;
            continue;
        }

        if (pend_count >= PEND_MAX)
        {
            /* Dropping here would leave every later assertion one frame out
               of step. */
            SAY("  !! frame queue overflow -- a case sent more than %u frames "
                "between directives", SAY_U(PEND_MAX));
            pend_tail = (UWORD)((pend_tail + 1) % PEND_MAX);
            pend_count--;
        }

        (VOID)decode(&pend[pend_head], scratch, len, stamp);
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

static BOOL wait_frame_for(Seg *out, ULONG ms, ULONG *took)
{
    ULONG start = tap_eclock_now();
    ULONG spent = 0;

    for (;;)
    {
        pump();
        if (pend_pop(out))
        {
            if (took != NULL)
                *took = ticks_to_ms(start, tap_eclock_now());
            return TRUE;
        }
        spent = ticks_to_ms(start, tap_eclock_now());
        if (spent >= ms)
        {
            if (took != NULL)
                *took = spent;
            return FALSE;
        }
        Delay(1);
    }
}

static BOOL wait_frame(Seg *out, ULONG ms)
{
    return wait_frame_for(out, ms, NULL);
}

typedef struct Inject
{
    UWORD   flags;
    ULONG   seq;                /* already absolute */
    ULONG   ack;
    UWORD   win;
    UWORD   urg;
    ULONG   dlen;
    LONG    mss;
    BOOL    ts;                 /* carry an RFC 7323 timestamps option */
    ULONG   tsval;
    ULONG   tsecr;
    LONG    wscale;             /* RFC 7323 window scale, -1 for none  */
    BOOL    sackok;             /* RFC 2018 SACK-Permitted             */
    UWORD   n_sack;
    ULONG   sack_lo[4];
    ULONG   sack_hi[4];
    LONG    corrupt;            /* payload byte to flip after the checksum */
    ULONG   dofs;
    ULONG   pad;                /* Ethernet padding past the datagram      */
    BOOL    unaligned;          /* hand the device an odd buffer           */
    BOOL    badopt;             /* an MSS option whose length byte says 3  */
    LONG    dst;                /* DST_UNICAST and the three below         */
} Inject;

static ULONG f_aligned[(TAP_FRAME_MAX / 4) + 2];

static BOOL  inject_quiet;
static ULONG n_inject_dropped;

static VOID build_and_inject(const Inject *in)
{
    UBYTE *f   = ((UBYTE *)f_aligned) + (in->unaligned ? 0 : 2);
    UBYTE *ip  = &f[ETH_HDR];
    UBYTE *tcp;
    /* NOP, NOP, kind 5, length, then 8 bytes a block: 4 + 8n, which is
       already a multiple of four and needs no padding. */
    UWORD  opt = (UWORD)(((in->mss >= 0) ? 4 : 0) + ((in->wscale >= 0) ? 4 : 0) +
                         (in->sackok ? 4 : 0) + (in->ts ? 12 : 0) +
                         ((in->n_sack > 0) ? (4 + (in->n_sack * 8)) : 0));
    UWORD  thl = (UWORD)(20 + opt);
    ULONG  i;
    ULONG  iplen;
    ULONG  dst_ip = LOCAL_IP;

    zero((UBYTE *)f_aligned, (ULONG)sizeof(f_aligned));

    cp(&f[0], local_mac, 6);
    cp(&f[6], peer_mac, 6);
    wr16(&f[12], ETYPE_IP);

    if (in->dst == DST_BCAST)
    {
        dst_ip = 0xFFFFFFFFUL;
        f[0] = f[1] = f[2] = f[3] = f[4] = f[5] = 0xFF;
    }
    else if (in->dst == DST_SUBNET)
    {
        dst_ip = (LOCAL_IP | 0x000000FFUL);
        f[0] = f[1] = f[2] = f[3] = f[4] = f[5] = 0xFF;
    }
    else if (in->dst == DST_MCAST)
    {
        dst_ip = 0xE0000001UL;
        f[0] = 0x01; f[1] = 0x00; f[2] = 0x5E;
        f[3] = 0x00; f[4] = 0x00; f[5] = 0x01;
    }

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
    wr32(&ip[16], dst_ip);
    wr16(&ip[10], ones_sum(ip, 20, 0));

    wr16(&tcp[0], cs.peer_port);
    wr16(&tcp[2], cs.local_port);
    wr32(&tcp[4], in->seq);
    wr32(&tcp[8], in->ack);
    tcp[12] = (UBYTE)((thl / 4) << 4);
    tcp[13] = (UBYTE)in->flags;
    wr16(&tcp[14], in->win);
    wr16(&tcp[18], in->urg);

    {
        UWORD at = 20;

        if (in->mss >= 0)
        {
            tcp[at]     = 2;
            /* `badopt=1`: the MSS option's length byte says 3, which is not
               its length.  A walk that trusts it steps into the middle of the
               next option and off the end of the area. */
            tcp[at + 1] = in->badopt ? 3 : 4;
            wr16(&tcp[at + 2], (UWORD)in->mss);
            at = (UWORD)(at + 4);
        }

        /* NOP, kind 3, length 3, shift.  The pad goes in FRONT: a zero behind
           a three-byte option is an end-of-option-list and everything after it
           is off the wire. */
        if (in->wscale >= 0)
        {
            tcp[at]     = 1;
            tcp[at + 1] = 3;
            tcp[at + 2] = 3;
            tcp[at + 3] = (UBYTE)in->wscale;
            at = (UWORD)(at + 4);
        }

        if (in->sackok)
        {
            tcp[at]     = 4;
            tcp[at + 1] = 2;
            tcp[at + 2] = 1;
            tcp[at + 3] = 1;
            at = (UWORD)(at + 4);
        }

        if (in->n_sack > 0)
        {
            UWORD b;

            tcp[at]     = 1;
            tcp[at + 1] = 1;
            tcp[at + 2] = 5;
            tcp[at + 3] = (UBYTE)(2 + (in->n_sack * 8));
            at = (UWORD)(at + 4);

            for (b = 0; b < in->n_sack; b++)
            {
                wr32(&tcp[at],     in->sack_lo[b]);
                wr32(&tcp[at + 4], in->sack_hi[b]);
                at = (UWORD)(at + 8);
            }
        }

        /* NOP, NOP, kind 8, length 10: the layout every stack sends, and the
           one _nx_tcp_timestamp_option_add writes. */
        if (in->ts)
        {
            tcp[at]     = 1;
            tcp[at + 1] = 1;
            tcp[at + 2] = 8;
            tcp[at + 3] = 10;
            wr32(&tcp[at + 4], in->tsval);
            wr32(&tcp[at + 8], in->tsecr);
        }
    }

    for (i = 0; i < in->dlen; i++)
        tcp[thl + i] = (UBYTE)('a' + ((in->dofs + i) % 26));

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

    if ((in->corrupt >= 0) && ((ULONG)in->corrupt < in->dlen))
    {
        tcp[thl + in->corrupt] ^= 0xFF;
    }

    if (tap_rx_put(f, ETH_HDR + iplen + in->pad) != 0)
    {
        /* The wire task (below) injects from a Process of its own, which has
           no Output() and must not reach SAY(); it counts instead and the
           main task reports the count at `join`. */
        if (inject_quiet)
            n_inject_dropped++;
        else
        {
            SAY0("  !! injection dropped -- no CMD_READ outstanding for 0x0800");
            cs.fails++;
        }
    }
}

static VOID inject_icmp(UBYTE type, UBYTE code, ULONG seq)
{
    static UBYTE f[TAP_FRAME_MAX];
    UBYTE *ip   = &f[ETH_HDR];
    UBYTE *icmp = ip + 20;
    UBYTE *q    = icmp + 8;
    ULONG  iplen = 20UL + 8UL + 20UL + 8UL;

    zero(f, (ULONG)sizeof(f));

    cp(&f[0], local_mac, 6);
    cp(&f[6], peer_mac, 6);
    wr16(&f[12], ETYPE_IP);

    ip[0] = 0x45;
    wr16(&ip[2], (UWORD)iplen);
    wr16(&ip[4], 0x4100);
    ip[8] = 64;
    ip[9] = 1;                          /* ICMP */
    wr32(&ip[12], PEER_IP);
    wr32(&ip[16], LOCAL_IP);
    wr16(&ip[10], ones_sum(ip, 20, 0));

    icmp[0] = type;
    icmp[1] = code;

    q[0] = 0x45;
    wr16(&q[2], (UWORD)(20 + 20));
    wr16(&q[4], 0x4200);
    q[8] = 64;
    q[9] = 6;                           /* TCP */
    wr32(&q[12], LOCAL_IP);
    wr32(&q[16], PEER_IP);
    wr16(&q[10], ones_sum(q, 20, 0));

    wr16(&q[20], cs.local_port);
    wr16(&q[22], cs.peer_port);
    wr32(&q[24], seq);

    wr16(&icmp[2], ones_sum(icmp, 8 + 20 + 8, 0));

    if (tap_rx_put(f, ETH_HDR + iplen) != 0)
    {
        SAY0("  !! injection dropped -- no CMD_READ outstanding for 0x0800");
        cs.fails++;
    }
}

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

static LONG cur_iter;

static BOOL is_word(char c)
{
    return (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
            ((c >= '0') && (c <= '9')) || (c == '_')) ? TRUE : FALSE;
}

static BOOL var_value(const char *name, LONG *out)
{
    if (streq(name, "i"))        { *out = cur_iter;                    return TRUE; }
    if (streq(name, "winfloor")) { *out = (LONG)DRILL_TCP_WINDOW;      return TRUE; }
    if (streq(name, "wincap"))   { *out = (LONG)DRILL_TCP_WINDOW_CEILING; return TRUE; }
    if (streq(name, "synwin"))
    {
        /* RFC 7323 2.2: the window field of a SYN is never scaled, so 65535 is
           the most one can carry however large the local buffer is. */
        *out = ((LONG)DRILL_TCP_WINDOW_CEILING > 65535L)
                   ? 65535L : (LONG)DRILL_TCP_WINDOW_CEILING;
        return TRUE;
    }
    if (streq(name, "rwnd"))     { *out = cs.rwnd;                     return TRUE; }
    return FALSE;
}

static LONG parse_operand(const char **pp)
{
    const char *s = *pp;
    LONG        v = 0;

    if (*s == '$')
    {
        char  name[24];
        ULONG n = 0;

        s++;
        while (is_word(*s) && (n + 1 < sizeof(name)))
            name[n++] = *s++;
        name[n] = '\0';

        if (!var_value(name, &v))
        {
            SAY("!! unknown variable $%s", SAY_S(name));
            n_fail++;
        }
    }
    else
    {
        BOOL neg = FALSE;

        if (*s == '-') { neg = TRUE; s++; }
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        if (neg)
            v = -v;
    }

    *pp = s;
    return v;
}

/* Left to right, no precedence.  `$rwnd/2/1460+2` says what it reads as, and
   the alternative is a script whose arithmetic has to be worked out. */
static LONG to_num(const char *s)
{
    LONG v = parse_operand(&s);

    while ((*s == '+') || (*s == '-') || (*s == '*') || (*s == '/'))
    {
        char op = *s++;
        LONG r  = parse_operand(&s);

        if (op == '+')
            v += r;
        else if (op == '-')
            v -= r;
        else if (op == '*')
            v *= r;
        else if (r != 0)
            v /= r;
    }
    return v;
}

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
    BOOL    have_ts;    LONG ts;
    BOOL    have_tsval; LONG tsval;
    BOOL    have_tsecr; LONG tsecr;
    BOOL    have_wsok;  LONG wsok;
    BOOL    have_wscale; LONG wscale;
    BOOL    have_sackok; LONG sackok;
    BOOL    have_sack;  UWORD n_sack; LONG sack_lo[4]; LONG sack_hi[4];
    BOOL    have_hdrlen; LONG hdrlen; BOOL hdrlen_auto;
    BOOL    have_badopt; LONG badopt;
    LONG    dst;                /* DST_* below */
    BOOL    have_within; LONG within;
    BOOL    have_after;  LONG after;
    BOOL    have_corrupt; LONG corrupt;
    BOOL    have_pad;    LONG pad;
    BOOL    have_unaligned;
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
            else if (streq(key, "ts"))    { e->have_ts = TRUE;     e->ts = to_num(eq + 1); }
            else if (streq(key, "tsval")) { e->have_tsval = TRUE;  e->tsval = to_num(eq + 1); }
            else if (streq(key, "tsecr")) { e->have_tsecr = TRUE;  e->tsecr = to_num(eq + 1); }
            else if (streq(key, "wsok"))  { e->have_wsok = TRUE;   e->wsok = to_num(eq + 1); }
            else if (streq(key, "wscale")){ e->have_wscale = TRUE; e->wscale = to_num(eq + 1); }
            else if (streq(key, "sackok")){ e->have_sackok = TRUE; e->sackok = to_num(eq + 1); }
            else if (streq(key, "sack"))
            {
                const char *v = eq + 1;

                e->have_sack = TRUE;
                e->n_sack    = 0;
                while ((*v != '\0') && (*v != '-') && (e->n_sack < 4))
                {
                    e->sack_lo[e->n_sack] = to_num(v);
                    while ((*v != '\0') && (*v != ':')) v++;
                    if (*v == ':') v++;
                    e->sack_hi[e->n_sack] = to_num(v);
                    e->n_sack++;
                    while ((*v != '\0') && (*v != ',')) v++;
                    if (*v == ',') v++;
                }
            }
            else if (streq(key, "hdrlen"))
            {
                e->have_hdrlen = TRUE;
                if (streq(eq + 1, "auto"))
                    e->hdrlen_auto = TRUE;
                else
                    e->hdrlen = to_num(eq + 1);
            }
            else if (streq(key, "within")){ e->have_within = TRUE; e->within = to_num(eq + 1); }
            else if (streq(key, "after")) { e->have_after = TRUE;  e->after = to_num(eq + 1); }
            else if (streq(key, "corrupt")) { e->have_corrupt = TRUE; e->corrupt = to_num(eq + 1); }
            else if (streq(key, "pad"))   { e->have_pad = TRUE;   e->pad = to_num(eq + 1); }
            else if (streq(key, "unaligned")) { e->have_unaligned = (to_num(eq + 1) != 0) ? TRUE : FALSE; }
            else if (streq(key, "badopt")) { e->have_badopt = TRUE; e->badopt = to_num(eq + 1); }
            else if (streq(key, "dst"))
            {
                const char *v = eq + 1;

                if (streq(v, "bcast"))       e->dst = DST_BCAST;
                else if (streq(v, "subnet")) e->dst = DST_SUBNET;
                else if (streq(v, "mcast"))  e->dst = DST_MCAST;
                else
                {
                    SAY("!! unknown dst=%s", SAY_S(v));
                    n_fail++;
                }
            }
            else
            {
                SAY("!! unknown key %s=", SAY_S(key));
                n_fail++;
            }
        }
        p = next;
    }
}

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
    if (s->mss >= 0)
    {
        const char *t = " mss="; while (*t != '\0') *o++ = *t++;
        fmt_num(&o, (ULONG)s->mss, 10, 0, FALSE);
    }
    {
        const char *t = " doff="; while (*t != '\0') *o++ = *t++;
        fmt_num(&o, s->doff, 10, 0, FALSE);
    }
    if (s->ts)
    {
        const char *t = " ts="; while (*t != '\0') *o++ = *t++;
        fmt_num(&o, s->tsval, 10, 0, FALSE);
        t = " ecr="; while (*t != '\0') *o++ = *t++;
        fmt_num(&o, s->tsecr, 10, 0, FALSE);
    }
    if (s->wscale >= 0)
    {
        const char *t = " wscale="; while (*t != '\0') *o++ = *t++;
        fmt_num(&o, (ULONG)s->wscale, 10, 0, FALSE);
    }
    if (s->sackok)
    {
        const char *t = " sackOK"; while (*t != '\0') *o++ = *t++;
    }
    if (s->has_sack)
    {
        const char *t = " sack="; UWORD b;

        while (*t != '\0') *o++ = *t++;
        for (b = 0; b < s->n_sack; b++)
        {
            if (b != 0)
                *o++ = ',';
            fmt_num(&o, s->sack_lo[b] - cs.p_isn, 10, 0, FALSE);
            *o++ = ':';
            fmt_num(&o, s->sack_hi[b] - cs.p_isn, 10, 0, FALSE);
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
    SAY("  ok   %s", SAY_S(what));
}

static VOID fail(const char *what, const char *why)
{
    n_fail++;
    cs.fails++;
    SAY("  FAIL %s", SAY_S(what));
    SAY("       %s", SAY_S(why));
}

static VOID do_tx(const char *args, const char *raw)
{
    char    tok[48];
    Expect  e;
    Seg     got;
    ULONG   limit;
    LONG    win_full = 0;
    char    desc[280];
    char    why[280];
    char   *w;

    zero((UBYTE *)&e, (ULONG)sizeof(e));
    args = token(args, tok, sizeof(tok));
    e.flags = parse_flags(tok);
    (VOID)parse_keys(args, &e);

    limit = e.have_within ? ((ULONG)e.within + 60UL) * 2UL : 4000UL;

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

    if ((got.flags & TF_SYN) != 0)
    {
        if (got.wscale >= 0)
            cs.our_wscale = got.wscale;
        win_full = (LONG)got.win;
    }
    else
    {
        LONG shift = ((cs.our_wscale >= 0) && (cs.peer_wscale >= 0))
                         ? cs.our_wscale : 0;

        win_full = (LONG)((ULONG)got.win << shift);
        if (cs.rwnd == 0)
            cs.rwnd = win_full;
    }

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
        SAY("       observed  %s", SAY_S(desc));
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
            SAY("       observed  %s", SAY_S(desc));                         \
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
        CHECK(win_full == e.win, "win", e.win, win_full);
    if (e.have_winmin)
        CHECK(win_full >= e.winmin, "win below minimum", e.winmin, win_full);
    if (e.have_winmax)
        CHECK(win_full <= e.winmax, "win above maximum", e.winmax, win_full);
    if (e.have_len)
        CHECK((LONG)got.dlen == e.len, "len", e.len, got.dlen);
    if (e.have_urg)
        CHECK((LONG)got.urg == e.urg, "urgent pointer", e.urg, got.urg);
    if (e.have_mss)
        CHECK(got.mss == e.mss, "mss option", e.mss, got.mss);
    if (e.have_ts)
        CHECK((LONG)(got.ts ? 1 : 0) == e.ts, "timestamps option present",
              e.ts, got.ts ? 1 : 0);
    /* TSecr is the peer's own TSval coming back, so a script may name it; TSval
       is this stack's clock and nothing may assert a value for it. */
    if (e.have_tsecr)
        CHECK(got.ts && ((LONG)got.tsecr == e.tsecr), "TSecr",
              e.tsecr, got.ts ? (LONG)got.tsecr : -1);
    if (e.have_wsok)
        CHECK((LONG)((got.wscale >= 0) ? 1 : 0) == e.wsok,
              "window scale option present", e.wsok, (got.wscale >= 0) ? 1 : 0);
    if (e.have_wscale)
        CHECK(got.wscale == e.wscale, "window scale shift",
              e.wscale, got.wscale);
    if (e.have_sackok)
        CHECK((LONG)(got.sackok ? 1 : 0) == e.sackok, "SACK-Permitted present",
              e.sackok, got.sackok ? 1 : 0);
    if (e.have_sack)
    {
        UWORD b;

        CHECK((LONG)got.n_sack == (LONG)e.n_sack, "SACK blocks",
              e.n_sack, got.n_sack);
        for (b = 0; b < e.n_sack; b++)
        {
            CHECK(got.sack_lo[b] == cs.p_isn + (ULONG)e.sack_lo[b],
                  "SACK block start", e.sack_lo[b],
                  (LONG)(got.sack_lo[b] - cs.p_isn));
            CHECK(got.sack_hi[b] == cs.p_isn + (ULONG)e.sack_hi[b],
                  "SACK block end", e.sack_hi[b],
                  (LONG)(got.sack_hi[b] - cs.p_isn));
        }
    }
    if (e.have_hdrlen)
    {
        LONG want = e.hdrlen;

        if (e.hdrlen_auto)
        {
            BOOL syn = ((e.flags & TF_SYN) != 0) ? TRUE : FALSE;
            BOOL sa  = (syn && ((e.flags & TF_ACK) != 0)) ? TRUE : FALSE;
            BOOL ws  = (DRILL_HAS_WSCALE && (!sa || (cs.peer_wscale >= 0)))
                           ? TRUE : FALSE;
            BOOL sk  = (DRILL_HAS_SACK && (!sa || cs.peer_sackok))
                           ? TRUE : FALSE;
            BOOL ts  = (DRILL_HAS_TS && ((syn && !sa) || cs.peer_ts))
                           ? TRUE : FALSE;

            want = 20;
            if (syn)
            {
                want += 8;
                if (ws && sk)
                    want += 4;
            }
            if (ts)
                want += 12;
            if (e.have_sack && (e.n_sack != 0))
                want += 4 + (LONG)(e.n_sack * 8);
        }
        CHECK((LONG)(got.doff * 4) == want, "data offset, bytes",
              want, got.doff * 4);
    }

    CHECK(got.tcp_ok, "TCP checksum (0 = valid)", 0, 1);
    CHECK(got.ip_ok,  "IP checksum (0 = valid)", 0, 1);

    if (e.have_within || e.have_after)
    {
        ULONG gap = ticks_to_ms(cs.t_last, got.stamp);

        CHECK(gap < GAP_BACKWARDS_MS,
              "sent BEFORE the previous directive, ms early", 0,
              (LONG)ticks_to_ms(got.stamp, cs.t_last));
        if (e.have_after)
            CHECK((LONG)gap >= e.after, "gap too short, ms", e.after, gap);
        if (e.have_within)
            CHECK((LONG)gap <= e.within, "gap too long, ms", e.within, gap);
    }
#undef CHECK

    {
        ULONG       gap  = ticks_to_ms(cs.t_last, got.stamp);
        const char *sign = "+";

        if (gap >= GAP_BACKWARDS_MS)
        {
            gap  = ticks_to_ms(got.stamp, cs.t_last);
            sign = "-";
        }

        cs.t_last = got.stamp;
        cs.wire_bytes += (ULONG)got.dlen;
        n_pass++;
        SAY("  ok   %s   [%s%ums]", SAY_S(raw), SAY_S(sign), SAY_U(gap));
    }
}

static VOID do_wirebytes(const char *args, const char *raw)
{
    char why[96];

    (VOID)args;

    if (cs.send_rc < 0)
    {
        fail(raw, "no send() to credit");
        return;
    }

    if ((ULONG)cs.send_rc != cs.wire_bytes)
    {
        char       *w = why;
        const char *t = "send() credited ";

        while (*t != '\0') *w++ = *t++;
        fmt_num(&w, (ULONG)cs.send_rc, 10, 0, TRUE);
        t = " but "; while (*t != '\0') *w++ = *t++;
        fmt_num(&w, cs.wire_bytes, 10, 0, TRUE);
        t = " left"; while (*t != '\0') *w++ = *t++;
        *w = '\0';
        fail(raw, why);
        return;
    }

    pass(raw);
}

#define WIRE_GRANT      1
#define WIRE_DRIBBLE    2
#define WIRE_FINACK     3
#define WIRE_SILENT     4

typedef struct Wire
{
    UWORD           w_Mode;
    ULONG           w_Win;      /* grant: the window each grant re-opens  */
    ULONG           w_Grants;   /* grant: how many, before the stall      */
    ULONG           w_Open;     /* grant: the window after the stall      */
    ULONG           w_Stall;    /* grant: ms of silence in between        */
    ULONG           w_Bytes;    /* dribble: payload to deliver            */
    ULONG           w_Chunk;    /* dribble: payload per segment           */
    ULONG           w_Gap;      /* dribble: ms between segments           */
    ULONG           w_After;    /* finack: ms to hold a FIN               */

    volatile BOOL   w_Run;      /* main -> wire: keep going               */
    volatile BOOL   w_Done;     /* wire -> main: left its own code        */

    /* Results.  Read after w_Done, so they need no volatile. */
    ULONG           w_Given;    /* grants issued                          */
    ULONG           w_Segs;     /* data segments seen                     */
    ULONG           w_Next;     /* contiguous stream bytes received       */
    ULONG           w_High;     /* highest stream offset seen             */
    LONG            w_BadAt;    /* first byte that was not the pattern    */
    LONG            w_GapAt;    /* first segment that came after a hole   */
    ULONG           w_Sent;     /* payload this peer injected             */
    LONG            w_FinAt;    /* ms from `wire` to the FIN, -1 = none   */
    ULONG           w_BadSum;   /* frames with a wrong TCP checksum       */
    ULONG           w_Other;    /* frames that were not ours              */
    BOOL            w_Opened;   /* grant: the stall is over               */
    BOOL            w_Answered; /* finack: the FIN has been answered      */
    ULONG           w_StallEnd; /* grant: ms at which it is               */
    ULONG           w_NextAt;   /* dribble: ms of the next segment        */
    LONG            w_FinDue;   /* finack: ms at which to answer, -1 none */
} Wire;

static Wire  wire;
static BOOL  wire_live;         /* a task exists and has not been joined  */
static ULONG wire_t0;
static UBYTE wire_scratch[TAP_FRAME_MAX];

static VOID wire_put(UWORD flags, ULONG win, ULONG dlen, ULONG ackx)
{
    Inject in;

    zero((UBYTE *)&in, (ULONG)sizeof(in));
    in.flags   = flags;
    in.seq     = cs.p_isn + 1UL + wire.w_Sent;
    in.ack     = cs.u_isn + 1UL + wire.w_Next + ackx;
    in.win     = (UWORD)win;
    in.dlen    = dlen;
    in.dofs    = wire.w_Sent;
    in.mss     = -1;
    in.wscale  = -1;
    in.corrupt = -1;

    build_and_inject(&in);
    wire.w_Sent += dlen;
}

static VOID wire_saw(const UBYTE *f, const Seg *s)
{
    ULONG        ihl, thl, o, i;
    const UBYTE *d;

    if (!s->tcp_ok)
        wire.w_BadSum++;

    if (s->dlen == 0)
        return;

    ihl = (ULONG)(f[ETH_HDR] & 0x0F) * 4UL;
    thl = (ULONG)((f[ETH_HDR + ihl + 12] >> 4) & 0x0F) * 4UL;
    d   = &f[ETH_HDR + ihl + thl];
    o   = s->seq - (cs.u_isn + 1UL);

    wire.w_Segs++;

    for (i = 0; i < s->dlen; i++)
    {
        if (d[i] != (UBYTE)('A' + ((o + i) % 26)))
        {
            if (wire.w_BadAt < 0)
                wire.w_BadAt = (LONG)(o + i);
            break;
        }
    }

    /* A hole.  A retransmission (o below the water mark) is not one: it is
       what a peer that stopped acknowledging is supposed to provoke. */
    if (o > wire.w_Next && wire.w_GapAt < 0)
        wire.w_GapAt = (LONG)o;

    if (o <= wire.w_Next && o + s->dlen > wire.w_Next)
        wire.w_Next = o + s->dlen;
    if (o + s->dlen > wire.w_High)
        wire.w_High = o + s->dlen;
}

static VOID wire_react(const Seg *s, ULONG now)
{
    switch (wire.w_Mode)
    {
    case WIRE_GRANT:
        if (s->dlen == 0)
            break;
        if (wire.w_Given < wire.w_Grants)
        {
            wire.w_Given++;
            wire_put(TF_ACK, wire.w_Win, 0UL, 0UL);
        }
        else if (wire.w_Opened)
            wire_put(TF_ACK, wire.w_Open, 0UL, 0UL);
        else if (wire.w_StallEnd == 0)
            wire.w_StallEnd = now + wire.w_Stall;
        break;

    case WIRE_DRIBBLE:
        break;                          /* driven by the clock, below */

    case WIRE_FINACK:
        if ((s->flags & TF_FIN) != 0 && wire.w_FinDue < 0)
            wire.w_FinDue = (LONG)(now + wire.w_After);
        else if (s->dlen != 0)
            wire_put(TF_ACK, 8192UL, 0UL, 0UL);
        break;

    default:
        break;
    }
}

static VOID wire_body(VOID)
{
    UWORD settle = 0;

    for (;;)
    {
        ULONG len, stamp, now;

        while ((len = tap_tx_get(wire_scratch, (ULONG)sizeof(wire_scratch),
                                 &stamp)) != 0)
        {
            Seg s;

            if (rd16(&wire_scratch[12]) == ETYPE_ARP)
            {
                if (len >= 42 && rd16(&wire_scratch[20]) == 1 &&
                    rd32(&wire_scratch[38]) == PEER_IP)
                    arp_reply(wire_scratch);
                continue;
            }

            if (!decode(&s, wire_scratch, len, stamp) || !s.is_tcp ||
                s.dst_ip != PEER_IP)
            {
                wire.w_Other++;
                continue;
            }

            if (wire.w_FinAt < 0 && (s.flags & TF_FIN) != 0)
                wire.w_FinAt = (LONG)ticks_to_ms(wire_t0, s.stamp);

            wire_saw(wire_scratch, &s);
            wire_react(&s, ticks_to_ms(wire_t0, tap_eclock_now()));
        }

        now = ticks_to_ms(wire_t0, tap_eclock_now());

        if (wire.w_Mode == WIRE_GRANT && !wire.w_Opened &&
            wire.w_StallEnd != 0 && now >= wire.w_StallEnd)
        {
            wire.w_Opened = TRUE;
            wire_put(TF_ACK, wire.w_Open, 0UL, 0UL);
        }

        if (wire.w_Mode == WIRE_DRIBBLE && wire.w_Sent < wire.w_Bytes &&
            now >= wire.w_NextAt)
        {
            ULONG n = wire.w_Bytes - wire.w_Sent;

            if (n > wire.w_Chunk)
                n = wire.w_Chunk;
            wire_put((UWORD)(TF_ACK | TF_PSH), 8192UL, n, 0UL);
            wire.w_NextAt = now + wire.w_Gap;
        }

        if (wire.w_Mode == WIRE_FINACK && !wire.w_Answered &&
            wire.w_FinDue >= 0 && now >= (ULONG)wire.w_FinDue)
        {
            wire.w_Answered = TRUE;
            wire_put((UWORD)(TF_FIN | TF_ACK), 8192UL, 0UL, 1UL);
        }

        if (!wire.w_Run)
        {
            if (settle >= 8)            /* 8 ticks, 160 ms */
                break;
            settle++;
        }

        Delay(1);
    }

    wire.w_Done = TRUE;
}

static VOID wire_entry(VOID)
{
    wire_body();
}

static VOID do_wire(const char *args, const char *raw)
{
    struct TagItem  tags[6];
    struct Process *proc;
    char            tok[40];

    if (wire_live)
    {
        fail(raw, "a wire task is already running; `join` it first");
        return;
    }

    zero((UBYTE *)&wire, (ULONG)sizeof(wire));
    wire.w_BadAt  = -1;
    wire.w_GapAt  = -1;
    wire.w_FinAt  = -1;
    wire.w_FinDue = -1;
    wire.w_Open   = 8192UL;
    wire.w_Stall  = 1200UL;
    wire.w_Chunk  = 100UL;
    wire.w_Gap    = 60UL;

    args = token(args, tok, sizeof(tok));
    if (streq(tok, "grant"))        wire.w_Mode = WIRE_GRANT;
    else if (streq(tok, "dribble")) wire.w_Mode = WIRE_DRIBBLE;
    else if (streq(tok, "finack"))  wire.w_Mode = WIRE_FINACK;
    else if (streq(tok, "silent"))  wire.w_Mode = WIRE_SILENT;
    else
    {
        fail(raw, "unknown wire mode");
        return;
    }

    for (;;)
    {
        const char *next = token(args, tok, sizeof(tok));
        char       *eq   = tok;
        LONG        v;

        if (tok[0] == '\0' || tok[0] == '#')
            break;

        while (*eq != '\0' && *eq != '=')
            eq++;
        if (*eq != '=')
        {
            args = next;
            continue;
        }
        *eq = '\0';
        v = to_num(eq + 1);

        if (streq(tok, "win"))          wire.w_Win    = (ULONG)v;
        else if (streq(tok, "grants"))  wire.w_Grants = (ULONG)v;
        else if (streq(tok, "open"))    wire.w_Open   = (ULONG)v;
        else if (streq(tok, "stall"))   wire.w_Stall  = (ULONG)v;
        else if (streq(tok, "bytes"))   wire.w_Bytes  = (ULONG)v;
        else if (streq(tok, "chunk"))   wire.w_Chunk  = (ULONG)v;
        else if (streq(tok, "gap"))     wire.w_Gap    = (ULONG)v;
        else if (streq(tok, "after"))   wire.w_After  = (ULONG)v;
        else
        {
            SAY("!! unknown wire key %s=", SAY_S(tok));
            n_fail++;
        }
        args = next;
    }

    /* Nothing the main task queued may be left for the wire task to trip
       over, and nothing the wire task collects goes back in the queue. */
    pump();
    while (pend_count != 0)
    {
        Seg junk;
        (VOID)pend_pop(&junk);
    }

    wire_t0          = tap_eclock_now();
    wire.w_NextAt    = wire.w_Gap;
    wire.w_Run       = TRUE;
    wire.w_Done      = FALSE;
    inject_quiet     = TRUE;
    n_inject_dropped = 0;

    tags[0].ti_Tag = NP_Entry;     tags[0].ti_Data = (ULONG)wire_entry;
    tags[1].ti_Tag = NP_Name;      tags[1].ti_Data = (ULONG)"tcpdrill wire";
    tags[2].ti_Tag = NP_StackSize; tags[2].ti_Data = 16384UL;
    tags[3].ti_Tag = NP_Cli;       tags[3].ti_Data = (ULONG)FALSE;
    tags[4].ti_Tag = NP_Priority;  tags[4].ti_Data = (ULONG)1;
    tags[5].ti_Tag = TAG_DONE;     tags[5].ti_Data = 0;

    proc = CreateNewProc(tags);
    if (proc == NULL)
    {
        inject_quiet = FALSE;
        wire.w_Run   = FALSE;
        fail(raw, "CreateNewProc() for the wire task failed");
        return;
    }

    wire_live = TRUE;
    pass(raw);
}

/* Stop the wire task.  Safe to call when none is running. */
static BOOL wire_stop(void)
{
    ULONG spent = 0;

    if (!wire_live)
        return TRUE;

    wire.w_Run = FALSE;
    while (!wire.w_Done && spent < 6000UL)
    {
        Delay(1);
        spent += 20UL;
    }

    /* Two ticks past w_Done: the task sets it as the last statement of
       wire_body() and still has to unwind out of this program's code. */
    Delay(2);
    wire_live    = FALSE;
    inject_quiet = FALSE;

    return wire.w_Done;
}

static VOID do_join(const char *args, const char *raw)
{
    (VOID)args;

    if (!wire_live)
    {
        fail(raw, "no wire task to join");
        return;
    }

    if (!wire_stop())
    {
        fail(raw, "the wire task never stopped");
        return;
    }

    SAY("       wire: %u seg(s), %u byte(s) contiguous, %u seen, "
        "%u grant(s), %u injected", SAY_U(wire.w_Segs), SAY_U(wire.w_Next),
        SAY_U(wire.w_High), SAY_U(wire.w_Given), SAY_U(wire.w_Sent));
    if (wire.w_FinAt >= 0)
        SAY("       wire: the FIN arrived %ums in", SAY_U(wire.w_FinAt));
    if (wire.w_BadSum != 0 || wire.w_Other != 0 || n_inject_dropped != 0)
        SAY("       wire: %u bad checksum(s), %u foreign frame(s), "
            "%u injection(s) dropped", SAY_U(wire.w_BadSum),
            SAY_U(wire.w_Other), SAY_U(n_inject_dropped));

    if (wire.w_BadSum != 0)
    {
        fail(raw, "the peer received a segment with a wrong TCP checksum");
        return;
    }
    pass(raw);
}

static VOID do_wirestream(const char *args, const char *raw)
{
    char  tok[24];
    ULONG want;
    char  why[120];
    char *w;

    (VOID)token(args, tok, sizeof(tok));
    want = (ULONG)to_num(tok);

    if (wire_live)
    {
        fail(raw, "`join` the wire task before reading what it saw");
        return;
    }

    if (wire.w_BadAt >= 0)
    {
        const char *t = "stream byte ";

        w = why;
        while (*t != '\0') *w++ = *t++;
        fmt_num(&w, (ULONG)wire.w_BadAt, 10, 0, FALSE);
        t = " is not the byte that offset carries -- the application's bytes "
            "went out twice"; while (*t != '\0') *w++ = *t++;
        *w = '\0';
        fail(raw, why);
        return;
    }

    if (wire.w_GapAt >= 0)
    {
        const char *t = "a hole in the stream at ";

        w = why;
        while (*t != '\0') *w++ = *t++;
        fmt_num(&w, (ULONG)wire.w_GapAt, 10, 0, FALSE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    if (wire.w_Next != want || wire.w_High != want)
    {
        const char *t = "the peer got ";

        w = why;
        while (*t != '\0') *w++ = *t++;
        fmt_num(&w, wire.w_Next, 10, 0, FALSE);
        t = " contiguous and "; while (*t != '\0') *w++ = *t++;
        fmt_num(&w, wire.w_High, 10, 0, FALSE);
        t = " in all, wanted "; while (*t != '\0') *w++ = *t++;
        fmt_num(&w, want, 10, 0, FALSE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    pass(raw);
}

static VOID do_wiregrants(const char *args, const char *raw)
{
    char  tok[24];
    ULONG want;
    char  why[80];
    char *w;

    (VOID)token(args, tok, sizeof(tok));
    want = (ULONG)to_num(tok);

    if (wire.w_Given == want)
    {
        pass(raw);
        return;
    }

    w = why;
    { const char *t = "the peer issued "; while (*t != '\0') *w++ = *t++; }
    fmt_num(&w, wire.w_Given, 10, 0, FALSE);
    { const char *t = " grant(s), wanted "; while (*t != '\0') *w++ = *t++; }
    fmt_num(&w, want, 10, 0, FALSE);
    *w = '\0';
    fail(raw, why);
}

static VOID do_notx(const char *args, const char *raw)
{
    char  tok[24];
    ULONG ms;
    ULONG took = 0;
    Seg   got;
    char  desc[280];

    (VOID)token(args, tok, sizeof(tok));
    ms = (ULONG)to_num(tok);

    if (wait_frame_for(&got, ms, &took))
    {
        describe(&got, desc, sizeof(desc));
        fail(raw, desc);
        SAY("       at        +%ums", SAY_U(took));
        return;
    }
    SAY("  ok   %s   [%ums quiet]", SAY_S(raw), SAY_U(took));
    n_pass++;
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
    in.badopt = (e.have_badopt && (e.badopt != 0)) ? TRUE : FALSE;
    in.dst    = e.dst;
    in.mss   = e.have_mss ? e.mss : (in.badopt ? 1460 : -1);
    in.ts    = (e.have_tsval || e.have_tsecr || (e.have_ts && e.ts != 0)) ? TRUE : FALSE;
    in.tsval = (ULONG)(e.have_tsval ? e.tsval : 0);
    in.tsecr = (ULONG)(e.have_tsecr ? e.tsecr : 0);
    in.wscale = e.have_wscale ? e.wscale : -1;
    in.sackok = (e.have_sackok && (e.sackok != 0)) ? TRUE : FALSE;
    in.corrupt   = e.have_corrupt ? e.corrupt : -1;

    if (e.have_sack && (e.n_sack > 0))
    {
        UWORD b;

        in.n_sack = e.n_sack;
        for (b = 0; b < e.n_sack; b++)
        {
            in.sack_lo[b] = cs.u_isn + (ULONG)e.sack_lo[b];
            in.sack_hi[b] = cs.u_isn + (ULONG)e.sack_hi[b];
        }
    }

    if ((in.flags & TF_SYN) != 0)
    {
        if (in.wscale >= 0)
            cs.peer_wscale = in.wscale;
        if (in.sackok)
            cs.peer_sackok = TRUE;
        if (in.ts)
            cs.peer_ts = TRUE;
    }
    in.pad       = (ULONG)(e.have_pad ? e.pad : 0);
    in.unaligned = e.have_unaligned;

    pump();

    cs.t_last = tap_eclock_now();
    build_and_inject(&in);

    Delay(1);
    pump();

    pass(raw);
}

static VOID do_icmp(const char *args, const char *raw)
{
    char   tok[48];
    Expect e;
    LONG   type;
    LONG   code;

    args = token(args, tok, sizeof(tok));
    type = to_num(tok);
    args = token(args, tok, sizeof(tok));
    code = to_num(tok);

    zero((UBYTE *)&e, (ULONG)sizeof(e));
    (VOID)parse_keys(args, &e);

    pump();
    cs.t_last = tap_eclock_now();
    inject_icmp((UBYTE)type, (UBYTE)code,
                cs.u_isn + (ULONG)(e.have_seq ? e.seq : 0));

    Delay(1);
    pump();

    pass(raw);
}

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

static VOID do_connect(const char *args, const char *raw)
{
    SockAddrIn a;
    LONG       rc;
    LONG       want_errno = 0;
    LONG       min_ms = 0;
    LONG       max_ms = 0;
    BOOL       no_break = FALSE;
    ULONG      t0;
    ULONG      took;

    {
        char tok[24];

        args = token(args, tok, sizeof(tok));       /* '=' or nothing */
        if (tok[0] == '=')
        {
            args = token(args, tok, sizeof(tok));
            want_errno = streq(tok, "ETIMEDOUT") ? E_TIMEDOUT : to_num(tok);
        }
    }

    for (;;)
    {
        char  tok[24];
        char *eq;

        args = token(args, tok, sizeof(tok));
        if (tok[0] == '\0')
            break;

        for (eq = tok; *eq != '\0' && *eq != '='; eq++)
            ;
        if (*eq != '=')
            continue;
        *eq++ = '\0';

        if (streq(tok, "min"))
            min_ms = to_num(eq);
        else if (streq(tok, "max"))
            max_ms = to_num(eq);
        else if (streq(tok, "break"))
            no_break = (to_num(eq) == 0) ? TRUE : FALSE;
    }

    zero((UBYTE *)&a, (ULONG)sizeof(a));
    a.sin_len    = (UBYTE)sizeof(a);
    a.sin_family = AF_INET_;
    a.sin_port   = cs.peer_port;
    a.sin_addr   = PEER_IP;

    t0 = tap_eclock_now();
    cs.t_last = t0;
    if (no_break)
        s_set_socket_signals(0);
    rc = s_connect(cs.sock, &a);
    if (no_break)
        s_set_socket_signals(SIGBREAKF_CTRL_C);
    took = ticks_to_ms(t0, tap_eclock_now());

    if (want_errno != 0)
    {
        if (rc < 0 && s_errno() == want_errno)
        {
            if ((min_ms > 0 && took < (ULONG)min_ms) ||
                (max_ms > 0 && took > (ULONG)max_ms))
            {
                char        why[112];
                char       *w = why;
                const char *t = "connect() took ";

                while (*t) *w++ = *t++;
                fmt_num(&w, took, 10, 0, FALSE);
                t = "ms, wanted "; while (*t) *w++ = *t++;
                fmt_num(&w, (ULONG)min_ms, 10, 0, FALSE);
                t = ".."; while (*t) *w++ = *t++;
                fmt_num(&w, (ULONG)max_ms, 10, 0, FALSE);
                t = "ms"; while (*t) *w++ = *t++;
                *w = '\0';
                fail(raw, why);
                return;
            }

            n_pass++;
            SAY("  ok   %s   [%ums]", SAY_S(raw), SAY_U(took));
            return;
        }
        {
            char  why[96];
            char *w = why;
            const char *t = "connect() returned ";

            while (*t) *w++ = *t++;
            fmt_num(&w, (ULONG)rc, 10, 0, TRUE);
            t = ", errno "; while (*t) *w++ = *t++;
            fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
            t = ", after "; while (*t) *w++ = *t++;
            fmt_num(&w, took, 10, 0, FALSE);
            t = "ms"; while (*t) *w++ = *t++;
            *w = '\0';
            fail(raw, why);
        }
        return;
    }

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

static VOID do_listen(const char *args, const char *raw)
{
    SockAddrIn a;
    LONG       one = 1;
    LONG       want_errno = 0;

    {
        char tok[24];

        args = token(args, tok, sizeof(tok));       /* '=' or nothing */
        if (tok[0] == '=')
        {
            (VOID)token(args, tok, sizeof(tok));
            want_errno = streq(tok, "EADDRNOTAVAIL") ? E_ADDRNOTAVAIL
                                                     : to_num(tok);
        }
    }

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
    a.sin_addr   = cs.local_addr;

    if (want_errno != 0)
    {
        LONG rc = s_bind(cs.lsock, &a);

        if (rc != 0 && s_errno() == want_errno)
            pass(raw);
        else
        {
            char  why[80];
            char *w = why;
            const char *t = "bind() returned ";

            while (*t) *w++ = *t++;
            fmt_num(&w, (ULONG)rc, 10, 0, TRUE);
            t = ", errno "; while (*t) *w++ = *t++;
            fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
            *w = '\0';
            fail(raw, why);
        }
        return;
    }

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

static VOID do_accept(const char *args, const char *raw)
{
    UWORD tries;
    LONG  last = 0;
    char  why[96];
    char *w;
    const char *t;
    BOOL  want_none = FALSE;
    LONG  wait_ms = 0;
    LONG  n_wait  = 1;
    LONG  min_ms  = 0;
    LONG  max_ms  = 0;

    {
        char tok[16];

        args = token(args, tok, sizeof(tok));       /* '=' or nothing */
        if (tok[0] == '=')
        {
            args = token(args, tok, sizeof(tok));
            want_none = streq(tok, "none");
        }
    }

    for (;;)
    {
        char  tok[24];
        char *eq;

        args = token(args, tok, sizeof(tok));
        if (tok[0] == '\0')
            break;

        for (eq = tok; *eq != '\0' && *eq != '='; eq++)
            ;
        if (*eq != '=')
            continue;
        *eq++ = '\0';

        if (streq(tok, "wait"))
            wait_ms = to_num(eq);
        else if (streq(tok, "tries"))
            n_wait = to_num(eq);
        else if (streq(tok, "min"))
            min_ms = to_num(eq);
        else if (streq(tok, "max"))
            max_ms = to_num(eq);
    }

    if (wait_ms > 0)
    {
        LONG  zero_ = 0;
        LONG  tv[2];
        LONG  i;
        ULONG t0;
        ULONG took;

        tv[0] = wait_ms / 1000;
        tv[1] = (wait_ms % 1000) * 1000;

        if (s_ioctl(cs.lsock, FIONBIO_, &zero_) != 0 ||
            s_setsockopt(cs.lsock, SOL_SOCKET_, SO_RCVTIMEO_, tv,
                         (LONG)sizeof(tv)) != 0)
        {
            fail(raw, "could not make the listener block with a timeout");
            return;
        }

        t0 = tap_eclock_now();

        for (i = 0; i < n_wait; i++)
        {
            LONG s = s_accept(cs.lsock);

            if (s >= 0)
            {
                if (want_none)
                {
                    s_close(s);
                    fail(raw, "accept() handed over a connection it had to "
                              "refuse");
                    return;
                }
                cs.sock = s;
                sock_nonblocking(s);
                took = ticks_to_ms(t0, tap_eclock_now());
                cs.t_last = tap_eclock_now();
                n_pass++;
                SAY("  ok   %s   [%ums]", SAY_S(raw), SAY_U(took));
                return;
            }
            last = s_errno();
        }

        took = ticks_to_ms(t0, tap_eclock_now());

        if (!want_none)
        {
            w = why;
            t = "accept() never produced a socket, errno ";
            while (*t != '\0') *w++ = *t++;
            fmt_num(&w, (ULONG)last, 10, 0, TRUE);
            *w = '\0';
            fail(raw, why);
            return;
        }

        if (min_ms == 0)
            min_ms = (wait_ms * n_wait) / 2;

        if (took < (ULONG)min_ms ||
            (max_ms > 0 && took > (ULONG)max_ms))
        {
            w = why;
            t = "the accept waits took ";
            while (*t != '\0') *w++ = *t++;
            fmt_num(&w, took, 10, 0, FALSE);
            t = "ms, wanted ";
            while (*t != '\0') *w++ = *t++;
            fmt_num(&w, (ULONG)min_ms, 10, 0, FALSE);
            t = "..";
            while (*t != '\0') *w++ = *t++;
            fmt_num(&w, (ULONG)max_ms, 10, 0, FALSE);
            t = "ms";
            while (*t != '\0') *w++ = *t++;
            *w = '\0';
            fail(raw, why);
            return;
        }

        cs.t_last = tap_eclock_now();
        n_pass++;
        SAY("  ok   %s   [%ums over %d wait(s)]", SAY_S(raw), SAY_U(took),
            SAY_D(n_wait));
        return;
    }

    for (tries = 0; tries < 50; tries++)
    {
        LONG s = s_accept(cs.lsock);

        if (s >= 0)
        {
            if (want_none)
            {
                s_close(s);
                fail(raw, "accept() handed over a connection it had to refuse");
                return;
            }
            cs.sock = s;
            sock_nonblocking(s);
            pass(raw);
            return;
        }
        last = s_errno();
        pump();
        Delay(1);
    }

    if (want_none)
    {
        pass(raw);
        return;
    }

    w = why;
    t = "accept() never produced a socket, errno ";
    while (*t != '\0') *w++ = *t++;
    fmt_num(&w, (ULONG)last, 10, 0, TRUE);
    *w = '\0';
    fail(raw, why);
}

static VOID do_send(const char *args, const char *raw)
{
    char  tok[24];
    LONG  want;
    LONG  rc;
    ULONG i;
    BOOL  want_again = FALSE;
    BOOL  want_short = FALSE;

    args = token(args, tok, sizeof(tok));
    want = to_num(tok);
    if (want > (LONG)sizeof(payload))
        want = (LONG)sizeof(payload);

    args = token(args, tok, sizeof(tok));
    if (tok[0] == '=')
    {
        (VOID)token(args, tok, sizeof(tok));
        want_again = streq(tok, "AGAIN");
        want_short = streq(tok, "SHORT");
    }

    for (i = 0; i < (ULONG)want; i++)
        payload[i] = (UBYTE)('A' + (i % 26));

    cs.t_last = tap_eclock_now();
    rc = s_send(cs.sock, payload, want, 0);

    /* Reset before the verdict, so `wirebytes` counts only what this send
       put on the wire. */
    cs.send_rc    = rc;
    cs.wire_bytes = 0UL;

    if (want_again)
    {
        if (rc < 0 && s_errno() == E_WOULDBLOCK)
        {
            pass(raw);
            return;
        }
    }
    else if (want_short)
    {
        if (rc > 0 && rc < want)
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
    pass(raw);
}

static VOID do_blocking(const char *raw)
{
    LONG zero_ = 0;

    if (s_ioctl(cs.sock, FIONBIO_, &zero_) != 0)
    {
        fail(raw, "ioctl(FIONBIO, 0) failed");
        return;
    }
    pass(raw);
}

static LONG bstream_short = -1;         /* what the first short send credited */

static VOID do_bstream(const char *args, const char *raw)
{
    char   tok[32];
    ULONG  want;
    ULONG  total  = 0;
    ULONG  calls  = 0;
    ULONG  shorts = 0;
    ULONG  start, ms;
    ULONG  lo = 0, hi = 0;
    BOOL   need_short = TRUE;
    char   why[120];
    char  *w;

    bstream_short = -1;

    args = token(args, tok, sizeof(tok));
    want = (ULONG)to_num(tok);

    for (;;)
    {
        const char *next = token(args, tok, sizeof(tok));
        char       *eq   = tok;

        if (tok[0] == '\0' || tok[0] == '#')
            break;
        while (*eq != '\0' && *eq != '=')
            eq++;
        if (*eq == '=')
        {
            *eq = '\0';
            if (streq(tok, "timeout"))
            {
                LONG tv[2];

                tv[0] = to_num(eq + 1) / 1000;
                tv[1] = (to_num(eq + 1) % 1000) * 1000;
                if (s_setsockopt(cs.sock, SOL_SOCKET_, SO_SNDTIMEO_, tv,
                                 (LONG)sizeof(tv)) != 0)
                {
                    fail(raw, "setsockopt(SO_SNDTIMEO) failed");
                    return;
                }
            }
            else if (streq(tok, "short"))
                need_short = (to_num(eq + 1) != 0) ? TRUE : FALSE;
            else if (streq(tok, "min"))
                lo = (ULONG)to_num(eq + 1);
            else if (streq(tok, "max"))
                hi = (ULONG)to_num(eq + 1);
            else
            {
                SAY("!! unknown bstream key %s=", SAY_S(tok));
                n_fail++;
            }
        }
        args = next;
    }

    start     = tap_eclock_now();
    cs.t_last = start;

    while (total < want)
    {
        ULONG chunk = want - total;
        ULONG i;
        LONG  rc;

        if (chunk > (ULONG)sizeof(payload))
            chunk = (ULONG)sizeof(payload);

        /* The pattern is the STREAM's, not the buffer's: byte k of the
           transfer is the same character wherever it is offered from. */
        for (i = 0; i < chunk; i++)
            payload[i] = (UBYTE)('A' + ((total + i) % 26));

        rc = s_send(cs.sock, payload, (LONG)chunk, 0);
        calls++;

        if (rc > 0)
        {
            if ((ULONG)rc < chunk)
            {
                if (shorts == 0)
                    bstream_short = rc;
                shorts++;
            }
            total += (ULONG)rc;
            continue;
        }

        if (rc < 0 && s_errno() == E_WOULDBLOCK)
        {
            if (ticks_to_ms(start, tap_eclock_now()) > 20000UL)
                break;
            Delay(1);
            continue;
        }
        break;
    }

    ms = ticks_to_ms(start, tap_eclock_now());

    SAY("       bstream: %u byte(s) in %u call(s), %u short, %ums",
        SAY_U(total), SAY_U(calls), SAY_U(shorts), SAY_U(ms));

    if (total != want)
    {
        w = why;
        { const char *t = "the write loop placed "; while (*t) *w++ = *t++; }
        fmt_num(&w, total, 10, 0, FALSE);
        { const char *t = " of "; while (*t) *w++ = *t++; }
        fmt_num(&w, want, 10, 0, FALSE);
        { const char *t = ", errno "; while (*t) *w++ = *t++; }
        fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    if (need_short && shorts == 0)
    {
        fail(raw, "no send() went short, so nothing here reached "
                  "bsd_send_consumed()");
        return;
    }

    if (ms < lo || (hi != 0 && ms > hi))
    {
        w = why;
        { const char *t = "the write loop took "; while (*t) *w++ = *t++; }
        fmt_num(&w, ms, 10, 0, FALSE);
        { const char *t = " ms, wanted "; while (*t) *w++ = *t++; }
        fmt_num(&w, lo, 10, 0, FALSE);
        { const char *t = ".."; while (*t) *w++ = *t++; }
        fmt_num(&w, hi, 10, 0, FALSE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    pass(raw);
}

static VOID do_bshort(const char *args, const char *raw)
{
    char  tok[24];
    LONG  want;
    char  why[90];
    char *w;

    (VOID)token(args, tok, sizeof(tok));
    want = to_num(tok);

    if (bstream_short == want)
    {
        pass(raw);
        return;
    }

    w = why;
    { const char *t = "the first short send credited "; while (*t) *w++ = *t++; }
    fmt_num(&w, (ULONG)bstream_short, 10, 0, TRUE);
    { const char *t = ", wanted "; while (*t) *w++ = *t++; }
    fmt_num(&w, (ULONG)want, 10, 0, TRUE);
    *w = '\0';
    fail(raw, why);
}

static VOID do_brecv(const char *args, const char *raw)
{
    char  tok[24];
    LONG  max;
    LONG  want;
    LONG  flags = 0;
    LONG  rc;
    ULONG i;
    char  why[110];
    char *w;

    args = token(args, tok, sizeof(tok));
    max  = to_num(tok);
    if (max > (LONG)sizeof(payload))
        max = (LONG)sizeof(payload);

    for (;;)
    {
        const char *next = token(args, tok, sizeof(tok));

        if (tok[0] == '\0' || tok[0] == '#')
        {
            want = max;
            break;
        }
        if (streq(tok, "waitall"))
        {
            flags |= MSG_WAITALL_;
            args = next;
            continue;
        }
        if (tok[0] == '=')
        {
            (VOID)token(next, tok, sizeof(tok));
            want = to_num(tok);
            break;
        }
        args = next;
    }

    zero(payload, (ULONG)max);
    cs.t_last = tap_eclock_now();
    rc = s_recv(cs.sock, payload, max, flags);

    if (rc != want)
    {
        w = why;
        { const char *t = "recv() returned "; while (*t) *w++ = *t++; }
        fmt_num(&w, (ULONG)rc, 10, 0, TRUE);
        { const char *t = " of "; while (*t) *w++ = *t++; }
        fmt_num(&w, (ULONG)want, 10, 0, TRUE);
        { const char *t = ", errno "; while (*t) *w++ = *t++; }
        fmt_num(&w, (ULONG)s_errno(), 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    for (i = 0; i < (ULONG)rc; i++)
    {
        if (payload[i] != (UBYTE)('a' + (i % 26)))
        {
            w = why;
            { const char *t = "byte "; while (*t) *w++ = *t++; }
            fmt_num(&w, i, 10, 0, FALSE);
            { const char *t = " of the receive is not the byte the peer sent "
                              "at that offset"; while (*t) *w++ = *t++; }
            *w = '\0';
            fail(raw, why);
            return;
        }
    }

    pass(raw);
}

static VOID do_bclose(const char *args, const char *raw)
{
    char  tok[32];
    ULONG lo = 0, hi = 0;
    ULONG before, ms;
    char  why[110];
    char *w;

    for (;;)
    {
        const char *next = token(args, tok, sizeof(tok));
        char       *eq   = tok;

        if (tok[0] == '\0' || tok[0] == '#')
            break;
        while (*eq != '\0' && *eq != '=')
            eq++;
        if (*eq == '=')
        {
            *eq = '\0';
            if (streq(tok, "min"))      lo = (ULONG)to_num(eq + 1);
            else if (streq(tok, "max")) hi = (ULONG)to_num(eq + 1);
            else
            {
                SAY("!! unknown bclose key %s=", SAY_S(tok));
                n_fail++;
            }
        }
        args = next;
    }

    before    = tap_eclock_now();
    cs.t_last = before;

    if (cs.sock >= 0)
    {
        (VOID)s_close(cs.sock);
        cs.sock = -1;
    }

    ms = ticks_to_ms(before, tap_eclock_now());

    if (ms < lo || (hi != 0 && ms > hi))
    {
        w = why;
        { const char *t = "CloseSocket() took "; while (*t) *w++ = *t++; }
        fmt_num(&w, ms, 10, 0, FALSE);
        { const char *t = " ms, wanted "; while (*t) *w++ = *t++; }
        fmt_num(&w, lo, 10, 0, FALSE);
        { const char *t = ".."; while (*t) *w++ = *t++; }
        fmt_num(&w, hi, 10, 0, FALSE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    n_pass++;
    SAY("  ok   %s   [%ums]", SAY_S(raw), SAY_U(ms));
}

static VOID do_recv(const char *args, const char *raw)
{
    char  tok[24];
    LONG  max;
    LONG  want;
    BOOL  want_again = FALSE;
    LONG  want_errno = 0;
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
    else if (streq(tok, "ENOTCONN"))
    {
        want = -1;
        want_errno = E_NOTCONN;
    }
    else
        want = to_num(tok);

    rc = s_recv(cs.sock, payload, max, 0);

    if (want_errno != 0)
    {
        if (rc < 0 && s_errno() == want_errno)
        {
            pass(raw);
            return;
        }
    }
    else if (want_again)
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

static VOID do_recvpat(const char *args, const char *raw)
{
    char tok[24];
    LONG want;
    LONG rc;
    LONG i;

    (VOID)token(args, tok, sizeof(tok));
    want = to_num(tok);

    if (want > (LONG)sizeof(payload))
        want = (LONG)sizeof(payload);

    rc = s_recv(cs.sock, payload, want, 0);

    if (rc != want)
    {
        char  why[110];
        char *w = why;
        const char *t = "recv() returned ";

        while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)rc, 10, 0, TRUE);
        t = ", wanted "; while (*t) *w++ = *t++;
        fmt_num(&w, (ULONG)want, 10, 0, TRUE);
        *w = '\0';
        fail(raw, why);
        return;
    }

    for (i = 0; i < rc; i++)
    {
        if (payload[i] != (UBYTE)('a' + (i % 26)))
        {
            char  why[110];
            char *w = why;
            const char *t = "byte ";

            while (*t) *w++ = *t++;
            fmt_num(&w, (ULONG)i, 10, 0, FALSE);
            t = " is 0x"; while (*t) *w++ = *t++;
            fmt_num(&w, (ULONG)payload[i], 16, 2, FALSE);
            t = ", expected 0x"; while (*t) *w++ = *t++;
            fmt_num(&w, (ULONG)('a' + (i % 26)), 16, 2, FALSE);
            *w = '\0';
            fail(raw, why);
            return;
        }
    }

    pass(raw);
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
    /* Milliseconds in the script, a struct timeval on the wire to the
       library, because that is what SO_{SND,RCV}TIMEO takes. */
    else if (streq(tok, "sndtimeo") || streq(tok, "rcvtimeo"))
    {
        LONG tv[2];
        tv[0] = v / 1000;
        tv[1] = (v % 1000) * 1000;
        rc = s_setsockopt(cs.sock, SOL_SOCKET_,
                          streq(tok, "sndtimeo") ? SO_SNDTIMEO_
                                                 : SO_RCVTIMEO_,
                          tv, (LONG)sizeof(tv));
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
    SAY("  ok   %s   [%ums]", SAY_S(raw), SAY_U(ms));
}

static VOID do_txsame(const char *args, const char *raw)
{
    char  tok[48];
    UWORD want;
    ULONG n = 0;
    Seg   got;
    char  desc[280];

    (VOID)token(args, tok, sizeof(tok));
    want = parse_flags(tok);

    pump();

    while (pend_pop(&got))
    {
        n++;

        if (!got.is_tcp || got.flags != want ||
            (cs.u_isn_known && got.seq != cs.u_isn))
        {
            describe(&got, desc, sizeof(desc));
            fail(raw, desc);
            while (pend_pop(&got))
                ;
            return;
        }
    }

    if (n == 0)
    {
        fail(raw, "no frame was sent at all, so nothing was compared");
        return;
    }

    cs.t_last = tap_eclock_now();
    n_pass++;
    SAY("  ok   %s   [%u identical frame(s)]", SAY_S(raw), SAY_U(n));
}

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
    SAY("  ok   %s   [%u frame(s)]", SAY_S(raw), SAY_U(n));
}

static VOID do_rxorder(const char *args, const char *raw)
{
    char tok[16];

    (VOID)token(args, tok, sizeof(tok));

    if (streq(tok, "lifo"))
        tap_set_rx_lifo(TRUE);
    else if (streq(tok, "fifo"))
        tap_set_rx_lifo(FALSE);
    else
    {
        fail(raw, "rxorder takes fifo or lifo");
        return;
    }

    pass(raw);
}

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

    /* A case that forgot its `join` leaves a task holding the tap ring, and
       the next case's first `tx` then reports a stack that is behaving. */
    if (wire_live)
    {
        (VOID)wire_stop();
        SAY0("  !! the wire task was still running at the end of the case");
        n_fail++;
        cs.fails++;
    }

    case_abort(cs.sock);
    cs.sock = -1;
    case_abort(cs.lsock);
    cs.lsock = -1;

    Delay(2);
    pump();
    while (pend_count != 0)
    {
        Seg junk;
        (VOID)pend_pop(&junk);
    }

    if (cs.fails == 0)
        SAY("PASS %s", SAY_S(cs.name));
    else
    {
        n_cases_failed++;
        SAY("FAIL %s (%u check(s))", SAY_S(cs.name), SAY_U(cs.fails));
    }
    SAY0("");
}

static VOID case_begin(const char *name)
{
    ULONG i;

    case_end();

    zero((UBYTE *)&cs, (ULONG)sizeof(cs));
    for (i = 0; i + 1 < sizeof(cs.name) && name[i] != '\0'; i++)
        cs.name[i] = name[i];

    tap_set_rx_lifo(FALSE);

    cs.sock       = -1;
    cs.lsock      = -1;
    cs.send_rc    = -1;
    cs.peer_port  = DEFAULT_PEER_PORT;
    cs.local_port = 0;
    /* A different ISN per case, so a stale segment from the previous one
       cannot be mistaken for a live one. */
    cs.p_isn      = 0x50000000UL + (n_cases * 0x00010000UL);
    cs.t_last     = tap_eclock_now();
    cs.our_wscale  = -1;
    cs.peer_wscale = -1;

    n_cases++;
    SAY("---- %s", SAY_S(cs.name));
}

#define REPEAT_MAX_LINES    12

static char  rep_body[REPEAT_MAX_LINES][160];
static UWORD rep_n;
static BOOL  rep_collecting;
static LONG  rep_count;

static VOID run_line(char *line);

static VOID repeat_run(VOID)
{
    LONG  n = rep_count;
    UWORD i;

    rep_collecting = FALSE;

    for (cur_iter = 0; cur_iter < n; cur_iter++)
    {
        for (i = 0; i < rep_n; i++)
        {
            char scratch_line[160];
            ULONG k;

            for (k = 0; k + 1 < sizeof(scratch_line) &&
                        rep_body[i][k] != '\0'; k++)
                scratch_line[k] = rep_body[i][k];
            scratch_line[k] = '\0';

            run_line(scratch_line);
        }
    }
    cur_iter = 0;
    rep_n    = 0;
}

static VOID run_line(char *line)
{
    char        verb[24];
    const char *args;
    char        raw[200];
    ULONG       i;

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
        SAY("!! directive before any `case`: %s", SAY_S(raw));
        n_fail++;
        return;
    }

    if (streq(verb, "peerport"))
    {
        char tok[16];
        (VOID)token(args, tok, sizeof(tok));
        cs.peer_port = (UWORD)to_num(tok);

        cs.u_isn_known = FALSE;
    }
    else if (streq(verb, "localport"))
    {
        char tok[16];
        (VOID)token(args, tok, sizeof(tok));
        cs.local_port = (UWORD)to_num(tok);
    }
    else if (streq(verb, "localaddr"))
    {
        char tok[16];

        (VOID)token(args, tok, sizeof(tok));
        if (streq(tok, "any"))           cs.local_addr = 0UL;
        else if (streq(tok, "iface"))    cs.local_addr = LOCAL_IP;
        else if (streq(tok, "loopback")) cs.local_addr = LOOPBACK_IP;
        else if (streq(tok, "foreign"))  cs.local_addr = FOREIGN_IP;
        else
        {
            SAY("!! unknown localaddr: %s", SAY_S(raw));
            n_fail++;
        }
    }
    else if (streq(verb, "socket"))   do_socket(raw);
    else if (streq(verb, "connect"))  do_connect(args, raw);
    else if (streq(verb, "listen"))   do_listen(args, raw);
    else if (streq(verb, "accept"))   do_accept(args, raw);
    else if (streq(verb, "send"))     do_send(args, raw);
    else if (streq(verb, "oob"))      do_oob(args, raw);
    else if (streq(verb, "recv"))     do_recv(args, raw);
    else if (streq(verb, "blocking")) do_blocking(raw);
    else if (streq(verb, "bstream"))  do_bstream(args, raw);
    else if (streq(verb, "bshort"))   do_bshort(args, raw);
    else if (streq(verb, "brecv"))    do_brecv(args, raw);
    else if (streq(verb, "bclose"))   do_bclose(args, raw);
    else if (streq(verb, "wire"))     do_wire(args, raw);
    else if (streq(verb, "join"))     do_join(args, raw);
    else if (streq(verb, "wirestream")) do_wirestream(args, raw);
    else if (streq(verb, "wiregrants")) do_wiregrants(args, raw);
    else if (streq(verb, "recvpat"))  do_recvpat(args, raw);
    else if (streq(verb, "readable")) do_select(args, raw, FALSE);
    else if (streq(verb, "writable")) do_select(args, raw, TRUE);
    else if (streq(verb, "shutdown")) do_shutdown(args, raw);
    else if (streq(verb, "opt"))      do_opt(args, raw);
    else if (streq(verb, "close"))    do_close(args, raw);
    else if (streq(verb, "idle"))     do_idle(args, raw);
    else if (streq(verb, "rxorder"))  do_rxorder(args, raw);
    else if (streq(verb, "tx"))       do_tx(args, raw);
    else if (streq(verb, "notx"))     do_notx(args, raw);
    else if (streq(verb, "txcount")) do_txcount(args, raw);
    else if (streq(verb, "txsame"))  do_txsame(args, raw);
    else if (streq(verb, "wirebytes")) do_wirebytes(args, raw);
    else if (streq(verb, "rx"))       do_rx(args, raw);
    else if (streq(verb, "repeat"))
    {
        char tok[32];

        (VOID)token(args, tok, sizeof(tok));
        if (rep_collecting)
        {
            SAY("!! nested `repeat`: %s", SAY_S(raw));
            n_fail++;
            return;
        }
        rep_count      = to_num(tok);
        rep_n          = 0;
        rep_collecting = TRUE;
        SAY("  -- repeat %d", SAY_D(rep_count));
    }
    else if (streq(verb, "end"))
    {
        SAY("!! `end` without `repeat`: %s", SAY_S(raw));
        n_fail++;
    }
    else if (streq(verb, "icmp"))     do_icmp(args, raw);
    else
    {
        SAY("!! unknown directive: %s", SAY_S(raw));
        n_fail++;
    }
}

static LONG run_script(const char *path)
{
    BPTR  fh;
    char  line[256];

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (fh == (BPTR)0)
    {
        SAY("!! cannot open %s", SAY_S(path));
        return 20;
    }

    while (FGets(fh, (STRPTR)line, (LONG)sizeof(line)) != NULL)
    {
        ULONG n = d_len(line);

        while (n != 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        if (rep_collecting)
        {
            char verb[24];

            (VOID)token(line, verb, sizeof(verb));

            if (streq(verb, "end"))
            {
                repeat_run();
                continue;
            }
            if (verb[0] == '\0' || verb[0] == '#')
                continue;
            if (rep_n >= REPEAT_MAX_LINES)
            {
                SAY("!! `repeat` body over %d lines: %s",
                    SAY_D(REPEAT_MAX_LINES), SAY_S(line));
                n_fail++;
                continue;
            }
            for (n = 0; n + 1 < sizeof(rep_body[0]) && line[n] != '\0'; n++)
                rep_body[rep_n][n] = line[n];

            if (line[n] != '\0')
            {
                SAY("!! `repeat` line is longer than %u bytes: %s",
                    SAY_U(sizeof(rep_body[0]) - 1UL), SAY_S(line));
                n_fail++;
                continue;
            }

            rep_body[rep_n][n] = '\0';
            rep_n++;
            continue;
        }

        run_line(line);
    }

    Close(fh);

    if (rep_collecting)
    {
        SAY("!! `repeat` with no `end`, %u line(s) never ran", SAY_U(rep_n));
        n_fail++;
        rep_collecting = FALSE;
    }

    case_end();
    return 0;
}

int main(void)
{
    TapStats st;
    UWORD    tries;
    LONG     rc = 0;

    out_file = Open((STRPTR)"DH0:tcpdrill.txt", MODE_NEWFILE);

    SAY0("tcpdrill -- packet-level TCP conformance");

    if (tap_install(local_mac) != 0)
    {
        SAY("!! cannot install %s", SAY_S(TAP_DEVICE_NAME));
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    eclock_per_ms = tap_eclock_rate() / 1000;
    SAY("E-Clock %u Hz (%u ticks/ms)", SAY_U(tap_eclock_rate()),
        SAY_U(eclock_per_ms));

    SockBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (SockBase == NULL)
    {
        SAY0("!! bsdsocket.library would not open");
        tap_remove();
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    for (tries = 0; tries < 250 && !tap_is_online(); tries++)
        Delay(1);

    if (!tap_is_online())
    {
        SAY0("!! the interface never came online -- DEVS:NetInterfaces/tap0?");
        CloseLibrary(SockBase);
        tap_remove();
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    for (tries = 0; tries < 50 && tap_reads_for(ETYPE_IP) == 0; tries++)
        Delay(1);

    SAY("device online, %u IPv4 read(s) and %u ARP read(s) outstanding",
        SAY_U(tap_reads_for(ETYPE_IP)), SAY_U(tap_reads_for(ETYPE_ARP)));
    SAY0("");

    rc = run_script("DH0:drill.txt");

    tap_get_stats(&st);
    SAY0("");
    SAY("tap: tx %u  rx delivered %u  rx no-reader %u  copy-failed %u  "
        "tx-overrun %u", SAY_U(st.tx_frames), SAY_U(st.rx_delivered),
        SAY_U(st.rx_no_reader), SAY_U(st.rx_copy_failed), SAY_U(st.tx_overrun));
    SAY("background frames ignored: %u", SAY_U(n_background));
    SAY("%u case(s), %u failed; %u check(s) passed, %u failed",
        SAY_U(n_cases), SAY_U(n_cases_failed), SAY_U(n_pass), SAY_U(n_fail));

    CloseLibrary(SockBase);
    SockBase = NULL;

    tap_remove();

    if (out_file != (BPTR)0)
        Close(out_file);

    if (rc != 0)
        return (int)rc;
    return (n_fail != 0) ? 5 : 0;
}
