/*
 * ping, ICMP echo, over a raw socket.
 *
 *     ping -c=COUNT/K/N,-i=INTERVAL/K/N,-l=LOAD/K/N,-n=NUMERICONLY=NUMERIC/S,
 *          -o=ONEREPLY/S,-q=QUIET/S,-s=SIZE/K/N,-t=TIMEOUT/K/N,BELL/S,HOST/A,
 *          IPV4=-4/S,IPV6=-6/S
 *
 * Every option carries both spellings, short flag and keyword, as netstat does
 * for its switches.
 *
 *   COUNT     echo requests to send. 0 means until Ctrl-C. Default 4.
 *   INTERVAL  seconds to wait between requests. Default 1.
 *   LOAD      send this many requests back to back before the interval
 *             starts being honoured.
 *   NUMERIC   do not put a name to an address given as a number. That lookup
 *             is the local host table only, see the note where it happens.
 *   ONEREPLY  stop as soon as one reply has come back.
 *   QUIET     only the summary.
 *   SIZE      payload bytes. Default 56, matching other implementations.
 *   TIMEOUT   give up after this many seconds. 0, the default, means no limit.
 *   BELL      ring the console bell for each reply.
 *   -4 / -6   pin the family the name resolves to.  Without either, the
 *             library answers AF_UNSPEC and the selection rules pick, so on a
 *             dual stack there is otherwise no way to ask for the other one.
 *
 * TIMEOUT limits the whole run, not one reply: the wait for an individual
 * reply is fixed at PING_REPLY_WAIT below and is not exposed.
 *
 * Raw socket rather than nx_icmp_ping(): no command links aminetxduo_netstack,
 * so netstack_ip() resolved to src/tools/netstack_weak.c's weak stub, returned
 * NULL, and this command printed "the network is up, but this command cannot
 * read it" and exited 5 (docs/RESEARCH.md 22). Handing out the running NX_IP
 * means a pointer into another task's stack on a machine with no memory
 * protection. An LVO on bsdsocket.library that runs the ping on the caller's
 * behalf was designed and rejected, because nx_icmp_ping() matches an inbound
 * echo reply on the sequence number alone. nx_icmpv4_process_echo_reply.c:124
 * compares tx_thread_suspend_info against nx_icmpv4_echo_sequence_num and
 * looks at nothing else, and nx_icmpv4.h:191 says the identifier "is not used
 * as a host". FS-UAE's SLIRP zeroes the sequence on a proxied reply and preserves
 * the identifier (docs/RESEARCH.md 20.2), so every probe after the first of an
 * NX_IP's lifetime would time out while its reply sat in the stack.
 *
 * SOCK_RAW is published ABI, carries IP_TTL and IP_TOS to the wire (measured,
 * docs/RESEARCH.md 20.1), and lets this command decide what counts as an
 * answer. traceroute reaches the wire the same way. This command therefore
 * runs on Roadshow and AmiTCP too, links no NetX Duo, and needs no ThreadX
 * adoption.
 *
 * Ctrl-C stops the loop and still prints the summary. The break is noticed
 * every 200 ms, including while waiting for a reply. The ThreadX version
 * suspended inside a kernel where an Exec signal means nothing.
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

#define TEMPLATE    "-c=COUNT/K/N,-i=INTERVAL/K/N,-l=LOAD/K/N," \
                    "-n=NUMERICONLY=NUMERIC/S,-o=ONEREPLY/S,-q=QUIET/S," \
                    "-s=SIZE/K/N,-t=TIMEOUT/K/N,BELL/S,HOST/A," \
                    "IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_COUNT = 0,
    ARG_INTERVAL,
    ARG_LOAD,
    ARG_NUMERIC,
    ARG_ONEREPLY,
    ARG_QUIET,
    ARG_SIZE,
    ARG_TIMEOUT,
    ARG_BELL,
    ARG_HOST,
    ARG_IPV4,
    ARG_IPV6,
    ARG_ARGCOUNT
};

#define PING_DEFAULT_COUNT      4UL
#define PING_DEFAULT_SIZE       56UL
#define PING_MAX_SIZE           1400UL
#define PING_DEFAULT_INTERVAL   1UL         /* seconds                        */
#define PING_REPLY_WAIT         5UL         /* seconds to wait for one reply  */

/* ICMP, the two types this command reads. ICMPv6 renumbered both. */
#define ICMP_ECHOREPLY          0
#define ICMP_ECHO               8
#define ICMP6_ECHO              128
#define ICMP6_ECHOREPLY         129

/* Static rather than automatic: a Shell command gets the Shell's stack, 4 KB
   on a stock 3.1. */
static UBYTE ping_probe[PING_MAX_SIZE + 8];
static UBYTE ping_reply[2048];

/* Delay() counts DOS ticks. dos/dos.h has TICKS_PER_SECOND (50). */

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

