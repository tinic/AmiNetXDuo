/*
 * traceroute -- report the routers between here and a host.
 *
 *     traceroute MAXTTL=-m/N/K,NUMERIC=-n/S,QUERIES=-q/N/K,TOS=-t/N/K,
 *                WAIT=-w/N/K,VERBOSE=-v/S,HOST/A,PACKETSIZE/N/K
 *
 *   MAXTTL      how far to go before giving up. Default 30.
 *   NUMERIC     addresses only; skip the reverse lookup for each hop.
 *   QUERIES     probes per hop. Default 3.
 *   TOS         the type-of-service byte to send with.
 *   WAIT        seconds to wait for each probe. Default 5.
 *   VERBOSE     also report ICMP that arrives and is not an answer.
 *   PACKETSIZE  the whole datagram, headers included. Default 60.
 *
 * Names and short forms are Roadshow's. Three of Roadshow's options are
 * absent; they are listed with reasons above the argument template below.
 *
 * Probes are ICMP echo rather than the classic Unix UDP datagrams, because a
 * UDP probe cannot carry the TTL here. setsockopt(IPPROTO_IP, IP_TTL) records
 * the value on the socket, and only the raw send path reads it: a NetX Duo UDP
 * socket carries the TTL it was created with -- NX_IP_TIME_TO_LIVE, fixed at
 * nx_udp_socket_create() time -- so a UDP probe leaves with TTL 128 whatever
 * was asked for, and every hop reports as the destination. Measured on the
 * wire; docs/RESEARCH.md 19 has the capture. nxd_ip_raw_packet_send() takes
 * the socket's TTL as an argument, so the ICMP path honours it.
 *
 * A raw ICMP socket sees the whole ICMP input: TIME_EXCEEDED from each router,
 * ECHOREPLY from the destination, and any ICMP belonging to another program's
 * ping. Probes are matched by identifier and sequence -- ours in the echo
 * request, and in the copy a router quotes back inside the TIME_EXCEEDED.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

#include <devices/timer.h>
#include <proto/timer.h>

const char *const tool_name = "traceroute";

static const char version_tag[] __attribute__((used)) =
    "$VER: traceroute 1.0 (26.7.2026)";

/*
 * Roadshow options not implemented here:
 *
 *   -p PORT       destination port of a UDP probe. There are no UDP probes
 *                 here (see above), so there is no port to set.
 *   -r DONTROUTE  SO_DONTROUTE. bsdsocket.library does not implement it and
 *                 NetX Duo has no equivalent to implement it with.
 *   -s SOURCE     the address to send from. bind() on a raw socket records an
 *                 address and nothing more -- NetX Duo binds sockets to ports,
 *                 and the route chooses the source address of a raw datagram
 *                 -- so the option would change nothing.
 *
 * Each would parse and then do nothing, so accepting them would misreport
 * which interface the probe left by.
 */
#define TEMPLATE                                                        \
    "MAXTTL=-m/N/K,NUMERIC=-n/S,QUERIES=-q/N/K,TOS=-t/N/K,WAIT=-w/N/K," \
    "VERBOSE=-v/S,HOST/A,PACKETSIZE/N/K"

enum
{
    ARG_MAXTTL = 0,
    ARG_NUMERIC,
    ARG_QUERIES,
    ARG_TOS,
    ARG_WAIT,
    ARG_VERBOSE,
    ARG_HOST,
    ARG_PACKETSIZE,
    ARG_COUNT
};

#define TR_DEFAULT_MAXTTL   30UL
#define TR_DEFAULT_QUERIES  3UL
#define TR_DEFAULT_WAIT     5UL         /* seconds                          */

/*
 * PACKETSIZE is the whole IP datagram. The floor is an IP header plus an echo
 * header with no payload.
 */
#define TR_DEFAULT_PACKET   60UL
#define TR_MIN_PACKET       28UL        /* 20 IP + 8 ICMP                   */
#define TR_MAX_PACKET       1400UL
#define TR_HEADERS          28UL
#define TR_MAX_SIZE         (TR_MAX_PACKET - TR_HEADERS)
#define TR_CEILING_HOPS     255UL

/* ICMP, the four types this command reads. */
#define ICMP_ECHOREPLY      0
#define ICMP_UNREACH        3
#define ICMP_ECHO           8
#define ICMP_TIMXCEED       11

/* Static rather than automatic: a Shell command gets whatever stack the Shell
   has, 4 KB on a stock 3.1. */
static UBYTE tr_probe[TR_MAX_SIZE + 8];
static UBYTE tr_reply[2048];

