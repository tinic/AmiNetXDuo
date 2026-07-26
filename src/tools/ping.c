/*
 * ping -- ICMP echo, over a raw socket.
 *
 *     ping -c=COUNT/K/N,-i=INTERVAL/K/N,-l=LOAD/K/N,-n=NUMERICONLY=NUMERIC/S,
 *          -o=ONEREPLY/S,-q=QUIET/S,-s=SIZE/K/N,-t=TIMEOUT/K/N,BELL/S,HOST/A
 *
 * Every option carries both spellings, short flag and keyword, because that is
 * the interface this command is expected to have; netstat does the same for
 * its switches.
 *
 *   COUNT     echo requests to send; 0 means "until Ctrl-C". Default 4.
 *   INTERVAL  seconds to wait between requests. Default 1.
 *   LOAD      send this many requests back to back before the interval
 *             starts being honoured.
 *   NUMERIC   do not try to put a name to an address that was given as a
 *             number. That lookup is the local host table only -- see the
 *             note where it happens for why it is not a DNS query.
 *   ONEREPLY  stop as soon as one reply has come back.
 *   QUIET     only the summary.
 *   SIZE      payload bytes. Default 56, which is what everyone else uses.
 *   TIMEOUT   give up after this many seconds no matter how the run is
 *             going. 0, the default, means there is no such limit.
 *   BELL      ring the console bell for each reply.
 *
 * TIMEOUT is a limit on the whole run rather than on one reply: the wait for
 * an individual reply is fixed at PING_REPLY_WAIT below and is not exposed,
 * which is what makes "stop after N seconds" mean the same thing here as it
 * does everywhere else the command is documented.
 *
 * WHY THIS IS A RAW SOCKET AND NOT nx_icmp_ping()
 *
 *   It used to call nx_icmp_ping() on netstack_ip(). That could not work in
 *   anything shipped and did not: no command links aminetxduo_netstack, so
 *   netstack_ip() was src/tools/netstack_weak.c's weak stub and returned NULL,
 *   and this command printed "the network is up, but this command cannot read
 *   it" and exited 5. docs/RESEARCH.md 22.
 *
 *   Reaching the real one needed a decision, and "give me the running NX_IP"
 *   was the wrong half of it -- a pointer into another task's stack, on a
 *   machine with no memory protection. "Do a ping for me" was a published LVO
 *   on bsdsocket.library, and was designed and then rejected, because
 *   nx_icmp_ping() matches an inbound echo reply on the SEQUENCE NUMBER ALONE:
 *   nx_icmpv4_process_echo_reply.c:124 compares tx_thread_suspend_info against
 *   nx_icmpv4_echo_sequence_num and looks at nothing else, and nx_icmpv4.h:191
 *   says outright that the identifier "is not used as a host". Under FS-UAE's
 *   SLIRP -- which zeroes the sequence on a proxied reply and preserves the
 *   identifier (docs/RESEARCH.md 20.2) -- that matches the one field the NAT
 *   destroys and ignores the one it keeps, so every probe after the first of
 *   the NX_IP's lifetime would time out while its reply sat in the stack.
 *
 *   SOCK_RAW is already published ABI, already carries IP_TTL and IP_TOS to
 *   the wire (measured, docs/RESEARCH.md 20.1), and lets THIS command decide
 *   what counts as an answer. traceroute reaches the wire the same way, and so
 *   this command now runs on Roadshow and AmiTCP as well, links no NetX Duo,
 *   and needs no ThreadX adoption at all.
 *
 * Ctrl-C stops the loop and still prints the summary -- an Amiga command that
 * cannot be interrupted is broken, and one that throws away its results when
 * interrupted is only slightly less so. The break is noticed every 200 ms,
 * including while waiting for a reply, which the ThreadX version could not
 * manage: it suspended inside a kernel where an Exec signal means nothing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "aminetxduo/compat.h"

#include <proto/exec.h>
#include <devices/timer.h>

const char *const tool_name = "ping";

static const char version_tag[] __attribute__((used)) =
    "$VER: ping 3.0 (26.7.2026)";

#define TEMPLATE    "-c=COUNT/K/N,-i=INTERVAL/K/N,-l=LOAD/K/N," \
                    "-n=NUMERICONLY=NUMERIC/S,-o=ONEREPLY/S,-q=QUIET/S," \
                    "-s=SIZE/K/N,-t=TIMEOUT/K/N,BELL/S,HOST/A"

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
    ARG_ARGCOUNT
};

#define PING_DEFAULT_COUNT      4UL
#define PING_DEFAULT_SIZE       56UL
#define PING_MAX_SIZE           1400UL
#define PING_DEFAULT_INTERVAL   1UL         /* seconds                        */
#define PING_REPLY_WAIT         5UL         /* seconds to wait for one reply  */

/* ICMP, the two types this command reads. */
#define ICMP_ECHOREPLY          0
#define ICMP_ECHO               8

