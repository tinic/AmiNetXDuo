/*
 * tickprobe, a TCP/IP stack's periodic-timer rate and packet turnaround,
 * measured from outside, with no knowledge of its internals and nothing of its
 * code inspected.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "tapdev.h"

#define LOCAL_IP        0x0A090901UL            /* 10.9.9.1, the stack       */
#define PEER_IP         0x0A090902UL            /* 10.9.9.2, this harness    */
#define PEER_PORT       9000

static const UBYTE local_mac[6] = { 0x02, 0x00, 0x54, 0x49, 0x43, 0x01 };
static const UBYTE peer_mac[6]  = { 0x02, 0x00, 0x54, 0x49, 0x43, 0x02 };

#define ETYPE_IP        0x0800
#define ETYPE_ARP       0x0806
#define ETH_HDR         14

#define AF_INET_        2
#define SOCK_STREAM_    1
#define FIONBIO_        0x8004667EUL

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

static BPTR out_file;

static ULONG d_len(const char *s)
{
    ULONG n = 0;
    while (s[n] != '\0') n++;
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

static VOID fmt_num(char **p, ULONG v, UWORD base)
{
    char  tmp[12];
    UWORD n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v != 0)
    {
        UWORD d = (UWORD)(v % base);
        tmp[n++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
        v /= base;
    }
    while (n != 0)
        *(*p)++ = tmp[--n];
}

/* %u decimal, %x hex, %s string.  No floats: the analyser does arithmetic. */
static VOID say(const char *fmt, ...)
{
    char     line[256];
    char    *o = line;
    va_list  ap;

    va_start(ap, fmt);

    while (*fmt != '\0' && (ULONG)(o - line) < sizeof(line) - 16)
    {
        if (*fmt != '%')
        {
            *o++ = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == 'u')      { fmt++; fmt_num(&o, va_arg(ap, ULONG), 10); }
        else if (*fmt == 'x') { fmt++; fmt_num(&o, va_arg(ap, ULONG), 16); }
        else if (*fmt == 's')
        {
            const char *s = va_arg(ap, const char *);
            fmt++;
            while (*s != '\0' && (ULONG)(o - line) < sizeof(line) - 4)
                *o++ = *s++;
        }
        else *o++ = *fmt++;
    }

    va_end(ap);

    *o++ = '\n';
    *o   = '\0';
    emit(line);
}

static ULONG rd32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
                                           ((ULONG)p[2] << 8) | (ULONG)p[3]; }
static UWORD rd16(const UBYTE *p) { return (UWORD)(((UWORD)p[0] << 8) | p[1]); }
static VOID  wr32(UBYTE *p, ULONG v) { p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16);
                                       p[2] = (UBYTE)(v >> 8);  p[3] = (UBYTE)v; }
static VOID  wr16(UBYTE *p, UWORD v) { p[0] = (UBYTE)(v >> 8); p[1] = (UBYTE)v; }

static VOID cp(UBYTE *to, const UBYTE *from, ULONG n)
{
    while (n-- != 0) *to++ = *from++;
}

static VOID zero(UBYTE *p, ULONG n)
{
    while (n-- != 0) *p++ = 0;
}

static ULONG ones_partial(const UBYTE *p, ULONG n, ULONG sum)
{
    while (n > 1) { sum += rd16(p); p += 2; n -= 2; }
    if (n != 0) sum += ((ULONG)*p) << 8;
    return sum;
}

static UWORD ones_fold(ULONG sum)
{
    while ((sum >> 16) != 0)
        sum = (sum & 0xFFFFUL) + (sum >> 16);
    return (UWORD)(~sum);
}

static struct MsgPort     *tmr_port;
static struct timerequest *tmr_req;
static BOOL                tmr_ok;

static BOOL fine_timer_open(VOID)
{
    tmr_port = CreateMsgPort();
    if (tmr_port == NULL)
        return FALSE;
    tmr_req = (struct timerequest *)CreateIORequest(tmr_port,
                                                   (LONG)sizeof(struct timerequest));
    if (tmr_req == NULL)
        return FALSE;
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest *)tmr_req, 0) != 0)
        return FALSE;
    tmr_ok = TRUE;
    return TRUE;
}

