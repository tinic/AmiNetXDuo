/*
 * NetTrace, capture the stack's own traffic to a pcap file while running a
 * workload underneath it.
 *
 *     NetTrace LOOPBACK/S,WIRE/S,HOST/K,PORT/N/K,PATH/K,BYTES/N/K,OUT/K,
 *              SNAP/N/K,BLEN/N/K,NOCAPTURE/S,IFACE/K
 *
 *   LOOPBACK and WIRE are alternatives, and HOST implies WIRE.
 *
 *   Capture and workload share one process so the throughput number and the
 *   trace come out of the same run.  Draining the capture between socket
 *   operations also bounds the buffer: the channel holds 2 x BLEN, and a
 *   megabyte at 1460 bytes a segment is well over a thousand records.
 *
 *   Nothing from src/.  Every call is a published bsdsocket.library LVO,
 *   including the bpf_* ones; they are in toolbpf.c, with the pcap writer and
 *   the record walk, and NetCapture shares them.  Before this existed the
 *   capture path in src/bpf/ had 201 unit-test checks, no caller anywhere in
 *   the product, and all eight vectors pointing at bsd_enosys().
 *
 *   The bpf ABI has no BIOCSSNAPLEN: a filter program returns the number of
 *   bytes to keep, so `BPF_RET|BPF_K, n` accepts every packet and truncates
 *   it to n.  96 bytes covers Ethernet + IP + TCP with 20 bytes of options,
 *   and was tcpdump's own default for twenty years.  This command installs
 *   that one instruction and nothing else; NetCapture is the one with a
 *   filter, because it is the one recording somebody else's traffic and
 *   therefore the one that has to pick it out.
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

/*
 * Everything the capture side of this command needs -- the eight published
 * bpf_* LVOs, the ioctls, the record walk and the pcap writer -- is in
 * toolbpf.c, which NetCapture shares.  It used to be four hundred lines here,
 * and the first thing a second capture command would have had to do was copy
 * them.
 */

/* ---------------------------------------------------------- the workloads */

#define NT_CHUNK    4096

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
 *
 * Single-threaded and therefore entirely non-blocking: listener, client and
 * accepted socket all go through one WaitSelect().  The capture is drained on
 * every pass, so the channel never holds more than a few hundred microseconds
 * of traffic.
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
 * One HTTP/1.0 GET over the wire, read to completion.
 *
 * HTTP/1.0 with no keep-alive, so the body ends when the peer closes: the
 * trace covers the shutdown as well, and there is no chunk parser here.  The
 * bytes are counted, not kept. tests/curl checks payloads byte for byte.
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
     * PATH comes off the command line and `req` is on the caller's stack,
     * which a Shell command gets four kilobytes of. Copying it unbounded let
     * any PATH longer than 462 characters overwrite this frame's return
     * address, silently, there is no MMU here. The trailer is appended only
     * if it still fits, so a truncated request is refused by the server rather
     * than sent as something else.
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

    if (tool_sock_send(base, s, req, (LONG)len) != (LONG)len)
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
