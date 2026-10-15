/*
 * ping, ICMP echo, over a raw socket.
 *
 * SOCK_RAW rather than nx_icmp_ping(): no command links aminetxduo_netstack,
 * and nx_icmp_ping() matches an inbound reply on the sequence number alone,
 * which FS-UAE's SLIRP zeroes on a proxied reply. TIMEOUT bounds the whole run,
 * not one reply; the per-reply wait is PING_REPLY_WAIT and is not exposed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

#include "aminetxduo/compat.h"

#include <proto/exec.h>
#include <devices/timer.h>
#include "aminetxduo/version.h"

const char *const tool_name = "ping";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ping");

/*
 * The AmigaDOS Command Reference's template, with -n written as it means:
 * the reference's -n=NUMERICONLY/S=NUMERIC/S puts NUMERIC among the modifiers.
 * test_refcompat holds TEMPLATE to it.  -d DEBUG does nothing (SO_DEBUG shows
 * nothing here), -R RECORDROUTE is ignored with a warning (no IP options on a
 * raw socket), and DONTROUTE refuses a host no interface reaches directly,
 * since bsdsocket.library has no SO_DONTROUTE.
 */
#define REFERENCE_TEMPLATE "-c=COUNT/K/N,-d=DEBUG/S,-i=INTERVAL/K/N,"      \
                           "-l=LOAD/K/N,-n=NUMERICONLY=NUMERIC/S,"         \
                           "-q=QUIET/S,-R=RECORDROUTE/S,DONTROUTE/S,"      \
                           "-s=SIZE/K/N,-v=VERBOSE/S,BELL/S,HOST/A"

#define TEMPLATE    "-c=COUNT/K/N,-d=DEBUG/S,-i=INTERVAL/K/N,-l=LOAD/K/N," \
                    "-n=NUMERICONLY=NUMERIC/S,-o=ONEREPLY/S,-q=QUIET/S," \
                    "-R=RECORDROUTE/S,DONTROUTE/S,-s=SIZE/K/N,"          \
                    "-t=TIMEOUT/K/N,-v=VERBOSE/S,BELL/S,HOST/A,"         \
                    "IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_COUNT = 0,
    ARG_DEBUG,
    ARG_INTERVAL,
    ARG_LOAD,
    ARG_NUMERIC,
    ARG_ONEREPLY,
    ARG_QUIET,
    ARG_RECORDROUTE,
    ARG_DONTROUTE,
    ARG_SIZE,
    ARG_TIMEOUT,
    ARG_VERBOSE,
    ARG_BELL,
    ARG_HOST,
    ARG_IPV4,
    ARG_IPV6,
    ARG_ARGCOUNT
};

#define PING_DEFAULT_COUNT      4UL
#define PING_DEFAULT_SIZE       56UL
/* 65535 - 60 - 8: reserve the largest IPv4 header, then ICMP's header, as BSD
   ping does.  It was 1400, which is less than one frame: the classic MTU
   probe, -s 1472 (a full 1500-byte datagram) beside -s 1473 (the first that
   fragments), could not be typed, and a card that drops full-size frames --
   genet.device 3.x did, and SMB2 froze on it until MTU=1450 -- could only be
   found with somebody else's ping. */
#define PING_MAX_SIZE           65467UL
#define PING_DEFAULT_INTERVAL   1UL         /* seconds                        */
#define PING_REPLY_WAIT         5UL         /* seconds to wait for one reply  */
/* The most TIMEOUT the millisecond clock can measure before it wraps. */
#define PING_MAX_TIMEOUT        4294967UL   /* 0xffffffff ms / 1000           */

/* ICMP, the two types this command reads. ICMPv6 renumbered both. */
#define ICMP_ECHOREPLY          0
#define ICMP_ECHO               8
#define ICMP6_ECHO              128
#define ICMP6_ECHOREPLY         129

/* Static rather than automatic: a Shell command gets the Shell's stack, 4 KB
   on a stock 3.1. */
/* Sized to the SIZE asked for, once it is known (ping_buffers()): a static
   PING_MAX_SIZE would be 128 KB of BSS for a command that usually sends 56. */