static VOID fine_timer_close(VOID)
{
    if (tmr_ok) { CloseDevice((struct IORequest *)tmr_req); tmr_ok = FALSE; }
    if (tmr_req != NULL) { DeleteIORequest((struct IORequest *)tmr_req); tmr_req = NULL; }
    if (tmr_port != NULL) { DeleteMsgPort(tmr_port); tmr_port = NULL; }
}

static VOID usleep_(ULONG usec)
{
    if (!tmr_ok || usec == 0)
        return;
    tmr_req->tr_node.io_Command = TR_ADDREQUEST;
    tmr_req->tr_time.tv_secs    = usec / 1000000UL;
    tmr_req->tr_time.tv_micro   = usec % 1000000UL;
    DoIO((struct IORequest *)tmr_req);
}

static VOID measure_timebases(ULONG rounds)
{
    struct MsgPort     *port;
    struct timerequest *req;
    ULONG               i;
    ULONG               t0;

    port = CreateMsgPort();
    if (port == NULL)
        return;
    req = (struct timerequest *)CreateIORequest(port,
                                               (LONG)sizeof(struct timerequest));
    if (req == NULL)
    {
        DeleteMsgPort(port);
        return;
    }

    /* UNIT_VBLANK rounds every request UP to the next vertical blank, so a
       request shorter than one frame period returns exactly one frame later
       and `rounds` of them measure `rounds` frame periods. */
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)req, 0) == 0)
    {
        t0 = tap_eclock_now();
        for (i = 0; i < rounds; i++)
        {
            req->tr_node.io_Command = TR_ADDREQUEST;
            req->tr_time.tv_secs    = 0;
            req->tr_time.tv_micro   = 1000;
            DoIO((struct IORequest *)req);
        }
        say("# timebase vblank rounds=%u t0=%u t1=%u", rounds, t0,
            tap_eclock_now());
        CloseDevice((struct IORequest *)req);
    }

    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest *)req, 0) == 0)
    {
        t0 = tap_eclock_now();
        for (i = 0; i < rounds; i++)
        {
            req->tr_node.io_Command = TR_ADDREQUEST;
            req->tr_time.tv_secs    = 0;
            req->tr_time.tv_micro   = 5000;
            DoIO((struct IORequest *)req);
        }
        say("# timebase microhz rounds=%u each=5000 t0=%u t1=%u", rounds, t0,
            tap_eclock_now());
        CloseDevice((struct IORequest *)req);
    }

    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
}

static ULONG rng_state = 2463534242UL;

static ULONG rnd(VOID)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

typedef struct Ev
{
    ULONG   stamp;
    UWORD   kind;               /* EV_*                                    */
    UWORD   icmp_type;
    UWORD   id;
    UWORD   seqno;              /* ICMP sequence, or TCP ports for EV_TCP  */
    ULONG   tseq;
    ULONG   tack;
    UWORD   flags;
    UWORD   win;
    ULONG   dlen;
    UWORD   sport;
    UWORD   dport;
} Ev;

#define EV_TCP      1
#define EV_ICMP     2

#define EVQ 64
static Ev    evq[EVQ];
static UWORD ev_head, ev_tail, ev_count;

static ULONG n_arp_answered;
static ULONG n_background;
static ULONG n_evq_lost;

static UBYTE scratch[TAP_FRAME_MAX];

static VOID arp_reply(const UBYTE *req)
{
    UBYTE f[42];
    const UBYTE *a = &req[ETH_HDR];

    zero(f, (ULONG)sizeof(f));
    cp(&f[0], &req[6], 6);
    cp(&f[6], peer_mac, 6);
    wr16(&f[12], ETYPE_ARP);
    wr16(&f[14], 1);
    wr16(&f[16], ETYPE_IP);
    f[18] = 6;
    f[19] = 4;
    wr16(&f[20], 2);
    cp(&f[22], peer_mac, 6);
    wr32(&f[28], PEER_IP);
    cp(&f[32], &a[8], 6);
    cp(&f[38], &a[14], 4);

    (VOID)tap_rx_put(f, (ULONG)sizeof(f));
    n_arp_answered++;
}

static VOID ev_push(const Ev *e)
{
    if (ev_count >= EVQ)
    {
        ev_tail = (UWORD)((ev_tail + 1) % EVQ);
        ev_count--;
        n_evq_lost++;
    }
    evq[ev_head] = *e;
    ev_head = (UWORD)((ev_head + 1) % EVQ);
    ev_count++;
}