/* Static rather than automatic: a Shell command gets the Shell's stack, 4 KB
   on a stock 3.1. */
static UBYTE ping_probe[PING_MAX_SIZE + 8];
static UBYTE ping_reply[2048];

/* Delay() counts DOS ticks; dos/dos.h has TICKS_PER_SECOND (50). */

static LONG arg_or(const LONG *args, int index, LONG fallback)
{
    const LONG *p = (const LONG *)args[index];

    return (p != NULL) ? *p : fallback;
}

/* --------------------------------------------------------------- packets -- */

static VOID ping_put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)(v & 0xff);
}

static UWORD ping_get16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

/* The 16-bit one's-complement sum every IP protocol uses. Nothing below this
   command computes the ICMP checksum: the raw send path prepends the IP header
   and nothing else. */
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

static ULONG ping_build(UWORD ident, UWORD seq, ULONG payload)
{
    ULONG total = 8UL + payload;
    ULONG i;

    ping_probe[0] = ICMP_ECHO;
    ping_probe[1] = 0;
    ping_put16(&ping_probe[2], 0);          /* checksum, filled in below */
    ping_put16(&ping_probe[4], ident);
    ping_put16(&ping_probe[6], seq);

    /* The usual incrementing pattern, so corruption is visible. */
    for (i = 0; i < payload; i++)
        ping_probe[8 + i] = (UBYTE)(i & 0xff);

    ping_put16(&ping_probe[2], ping_checksum(ping_probe, total));

    return total;
}

/*
 * Is this datagram the reply to the probe we are waiting on?
 *
 * A raw ICMP socket sees EVERY inbound ICMP datagram -- src/bsdsocket/raw.c
 * filters on the IP protocol number, not on the type -- so this has to be
 * strict about what it accepts, and recvfrom() hands back the IP header too.
 *
 * The sequence rule is the one piece that is not textbook. `seq == 0` is
 * accepted alongside the expected number because FS-UAE's SLIRP zeroes the
 * sequence on a proxied reply while preserving the identifier
 * (docs/RESEARCH.md 20.2), and a ping that rejected those would report 100%
 * loss against every address outside the emulated LAN while the replies were
 * arriving. It is safe HERE and is not safe in traceroute, and the difference
 * is real: this command has exactly one probe outstanding at a time, so there
 * is no other probe a reply could belong to. traceroute has three per hop,
 * which is why it prints stars instead.
 */
static BOOL ping_is_reply(const UBYTE *buf, ULONG len, UWORD ident, UWORD seq,
                          ULONG from, ULONG target, ULONG *payload_out)
{
    ULONG hlen;

    if (len < 20)
        return FALSE;

    if ((buf[0] >> 4) != 4)
        return FALSE;

    hlen = (ULONG)(buf[0] & 0x0f) * 4UL;
    if (hlen < 20 || len < hlen + 8)
        return FALSE;

    if (buf[hlen] != ICMP_ECHOREPLY)
        return FALSE;

    if (ping_get16(&buf[hlen + 4]) != ident)
        return FALSE;

    {
        UWORD got = ping_get16(&buf[hlen + 6]);

        if (got != seq && got != 0)
            return FALSE;
    }

    /* A forged reply from somewhere else is not an answer from the host that
       was asked about. SLIRP's proxied replies do carry the destination's
       address, which is exactly why this check is worth making. */
    if (from != target)
        return FALSE;

    if (payload_out != NULL)
        *payload_out = len - hlen - 8UL;

    return TRUE;
}

/* ----------------------------------------------------------- the clock --- */

/*
 * Whole milliseconds is the right resolution for this command -- it is what
 * every ping has printed -- and ami_millis() already counts them through the
 * timer.device it opens on first use. Opening it BEFORE the first send, so
 * that the first round trip is not measured with a device open inside it.
 */