static UBYTE *ping_probe;
static UBYTE *ping_reply;
static ULONG  ping_reply_size;

static LONG arg_or(const LONG *args, int index, LONG fallback)
{
    const LONG *p = (const LONG *)args[index];

    return (p != NULL) ? *p : fallback;
}

/* --------------------------------------------------------------- packets, */

static VOID ping_put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)(v & 0xff);
}

static UWORD ping_get16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

/* The 16-bit one's-complement sum. ICMPv6 is the exception: its checksum covers
   the IPv6 pseudo-header, so only the stack knows the source address. */
static UWORD ping_checksum(const UBYTE *data, ULONG len)
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

/* The probe holds header + payload; the reply also the IPv4 header a raw
   socket hands back (up to 60 bytes), with room to notice a longer one. */
static BOOL ping_buffers(ULONG payload)
{
    ping_reply_size = 8UL + payload + 128UL;
    ping_probe = AllocVec(8UL + payload, MEMF_ANY);
    ping_reply = AllocVec(ping_reply_size, MEMF_ANY);
    return (ping_probe != NULL && ping_reply != NULL) ? TRUE : FALSE;
}

static VOID ping_buffers_free(VOID)
{
    FreeVec(ping_probe);
    FreeVec(ping_reply);
    ping_probe = NULL;
    ping_reply = NULL;
}

static ULONG ping_build(BOOL v6, UWORD ident, UWORD seq, ULONG payload)
{
    ULONG total = 8UL + payload;
    ULONG i;

    ping_probe[0] = v6 ? ICMP6_ECHO : ICMP_ECHO;
    ping_probe[1] = 0;
    ping_put16(&ping_probe[2], 0);          /* checksum */
    ping_put16(&ping_probe[4], ident);
    ping_put16(&ping_probe[6], seq);

    /* The usual incrementing pattern, so corruption is visible. */
    for (i = 0; i < payload; i++)
        ping_probe[8 + i] = (UBYTE)(i & 0xff);

    if (!v6)
        ping_put16(&ping_probe[2], ping_checksum(ping_probe, total));

    return total;
}

/*
 * Is this datagram the reply to the probe we are waiting on? A raw ICMP socket
 * sees every inbound ICMP datagram. `seq == 0` is accepted alongside the
 * expected number because FS-UAE's SLIRP zeroes it on a proxied reply; safe
 * only because this command has exactly one probe outstanding at a time.
 */
static BOOL ping_is_reply(BOOL v6, const UBYTE *buf, ULONG len, UWORD ident,
                          UWORD seq, const ToolAddr *from,
                          const ToolAddr *target, ULONG *payload_out)
{
    const UBYTE *icmp;
    ULONG        icmp_len;

    if (v6)
    {
        /* No IP header: a raw IPv6 read starts at the ICMPv6 header. */
        icmp     = buf;
        icmp_len = len;
    }
    else
    {
        ULONG hlen;

        if (len < 20 || (buf[0] >> 4) != 4)
            return FALSE;

        hlen = (ULONG)(buf[0] & 0x0f) * 4UL;
        if (hlen < 20 || len < hlen + 8)
            return FALSE;

        icmp     = buf + hlen;
        icmp_len = len - hlen;
    }

    if (icmp_len < 8)
        return FALSE;

    if (icmp[0] != (v6 ? ICMP6_ECHOREPLY : ICMP_ECHOREPLY))
        return FALSE;

    if (ping_get16(&icmp[4]) != ident)
        return FALSE;

    {
        UWORD got = ping_get16(&icmp[6]);

        if (got != seq && got != 0)
            return FALSE;
    }

    /* A reply from another address is not an answer from the host that was
       asked about. SLIRP's proxied replies do carry the destination's. */
    if (!tool_addr_same(from, target))
        return FALSE;

    if (payload_out != NULL)
        *payload_out = icmp_len - 8UL;

    return TRUE;
}