/* ------------------------------------------------------------- the clock -- */

/*
 * Tenths of a millisecond since the first call.
 *
 * ami_millis() counts whole milliseconds, which is too coarse for the first
 * hop on a local link -- every probe would read "0 ms". The EClock is read
 * directly instead, through the timer.device ami_millis() has already opened
 * (that is what TimerBase is), which is why the first call here is to
 * ami_millis() with its result discarded.
 */
extern struct Device *TimerBase;        /* compat.c; the ReadEClock() inline */

static ULONG            tr_ticks_per_tenth_ms;
static struct EClockVal tr_epoch;

static BOOL tr_clock_open(VOID)
{
    ULONG rate;

    (VOID)ami_millis();                 /* opens timer.device, sets TimerBase */

    if (TimerBase == NULL)
        return FALSE;

    rate = ReadEClock(&tr_epoch);
    tr_ticks_per_tenth_ms = rate / 10000UL;     /* ~71 ticks per 0.1 ms */
    if (tr_ticks_per_tenth_ms == 0)
        tr_ticks_per_tenth_ms = 1;

    return TRUE;
}

static ULONG tr_now(VOID)
{
    struct EClockVal ev;

    if (TimerBase == NULL)
        return 0;

    ReadEClock(&ev);

    /* The low word alone: at ~710 kHz it wraps every ~100 minutes, and the
       subtraction stays correct across one wrap. */
    return (ev.ev_lo - tr_epoch.ev_lo) / tr_ticks_per_tenth_ms;
}

/* --------------------------------------------------------------- checksum -- */

/* The 16-bit one's-complement sum every IP protocol uses. */
static UWORD tr_checksum(const UBYTE *data, ULONG len)
{
    ULONG sum = 0;
    ULONG i;

    for (i = 0; i + 1 < len; i += 2)
        sum += ((ULONG)data[i] << 8) | (ULONG)data[i + 1];

    if ((len & 1) != 0)
        sum += (ULONG)data[len - 1] << 8;

    while ((sum >> 16) != 0)
        sum = (sum & 0xffffUL) + (sum >> 16);

    return (UWORD)(~sum & 0xffffUL);
}

/* ---------------------------------------------------------------- probes --- */

static VOID tr_put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)(v & 0xff);
}

static UWORD tr_get16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

/* An ICMP echo request, checksummed, ready for the raw socket. */
static ULONG tr_build_echo(UWORD ident, UWORD seq, ULONG payload)
{
    ULONG total = 8UL + payload;
    ULONG i;

    tr_probe[0] = ICMP_ECHO;
    tr_probe[1] = 0;
    tr_put16(&tr_probe[2], 0);              /* checksum, filled in below */
    tr_put16(&tr_probe[4], ident);
    tr_put16(&tr_probe[6], seq);

    for (i = 0; i < payload; i++)
        tr_probe[8 + i] = (UBYTE)(i & 0xff);

    tr_put16(&tr_probe[2], tr_checksum(tr_probe, total));

    return total;
}

/* --------------------------------------------------------------- replies --- */

/*
 * What one received datagram means for the probe we are waiting on. TR_OTHER
 * covers both somebody else's ICMP and a reply to a probe already given up on;
 * the caller goes back to waiting with the time it has left.
 */
enum
{
    TR_OTHER = 0,       /* not ours -- keep waiting                         */
    TR_HOP,             /* TIME_EXCEEDED: a router on the way              */
    TR_ARRIVED,         /* ECHOREPLY: the destination itself               */
    TR_UNREACH          /* DEST_UNREACHABLE: the path ends here            */
};

