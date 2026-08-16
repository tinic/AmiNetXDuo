/*
 * iperf, a throughput measurement.
 *
 *     iperf HOST,SERVER=-s/S,UDP=-u/S,PORT=-p/N/K,TIME=-t/N/K,SIZE=-n/N/K,
 *           LENGTH=-l/N/K,BANDWIDTH=-b/N/K,QUIET=-q/S,HELP=-h/S,
 *           IPV4=-4/S,IPV6=-6/S
 *
 *   iperf HOST         send to HOST for ten seconds and say how fast it went.
 *   iperf -s           wait for somebody to send here, and say the same.
 *   iperf -u HOST      the same over UDP, which also reports loss.
 *   iperf -s -u        receive UDP, and answer the sender's end-of-test
 *                      marker with the report it is waiting for.
 *
 * This speaks iperf 2, on port 5001.  The far end has to be `iperf` 2.x.
 * iperf 3 is a different protocol on a different port and the two do not
 * interoperate: pointing this at an `iperf3 -s` gets a connection that goes
 * nowhere, not a slow number.  Debian and Ubuntu package 2.x as `iperf` and
 * 3.x as `iperf3`.  Homebrew calls them `iperf` and `iperf3` too.
 *
 * It exists so that a report of this stack being slower than another one can
 * carry a figure from the reporter's own machine and card, which is where the
 * differences we cannot test live.
 * It goes through bsdsocket.library rather than linking any part of the stack,
 * so the same binary runs on Roadshow and AmiTCP and the comparison is one
 * tool against one peer.
 *
 * The measurement is in src/tools/iperfcore.c, which httpd drives too.
 *
 * SPDX-License-Identifier: MIT
 */

#include "iperfcore.h"
#include "aminetxduo/version.h"

const char *const tool_name = "iperf";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("iperf");

#define TEMPLATE                                                        \
    "HOST,SERVER=-s/S,UDP=-u/S,PORT=-p/N/K,TIME=-t/N/K,SIZE=-n/N/K,"    \
    "LENGTH=-l/N/K,BANDWIDTH=-b/N/K,QUIET=-q/S,HELP=-h/S,"              \
    "IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_HOST = 0,
    ARG_SERVER,
    ARG_UDP,
    ARG_PORT,
    ARG_TIME,
    ARG_SIZE,
    ARG_LENGTH,
    ARG_BANDWIDTH,
    ARG_QUIET,
    ARG_HELP,
    ARG_IPV4,
    ARG_IPV6,
    ARG_COUNT
};

/*
 * Static, not automatic: a Shell command gets whatever stack the Shell has,
 * 4 KB on a stock Kickstart 3.1.  Same reasoning as nc.c and fetch.c.
 */
static UBYTE iperf_payload[IPERF_BUF_MAX];
static IperfRun iperf_run;
static char iperf_line[192];

/* iperf 2's own defaults, so a run here and a run there are the same run. */
#define IPERF_TCP_DEFAULT_LEN   4096
#define IPERF_UDP_DEFAULT_RATE  1000UL      /* kbit/s */

/* ------------------------------------------------------------------- help - */

/*
 * One line per option, description at column 34.  Not built from the template:
 * the template is what ReadArgs parses and "?" already prints it, and a table
 * generated from it would name keywords rather than say what they do.
 */
static VOID iperf_help(VOID)
{
    tool_printf("Usage: %s [-s] [-u] [<host>]\n", (LONG)tool_name);
    tool_printf("  Measures throughput against an iperf 2 peer on port 5001.\n");
    tool_printf("\n");
    tool_printf("  -s                              receive, wait for a sender\n");
    tool_printf("  -u                              UDP instead of TCP\n");
    tool_printf("  -p PORT                         port to use (5001)\n");
    tool_printf("  -t SECONDS                      how long to run (10)\n");
    tool_printf("  -n KBYTES                       stop after this much instead\n");
    tool_printf("  -l BYTES                        bytes per send (4096, UDP 1470)\n");
    tool_printf("  -b KBIT                         UDP send rate (1000)\n");
    tool_printf("  -q                              print only the key=value line\n");
    tool_printf("  -4 / -6                         pin the address family\n");
    tool_printf("\n");
    tool_printf("  The far end must be iperf 2.x, not iperf3, which is a\n");
    tool_printf("  different protocol on port 5201 and will not answer.\n");
}

/* ---------------------------------------------------------------- reporting - */