/* What -v names.  The rest print as their number. */
static const char *ping_icmp_name(BOOL v6, UBYTE type)
{
    if (v6)
    {
        switch (type)
        {
            case 1:   return "destination unreachable";
            case 2:   return "packet too big";
            case 3:   return "time exceeded";
            case 4:   return "parameter problem";
            case 133: return "router solicitation";
            case 134: return "router advertisement";
            case 135: return "neighbour solicitation";
            case 136: return "neighbour advertisement";
            case 137: return "redirect";
            default:  return NULL;
        }
    }

    switch (type)
    {
        case 3:  return "destination unreachable";
        case 4:  return "source quench";
        case 5:  return "redirect";
        case 11: return "time exceeded";
        case 12: return "parameter problem";
        default: return NULL;
    }
}

/*
 * -v: an ICMP message that is neither an echo request nor a reply, from
 * anybody.  The raw socket is handed every inbound ICMP datagram (raw.c), so
 * this is what BSD ping -v lists.
 */
static VOID ping_verbose(struct Library *sb, BOOL v6, const UBYTE *buf,
                         ULONG len, const ToolAddr *from)
{
    const UBYTE *icmp = buf;
    const char  *name;
    char         text[TOOL_ADDR_STRLEN];

    if (!v6)
    {
        ULONG hlen;

        if (len < 20 || (buf[0] >> 4) != 4)
            return;
        hlen = (ULONG)(buf[0] & 0x0f) * 4UL;
        if (hlen < 20 || len < hlen + 4)
            return;
        icmp = buf + hlen;
        len -= hlen;
    }
    else if (len < 4)
    {
        return;
    }

    if (icmp[0] == (v6 ? ICMP6_ECHO : ICMP_ECHO) ||
        icmp[0] == (v6 ? ICMP6_ECHOREPLY : ICMP_ECHOREPLY))
        return;

    tool_addr_text(sb, from, text, sizeof(text));
    name = ping_icmp_name(v6, icmp[0]);

    if (name != NULL)
        tool_printf("%lu bytes from %s: %s, code %lu\n", len, (LONG)text,
                    (LONG)name, (ULONG)icmp[1]);
    else
        tool_printf("%lu bytes from %s: ICMP type %lu, code %lu\n", len,
                    (LONG)text, (ULONG)icmp[0], (ULONG)icmp[1]);
}

/* ----------------------------------------------------------- the clock --- */

/*
 * ami_millis() counts whole milliseconds through the timer.device it opens on
 * first use. Called before the first send so the open is not inside the first
 * round-trip measurement.
 */

/*
 * The body, so that ami_netdb_free() has exactly one place to run. atexit() is
 * not free here: libnix drags in about 7.7 KB of malloc and stdio machinery
 * nothing in this command calls.
 */
/* INTERVAL seconds, a second at a time: in ticks, anything over 85899345
   seconds wrapped to a pause of a few ticks (F-209).  TRUE on a break. */
static BOOL ping_pause(ULONG seconds)
{
    while (seconds-- > 0)
        if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
            return TRUE;

    return FALSE;
}

/* Milliseconds TIMEOUT has left.  TIMEOUT is at most PING_MAX_TIMEOUT, so
   the product fits in 32 bits. */
static ULONG ping_left_ms(ULONG timeout, ULONG elapsed_ms)
{
    ULONG secs = elapsed_ms / 1000UL;

    if (secs >= timeout)
        return 0;

    return (timeout - secs) * 1000UL - elapsed_ms % 1000UL;
}

/* The last pause of a run that TIMEOUT ends: whole seconds, then the rest in
   ticks rounded up.  TRUE on a break. */
static BOOL ping_pause_ms(ULONG ms)
{
    if (ping_pause(ms / 1000UL))
        return TRUE;

    return tool_delay_ticks((ms % 1000UL + 19UL) / 20UL);
}

static int ping_main(int argc, char **argv);

int main(int argc, char **argv)
{
    int rc = ping_main(argc, argv);

    ami_netdb_free();
    return rc;
}