static BOOL ev_pop(Ev *out)
{
    if (ev_count == 0)
        return FALSE;
    *out = evq[ev_tail];
    ev_tail = (UWORD)((ev_tail + 1) % EVQ);
    ev_count--;
    return TRUE;
}

static VOID pump(VOID)
{
    ULONG stamp;
    ULONG len;

    while ((len = tap_tx_get(scratch, (ULONG)sizeof(scratch), &stamp)) != 0)
    {
        UWORD        ether = rd16(&scratch[12]);
        const UBYTE *ip;
        UWORD        ihl;
        Ev           e;

        if (ether == ETYPE_ARP)
        {
            if (len >= 42 && rd16(&scratch[20]) == 1 &&
                rd32(&scratch[38]) == PEER_IP)
                arp_reply(scratch);
            continue;
        }

        if (ether != ETYPE_IP || len < ETH_HDR + 20)
        {
            n_background++;
            continue;
        }

        ip  = &scratch[ETH_HDR];
        ihl = (UWORD)((ip[0] & 0x0F) * 4);
        if (ihl < 20 || len < (ULONG)(ETH_HDR + ihl) || rd32(&ip[16]) != PEER_IP)
        {
            n_background++;
            continue;
        }

        zero((UBYTE *)&e, (ULONG)sizeof(e));
        e.stamp = stamp;

        if (ip[9] == 1 && len >= (ULONG)(ETH_HDR + ihl + 8))
        {
            const UBYTE *ic = ip + ihl;

            e.kind      = EV_ICMP;
            e.icmp_type = ic[0];
            e.id        = rd16(&ic[4]);
            e.seqno     = rd16(&ic[6]);
            e.dlen      = rd16(&ip[2]) - ihl - 8UL;
            ev_push(&e);
            continue;
        }

        if (ip[9] == 6 && len >= (ULONG)(ETH_HDR + ihl + 20))
        {
            const UBYTE *tcp = ip + ihl;
            UWORD        thl = (UWORD)(((tcp[12] >> 4) & 0x0F) * 4);

            if (thl < 20)
            {
                n_background++;
                continue;
            }
            e.kind  = EV_TCP;
            e.sport = rd16(&tcp[0]);
            e.dport = rd16(&tcp[2]);
            e.tseq  = rd32(&tcp[4]);
            e.tack  = rd32(&tcp[8]);
            e.flags = tcp[13];
            e.win   = rd16(&tcp[14]);
            e.dlen  = rd16(&ip[2]) - ihl - (ULONG)thl;
            ev_push(&e);
            continue;
        }

        n_background++;
    }
}

static BOOL wait_ev(Ev *out, ULONG ms)
{
    ULONG spent = 0;

    for (;;)
    {
        pump();
        if (ev_pop(out))
            return TRUE;
        if (spent >= ms)
            return FALSE;
        usleep_(1000);
        spent += 1;
    }
}

static VOID drain(VOID)
{
    Ev e;

    pump();
    while (ev_pop(&e))
        ;
}

static UBYTE injf[TAP_FRAME_MAX];

/* An ICMP echo request from the peer.  `dlen` is the ICMP payload, so 56
   matches what ping sends by default. */
static ULONG inject_echo(UWORD id, UWORD seqno, ULONG dlen)
{
    UBYTE *ip = &injf[ETH_HDR];
    UBYTE *ic = ip + 20;
    ULONG  iplen = 20UL + 8UL + dlen;
    ULONG  i;
    ULONG  t;

    zero(injf, ETH_HDR + iplen);

    cp(&injf[0], local_mac, 6);
    cp(&injf[6], peer_mac, 6);
    wr16(&injf[12], ETYPE_IP);

    ip[0] = 0x45;
    wr16(&ip[2], (UWORD)iplen);
    wr16(&ip[4], 0x5400);
    ip[8] = 64;
    ip[9] = 1;
    wr32(&ip[12], PEER_IP);
    wr32(&ip[16], LOCAL_IP);
    wr16(&ip[10], ones_fold(ones_partial(ip, 20, 0)));

    ic[0] = 8;                          /* echo request */
    ic[1] = 0;
    wr16(&ic[4], id);
    wr16(&ic[6], seqno);
    for (i = 0; i < dlen; i++)
        ic[8 + i] = (UBYTE)('a' + (i % 26));
    wr16(&ic[2], ones_fold(ones_partial(ic, 8 + dlen, 0)));

    t = tap_eclock_now();
    if (tap_rx_put(injf, ETH_HDR + iplen) != 0)
        return 0;
    return t;
}