static VOID iperf_print_summary(struct Library *sb, const IperfResult *res)
{
    char bytes[32];
    char rate[32];
    char addr[TOOL_ADDR_STRLEN];

    iperf_format_bytes(bytes, sizeof(bytes), res->bytes_hi, res->bytes_lo);
    iperf_format_rate(rate, sizeof(rate), res->bits);
    tool_addr_text(sb, &res->peer, addr, sizeof(addr));

    tool_printf("%s with %s, %s in %lu.%02lu sec, %s\n",
                (LONG)iperf_dir_name(res->dir), (LONG)addr, (LONG)bytes,
                (LONG)(res->ms / 1000UL), (LONG)((res->ms % 1000UL) / 10UL),
                (LONG)rate);

    if (res->dir == IPERF_UDP_TX)
    {
        if (res->got_report)
        {
            char got[32];
            char gotrate[32];
            ULONG ms = res->report.stop_sec * 1000UL
                     + res->report.stop_usec / 1000UL;

            iperf_format_bytes(got, sizeof(got), res->report.bytes_hi,
                               res->report.bytes_lo);
            iperf_format_rate(gotrate, sizeof(gotrate),
                              iperf_bits_per_sec(res->report.bytes_hi,
                                                 res->report.bytes_lo, ms));
            tool_printf("  the far end received %s, %s, %lu lost, "
                        "%lu out of order\n",
                        (LONG)got, (LONG)gotrate, (LONG)res->report.lost,
                        (LONG)res->report.outoforder);
        }
        else
        {
            tool_printf("  the far end sent no report, so this is the "
                        "send rate and not the delivered one\n");
        }
    }
    else if (res->dir == IPERF_UDP_RX)
    {
        tool_printf("  %lu datagrams, %lu lost, %lu out of order\n",
                    (LONG)res->packets, (LONG)res->lost,
                    (LONG)res->outoforder);
    }
}

/*
 * What went wrong, in words, rather than an errno.  A user who has just been
 * told their stack is slow needs to know whether they reached the peer at all.
 */
static VOID iperf_print_error(struct Library *sb, const IperfResult *res,
                              const IperfPlan *plan)
{
    char addr[TOOL_ADDR_STRLEN];

    tool_addr_text(sb, &plan->peer, addr, sizeof(addr));

    switch (res->err)
    {
    case TOOL_ECONNREFUSED:
        tool_error("nothing is listening on %s port %lu. Start `iperf -s` "
                   "there first", (LONG)addr, (LONG)plan->port);
        break;
    case TOOL_ETIMEDOUT:
        tool_error("%s port %lu did not answer", (LONG)addr,
                   (LONG)plan->port);
        break;
    case TOOL_EHOSTUNREACH:
    case TOOL_ENETUNREACH:
        tool_error("no route to %s", (LONG)addr);
        break;
    case TOOL_EADDRINUSE:
        tool_error("port %lu is already in use here", (LONG)plan->port);
        break;
    case TOOL_ECONNRESET:
        tool_error("%s closed the connection", (LONG)addr);
        break;
    default:
        tool_error("%s failed: %s", (LONG)res->stage,
                   (LONG)tool_net_error(res->err));
        break;
    }
}