static LONG tr_classify(const UBYTE *buf, ULONG len, UWORD ident, UWORD seq,
                        UBYTE *code_out)
{
    ULONG ip_hlen;
    ULONG icmp_len;
    const UBYTE *icmp;

    *code_out = 0;

    if (len < 20 || (buf[0] >> 4) != 4)
        return TR_OTHER;

    ip_hlen = (ULONG)(buf[0] & 0x0f) * 4UL;
    if (ip_hlen < 20 || ip_hlen + 8 > len)
        return TR_OTHER;

    icmp     = buf + ip_hlen;
    icmp_len = len - ip_hlen;

    if (icmp[0] == ICMP_ECHOREPLY)
    {
        if (tr_get16(&icmp[4]) != ident || tr_get16(&icmp[6]) != seq)
            return TR_OTHER;

        return TR_ARRIVED;
    }

    if (icmp[0] == ICMP_TIMXCEED || icmp[0] == ICMP_UNREACH)
    {
        /*
         * RFC 792: the message quotes the offending datagram's IP header and
         * the first 64 bits after it, which for our probe is the echo header
         * including identifier and sequence. Anything quoting less than that
         * cannot be attributed.
         */
        const UBYTE *orig = icmp + 8;
        ULONG        orig_hlen;

        if (icmp_len < 8 + 20)
            return TR_OTHER;

        orig_hlen = (ULONG)(orig[0] & 0x0f) * 4UL;
        if (orig_hlen < 20 || (ULONG)(8 + orig_hlen + 8) > icmp_len)
            return TR_OTHER;

        if (orig[9] != TOOL_IPPROTO_ICMP)
            return TR_OTHER;

        if (orig[orig_hlen] != ICMP_ECHO ||
            tr_get16(&orig[orig_hlen + 4]) != ident ||
            tr_get16(&orig[orig_hlen + 6]) != seq)
            return TR_OTHER;

        *code_out = icmp[1];

        return (icmp[0] == ICMP_TIMXCEED) ? TR_HOP : TR_UNREACH;
    }

    return TR_OTHER;
}

/*
 * The one-letter annotations traceroute has printed since 1988 for an
 * unreachable. Codes not in the list print as their number.
 */
static VOID tr_print_unreach(UBYTE code)
{
    static char other[8];

    switch (code)
    {
        case 0:  tool_printf(" !N"); return;     /* network unreachable    */
        case 1:  tool_printf(" !H"); return;     /* host unreachable       */
        case 2:  tool_printf(" !P"); return;     /* protocol unreachable   */
        case 3:  tool_printf(" !");  return;     /* port: we have arrived  */
        case 4:  tool_printf(" !F"); return;     /* fragmentation needed   */
        case 5:  tool_printf(" !S"); return;     /* source route failed    */
        case 13: tool_printf(" !X"); return;     /* administratively barred */
        default: break;
    }

    other[0] = ' ';
    other[1] = '!';
    other[2] = '<';
    other[3] = (char)('0' + (code / 10) % 10);
    other[4] = (char)('0' + code % 10);
    other[5] = '>';
    other[6] = '\0';
    tool_printf("%s", (LONG)other);
}

/* ------------------------------------------------------------------ names -- */

/*
 * gethostbyaddr(), LVO -0x0d8. Open-coded here rather than added to toolsock:
 * this is the only command in the tree that wants the reverse direction.
 */