/* The 16-bit one's-complement sum. Nothing below this command computes the
   ICMP checksum: the raw send path prepends the IP header and nothing else.
   ICMPv6 is the exception, its checksum covers the IPv6 pseudo-header, so
   the source address decides it and only the stack knows that. */
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
 * Is this datagram the reply to the probe we are waiting on?
 *
 * A raw ICMP socket sees every inbound ICMP datagram, src/bsdsocket/raw.c
 * filters on the IP protocol number, not on the type, so the checks have to
 * be strict. An IPv4 read hands back the IP header too. An IPv6 read does not.
 *
 * `seq == 0` is accepted alongside the expected number because FS-UAE's SLIRP
 * zeroes the sequence on a proxied reply while preserving the identifier
 * (docs/RESEARCH.md 20.2). Rejecting those would report 100% loss against
 * every address outside the emulated LAN. Safe here because this command has
 * exactly one probe outstanding at a time. traceroute has three per hop and
 * cannot do the same.
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

/* ----------------------------------------------------------- the clock --- */

/*
 * ami_millis() counts whole milliseconds through the timer.device it opens on
 * first use. It is called before the first send so the device open is not
 * inside the first round-trip measurement.
 */

/*
 * The body, so that ami_netdb_free() has exactly one place to run and this
 * command does not have to carry atexit(). atexit() is not free here: libnix
 * satisfies it out of an object that also references malloc and __errno, which
 * pulls in the C++ AVL allocator and the stdio FILE machinery, about 7.7 KB
 * that nothing in the command calls. A wrapper gives the same guarantee for
 * nothing.
 */
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

    if (size > PING_MAX_SIZE)
    {
        tool_error("SIZE must be %lu bytes or less", PING_MAX_SIZE);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6], &family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * tool_socket_open(), not tool_require_stack(): opening bsdsocket.library
     * brings the stack up, which is right here and wrong for a status command.
     */
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
         * A numeric address can only get a name from a reverse lookup, and
         * that lookup is DEVS:Internet/hosts and nothing else.
         *
         * gethostbyaddr() would cost BSD_RESOLVE_TIMEOUT: thirty seconds
         * (src/bsdsocket/resolver.c:18) per name server, against a server that
         * need not answer a PTR query at all, FS-UAE's SLIRP does not, all of
         * it before the first packet leaves, for a cosmetic change to one line
         * of output. The host table is instant and needs no name server.
         * NUMERIC skips even that.
         */
        const AmiNetdbEntry *local;

        /*
         * AmigaOS does not reclaim AllocVec() memory when a process exits, and
         * ami_alloc() is AllocVec(), so the twelve blocks ami_netdb_load()
         * builds out of DEVS:Internet outlive this command, 12,616 bytes per
         * run on a stock netdb, gone until reboot. main() frees it on the way
         * out.
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
     * Non-blocking, because select() readiness is advisory: the datagram can
     * be taken by another reader, or dropped by a checksum test, between the
     * poll and the read. FIONBIO makes the read return EWOULDBLOCK instead,
     * the deadline below does its job, and a lost reply costs one "Request
     * timed out" line rather than the command.
     *
     * An earlier note here blamed WaitSelect() and bsd_raw_receive() for
     * disagreeing about the queue, on the strength of a serial trace that
     * stopped at `recvfrom`. The real fault was a mis-resolved 32-bit
     * PC-relative branch out of tool_delay_ticks(), docs/RESEARCH.md 25, and
     * the trace stopped because the machine stopped. Adding FIONBIO changed
     * nothing. The raw receive path is not to be changed on that old note.
     */
    {
        LONG nonblock = 1;

        if (tool_sock_ioctl(sb, sock, TOOL_FIONBIO, &nonblock) != 0)
        {
            tool_error("this stack will not set non-blocking mode: %s",
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
     * src/bsdsocket/raw.c tees every inbound ICMP datagram to every open raw
     * socket, so two pings at once would credit each other's answers. The task
     * pointer is unique on the machine. Its low two bits are always zero, so
     * they are shifted off.
     */
    ident = (UWORD)((((ULONG)FindTask(NULL)) >> 2) & 0xffffUL);

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
        ULONG wait = PING_REPLY_WAIT;
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
         * TIMEOUT bounds the run, not the reply, so it is checked here and
         * again against the wait below: a five-second reply wait must not
         * overshoot a two-second limit.
         */
        if (timeout != 0)
        {
            ULONG elapsed = (ami_millis() - started) / 1000UL;

            if (elapsed >= timeout)
            {
                expired = TRUE;
                break;
            }

            if (timeout - elapsed < wait)
                wait = timeout - elapsed;
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
        deadline = t0 + wait * 1000UL;

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
                                   (LONG)sizeof(ping_reply), &from);
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

        if (tool_delay_ticks(interval * (ULONG)TICKS_PER_SECOND))
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
