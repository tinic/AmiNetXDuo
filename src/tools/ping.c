/*
 * ping -- ICMP echo, over NetX Duo's nx_icmp_ping().
 *
 *     ping HOST/A,COUNT/N/K,SIZE/N/K,TIMEOUT/N/K,DELAY/N/K,QUIET/S
 *
 *   COUNT    echo requests to send; 0 means "until Ctrl-C". Default 4.
 *   SIZE     payload bytes. Default 56, which is what everyone else uses.
 *   TIMEOUT  seconds to wait for each reply. Default 5.
 *   DELAY    milliseconds between requests. Default 1000.
 *   QUIET    only the summary.
 *
 * Ctrl-C stops the loop and still prints the summary -- an Amiga command that
 * cannot be interrupted is broken, and one that throws away its results when
 * interrupted is only slightly less so. The break is noticed between requests
 * and every 200 ms of the pause between them, so the worst case for noticing
 * it is one TIMEOUT: nx_icmp_ping() suspends inside ThreadX, where an Exec
 * signal means nothing.
 *
 * Each request is bracketed by tool_stack_enter()/tool_stack_leave(): NetX
 * Duo suspends "the calling thread", so the Process has to be an adopted
 * TX_THREAD while it calls in -- and must NOT be one while it Delay()s
 * between requests, because an adopted Task blocked on DOS holds the ThreadX
 * baton and stalls the whole stack (docs/RESEARCH.md 6.3).
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"
#include "aminetxduo/compat.h"

#include <proto/exec.h>

const char *const tool_name = "ping";

static const char version_tag[] __attribute__((used)) =
    "$VER: ping 1.0 (24.7.2026)";

#define TEMPLATE    "HOST/A,COUNT/N/K,SIZE/N/K,TIMEOUT/N/K,DELAY/N/K,QUIET/S"

enum
{
    ARG_HOST = 0,
    ARG_COUNT,
    ARG_SIZE,
    ARG_TIMEOUT,
    ARG_DELAY,
    ARG_QUIET,
    ARG_ARGCOUNT
};

#define PING_DEFAULT_COUNT      4UL
#define PING_DEFAULT_SIZE       56UL
#define PING_MAX_SIZE           1400UL
#define PING_DEFAULT_TIMEOUT    5UL         /* seconds */
#define PING_DEFAULT_DELAY      1000UL      /* milliseconds */

/* Delay() counts DOS ticks; dos/dos.h has TICKS_PER_SECOND (50). */

static LONG arg_or(const LONG *args, int index, LONG fallback)
{
    const LONG *p = (const LONG *)args[index];

    return (p != NULL) ? *p : fallback;
}

int main(int argc, char **argv)
{
    LONG            args[ARG_ARGCOUNT];
    struct RDArgs  *rda;
    const char     *host;
    ULONG           target = 0;
    ULONG           count;
    ULONG           size;
    ULONG           timeout;
    ULONG           delay_ms;
    BOOL            quiet;
    NX_IP          *ip;
    CHAR           *payload = NULL;
    ULONG           sent = 0;
    ULONG           received = 0;
    ULONG           rtt_min = 0xffffffffUL;
    ULONG           rtt_max = 0;
    ULONG           rtt_total = 0;
    BOOL            interrupted = FALSE;
    LONG            rc = RETURN_OK;
    ULONG           i;
    char            addrtext[16];

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
    count    = (ULONG)arg_or(args, ARG_COUNT,   (LONG)PING_DEFAULT_COUNT);
    size     = (ULONG)arg_or(args, ARG_SIZE,    (LONG)PING_DEFAULT_SIZE);
    timeout  = (ULONG)arg_or(args, ARG_TIMEOUT, (LONG)PING_DEFAULT_TIMEOUT);
    delay_ms = (ULONG)arg_or(args, ARG_DELAY,   (LONG)PING_DEFAULT_DELAY);
    quiet    = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    if (size > PING_MAX_SIZE)
    {
        tool_error("SIZE must be %lu bytes or less", PING_MAX_SIZE);
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (timeout == 0)
        timeout = PING_DEFAULT_TIMEOUT;

    if (tool_require_stack() == NULL)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    ip = netstack_ip();
    if (ip == NULL)
    {
        tool_error("the stack is running but has no IP instance");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* A dotted quad is used as-is; anything else goes to the resolver. */
    if (!ami_config_parse_ip(host, &target))
    {
        LONG err = netstack_resolve(host, &target,
                                    timeout * TX_TIMER_TICKS_PER_SECOND);

        if (err != AMI_NET_OK)
        {
            tool_error("cannot resolve \"%s\": %s", (LONG)host,
                       (LONG)tool_net_error(err));
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    ami_config_format_ip(target, addrtext, sizeof(addrtext));

    if (size > 0)
    {
        payload = (CHAR *)AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);
        if (payload == NULL)
        {
            tool_error("out of memory");
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        /* The usual incrementing pattern, so corruption is visible. */
        for (i = 0; i < size; i++)
            payload[i] = (CHAR)(i & 0xff);
    }

    if (!quiet)
    {
        tool_printf("PING %s (%s): %lu data bytes\n",
                    (LONG)host, (LONG)addrtext, size);
    }

    /*
     * Force the EClock timer open before the first adoption: while adopted we
     * hold the ThreadX baton and must not block on a device.
     */
    (VOID)ami_millis();

    for (i = 0; count == 0 || i < count; i++)
    {
        NX_PACKET *response = NULL;
        ULONG      t0;
        ULONG      rtt;
        UINT       status;

        if (tool_break())
        {
            interrupted = TRUE;
            break;
        }

        if (tool_stack_enter() != 0)
        {
            rc = RETURN_FAIL;
            break;
        }

        t0     = ami_millis();
        status = nx_icmp_ping(ip, target, payload, size, &response,
                              timeout * TX_TIMER_TICKS_PER_SECOND);
        rtt    = ami_millis() - t0;

        if (response != NULL)
            nx_packet_release(response);

        tool_stack_leave();

        sent++;

        if (status == NX_SUCCESS)
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
                            size, (LONG)addrtext, i, rtt);
            }
        }
        else if (status == NX_NO_RESPONSE)
        {
            if (!quiet)
                tool_printf("Request timed out for icmp_seq=%lu\n", i);
        }
        else if (status == NX_NOT_ENABLED)
        {
            tool_error("ICMP is not enabled on this stack");
            rc = RETURN_FAIL;
            break;
        }
        else
        {
            tool_error("send failed for icmp_seq=%lu (NetX Duo status %ld)",
                       i, (LONG)status);
            rc = RETURN_ERROR;
            break;
        }

        /* No trailing pause after the last request. */
        if ((count != 0 && i + 1 >= count) || delay_ms == 0)
            continue;

        if (tool_delay_ticks((delay_ms * (ULONG)TICKS_PER_SECOND) / 1000UL))
        {
            interrupted = TRUE;
            break;
        }
    }

    tool_printf("\n--- %s ping statistics ---\n", (LONG)host);
    tool_printf("%lu packets transmitted, %lu received, %lu%% packet loss\n",
                sent, received,
                (sent > 0) ? ((sent - received) * 100UL) / sent : 0UL);

    if (received > 0)
    {
        tool_printf("round-trip min/avg/max = %lu/%lu/%lu ms\n",
                    rtt_min, rtt_total / received, rtt_max);
    }

    if (payload != NULL)
        FreeVec(payload);

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