static ToolHostEnt *tr_gethostbyaddr(struct Library *base, const UBYTE *addr,
                                     LONG len, LONG type)
{
    register struct Library *a6  __asm("a6") = base;
    register CONST_APTR      a0  __asm("a0") = (CONST_APTR)addr;
    register LONG            d0  __asm("d0") = len;
    register LONG            d1  __asm("d1") = type;
    register ToolHostEnt    *res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-216:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0), "r" (d0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

/*
 * " name (1.2.3.4)", or just " 1.2.3.4" under NUMERIC. The lookup is on by
 * default since -n turns it off, and runs once per distinct hop rather than
 * once per probe.
 */
static VOID tr_show_address(struct Library *sb, ULONG address, BOOL numeric)
{
    char dotted[16];

    ami_config_format_ip(address, dotted, sizeof(dotted));

    if (!numeric)
    {
        UBYTE quad[4];
        ToolHostEnt *he;

        quad[0] = (UBYTE)((address >> 24) & 0xff);
        quad[1] = (UBYTE)((address >> 16) & 0xff);
        quad[2] = (UBYTE)((address >>  8) & 0xff);
        quad[3] = (UBYTE)(address & 0xff);

        he = tr_gethostbyaddr(sb, quad, 4L, (LONG)TOOL_AF_INET);
        if (he != NULL && he->h_name != NULL && he->h_name[0] != '\0')
        {
            tool_printf(" %s (%s)", (LONG)he->h_name, (LONG)dotted);
            return;
        }
    }

    tool_printf(" %s", (LONG)dotted);
}

/* ------------------------------------------------------------------- main -- */

static LONG arg_or(const LONG *args, int index, LONG fallback)
{
    const LONG *p = (const LONG *)args[index];

    return (p != NULL) ? *p : fallback;
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    ToolSockAddr    dest;
    const char     *host;
    ULONG           address = 0;
    ULONG           maxttl;
    ULONG           queries;
    ULONG           wait;
    ULONG           packetsize;
    ULONG           size;
    LONG            tos;
    BOOL            numeric;
    BOOL            verbose;
    LONG            sock = -1;
    UWORD           ident;
    UWORD           seq = 0;
    ULONG           ttl;
    LONG            rc = RETURN_OK;
    BOOL            done = FALSE;
    BOOL            interrupted = FALSE;
    ULONG           i;
    char            dotted[16];

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (ULONG)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<host>",
                   "Reports every router between here and a host.");
        return RETURN_ERROR;
    }

    host       = (const char *)args[ARG_HOST];
    maxttl     = (ULONG)arg_or(args, ARG_MAXTTL,  (LONG)TR_DEFAULT_MAXTTL);
    queries    = (ULONG)arg_or(args, ARG_QUERIES, (LONG)TR_DEFAULT_QUERIES);
    wait       = (ULONG)arg_or(args, ARG_WAIT,    (LONG)TR_DEFAULT_WAIT);
    packetsize = (ULONG)arg_or(args, ARG_PACKETSIZE,
                               (LONG)TR_DEFAULT_PACKET);
    tos        = arg_or(args, ARG_TOS, 0L);
    numeric    = (args[ARG_NUMERIC] != 0) ? TRUE : FALSE;
    verbose    = (args[ARG_VERBOSE] != 0) ? TRUE : FALSE;

    if (maxttl == 0 || maxttl > TR_CEILING_HOPS)
        maxttl = TR_DEFAULT_MAXTTL;
    if (queries == 0)
        queries = TR_DEFAULT_QUERIES;
    if (wait == 0)
        wait = TR_DEFAULT_WAIT;

    if (packetsize < TR_MIN_PACKET || packetsize > TR_MAX_PACKET)
    {
        tool_error("PACKETSIZE must be between %lu and %lu bytes",
                   TR_MIN_PACKET, TR_MAX_PACKET);
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (tos < 0 || tos > 255)
    {
        tool_error("TOS is a single byte, so 0 to 255");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    size = packetsize - TR_HEADERS;     /* what is left for the payload */

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!tool_sock_resolve(sb, host, &address))
    {
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    sock = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_RAW,
                            TOOL_IPPROTO_ICMP);
    if (sock < 0)
    {
        LONG err = tool_sock_errno(sb);

        tool_error("cannot open a raw socket: %s",
                   (LONG)tool_sock_errstr(err));

        if (err == TOOL_EPROTONOSUPPORT || err == TOOL_ESOCKTNOSUPPORT ||
            err == TOOL_EOPNOTSUPP || err == TOOL_EAFNOSUPPORT)
        {
            tool_advise_blank();
            tool_advise("This command needs SOCK_RAW, and the TCP/IP stack on");
            tool_advise("this machine does not offer it.  Neither ping nor");
            tool_advise("traceroute can work without it.");
        }

        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /*
     * TOS is set once. Unlike IP_TTL on a UDP socket it does reach the wire:
     * the raw send path hands it to nxd_ip_raw_packet_send() with the TTL.
     */
    if (tos != 0 &&
        tool_sock_setsockopt(sb, sock, TOOL_IPPROTO_IP, TOOL_IP_TOS, &tos,
                             (LONG)sizeof(tos)) != 0)
    {
        tool_error("this stack will not set the type of service: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        (VOID)tool_sock_close(sb, sock);
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!tr_clock_open())
    {
        tool_error("cannot open the timer");
        (VOID)tool_sock_close(sb, sock);
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /*
     * The identifier that makes a reply ours. Every raw ICMP socket on this
     * machine is handed a copy of every ICMP packet that arrives, so a
     * concurrent traceroute and ping each see the other's. The Task address is
     * unique for as long as this command runs.
     */
    ident = (UWORD)(((ULONG)FindTask(NULL) >> 2) & 0xffffUL);
    if (ident == 0)
        ident = 1;

    tool_sock_addr(&dest, address, 0);
    ami_config_format_ip(address, dotted, sizeof(dotted));

    tool_printf("traceroute to %s (%s), %lu hops max, %lu byte packets\n",
                (LONG)host, (LONG)dotted, maxttl, packetsize);

    for (ttl = 1; ttl <= maxttl && !done; ttl++)
    {
        ULONG last_shown = 0;           /* the address already printed      */
        BOOL  shown = FALSE;
        LONG  ttlval = (LONG)ttl;
        ULONG q;

        if (tool_break())
        {
            interrupted = TRUE;
            break;
        }

        tool_printf("%2ld ", (LONG)ttl);

        if (tool_sock_setsockopt(sb, sock, TOOL_IPPROTO_IP, TOOL_IP_TTL,
                                 &ttlval, (LONG)sizeof(ttlval)) != 0)
        {
            tool_printf("\n");
            tool_error("this stack will not set the time-to-live: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            rc = RETURN_FAIL;
            break;
        }

        for (q = 0; q < queries; q++)
        {
            ULONG probe_len;
            ULONG t0;
            ULONG deadline;
            LONG  verdict = TR_OTHER;
            UBYTE code = 0;
            ULONG from_addr = 0;
            ULONG elapsed = 0;
            BOOL  bailed = FALSE;

            if (tool_break())
            {
                interrupted = TRUE;
                break;
            }

            probe_len = tr_build_echo(ident, ++seq, size);

            t0 = tr_now();

            if (tool_sock_sendto(sb, sock, tr_probe, (LONG)probe_len,
                                 &dest) != (LONG)probe_len)
            {
                tool_printf("\n");
                tool_error("cannot send: %s",
                           (LONG)tool_sock_errstr(tool_sock_errno(sb)));
                rc = RETURN_ERROR;
                done = TRUE;
                bailed = TRUE;
                break;
            }

            deadline = t0 + wait * 10000UL;         /* tenths of a ms */

            while (verdict == TR_OTHER)
            {
                ToolFdSet    readfds;
                ToolTimeval  tv;
                ToolSockAddr from;
                ULONG        now = tr_now();
                ULONG        left;
                LONG         ready;
                LONG         n;

                if (tool_break())
                {
                    interrupted = TRUE;
                    break;
                }

                /*
                 * Signed difference, not `now >= deadline`: the EClock's low
                 * word wraps about every hundred minutes, and an unsigned
                 * comparison across the wrap would wait out the rest of it.
                 */
                if ((LONG)(now - deadline) >= 0)
                    break;

                left = deadline - now;
                if (left > 2000UL)
                    left = 2000UL;      /* 200 ms, so Ctrl-C is noticed */

                tool_fd_zero(&readfds);
                tool_fd_add(&readfds, sock);

                tv.tv_secs  = 0;
                tv.tv_micro = (LONG)(left * 100UL);

                ready = tool_sock_select(sb, sock + 1, &readfds, NULL, &tv);
                if (ready < 0)
                {
                    if (tool_sock_errno(sb) == TOOL_EINTR)
                        continue;
                    break;
                }
                if (ready == 0)
                    continue;

                n = tool_sock_recvfrom(sb, sock, tr_reply,
                                       (LONG)sizeof(tr_reply), &from);
                if (n <= 0)
                    continue;

                verdict = tr_classify(tr_reply, (ULONG)n, ident, seq, &code);
                if (verdict != TR_OTHER)
                {
                    from_addr = from.sin_addr;
                    elapsed   = tr_now() - t0;
                }
                else if (verbose)
                {
                    /*
                     * ICMP that arrived and was not an answer to this probe.
                     * Reported under -v: a router answering in an unexpected
                     * way and nothing coming back at all are different faults
                     * that otherwise look identical.
                     */
                    ULONG hlen = (ULONG)(tr_reply[0] & 0x0f) * 4UL;
                    char  who[16];

                    ami_config_format_ip(from.sin_addr, who, sizeof(who));
                    tool_printf(" (%s sent ICMP type %ld)", (LONG)who,
                                (hlen + 1 <= (ULONG)n)
                                    ? (LONG)tr_reply[hlen] : -1L);
                }
            }

            if (interrupted || bailed)
                break;

            if (verdict == TR_OTHER)
            {
                tool_printf(" *");
                continue;
            }

            if (!shown || from_addr != last_shown)
            {
                tr_show_address(sb, from_addr, numeric);
                last_shown = from_addr;
                shown = TRUE;
            }

            tool_printf("  %lu.%lu ms", elapsed / 10UL, elapsed % 10UL);

            /*
             * Either verdict ends the trace but not this hop: the remaining
             * queries still run, so the last line carries as many timings as
             * the others.
             */
            if (verdict == TR_UNREACH)
                tr_print_unreach(code);

            if (verdict == TR_UNREACH || verdict == TR_ARRIVED)
                done = TRUE;
        }

        tool_printf("\n");
    }

    if (!interrupted && rc == RETURN_OK && !done)
    {
        tool_error("%s did not answer within %lu hops", (LONG)host, maxttl);
        rc = RETURN_WARN;
    }

    if (sock >= 0)
        (VOID)tool_sock_close(sb, sock);

    CloseLibrary(sb);
    FreeArgs(rda);

    if (interrupted)
    {
        tool_fault(ERROR_BREAK);
        return RETURN_WARN;
    }

    return (int)rc;
}