/* ------------------------------------------------------------------- main - */

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    IperfPlan       plan;
    IperfResult     res;
    const char     *host;
    LONG            family;
    LONG            rc = RETURN_OK;
    LONG            state;
    BOOL            quiet;
    BOOL            server;
    BOOL            udp;
    ULONG           i;
    ULONG           next_tick;
    ULONG           last_lo = 0;

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
        iperf_help();
        return RETURN_ERROR;
    }

    if (args[ARG_HELP] != 0)
    {
        iperf_help();
        FreeArgs(rda);
        return RETURN_OK;
    }

    server = (args[ARG_SERVER] != 0) ? TRUE : FALSE;
    udp    = (args[ARG_UDP]    != 0) ? TRUE : FALSE;
    quiet  = (args[ARG_QUIET]  != 0) ? TRUE : FALSE;
    host   = (const char *)args[ARG_HOST];

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6], &family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!server && host == NULL)
    {
        tool_error("no host was given. Name one to send to, or use -s "
                   "to receive");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (server && host != NULL)
    {
        tool_error("-s waits for whoever calls, so it takes no host");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    iperf_plan_init(&plan);

    plan.dir = (UBYTE)(server ? (udp ? IPERF_UDP_RX : IPERF_TCP_RX)
                              : (udp ? IPERF_UDP_TX : IPERF_TCP_TX));

    plan.buflen = udp ? (ULONG)IPERF_UDP_DEFAULT
                      : (ULONG)IPERF_TCP_DEFAULT_LEN;

    if (udp && !server)
        plan.rate_kbit = IPERF_UDP_DEFAULT_RATE;

    if (args[ARG_PORT] != 0)
    {
        LONG p = *(LONG *)args[ARG_PORT];

        if (p < 1 || p > 65535)
        {
            tool_error("%ld is not a port", (LONG)p);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        plan.port = (UWORD)p;
    }

    if (args[ARG_TIME] != 0)
    {
        LONG t = *(LONG *)args[ARG_TIME];

        if (t < 1)
        {
            tool_error("a run of %ld seconds is not a measurement", (LONG)t);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        plan.seconds = (ULONG)t;
    }

    if (args[ARG_SIZE] != 0)
    {
        LONG n = *(LONG *)args[ARG_SIZE];

        if (n < 1)
        {
            tool_error("%ld KBytes is not a size to transfer", (LONG)n);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        plan.kbytes  = (ULONG)n;
        plan.seconds = 0;
    }

    if (args[ARG_LENGTH] != 0)
        plan.buflen = (ULONG)(*(LONG *)args[ARG_LENGTH]);

    if (args[ARG_BANDWIDTH] != 0)
    {
        LONG b = *(LONG *)args[ARG_BANDWIDTH];

        if (b < 0)
        {
            tool_error("a rate cannot be negative");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        plan.rate_kbit = (ULONG)b;
    }

    {
        const char *why = iperf_plan_check(&plan);

        if (why != NULL)
        {
            tool_error("%s", (LONG)why);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!server)
    {
        char        unbracket[TOOL_ADDR_STRLEN];
        const char *bare = tool_host_unbracket(host, unbracket,
                                               sizeof(unbracket));

        if (!tool_sock_resolve_af(sb, bare, family, &plan.peer))
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }
    else
    {
        tool_addr_v4(&plan.peer, 0);
    }

    if (!quiet)
    {
        char addr[TOOL_ADDR_STRLEN];

        if (server)
        {
            tool_printf("%s: waiting on port %lu for an iperf 2 %s sender\n",
                        (LONG)tool_name, (LONG)plan.port,
                        (LONG)(udp ? "UDP" : "TCP"));
        }
        else
        {
            tool_addr_text(sb, &plan.peer, addr, sizeof(addr));
            tool_printf("%s: %s to %s port %lu for %lu %s\n",
                        (LONG)tool_name, (LONG)(udp ? "UDP" : "TCP"),
                        (LONG)addr, (LONG)plan.port,
                        (LONG)(plan.seconds != 0 ? plan.seconds : plan.kbytes),
                        (LONG)(plan.seconds != 0 ? "seconds" : "KBytes"));
        }
    }

    if (iperf_begin(&iperf_run, sb, &plan, iperf_payload) != 0)
    {
        iperf_end(&iperf_run, &res);
        iperf_print_error(sb, &res, &plan);
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    next_tick = ami_millis() + 1000UL;

    for (;;)
    {
        state = iperf_slice(&iperf_run);

        if (state != IPERF_RUNNING)
            break;

        if (tool_break())
        {
            if (!quiet)
                tool_printf("%s: interrupted\n", (LONG)tool_name);
            iperf_abort(&iperf_run);
            rc = RETURN_WARN;
            break;
        }

        /* One line a second while it runs.  A single average hides whether the
           transfer starts slow or falls off part way. */
        if (!quiet && iperf_run.res.ms == 0)
        {
            ULONG now = ami_millis();

            if ((LONG)(now - next_tick) >= 0)
            {
                ULONG delta = iperf_run.res.bytes_lo - last_lo;
                char  rate[32];

                iperf_format_rate(rate, sizeof(rate),
                                  iperf_bits_per_sec(0, delta, 1000UL));
                tool_printf("  %s\n", (LONG)rate);

                last_lo   = iperf_run.res.bytes_lo;
                next_tick = now + 1000UL;
            }
        }
    }

    iperf_end(&iperf_run, &res);

    if (state == IPERF_FAILED)
    {
        iperf_print_error(sb, &res, &plan);
        rc = RETURN_ERROR;
    }
    else
    {
        if (!quiet)
            iperf_print_summary(sb, &res);

        iperf_result_line(&res, sb, iperf_line, sizeof(iperf_line));
        tool_printf("%s\n", (LONG)iperf_line);
    }

    CloseLibrary(sb);
    FreeArgs(rda);
    return rc;
}