typedef struct Tcp4
{
    UWORD   sport;
    UWORD   dport;
    ULONG   seq;
    ULONG   ack;
    UWORD   flags;
    UWORD   win;
    LONG    mss;
    ULONG   dlen;
} Tcp4;

static ULONG inject_tcp(const Tcp4 *in)
{
    UBYTE *ip  = &injf[ETH_HDR];
    UBYTE *tcp = ip + 20;
    UWORD  opt = (in->mss >= 0) ? 4 : 0;
    UWORD  thl = (UWORD)(20 + opt);
    ULONG  iplen = 20UL + thl + in->dlen;
    ULONG  i;
    ULONG  t;

    zero(injf, ETH_HDR + iplen);

    cp(&injf[0], local_mac, 6);
    cp(&injf[6], peer_mac, 6);
    wr16(&injf[12], ETYPE_IP);

    ip[0] = 0x45;
    wr16(&ip[2], (UWORD)iplen);
    wr16(&ip[4], 0x4000);
    wr16(&ip[6], 0x4000);
    ip[8] = 64;
    ip[9] = 6;
    wr32(&ip[12], PEER_IP);
    wr32(&ip[16], LOCAL_IP);
    wr16(&ip[10], ones_fold(ones_partial(ip, 20, 0)));

    wr16(&tcp[0], in->sport);
    wr16(&tcp[2], in->dport);
    wr32(&tcp[4], in->seq);
    wr32(&tcp[8], in->ack);
    tcp[12] = (UBYTE)((thl / 4) << 4);
    tcp[13] = (UBYTE)in->flags;
    wr16(&tcp[14], in->win);

    if (opt != 0)
    {
        tcp[20] = 2;
        tcp[21] = 4;
        wr16(&tcp[22], (UWORD)in->mss);
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
        wr16(&tcp[16], ones_fold(sum));
    }

    t = tap_eclock_now();
    if (tap_rx_put(injf, ETH_HDR + iplen) != 0)
        return 0;
    return t;
}

#define ICMP_ID 0x7469

static VOID phase_icmp(ULONG count, ULONG dlen)
{
    ULONG i;
    ULONG got = 0;

    say("# phase icmp count=%u payload=%u", count, dlen);

    for (i = 0; i < count; i++)
    {
        ULONG t0;
        ULONG arp_before = n_arp_answered;
        Ev    e;

        usleep_(rnd() % 30000UL);
        drain();

        t0 = inject_echo(ICMP_ID, (UWORD)i, dlen);
        if (t0 == 0)
        {
            say("icmp seq=%u dropped=1", i);
            continue;
        }

        if (!wait_ev(&e, 400))
        {
            say("icmp seq=%u timeout=1", i);
            continue;
        }

        if (e.kind != EV_ICMP || e.icmp_type != 0)
        {
            say("icmp seq=%u unexpected kind=%u type=%u", i, (ULONG)e.kind,
                (ULONG)e.icmp_type);
            continue;
        }

        got++;
        /* arp=1 marks a sample that had to resolve the peer first, which is a
           different measurement and is excluded by the analyser. */
        say("icmp seq=%u len=%u t0=%u t1=%u arp=%u", i, dlen, t0, e.stamp,
            n_arp_answered - arp_before);
    }

    say("# phase icmp done replies=%u", got);
}

static LONG  sock = -1;
static UWORD local_port;
static ULONG u_isn;                     /* the stack's initial sequence     */
static ULONG p_isn = 0x50000000UL;      /* ours                             */