int main(int argc, char **argv)
{
    LONG            args[ARG_ARGCOUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    const char     *host;
    const char     *shown;
    ToolSockAddr    to;
    LONG            sock = -1;
    ULONG           target = 0;
    ULONG           count;
    ULONG           interval;
    ULONG           preload;
    ULONG           size;
    ULONG           timeout;
    UWORD           ident;
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
    char            addrtext[16];
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

    /*
     * tool_socket_open(), not tool_require_stack(): this command's whole job
     * is to use the network, and bsdsocket.library brings the stack up on its
     * first open. A status command must not do that; this one must.
     */
    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    shown       = host;
    hostname[0] = '\0';

    if (!tool_sock_resolve(sb, host, &target))
    {
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!numeric && ami_config_parse_ip(host, &target))
    {
        /*
         * The address was typed as a number, so the only name that can be put
         * to it has to be looked up backwards -- and that lookup is
         * DEVS:Internet/hosts and nothing else, which is a decision rather
         * than a shortcut.
         *
         * The other route is gethostbyaddr(), and on this stack it costs
         * BSD_RESOLVE_TIMEOUT: thirty seconds (src/bsdsocket/resolver.c:18),
         * per name server, against a server that may simply never answer a
         * PTR query -- FS-UAE's SLIRP does not answer them. Every second of
         * that is spent BEFORE the first packet leaves, which is the one
         * place a ping must not be slow, and the reward is a cosmetic change
         * to one line of output.
         *
         * The host table is instant, needs no name server at all, and is
         * where the names on a small network actually live. NUMERIC still
         * means something: it skips even that.
         */
        const AmiNetdbEntry *local;

        (VOID)ami_netdb_load();

        local = ami_netdb_host_by_addr(target);
        if (local != NULL && local->name != NULL && local->name[0] != '\0')
        {
            tool_copy_string(hostname, sizeof(hostname), local->name);
            shown = hostname;
        }
    }

    ami_config_format_ip(target, addrtext, sizeof(addrtext));

    sock = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_RAW,
                            TOOL_IPPROTO_ICMP);
    if (sock < 0)
    {
        LONG err = tool_sock_errno(sb);

        tool_error("cannot open a raw socket: %s", (LONG)tool_sock_errstr(err));

        if (err == TOOL_EPROTONOSUPPORT || err == TOOL_ESOCKTNOSUPPORT ||
            err == TOOL_EOPNOTSUPP || err == TOOL_EAFNOSUPPORT)
        {
            tool_advise_blank();
            tool_advise("An ICMP echo needs SOCK_RAW, and the TCP/IP stack on");
            tool_advise("this machine does not offer it.");
        }

        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /*
     * NON-BLOCKING, and this is not belt-and-braces -- it is the only correct
     * way to use a descriptor that has been through select().
     *
     * select() readiness is advisory in every stack there has ever been: the
     * datagram can be taken by another reader, or dropped by a checksum test,
     * between the poll and the read. A program that blocks on the strength of
     * it is relying on a guarantee no socket API offers. FIONBIO makes the
     * read return EWOULDBLOCK instead, the deadline below does its job, and a
     * stack that loses a reply costs one "Request timed out" line rather than
     * the command.
     *
     * AN EARLIER VERSION OF THIS COMMENT BLAMED THE STACK, AND WAS WRONG.
     * It read a serial trace that stopped at `recvfrom` as proof that
     * WaitSelect() and bsd_raw_receive() disagreed about the queue. They never
     * did. The command was jumping into the middle of another function on the
     * way out of tool_delay_ticks() -- a mis-resolved 32-bit PC-relative
     * branch, docs/RESEARCH.md 25 -- and the trace stopped where it did
     * because the MACHINE stopped, not because a read blocked. Adding FIONBIO
     * changed nothing, which should have been the clue. The raw receive path
     * was innocent throughout and is not to be "fixed" on the strength of that
     * old note.
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

    tool_sock_addr(&to, target, 0);

    /*
     * The identifier tells this command's replies from another raw reader's.
     * src/bsdsocket/raw.c tees every inbound ICMP datagram to every open raw
     * socket, so two pings running at once would otherwise credit each other's
     * answers. The task pointer is unique on the machine and its low two bits
     * are always zero, which is why they are shifted off.
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
         * again against the wait below -- a five-second reply wait must not be
         * able to overshoot a two-second limit.
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

        total = ping_build(ident, (UWORD)(i & 0xffffUL), size);

        t0 = ami_millis();
        n  = tool_sock_sendto(sb, sock, ping_probe, (LONG)total, &to);
        if (n != (LONG)total)
        {
            LONG err = tool_sock_errno(sb);

            tool_error("could not send the request: %s",
                       (LONG)tool_sock_errstr(err));

            if (err == TOOL_ENETUNREACH || err == TOOL_EHOSTUNREACH)
            {
                tool_advise_blank();
                tool_advise("This machine has no address on the network that");
                tool_advise("would reach that host, so the request never left.");
                tool_advise_blank();
                tool_advise("ShowNetStatus  says what the interfaces have; the");
                tool_advise("usual causes are an interface that is offline and");
                tool_advise("a DHCP server that never answered.");
            }

            rc = RETURN_ERROR;
            break;
        }

        sent++;
        deadline = t0 + wait * 1000UL;

        while (!answered)
        {
            ToolFdSet    readfds;
            ToolTimeval  tv;
            ToolSockAddr from;
            ULONG        now = ami_millis();
            ULONG        left;
            LONG         ready;

            if (tool_break())
            {
                interrupted = TRUE;
                break;
            }

            /*
             * Signed difference rather than `now >= deadline`, so the
             * comparison stays right across the millisecond counter's wrap.
             */
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

            if (ping_is_reply(ping_reply, (ULONG)n, ident,
                              (UWORD)(i & 0xffffUL), from.sin_addr, target,
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

        /*
         * No trailing pause after the last request, and none at all while the
         * preload is going out -- that is what LOAD asks for.
         */
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