static int ping_main(int argc, char **argv)
{
    LONG            args[ARG_ARGCOUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    const char     *host;
    const char     *shown;
    ToolSockAddrAny to;
    LONG            sock = -1;
    LONG            family;
    ToolAddr        target;
    ULONG           count;
    ULONG           interval;
    ULONG           preload;
    ULONG           size;
    ULONG           timeout;
    UWORD           ident;
    BOOL            v6;
    BOOL            numeric;
    BOOL            onereply;
    BOOL            quiet;
    BOOL            bell;
    BOOL            verbose;
    ULONG           sent = 0;
    ULONG           received = 0;
    ULONG           rtt_min = 0xffffffffUL;
    ULONG           rtt_max = 0;
    ULONG           rtt_total = 0;
    ULONG           started = 0;
    BOOL            interrupted = FALSE;
    BOOL            expired = FALSE;
    LONG            rc = RETURN_OK;
    ULONG           i;
    ULONG           parsed = 0;
    char            addrtext[TOOL_ADDR_STRLEN];
    char            hostname[AMI_CFG_NAME_LEN];

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (ULONG)ARG_ARGCOUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    if ((args[ARG_COUNT]    != 0 && *(LONG *)args[ARG_COUNT]    < 0) ||
        (args[ARG_INTERVAL] != 0 && *(LONG *)args[ARG_INTERVAL] < 0) ||
        (args[ARG_LOAD]     != 0 && *(LONG *)args[ARG_LOAD]     < 0) ||
        (args[ARG_SIZE]     != 0 && *(LONG *)args[ARG_SIZE]     < 0) ||
        (args[ARG_TIMEOUT]  != 0 && *(LONG *)args[ARG_TIMEOUT]  < 0))
    {
        tool_error("COUNT, INTERVAL, LOAD, SIZE and TIMEOUT cannot be negative");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    host     = (const char *)args[ARG_HOST];
    count    = (ULONG)arg_or(args, ARG_COUNT,    (LONG)PING_DEFAULT_COUNT);
    interval = (ULONG)arg_or(args, ARG_INTERVAL, (LONG)PING_DEFAULT_INTERVAL);
    preload  = (ULONG)arg_or(args, ARG_LOAD,     0L);
    size     = (ULONG)arg_or(args, ARG_SIZE,     (LONG)PING_DEFAULT_SIZE);
    timeout  = (ULONG)arg_or(args, ARG_TIMEOUT,  0L);
    numeric  = (args[ARG_NUMERIC]  != 0) ? TRUE : FALSE;
    onereply = (args[ARG_ONEREPLY] != 0) ? TRUE : FALSE;
    quiet    = (args[ARG_QUIET]    != 0) ? TRUE : FALSE;
    bell     = (args[ARG_BELL]     != 0) ? TRUE : FALSE;
    verbose  = (args[ARG_VERBOSE]  != 0) ? TRUE : FALSE;

    if (args[ARG_RECORDROUTE] != 0)
        tool_error("RECORDROUTE ignored: no IP options on a raw socket");

    if (size > PING_MAX_SIZE)
    {
        tool_error("SIZE must be %lu bytes or less", PING_MAX_SIZE);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* ami_millis() wraps every 49.7 days, and a TIMEOUT past that would
       never be reached. */
    if (timeout > PING_MAX_TIMEOUT)
    {
        tool_error("TIMEOUT must be %lu seconds or less", PING_MAX_TIMEOUT);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6], &family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* tool_socket_open() supplies the API; it does not select or start a
       configured network interface. */
    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    shown       = host;
    hostname[0] = '\0';

    if (!tool_sock_resolve_af(sb, host, family, &target))
    {
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!numeric && !TOOL_ADDR_IS6(&target) &&
        ami_config_parse_ip(host, &parsed))
    {
        /*
         * A numeric address can only get a name from a reverse lookup, and that
         * lookup is DEVS:Internet/hosts and nothing else: gethostbyaddr() would
         * cost BSD_RESOLVE_TIMEOUT per name server before the first packet
         * leaves, for a cosmetic change to one line.
         */
        const AmiNetdbEntry *local;

        /*
         * AmigaOS does not reclaim AllocVec() memory when a process exits, so
         * the blocks ami_netdb_load() builds outlive this command. main() frees
         * it on the way out.
         */
        (VOID)ami_netdb_load();

        local = ami_netdb_host_by_addr(parsed);
        if (local != NULL && local->name != NULL && local->name[0] != '\0')
        {
            tool_copy_string(hostname, sizeof(hostname), local->name);
            shown = hostname;
        }
    }

    tool_addr_text(sb, &target, addrtext, sizeof(addrtext));

    v6 = TOOL_ADDR_IS6(&target);

    if (args[ARG_DONTROUTE] != 0)
    {
        if (v6)
            tool_error("DONTROUTE ignored for IPv6");
        else if (!tool_on_link(target.ta_V4))
        {
            tool_error("DONTROUTE: %s is not on a local network",
                       (LONG)addrtext);
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    sock = tool_sock_socket(sb,
                            v6 ? TOOL_AF_INET6 : TOOL_AF_INET, TOOL_SOCK_RAW,
                            v6 ? TOOL_IPPROTO_ICMPV6 : TOOL_IPPROTO_ICMP);
    if (sock < 0)
    {
        LONG err = tool_sock_errno(sb);

        tool_error("cannot open a raw socket: %s", (LONG)tool_sock_errstr(err));

        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /*
     * Non-blocking, because select() readiness is advisory: the datagram can be
     * taken by another reader, or dropped by a checksum test, between the poll
     * and the read. The raw receive path is not to be changed on the old note
     * that blamed WaitSelect(); that fault was docs/RESEARCH.md 25.
     */
    {
        LONG nonblock = 1;

        if (tool_sock_ioctl(sb, sock, TOOL_FIONBIO, &nonblock) != 0)
        {
            tool_error("cannot set non-blocking mode: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            (VOID)tool_sock_close(sb, sock);
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_FAIL;
        }
    }

    (VOID)tool_sock_addr(&to, &target, 0);

    /*
     * The identifier tells this command's replies from another raw reader's:
     * raw.c tees every inbound ICMP datagram to every open raw socket. The task
     * pointer is unique on the machine; its low two bits are always zero.
     */
    ident = (UWORD)((((ULONG)FindTask(NULL)) >> 2) & 0xffffUL);

    if (!ping_buffers(size))
    {
        ping_buffers_free();
        tool_error("no memory for a %lu-byte probe", size);
        (VOID)tool_sock_close(sb, sock);
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!quiet)
    {
        tool_printf("PING %s (%s): %lu data bytes\n",
                    (LONG)shown, (LONG)addrtext, size);
    }

    /* Force timer.device open before the first send is timed. */
    started = ami_millis();

    for (i = 0; count == 0 || i < count; i++)
    {
        ULONG total;
        ULONG wait = PING_REPLY_WAIT * 1000UL;     /* milliseconds */
        ULONG t0;
        ULONG deadline;
        ULONG rtt = 0;
        ULONG got_bytes = 0;
        BOOL  answered = FALSE;
        LONG  n;

        if (tool_break())
        {
            interrupted = TRUE;
            break;
        }

        /*
         * TIMEOUT bounds the run, not the reply, so a five-second reply wait
         * must not overshoot a two-second limit.  The wait is cut to the
         * milliseconds left once the request is out.
         */
        if (timeout != 0)
        {
            ULONG elapsed = (ami_millis() - started) / 1000UL;

            if (elapsed >= timeout)
            {
                expired = TRUE;
                break;
            }
        }

        total = ping_build(v6, ident, (UWORD)(i & 0xffffUL), size);

        t0 = ami_millis();
        n  = tool_sock_sendto(sb, sock, ping_probe, (LONG)total, &to);
        if (n != (LONG)total)
        {
            LONG err = tool_sock_errno(sb);

            tool_error("cannot send the request: %s",
                       (LONG)tool_sock_errstr(err));

            rc = RETURN_ERROR;
            break;
        }

        sent++;
        if (timeout != 0)
        {
            ULONG left = ping_left_ms(timeout, t0 - started);

            if (left < wait)
                wait = left;
        }
        deadline = t0 + wait;

        while (!answered)
        {
            ToolFdSet       readfds;
            ToolTimeval     tv;
            ToolSockAddrAny from;
            ToolAddr        from_addr;
            ULONG           now = ami_millis();
            ULONG           left;
            LONG            ready;

            if (tool_break())
            {
                interrupted = TRUE;
                break;
            }

            /* Signed difference rather than `now >= deadline`, so the
               comparison stays right across the millisecond counter's wrap. */
            if ((LONG)(now - deadline) >= 0)
                break;

            left = deadline - now;
            if (left > 200UL)
                left = 200UL;       /* so Ctrl-C is noticed inside the wait */

            tool_fd_zero(&readfds);
            tool_fd_add(&readfds, sock);

            tv.tv_secs  = 0;
            tv.tv_micro = (LONG)(left * 1000UL);

            ready = tool_sock_select(sb, sock + 1, &readfds, NULL, &tv);
            if (ready < 0)
            {
                if (tool_sock_errno(sb) == TOOL_EINTR)
                    continue;
                break;
            }
            if (ready == 0)
                continue;

            n = tool_sock_recvfrom(sb, sock, ping_reply,
                                   (LONG)ping_reply_size, &from);
            if (n <= 0)
                continue;

            if (!tool_sock_addr_get(&from, &from_addr))
                continue;

            if (ping_is_reply(v6, ping_reply, (ULONG)n, ident,
                              (UWORD)(i & 0xffffUL), &from_addr, &target,
                              &got_bytes))
            {
                rtt      = ami_millis() - t0;
                answered = TRUE;
            }
            else if (verbose)
            {
                ping_verbose(sb, v6, ping_reply, (ULONG)n, &from_addr);
            }
        }

        if (interrupted)
            break;

        if (answered)
        {
            received++;
            rtt_total += rtt;
            if (rtt < rtt_min)
                rtt_min = rtt;
            if (rtt > rtt_max)
                rtt_max = rtt;

            if (!quiet)
            {
                tool_printf("%lu bytes from %s: icmp_seq=%lu time=%lu ms\n",
                            got_bytes, (LONG)addrtext, i, rtt);
            }

            /* On the Amiga this flashes the screen or plays a sound. */
            if (bell)
                tool_printf("\007");

            if (onereply)
                break;
        }
        else if (!quiet)
        {
            tool_printf("Request timed out for icmp_seq=%lu\n", i);
        }

        /* No pause after the last request, and none while the preload is
           going out, which is what LOAD asks for. */
        if ((count != 0 && i + 1 >= count) || interval == 0)
            continue;
        if (i + 1 < preload)
            continue;

        /* TIMEOUT bounds the pause too: one that would run past it is cut to
           the milliseconds left, and then the run is over. */
        if (timeout != 0)
        {
            ULONG left = ping_left_ms(timeout, ami_millis() - started);

            if (left / 1000UL < interval)
            {
                if (ping_pause_ms(left))
                    interrupted = TRUE;
                else
                    expired = TRUE;
                break;
            }
        }

        if (ping_pause(interval))
        {
            interrupted = TRUE;
            break;
        }
    }

    if (expired && !quiet)
        tool_printf("Stopped after %lu seconds.\n", timeout);

    tool_printf("\n--- %s ping statistics ---\n", (LONG)shown);
    tool_printf("%lu packets transmitted, %lu received, %lu%% packet loss\n",
                sent, received,
                (sent > 0) ? ((sent - received) * 100UL) / sent : 0UL);

    if (received > 0)
    {
        tool_printf("round-trip min/avg/max = %lu/%lu/%lu ms\n",
                    rtt_min, rtt_total / received, rtt_max);
    }

    ping_buffers_free();
    if (sock >= 0)
        (VOID)tool_sock_close(sb, sock);
    CloseLibrary(sb);

    FreeArgs(rda);

    if (interrupted)
    {
        tool_fault(ERROR_BREAK);
        return RETURN_WARN;
    }
    if (rc == RETURN_OK && received == 0)
        return RETURN_WARN;

    return (int)rc;
}