static BOOL handshake(VOID)
{
    SockAddrIn sa;
    Ev         e;
    Tcp4       t;
    LONG       nb = 1;
    UWORD      tries;

    sock = s_socket(AF_INET_, SOCK_STREAM_, 0);
    if (sock < 0)
    {
        say("!! socket() failed");
        return FALSE;
    }
    (VOID)s_ioctl(sock, FIONBIO_, &nb);

    zero((UBYTE *)&sa, (ULONG)sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = AF_INET_;
    sa.sin_port   = PEER_PORT;
    sa.sin_addr   = PEER_IP;

    drain();
    (VOID)s_connect(sock, &sa);

    /* The SYN may be preceded by an ARP exchange, so look past anything that
       is not it. */
    for (tries = 0; tries < 8; tries++)
    {
        if (!wait_ev(&e, 3000))
        {
            say("!! no SYN from the stack");
            return FALSE;
        }
        if (e.kind == EV_TCP && (e.flags & 0x02) != 0 && (e.flags & 0x10) == 0)
            break;
    }
    if (e.kind != EV_TCP || (e.flags & 0x02) == 0)
    {
        say("!! first segment was not a SYN");
        return FALSE;
    }

    local_port = e.sport;
    u_isn      = e.tseq;

    zero((UBYTE *)&t, (ULONG)sizeof(t));
    t.sport = PEER_PORT;
    t.dport = local_port;
    t.seq   = p_isn;
    t.ack   = u_isn + 1;
    t.flags = 0x12;                      /* SYN|ACK */
    t.win   = 8192;
    t.mss   = 1460;
    (VOID)inject_tcp(&t);

    if (!wait_ev(&e, 3000) || e.kind != EV_TCP || (e.flags & 0x10) == 0)
    {
        say("!! no ACK completing the handshake");
        return FALSE;
    }

    say("# connected localport=%u u_isn=%u p_isn=%u win=%u",
        (ULONG)local_port, u_isn, p_isn, (ULONG)e.win);
    return TRUE;
}

static VOID phase_dack(ULONG count)
{
    ULONG i;
    ULONG got = 0;
    ULONG next = p_isn + 1;
    UBYTE rbuf[64];

    say("# phase dack count=%u", count);

    for (i = 0; i < count; i++)
    {
        Tcp4  t;
        Ev    e;
        ULONG t0;
        ULONG waited = 0;
        BOOL  ok = FALSE;

        usleep_(rnd() % 400000UL);
        drain();

        zero((UBYTE *)&t, (ULONG)sizeof(t));
        t.sport = PEER_PORT;
        t.dport = local_port;
        t.seq   = next;
        t.ack   = u_isn + 1;
        t.flags = 0x18;                  /* PSH|ACK */
        t.win   = 8192;
        t.mss   = -1;
        t.dlen  = 1;

        t0 = inject_tcp(&t);
        if (t0 == 0)
        {
            say("dack i=%u dropped=1", i);
            continue;
        }

        while (waited < 3)
        {
            if (!wait_ev(&e, 1200))
                break;
            waited++;
            if (e.kind == EV_TCP && (e.flags & 0x10) != 0 &&
                e.tack == next + 1)
            {
                ok = TRUE;
                break;
            }
            say("dack i=%u other flags=%x seq=%u ack=%u len=%u", i,
                (ULONG)e.flags, e.tseq, e.tack, e.dlen);
        }

        if (!ok)
        {
            say("dack i=%u timeout=1", i);
            continue;
        }

        got++;
        next += 1;
        say("dack i=%u t0=%u t1=%u win=%u", i, t0, e.stamp, (ULONG)e.win);

        /* Take the byte out of the receive queue, or the window walks down to
           zero and the measurement turns into a zero-window probe test. */
        (VOID)s_recv(sock, rbuf, (LONG)sizeof(rbuf), 0);
    }

    say("# phase dack done acks=%u", got);
}

static VOID phase_retransmit(ULONG ms)
{
    SockAddrIn sa;
    LONG       s2;
    LONG       nb = 1;
    ULONG      n = 0;
    ULONG      t_end;
    ULONG      hz = tap_eclock_rate();

    say("# phase retransmit window=%u", ms);

    s2 = s_socket(AF_INET_, SOCK_STREAM_, 0);
    if (s2 < 0)
    {
        say("# phase retransmit skipped: socket() failed");
        return;
    }
    (VOID)s_ioctl(s2, FIONBIO_, &nb);

    zero((UBYTE *)&sa, (ULONG)sizeof(sa));
    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = AF_INET_;
    sa.sin_port   = PEER_PORT + 1;
    sa.sin_addr   = PEER_IP;

    drain();
    t_end = tap_eclock_now() + (hz / 1000UL) * ms;
    (VOID)s_connect(s2, &sa);

    while ((LONG)(t_end - tap_eclock_now()) > 0)
    {
        Ev e;

        if (wait_ev(&e, 250) && e.kind == EV_TCP)
        {
            say("rtx n=%u t=%u flags=%x seq=%u", n, e.stamp,
                (ULONG)e.flags, e.tseq);
            n++;
        }
    }

    (VOID)s_close(s2);
    say("# phase retransmit done frames=%u", n);
}

static VOID run_interface_command(VOID)
{
    BPTR  fh;
    char  line[200];
    ULONG n;
    LONG  rc;

    fh = Open((STRPTR)"DH0:tickif.txt", MODE_OLDFILE);
    if (fh == (BPTR)0)
        return;

    if (FGets(fh, (STRPTR)line, (LONG)sizeof(line)) == NULL)
    {
        Close(fh);
        return;
    }
    Close(fh);

    n = d_len(line);
    while (n != 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0)
        return;

    say("# interface command: %s", line);

    {
        BPTR log = Open((STRPTR)"DH0:tickif.out", MODE_NEWFILE);

        rc = SystemTags((STRPTR)line,
                        SYS_Input,  (ULONG)Input(),
                        SYS_Output, (ULONG)log,
                        TAG_END);
        /* SystemTags() closes SYS_Output itself. */
    }
    say("# interface command rc=%u", (ULONG)rc);
}

int main(void)
{
    TapStats st;
    UWORD    tries;

    out_file = Open((STRPTR)"DH0:tickprobe.txt", MODE_NEWFILE);

    say("# tickprobe, periodic-timer rate and packet turnaround, from outside");

    if (tap_install(local_mac) != 0)
    {
        say("!! cannot install %s", TAP_DEVICE_NAME);
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    say("# eclock_hz=%u", tap_eclock_rate());
    rng_state ^= tap_eclock_now() | 1UL;

    if (!fine_timer_open())
        say("!! no UNIT_MICROHZ timer, phases will alias, results are void");

    measure_timebases(100);

    SockBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (SockBase == NULL)
    {
        say("!! bsdsocket.library would not open");
        fine_timer_close();
        tap_remove();
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    run_interface_command();

    for (tries = 0; tries < 500 && !tap_is_online(); tries++)
        Delay(1);

    if (!tap_is_online())
    {
        TapStats bad;

        tap_get_stats(&bad);
        say("!! the interface never came online");
        say("# tap online=%u offline=%u reads_queued=%u tx=%u",
            bad.online_count, bad.offline_count, bad.reads_queued,
            bad.tx_frames);
        CloseLibrary(SockBase);
        fine_timer_close();
        tap_remove();
        if (out_file != (BPTR)0) Close(out_file);
        return 20;
    }

    for (tries = 0; tries < 50 && tap_reads_for(ETYPE_IP) == 0; tries++)
        Delay(1);

    say("# online ipreads=%u arpreads=%u", tap_reads_for(ETYPE_IP),
        tap_reads_for(ETYPE_ARP));

    {
        Ev e;
        (VOID)inject_echo(ICMP_ID, 0xFFFF, 56);
        (VOID)wait_ev(&e, 2000);
        drain();
    }

    phase_icmp(64, 56);
    phase_icmp(32, 1400);

    if (handshake())
        phase_dack(160);

    phase_retransmit(20000);

    if (sock >= 0)
        (VOID)s_close(sock);

    tap_get_stats(&st);
    say("# tap tx=%u rx_delivered=%u rx_no_reader=%u copy_failed=%u overrun=%u",
        st.tx_frames, st.rx_delivered, st.rx_no_reader, st.rx_copy_failed,
        st.tx_overrun);
    say("# arp_answered=%u background=%u evq_lost=%u",
        n_arp_answered, n_background, n_evq_lost);
    say("# done");

    CloseLibrary(SockBase);
    SockBase = NULL;

    fine_timer_close();
    tap_remove();

    if (out_file != (BPTR)0)
        Close(out_file);

    return 0;
}
