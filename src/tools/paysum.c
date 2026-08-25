/*
 * paysum. Move a deterministic byte pattern and say what actually arrived.
 *
 * Both ends hash CONTENT, not byte counts: the pattern is position-dependent,
 * so any shift, duplication, skip or cross-connection leak changes the bytes.
 * The peer is tests/tools/paypeer.py. Output is key=value, one line per
 * connection plus a verdict line.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "aminetxduo/version.h"

const char *const tool_name = "paysum";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("paysum");

#define TEMPLATE                                                        \
    "HOST/A,PORT/N/A,LEN/N/K,SEED/N/K,SEND/S,CONNS/N/K,"                \
    "TIMEOUT=-w/N/K,IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_HOST = 0,
    ARG_PORT,
    ARG_LEN,
    ARG_SEED,
    ARG_SEND,
    ARG_CONNS,
    ARG_TIMEOUT,
    ARG_IPV4,
    ARG_IPV6,
    ARG_COUNT
};

#define PAY_MAX_CONNS   4L
#define PAY_CHUNK       4096L

/* Static, not automatic: a Shell command gets whatever stack the Shell has,
   4 KB on a stock Kickstart 3.1. */
static UBYTE pay_data[PAY_CHUNK];
static UBYTE pay_want[PAY_CHUNK];
static ULONG pay_crc_table[256];

/* ------------------------------------------------------------ the pattern --- */

/*
 * Word j of the stream is mix32(seed ^ (j * 0x9E3779B9)), bytes big-endian --
 * the same function paypeer.py computes. A CONSTANT fill would certify a
 * placement bug instead of catching it.
 */
static ULONG pay_word(ULONG seed, ULONG j)
{
    ULONG x = seed ^ (j * 0x9E3779B9UL);

    x ^= x >> 16;
    x *= 0x7FEB352DUL;
    x ^= x >> 15;
    x *= 0x846CA68BUL;
    x ^= x >> 16;
    return x;
}

/* `off` and `len` are byte positions in the stream, any alignment. */
static VOID pay_fill(UBYTE *out, ULONG seed, ULONG off, ULONG len)
{
    ULONG i;

    for (i = 0; i < len; i++)
    {
        ULONG pos  = off + i;
        ULONG word = pay_word(seed, pos >> 2);

        out[i] = (UBYTE)(word >> (24 - (8 * (pos & 3UL))));
    }
}

/* --------------------------------------------------------------- the CRC --- */

/* IEEE 802.3 CRC-32, reflected, the one binascii.crc32 computes, so the two
   ends can be compared without either translating. */
static VOID pay_crc_init(VOID)
{
    ULONG n;

    for (n = 0; n < 256; n++)
    {
        ULONG c = n;
        ULONG k;

        for (k = 0; k < 8; k++)
            c = (c & 1UL) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);

        pay_crc_table[n] = c;
    }
}

