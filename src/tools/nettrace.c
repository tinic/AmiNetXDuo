/*
 * NetTrace, capture the stack's own traffic to a pcap file while running a
 * workload underneath it. Capture and workload share one process, and the
 * capture is drained between socket operations so the channel's 2 x BLEN
 * never fills. Every call is a published bsdsocket.library LVO.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "toolbpf.h"

#include "aminetxduo/version.h"

const char *const tool_name = "NetTrace";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("NetTrace");

#define TEMPLATE \
    "LOOPBACK/S,WIRE/S,HOST/K,PORT/N/K,PATH/K,BYTES/N/K,OUT/K,SNAP/N/K," \
    "BLEN/N/K,NOCAPTURE/S,IFACE/K"

enum
{
    ARG_LOOPBACK = 0,
    ARG_WIRE,
    ARG_HOST,
    ARG_PORT,
    ARG_PATH,
    ARG_BYTES,
    ARG_OUT,
    ARG_SNAP,
    ARG_BLEN,
    ARG_NOCAPTURE,
    ARG_IFACE,
    ARG_COUNT
};

/* ---------------------------------------------------------- the workloads */

#define NT_CHUNK    4096
#define NT_SNAP_MIN TOOL_BPF_MIN_SNAP
#define NT_SNAP_MAX TOOL_BPF_MAX_SNAP

static UBYTE nt_buf[NT_CHUNK];

/* See toolbpf.h: 16 KB of write buffer cannot live on a 4 KB stack. */
static ToolBpfChan nt_cap;

typedef struct NtResult
{
    ULONG bytes;
    ULONG ticks;
    BOOL  ok;
} NtResult;

/*
 * A bulk transfer between two sockets in this one process, over 127.0.0.1.
 * Entirely non-blocking: listener, client and accepted socket all go through
 * one WaitSelect(), and the capture is drained on every pass.
 */