static ULONG pay_crc_run(ULONG crc, const UBYTE *buf, ULONG len)
{
    ULONG i;

    crc ^= 0xFFFFFFFFUL;
    for (i = 0; i < len; i++)
        crc = pay_crc_table[(crc ^ buf[i]) & 0xFFUL] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

/* ---------------------------------------------------------- a connection --- */

typedef struct PayConn
{
    LONG    sock;
    UWORD   port;
    ULONG   seed;
    ULONG   crc;            /* over every byte so far, both directions      */
    ULONG   moved;          /* bytes received, or bytes sent                */
    LONG    first_bad;      /* -1, or the first offset off the pattern      */
    BOOL    done;
    BOOL    failed;
    ULONG   t0;             /* ami_millis() at the first byte               */
    ULONG   t1;             /*               and at the last                */
} PayConn;

static PayConn pay_conns[PAY_MAX_CONNS];

typedef struct PayOptions
{
    BOOL    send;
    LONG    family;
    LONG    conns;
    ULONG   len;            /* sent per connection; 0 on receive = to EOF   */
    ULONG   seed;
    BOOL    verify;         /* SEED given, so a receive checks the pattern  */
    ULONG   timeout;        /* seconds of connect retry and of idle         */
} PayOptions;

static VOID pay_report(const PayConn *c, const PayOptions *opt)
{
    ULONG ms = (c->moved > 0) ? (c->t1 - c->t0) : 0;

    tool_printf("paysum dir=%s conn=%ld port=%ld bytes=%lu ms=%lu "
                "crc32=%08lx first_bad=%ld seed=%lu\n",
                (LONG)(opt->send ? "tx" : "rx"),
                (LONG)(c - pay_conns), (LONG)c->port, c->moved, ms,
                c->crc, c->first_bad, c->seed);
    (VOID)Flush(Output());
}

/*
 * Connect with a retry loop rather than one attempt: the interface, and on
 * IPv6 the SLAAC address, settles seconds after the command runs. One new
 * socket per attempt; a socket whose connect() failed is not reusable.
 */
static LONG pay_connect(struct Library *sb, const ToolAddr *addr,
                        UWORD port, ULONG timeout)
{
    ULONG started = ami_millis();

    for (;;)
    {
        ToolSockAddrAny sa;
        LONG            sock;
        LONG            why = 0;

        if (tool_break())
            return -1;

        sock = tool_sock_socket(sb, (LONG)addr->ta_Family,
                                TOOL_SOCK_STREAM, 0);
        if (sock < 0)
            return -1;

        (VOID)tool_sock_addr(&sa, addr, port);

        if (tool_sock_connect_timed(sb, sock, &sa, 5UL, &why) == 0)
            return sock;

        (VOID)tool_sock_close(sb, sock);

        if ((ami_millis() - started) / 1000UL >= timeout)
        {
            tool_error("port %ld never answered within %lu seconds: %s",
                       (LONG)port, timeout, (LONG)tool_sock_errstr(why));
            return -1;
        }

        (VOID)tool_delay_ticks(25);     /* half a second between offers */
    }
}

/* One readable socket: drain a chunk, hash it, check it against the pattern
   when there is a pattern to check against. */
static VOID pay_rx_chunk(struct Library *sb, PayConn *c,
                         const PayOptions *opt)
{
    LONG n = tool_sock_recv(sb, c->sock, pay_data, PAY_CHUNK);

    if (n == 0)
    {
        c->done = TRUE;
        return;
    }

    if (n < 0)
    {
        LONG err = tool_sock_errno(sb);

        if (err == TOOL_EINTR || err == TOOL_EWOULDBLOCK)
            return;

        tool_error("conn %ld: receive failed after %lu bytes: %s",
                   (LONG)(c - pay_conns), c->moved,
                   (LONG)tool_sock_errstr(err));
        c->done   = TRUE;
        c->failed = TRUE;
        return;
    }

    if (c->moved == 0)
        c->t0 = ami_millis();
    c->t1 = ami_millis();

    c->crc = pay_crc_run(c->crc, pay_data, (ULONG)n);

    if (opt->verify && c->first_bad < 0)
    {
        ULONG i;

        pay_fill(pay_want, c->seed, c->moved, (ULONG)n);
        for (i = 0; i < (ULONG)n; i++)
        {
            if (pay_data[i] != pay_want[i])
            {
                c->first_bad = (LONG)(c->moved + i);
                break;
            }
        }
    }

    c->moved += (ULONG)n;
}

/* One writable socket: generate the next chunk of its pattern and offer it.
   send() may take less than a chunk; the cursor only advances by what it took. */
static VOID pay_tx_chunk(struct Library *sb, PayConn *c,
                         const PayOptions *opt)
{
    ULONG left = opt->len - c->moved;
    ULONG ask  = (left > (ULONG)PAY_CHUNK) ? (ULONG)PAY_CHUNK : left;
    LONG  n;

    pay_fill(pay_data, c->seed, c->moved, ask);

    n = tool_sock_send(sb, c->sock, pay_data, (LONG)ask);

    if (n < 0)
    {
        LONG err = tool_sock_errno(sb);

        if (err == TOOL_EINTR || err == TOOL_EWOULDBLOCK)
            return;

        tool_error("conn %ld: send failed after %lu bytes: %s",
                   (LONG)(c - pay_conns), c->moved,
                   (LONG)tool_sock_errstr(err));
        c->done   = TRUE;
        c->failed = TRUE;
        return;
    }

    if (c->moved == 0)
        c->t0 = ami_millis();
    c->t1 = ami_millis();

    c->crc    = pay_crc_run(c->crc, pay_data, (ULONG)n);
    c->moved += (ULONG)n;

    /* Everything is sent. Half-close and stay in the loop: the peer's own close
       is its receipt, so the report follows delivery rather than send(). */
    if (c->moved == opt->len)
        (VOID)tool_sock_shutdown(sb, c->sock, TOOL_SHUT_WR);
}

/*
 * All the connections under one select(), so with CONNS > 1 the streams
 * genuinely interleave: a receive placed into the wrong connection's buffer
 * lands in a stream whose pattern disagrees with it at every byte.
 */
static LONG pay_run(struct Library *sb, const PayOptions *opt)
{
    ULONG idle  = 0;
    ULONG fails = 0;
    LONG  i;

    for (;;)
    {
        ToolFdSet readfds;
        ToolFdSet writefds;
        ToolTimeval tv;
        LONG      nfds = 0;
        LONG      open = 0;
        LONG      ready;

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            return RETURN_WARN;
        }

        tool_fd_zero(&readfds);
        tool_fd_zero(&writefds);

        for (i = 0; i < opt->conns; i++)
        {
            PayConn *c = &pay_conns[i];

            if (c->done)
                continue;

            open++;
            if (opt->send && c->moved < opt->len)
                tool_fd_add(&writefds, c->sock);
            else
                tool_fd_add(&readfds, c->sock);

            if (c->sock >= nfds)
                nfds = c->sock + 1;
        }

        if (open == 0)
            break;

        tv.tv_secs  = 0;
        tv.tv_micro = 200000;

        ready = tool_sock_select(sb, nfds, &readfds,
                                 opt->send ? &writefds : NULL, &tv);

        if (ready < 0)
        {
            if (tool_sock_errno(sb) == TOOL_EINTR)
                continue;

            tool_error("select failed: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            return RETURN_ERROR;
        }

        if (ready == 0)
        {
            idle++;
            if (idle >= opt->timeout * 5UL)     /* 200 ms per tick */
            {
                tool_error("nothing moved for %lu seconds", opt->timeout);
                for (i = 0; i < opt->conns; i++)
                    if (!pay_conns[i].done)
                        pay_conns[i].failed = TRUE;
                break;
            }
            continue;
        }

        idle = 0;

        for (i = 0; i < opt->conns; i++)
        {
            PayConn *c = &pay_conns[i];

            if (c->done)
                continue;

            if (tool_fd_isset(&readfds, c->sock))
                pay_rx_chunk(sb, c, opt);
            else if (opt->send && tool_fd_isset(&writefds, c->sock))
                pay_tx_chunk(sb, c, opt);
        }
    }

    for (i = 0; i < opt->conns; i++)
    {
        PayConn *c = &pay_conns[i];

        pay_report(c, opt);

        if (c->failed)
            fails++;
        else if (opt->verify && !opt->send && c->first_bad >= 0)
            fails++;
        else if (opt->send && c->moved != opt->len)
            fails++;
    }

    tool_printf("paysum done conns=%ld fails=%lu\n", opt->conns, fails);
    (VOID)Flush(Output());

    return (fails == 0) ? RETURN_OK : RETURN_ERROR;
}

/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    PayOptions      opt;
    ToolAddr        address;
    const char     *host;
    LONG            port;
    LONG            rc;
    LONG            i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();
    pay_crc_init();

    for (i = 0; i < ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[-4|-6] <host> <port> [LEN n] [SEED s] [SEND] [CONNS k]",
                   "Moves a seeded byte pattern and prints the CRC of what "
                   "actually crossed, so the two ends can be compared.");
        return RETURN_ERROR;
    }

    opt.send = (args[ARG_SEND] != 0) ? TRUE : FALSE;

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6], &opt.family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    host = (const char *)args[ARG_HOST];
    port = *(LONG *)args[ARG_PORT];

    if (port < 1 || port > 65535)
    {
        tool_error("%ld is not a port", port);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    opt.len = 0;
    if (args[ARG_LEN] != 0)
    {
        LONG len = *(LONG *)args[ARG_LEN];

        if (len < 0)
        {
            tool_error("%ld is not a length", len);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        opt.len = (ULONG)len;
    }

    if (opt.send && opt.len == 0)
    {
        tool_error("SEND needs LEN: a sender must know how much to make");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    opt.seed   = 0;
    opt.verify = FALSE;
    if (args[ARG_SEED] != 0)
    {
        opt.seed   = (ULONG)*(LONG *)args[ARG_SEED];
        opt.verify = TRUE;
    }

    if (opt.send && !opt.verify)
    {
        tool_error("SEND needs SEED: the pattern is the point");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    opt.conns = 1;
    if (args[ARG_CONNS] != 0)
    {
        opt.conns = *(LONG *)args[ARG_CONNS];

        if (opt.conns < 1 || opt.conns > PAY_MAX_CONNS)
        {
            tool_error("CONNS must be between 1 and %ld", PAY_MAX_CONNS);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    opt.timeout = 30;
    if (args[ARG_TIMEOUT] != 0)
    {
        LONG t = *(LONG *)args[ARG_TIMEOUT];

        if (t < 1 || t > 3600)
        {
            tool_error("a timeout must be between 1 and 3600 seconds");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        opt.timeout = (ULONG)t;
    }

    if (port + opt.conns - 1 > 65535)
    {
        tool_error("CONNS %ld runs past the last port", opt.conns);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (opt.family == TOOL_AF_INET6 && !tool_sock_have_ipv6(sb))
    {
        tool_sock_say_no_family(host, opt.family);
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!tool_sock_resolve_af(sb, host, opt.family, &address))
    {
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    for (i = 0; i < PAY_MAX_CONNS; i++)
        pay_conns[i].sock = -1;

    /* Connection i takes port+i and seed+i, which is how one command line names
       several distinguishable streams. */
    rc = RETURN_OK;
    for (i = 0; i < opt.conns; i++)
    {
        PayConn *c = &pay_conns[i];

        c->port      = (UWORD)(port + i);
        c->seed      = opt.seed + (ULONG)i;
        c->crc       = 0;
        c->moved     = 0;
        c->first_bad = -1;
        c->done      = FALSE;
        c->failed    = FALSE;

        c->sock = pay_connect(sb, &address, c->port, opt.timeout);
        if (c->sock < 0)
        {
            rc = RETURN_ERROR;
            break;
        }
    }

    if (rc == RETURN_OK)
        rc = pay_run(sb, &opt);

    for (i = 0; i < opt.conns; i++)
        if (pay_conns[i].sock >= 0)
            (VOID)tool_sock_close(sb, pay_conns[i].sock);

    CloseLibrary(sb);
    FreeArgs(rda);

    return (int)rc;
}