static VOID nt_loopback(struct Library *base, ToolBpfChan *cap, ULONG want,
                        NtResult *res)
{
    ToolSockAddrAny sa;
    ToolFdSet    rd;
    ToolFdSet    wr;
    ToolTimeval  tv;
    LONG         lst = -1;
    LONG         cli = -1;
    LONG         srv = -1;
    LONG         one = 1;
    ULONG        sent = 0;
    ULONG        recvd = 0;
    ULONG        start;
    ULONG        i;
    BOOL         connected = FALSE;

    res->bytes = 0;
    res->ticks = 0;
    res->ok    = FALSE;

    for (i = 0; i < NT_CHUNK; i++)
        nt_buf[i] = (UBYTE)(i & 0xFF);

    lst = tool_sock_socket(base, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
    cli = tool_sock_socket(base, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
    if (lst < 0 || cli < 0)
    {
        tool_error("socket() failed: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(base)));
        goto done;
    }

    (VOID)tool_sock_setsockopt(base, lst, TOOL_SOL_SOCKET, TOOL_SO_REUSEADDR,
                               &one, (LONG)sizeof(one));

    (VOID)tool_sock_addr_v4(&sa, 0x7F000001UL, 0);
    if (tool_sock_bind(base, lst, &sa) < 0 ||
        tool_sock_listen(base, lst, 1) < 0 ||
        tool_sock_getsockname(base, lst, &sa) < 0)
    {
        tool_error("cannot create the loopback listener: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(base)));
        goto done;
    }

    (VOID)tool_sock_ioctl(base, lst, TOOL_FIONBIO, &one);
    (VOID)tool_sock_ioctl(base, cli, TOOL_FIONBIO, &one);

    if (tool_sock_connect(base, cli, &sa) < 0)
    {
        LONG err = tool_sock_errno(base);

        if (err != TOOL_EINPROGRESS && err != TOOL_EWOULDBLOCK)
        {
            tool_error("loopback connect failed: %s",
                       (LONG)tool_sock_errstr(err));
            goto done;
        }
    }

    start = ami_millis();

    while (recvd < want)
    {
        LONG n;
        LONG nfds;

        if (tool_break())
            break;

        tool_fd_zero(&rd);
        tool_fd_zero(&wr);

        if (srv < 0)
            tool_fd_add(&rd, lst);
        else
            tool_fd_add(&rd, srv);

        if (sent < want)
            tool_fd_add(&wr, cli);

        nfds = lst;
        if (cli > nfds) nfds = cli;
        if (srv > nfds) nfds = srv;

        tv.tv_secs  = 5;
        tv.tv_micro = 0;

        n = tool_sock_select(base, nfds + 1, &rd, &wr, &tv);
        if (n < 0)
        {
            tool_error("WaitSelect failed: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(base)));
            break;
        }
        if (n == 0)
        {
            tool_error("loopback transfer stalled with %lu of %lu bytes",
                       (LONG)recvd, (LONG)want);
            break;
        }

        if (srv < 0 && tool_fd_isset(&rd, lst))
        {
            srv = tool_sock_accept(base, lst, NULL);
            if (srv >= 0)
                (VOID)tool_sock_ioctl(base, srv, TOOL_FIONBIO, &one);
        }

        if (!connected && tool_fd_isset(&wr, cli))
            connected = TRUE;

        if (connected && sent < want && tool_fd_isset(&wr, cli))
        {
            ULONG left = want - sent;
            LONG  ask  = (left > NT_CHUNK) ? (LONG)NT_CHUNK : (LONG)left;

            n = tool_sock_send(base, cli, nt_buf, ask);
            if (n > 0)
                sent += (ULONG)n;
            else if (n < 0 && tool_sock_errno(base) != TOOL_EWOULDBLOCK)
            {
                tool_error("send failed: %s",
                           (LONG)tool_sock_errstr(tool_sock_errno(base)));
                break;
            }
        }

        if (srv >= 0 && tool_fd_isset(&rd, srv))
        {
            n = tool_sock_recv(base, srv, nt_buf, (LONG)NT_CHUNK);
            if (n > 0)
                recvd += (ULONG)n;
            else if (n == 0)
                break;
            else if (tool_sock_errno(base) != TOOL_EWOULDBLOCK)
                break;
        }

        (VOID)tool_bpf_drain(cap);
    }

    res->ticks = ami_millis() - start;
    res->bytes = recvd;
    res->ok    = (recvd >= want);

done:
    if (srv >= 0) (VOID)tool_sock_close(base, srv);
    if (cli >= 0) (VOID)tool_sock_close(base, cli);
    if (lst >= 0) (VOID)tool_sock_close(base, lst);

    (VOID)tool_bpf_drain(cap);
}

/*
 * One HTTP/1.0 GET over the wire, read to completion. No keep-alive, so the
 * body ends when the peer closes and there is no chunk parser here.
 */
static VOID nt_wire(struct Library *base, ToolBpfChan *cap, const ToolAddr *address,
                    UWORD port, const char *path, NtResult *res)
{
    ToolSockAddrAny sa;
    ToolFdSet    rd;
    ToolTimeval  tv;
    LONG         s;
    LONG         n;
    ULONG        start;
    ULONG        body = 0;
    ULONG        total = 0;
    BOOL         in_body = FALSE;
    ULONG        match = 0;
    char         req[512];
    ULONG        len = 0;
    const char  *p;

    res->bytes = 0;
    res->ticks = 0;
    res->ok    = FALSE;

    s = tool_sock_socket(base, (LONG)address->ta_Family, TOOL_SOCK_STREAM,
                         0);
    if (s < 0)
    {
        tool_error("socket() failed: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(base)));
        return;
    }

    (VOID)tool_sock_addr(&sa, address, port);

    start = ami_millis();

    if (tool_sock_connect(base, s, &sa) < 0)
    {
        tool_sock_fail(base, "connect to", address, port);
        (VOID)tool_sock_close(base, s);
        return;
    }

    /*
     * PATH comes off the command line and `req` is on a 4 KB Shell stack, so
     * the copy is bounded. The trailer is appended only if it still fits.
     */
    {
        static const char nt_head[]  = "GET ";
        static const char nt_tail[]  =
            " HTTP/1.0\r\nHost: amiga\r\nConnection: close\r\n\r\n";
        const ULONG       room = (ULONG)sizeof(req) - (ULONG)sizeof(nt_tail);

        for (p = nt_head; *p != '\0'; p++)
            req[len++] = *p;

        for (p = path; *p != '\0' && len < room; p++)
            req[len++] = *p;

        if (*p != '\0')
        {
            tool_error("the path is too long for a request buffer of %ld bytes",
                       (LONG)sizeof(req));
            (VOID)tool_sock_close(base, s);
            return;
        }

        for (p = nt_tail; *p != '\0'; p++)
            req[len++] = *p;
    }

    if (tool_sock_send_full(base, s, req, (LONG)len) != (LONG)len)
    {
        tool_error("cannot send the request: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(base)));
        (VOID)tool_sock_close(base, s);
        return;
    }

    for (;;)
    {
        LONG i;

        if (tool_break())
            break;

        tool_fd_zero(&rd);
        tool_fd_add(&rd, s);
        tv.tv_secs  = 20;
        tv.tv_micro = 0;

        n = tool_sock_select(base, s + 1, &rd, NULL, &tv);
        if (n <= 0)
        {
            if (n == 0)
                tool_error("the peer went quiet after %lu bytes", (LONG)total);
            break;
        }

        n = tool_sock_recv(base, s, nt_buf, (LONG)NT_CHUNK);
        if (n <= 0)
        {
            if (n == 0)
                res->ok = TRUE;         /* clean close: the body is complete */
            break;
        }

        total += (ULONG)n;

        /* Find the blank line once, then count body bytes. */
        if (!in_body)
        {
            for (i = 0; i < n; i++)
            {
                UBYTE c = nt_buf[i];

                if ((match == 0 && c == '\r') || (match == 2 && c == '\r'))
                    match++;
                else if ((match == 1 && c == '\n') || (match == 3 && c == '\n'))
                    match++;
                else
                    match = (c == '\r') ? 1 : 0;

                if (match == 4)
                {
                    in_body = TRUE;
                    body += (ULONG)(n - i - 1);
                    break;
                }
            }
        }
        else
        {
            body += (ULONG)n;
        }

        (VOID)tool_bpf_drain(cap);
    }

    res->ticks = ami_millis() - start;
    res->bytes = body;

    (VOID)tool_sock_close(base, s);
    (VOID)tool_bpf_drain(cap);
}

/* ------------------------------------------------------------------- main */

static VOID nt_report(const char *what, const NtResult *r)
{
    ULONG rate;

    if (r->ticks == 0)
    {
        tool_say("%s: %lu bytes in under a millisecond\n", (LONG)what,
               (LONG)r->bytes);
        return;
    }

    /* Bytes per second without a 64-bit divide: bytes/ms * 1000 loses too much
       on a short run, so scale by 1000 in the safe order. */
    rate = (r->bytes / r->ticks) * 1000UL +
           ((r->bytes % r->ticks) * 1000UL) / r->ticks;

    tool_say("%s: %lu bytes in %lu ms = %lu bytes/s (%lu KB/s)%s\n",
                (LONG)what, (LONG)r->bytes, (LONG)r->ticks, (LONG)rate,
                (LONG)(rate / 1024UL), (LONG)(r->ok ? "" : "  INCOMPLETE"));
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *base;
    NtResult        result;
    const char     *out;
    const char     *iface;
    const char     *path;
    ULONG           snaplen;
    ULONG           blen;
    ULONG           bytes;
    ToolAddr        address;
    UWORD           port    = 80;
    BOOL            wire;
    BOOL            capture;
    int             rc = RETURN_OK;
    UWORD           i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (UWORD)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    wire    = (args[ARG_WIRE] != 0) || (args[ARG_HOST] != 0);
    capture = (args[ARG_NOCAPTURE] == 0);

    /* LOOPBACK is the default, so it only ever has to say no to the other. */
    if (args[ARG_LOOPBACK] != 0 && wire)
    {
        tool_error("LOOPBACK and WIRE are alternatives, and HOST means WIRE");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    path  = (args[ARG_PATH] != 0) ? (const char *)args[ARG_PATH] : "/";
    out   = (args[ARG_OUT] != 0) ? (const char *)args[ARG_OUT]
                                 : (wire ? "DH0:wire.pcap" : "DH0:loop.pcap");
    iface = (args[ARG_IFACE] != 0) ? (const char *)args[ARG_IFACE]
                                   : (wire ? "eth0" : "lo0");

    snaplen = (args[ARG_SNAP] != 0) ? (ULONG)(*(LONG *)args[ARG_SNAP]) : 96UL;
    blen    = (args[ARG_BLEN] != 0) ? (ULONG)(*(LONG *)args[ARG_BLEN]) : 32768UL;
    bytes   = (args[ARG_BYTES] != 0) ? (ULONG)(*(LONG *)args[ARG_BYTES])
                                     : 1048576UL;

    if (args[ARG_BYTES] != 0 && *(LONG *)args[ARG_BYTES] < 1)
    {
        tool_error("BYTES must be at least 1");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (snaplen < NT_SNAP_MIN || snaplen > NT_SNAP_MAX)
    {
        tool_error("SNAP must be between %lu and %lu bytes",
                   (LONG)NT_SNAP_MIN, (LONG)NT_SNAP_MAX);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (blen < TOOL_BPF_MIN_BLEN || blen > TOOL_BPF_MAX_BLEN)
    {
        tool_error("BLEN must be between %lu and %lu bytes",
                   (LONG)TOOL_BPF_MIN_BLEN, (LONG)TOOL_BPF_MAX_BLEN);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (blen < snaplen + TOOL_BPF_SNAP_SLACK)
    {
        tool_error("BLEN %lu is too small for a snap length of %lu",
                   (LONG)blen, (LONG)snaplen);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_PORT] != 0)
    {
        LONG p = *(LONG *)args[ARG_PORT];

        if (p < 1 || p > 65535)
        {
            tool_error("%ld is not a port", p);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        port = (UWORD)p;
    }

    tool_say("NetTrace: opening bsdsocket.library\n");

    base = tool_socket_open();
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (wire)
    {
        const char *host = (args[ARG_HOST] != 0)
                               ? (const char *)args[ARG_HOST] : "10.0.2.2";

        if (!tool_sock_resolve(base, host, &address))
        {
            CloseLibrary(base);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    nt_cap.open = FALSE;

    /* NULL program: accept everything and keep `snaplen` bytes of it, which is
       what this command has always installed. */
    if (capture &&
        !tool_bpf_start(&nt_cap, base, iface, out, snaplen, blen, NULL, 0))
    {
        tool_bpf_stop(&nt_cap);
        CloseLibrary(base);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (capture)
        tool_say("capture: channel 0 on %s, %lu byte buffers, snaplen %lu\n",
                 (LONG)iface, (LONG)nt_cap.buflen, (LONG)snaplen);

    if (wire)
    {
        char text[TOOL_ADDR_STRLEN];

        tool_addr_text(base, &address, text, sizeof(text));
        tool_say("NetTrace: GET %s from %s port %lu, capturing %s\n",
                    (LONG)path, (LONG)text, (LONG)port,
                    (LONG)(capture ? iface : "nothing"));
        nt_wire(base, &nt_cap, &address, port, path, &result);
        nt_report("wire", &result);
    }
    else
    {
        tool_say("NetTrace: %lu bytes over 127.0.0.1, capturing %s\n",
               (LONG)bytes, (LONG)(capture ? iface : "nothing"));
        nt_loopback(base, &nt_cap, bytes, &result);
        nt_report("loopback", &result);
    }

    if (!result.ok)
        rc = RETURN_WARN;

    if (capture)
    {
        tool_bpf_stop(&nt_cap);

        tool_say("capture: %lu records written, %lu bytes, "
                 "%lu seen, %lu dropped by the channel, %lu short reads\n",
                 (LONG)nt_cap.out.records, (LONG)nt_cap.out.caplen_total,
                 (LONG)nt_cap.recv, (LONG)nt_cap.drop,
                 (LONG)nt_cap.short_reads);

        (VOID)tool_bpf_warn(&nt_cap);
    }

    CloseLibrary(base);
    FreeArgs(rda);

    return rc;
}
