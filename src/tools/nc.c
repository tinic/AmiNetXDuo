/*
 * nc -- netcat.  Shovel bytes between a socket and the Shell.
 *
 *     nc HOST,PORT,LISTEN=-l/S,UDP=-u/S,SCAN=-z/S,TIMEOUT=-w/N/K,
 *        LOCALPORT=-p/N/K,VERBOSE=-v/S,CRLF/S
 *
 * Three things in one command, as everywhere else:
 *
 *   nc HOST PORT      connect, and copy standard input to it and it to
 *                     standard output until one end stops.
 *   nc -l PORT        listen instead, and do the same with the first caller.
 *   nc -z HOST PORT   connect, say whether it worked, and stop.  PORT may be
 *                     a range, "20-25", which is a port scan.
 *
 * WHY IT IS THE FIRST OF THE THREE
 *
 *   Every other network command in this tree is a client: it calls socket()
 *   and connect() and nothing else.  `nc -l` is the first thing here that
 *   calls bind(), listen() and accept(), which is half of the socket ABI and
 *   was until now exercised only by the conformance probe.  A stack that gets
 *   the client half right and the server half wrong looks perfectly healthy
 *   from `fetch`.
 *
 * WHAT IT IS NOT
 *
 *   Not a proxy, not -e (running a program on the far end of a socket is a
 *   remote shell, and nobody should ship one by accident), and it takes one
 *   caller in listen mode rather than staying up for the next.  UDP is here
 *   because it costs two `if`s: connect() on a datagram socket is only a
 *   remembered destination, which is exactly what this needs.
 *
 *   End of input does not close the connection unless -N says so, which is
 *   OpenBSD nc's rule -- see the comment on -N in nc_shovel().
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

const char *const tool_name = "nc";

static const char version_tag[] __attribute__((used)) =
    "$VER: nc 1.0 (25.7.2026)";

#define TEMPLATE                                                        \
    "HOST,PORT,LISTEN=-l/S,UDP=-u/S,SCAN=-z/S,TIMEOUT=-w/N/K,"          \
    "LOCALPORT=-p/N/K,HALFCLOSE=-N/S,VERBOSE=-v/S,CRLF/S"

enum
{
    ARG_HOST = 0,
    ARG_PORT,
    ARG_LISTEN,
    ARG_UDP,
    ARG_SCAN,
    ARG_TIMEOUT,
    ARG_LOCALPORT,
    ARG_HALFCLOSE,
    ARG_VERBOSE,
    ARG_CRLF,
    ARG_COUNT
};

/*
 * Static, not automatic: a Shell command gets whatever stack the Shell has --
 * 4 KB on a stock Kickstart 3.1 -- and these are 12 KB between them.  Same
 * reasoning as src/tools/fetch.c.
 */
#define NC_CHUNK        4096

static UBYTE nc_from_net[NC_CHUNK];
static UBYTE nc_from_user[NC_CHUNK];
static UBYTE nc_staged[NC_CHUNK * 2];   /* CRLF expansion can double it */

/* How often the socket is polled when there is a keyboard to watch too. */
#define NC_TICK_MICROS  50000UL


/* ------------------------------------------------------------- options --- */

typedef struct NcOptions
{
    BOOL    listen;
    BOOL    udp;
    BOOL    scan;
    BOOL    halfclose;
    BOOL    verbose;
    BOOL    crlf;
    ULONG   timeout;                /* seconds; 0 means "no limit"          */
    UWORD   localport;
} NcOptions;


/* ------------------------------------------------------------- scanning --- */

/* "80" -> 80..80, "20-25" -> 20..25.  FALSE after printing why not. */
static BOOL parse_range(const char *text, UWORD *lo, UWORD *hi)
{
    ULONG a = 0;
    ULONG b;
    ULONG i = 0;

    if (text == NULL || text[0] < '0' || text[0] > '9')
    {
        tool_error("a port range has to be numbers, not \"%s\"", (LONG)text);
        return FALSE;
    }

    while (text[i] >= '0' && text[i] <= '9')
        a = (a * 10UL) + (ULONG)(text[i++] - '0');

    if (text[i] == '\0')
    {
        b = a;
    }
    else if (text[i] == '-' && text[i + 1] >= '0' && text[i + 1] <= '9')
    {
        b = 0;
        i++;
        while (text[i] >= '0' && text[i] <= '9')
            b = (b * 10UL) + (ULONG)(text[i++] - '0');

        if (text[i] != '\0')
        {
            tool_error("\"%s\" is not a port range", (LONG)text);
            return FALSE;
        }
    }
    else
    {
        tool_error("\"%s\" is not a port range", (LONG)text);
        return FALSE;
    }

    if (a == 0 || b == 0 || a > 65535UL || b > 65535UL || a > b)
    {
        tool_error("\"%s\" is not a port range this machine can reach",
                   (LONG)text);
        return FALSE;
    }

    *lo = (UWORD)a;
    *hi = (UWORD)b;
    return TRUE;
}


/* ------------------------------------------------------------ connecting -- */

/*
 * connect(), with a ceiling on how long it may take.
 *
 * The plain call blocks for as long as the stack's own retransmit schedule
 * says, which on an unreachable address is the better part of a minute.  With
 * TIMEOUT the socket goes non-blocking (FIONBIO), the connect returns
 * EINPROGRESS, and WaitSelect() watches the WRITE set -- which is how BSD has
 * always spelled "tell me when the handshake finishes".  SO_ERROR then says
 * whether it finished or failed.
 *
 * 0 on success, -1 on failure, -2 on timeout.  Nothing is printed: a port
 * scan calls this once per port and would drown in it, so the reason comes
 * back in *why and the caller decides whether it is worth saying.
 */
#define NC_TIMED_OUT    (-2)

static LONG nc_connect(struct Library *sb, LONG sock, const ToolSockAddr *sa,
                       ULONG timeout, LONG *why)
{
    ToolFdSet   writefds;
    ToolTimeval tv;
    LONG        nonblock = 1;
    LONG        err = 0;
    LONG        errlen = (LONG)sizeof(err);
    LONG        ready;

    *why = 0;

    if (timeout == 0)
    {
        if (tool_sock_connect(sb, sock, sa) == 0)
            return 0;

        *why = tool_sock_errno(sb);
        return -1;
    }

    if (tool_sock_ioctl(sb, sock, TOOL_FIONBIO, &nonblock) != 0)
    {
        /* No non-blocking mode: the blocking call is still correct, it just
           cannot be cut short.  Say nothing and do the right thing. */
        if (tool_sock_connect(sb, sock, sa) == 0)
            return 0;

        *why = tool_sock_errno(sb);
        return -1;
    }

    if (tool_sock_connect(sb, sock, sa) != 0)
    {
        LONG e = tool_sock_errno(sb);

        if (e != TOOL_EINPROGRESS && e != TOOL_EWOULDBLOCK)
        {
            *why = e;
            return -1;
        }

        tool_fd_zero(&writefds);
        tool_fd_add(&writefds, sock);

        tv.tv_secs  = (LONG)timeout;
        tv.tv_micro = 0;

        ready = tool_sock_select(sb, sock + 1, NULL, &writefds, &tv);
        if (ready == 0)
            return NC_TIMED_OUT;
        if (ready < 0)
        {
            *why = tool_sock_errno(sb);
            return -1;
        }

        if (tool_sock_getsockopt(sb, sock, TOOL_SOL_SOCKET, TOOL_SO_ERROR,
                                 &err, &errlen) != 0)
        {
            /*
             * No SO_ERROR to read.  A second connect() on a socket that is
             * already through reports EISCONN and one that failed reports the
             * real reason, so ask that way instead.
             */
            err = (tool_sock_connect(sb, sock, sa) == 0)
                      ? 0 : tool_sock_errno(sb);
            if (err == 56 /* EISCONN */)
                err = 0;
        }

        if (err != 0)
        {
            *why = err;
            return -1;
        }
    }

    nonblock = 0;
    (VOID)tool_sock_ioctl(sb, sock, TOOL_FIONBIO, &nonblock);

    return 0;
}


/* --------------------------------------------------------------- shovel --- */

/*
 * The transfer itself: everything standard input produces goes to the socket,
 * everything the socket produces goes to standard output, until one of them
 * stops.
 *
 * The two halves are polled rather than waited on together, because they are
 * different kinds of thing: WaitSelect() understands sockets and knows nothing
 * about a DOS handle, and there is no Amiga call that waits on both.  A 50 ms
 * poll is imperceptible at a keyboard and costs nothing on a bulk transfer,
 * where the socket is ready every time round.
 *
 * When standard input ends the write half is shut down rather than the whole
 * socket, so `nc host 80 <request.txt` still reads the answer.
 */
static LONG nc_shovel(struct Library *sb, LONG sock, const NcOptions *opt)
{
    ToolInput   in;
    ToolFdSet   readfds;
    ToolTimeval tv;
    ULONG       idle = 0;           /* ticks of 50 ms with nothing arriving */
    ULONG       idle_limit;
    BOOL        wrote_eof = FALSE;
    LONG        rc = RETURN_OK;

    idle_limit = (opt->timeout > 0) ? (opt->timeout * 1000UL / 50UL) : 0;

    tool_input_open(&in, TRUE);

    for (;;)
    {
        LONG n;
        LONG ready;

        if (tool_break())
        {
            rc = RETURN_WARN;
            tool_fault(ERROR_BREAK);
            break;
        }

        /* ---- the user's side ------------------------------------------- */

        if (!in.eof)
        {
            n = tool_input_read(&in, nc_from_user, (LONG)sizeof(nc_from_user),
                                0UL);

            if (n == 0 && opt->halfclose)
            {
                /*
                 * -N: end of input sends a FIN, so a far end that is
                 * waiting for the end of the request can answer it.
                 *
                 * Off by default, which is OpenBSD nc's rule and has been
                 * since 2015: a connection that goes on carrying the answer
                 * is the more useful default, and the caller is the one who
                 * knows whether the protocol needs the FIN.  HTTP/1.0 and
                 * every "send a file, then read the reply" case do.
                 *
                 * This is also the switch that found the half-close defect in
                 * bsdsocket.library -- a same-machine shutdown(SHUT_WR) used
                 * to wedge the caller where not even Ctrl-C reached it.
                 * Fixed in the library, not here; docs/RESEARCH.md has the
                 * bisect.
                 */
                if (!opt->udp && !wrote_eof)
                {
                    (VOID)tool_sock_shutdown(sb, sock, TOOL_SHUT_WR);
                    wrote_eof = TRUE;

                    if (opt->verbose)
                    {
                        tool_printf("end of input; the write half is "
                                    "closed\n");
                        (VOID)Flush(Output());
                    }
                }
            }
            else if (n > 0)
            {
                const UBYTE *out = nc_from_user;
                LONG         len = n;

                if (opt->crlf)
                {
                    LONG i;
                    LONG o = 0;

                    for (i = 0; i < n; i++)
                    {
                        if (nc_from_user[i] == '\n')
                            nc_staged[o++] = '\r';
                        nc_staged[o++] = nc_from_user[i];
                    }
                    out = nc_staged;
                    len = o;
                }

                /* Both modes reach here on a connected socket -- a datagram
                   socket included, where connect() is only a remembered
                   destination -- so one send() covers all four cases. */
                n = tool_sock_send(sb, sock, out, len);

                if (n != len)
                {
                    tool_error("cannot send: %s",
                               (LONG)tool_sock_errstr(tool_sock_errno(sb)));
                    rc = RETURN_ERROR;
                    break;
                }

                idle = 0;
                continue;           /* drain the input before waiting again */
            }
        }

        /* ---- the network's side ---------------------------------------- */

        tool_fd_zero(&readfds);
        tool_fd_add(&readfds, sock);

        tv.tv_secs  = 0;
        tv.tv_micro = (LONG)NC_TICK_MICROS;

        ready = tool_sock_select(sb, sock + 1, &readfds, NULL, &tv);

        if (ready < 0)
        {
            LONG err = tool_sock_errno(sb);

            if (err == TOOL_EINTR)
                continue;

            tool_error("the connection failed: %s",
                       (LONG)tool_sock_errstr(err));
            rc = RETURN_ERROR;
            break;
        }

        n = (ready > 0)
                ? tool_sock_recv(sb, sock, nc_from_net,
                                 (LONG)sizeof(nc_from_net))
                : -1;

        if (ready > 0 && n == 0)
            break;                  /* the other end hung up: normal */

        if (ready > 0 && n < 0)
        {
            LONG err = tool_sock_errno(sb);

            if (err != TOOL_EINTR && err != TOOL_EWOULDBLOCK)
            {
                tool_error("the connection failed: %s",
                           (LONG)tool_sock_errstr(err));
                rc = RETURN_ERROR;
                break;
            }

            /*
             * Ready, and then nothing there.  It happens -- a select() that
             * wakes for a state change rather than for data is within its
             * rights -- and the naive answer, `continue`, is a spin at full
             * CPU that no timeout can end because it never waits.  So this
             * counts as an idle tick like any other and is paced like one.
             */
            ready = 0;
            (VOID)tool_delay_ticks(2);      /* ~40 ms, near the poll period */
        }

        if (ready == 0)
        {
            idle++;
            if (idle_limit != 0 && idle >= idle_limit)
            {
                if (opt->verbose)
                    tool_error("nothing for %lu seconds; stopping",
                               opt->timeout);
                rc = RETURN_WARN;
                break;
            }
            continue;
        }

        idle = 0;

        if (tool_output_write(nc_from_net, n) != n)
        {
            tool_fault(IoErr());
            rc = RETURN_FAIL;
            break;
        }
    }

    tool_input_close(&in);

    return rc;
}


/* --------------------------------------------------------------- listen --- */

/*
 * bind(), listen(), accept() -- the half of the ABI a client never reaches.
 *
 * SO_REUSEADDR goes on first, and not as a formality: a listener that has just
 * exited leaves the port in TIME-WAIT, and without it the next `nc -l` on the
 * same port fails with "address already in use" for two minutes.  That is the
 * single most common way a hand-run listener looks broken.
 */
static LONG nc_listen(struct Library *sb, const NcOptions *opt, ULONG bindaddr,
                      UWORD port, LONG *accepted)
{
    ToolSockAddr sa;
    ToolSockAddr from;
    LONG         lsock;
    LONG         one = 1;
    ULONG        waited;
    char         dotted[16];

    lsock = tool_sock_socket(sb, TOOL_AF_INET,
                             opt->udp ? TOOL_SOCK_DGRAM : TOOL_SOCK_STREAM, 0);
    if (lsock < 0)
    {
        tool_error("no socket: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        return -1;
    }

    (VOID)tool_sock_setsockopt(sb, lsock, TOOL_SOL_SOCKET, TOOL_SO_REUSEADDR,
                               &one, (LONG)sizeof(one));

    tool_sock_addr(&sa, bindaddr, port);

    if (tool_sock_bind(sb, lsock, &sa) != 0)
    {
        LONG err = tool_sock_errno(sb);

        tool_error("cannot listen on port %ld: %s", (LONG)port,
                   (LONG)tool_sock_errstr(err));

        if (err == TOOL_EADDRINUSE)
        {
            tool_advise_blank();
            tool_advise("Something else has that port, or the last program to "
                        "use it has not finished closing it down.  netstat -a "
                        "says which.");
        }

        (VOID)tool_sock_close(sb, lsock);
        return -1;
    }

    if (opt->udp)
    {
        /*
         * A datagram listener has nothing to accept.  It waits for the first
         * packet, and whoever sent it becomes the peer -- which is what every
         * netcat does and what makes `nc -u -l` usable at all.
         */
        LONG n;

        if (opt->verbose)
            tool_printf("listening on UDP port %ld\n", (LONG)port);

        n = tool_sock_recvfrom(sb, lsock, nc_from_net,
                               (LONG)sizeof(nc_from_net), &from);
        if (n < 0)
        {
            tool_error("nothing arrived: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            (VOID)tool_sock_close(sb, lsock);
            return -1;
        }

        if (opt->verbose)
        {
            ami_config_format_ip(from.sin_addr, dotted, sizeof(dotted));
            tool_printf("datagram from %s port %ld\n",
                        (LONG)dotted, (LONG)from.sin_port);
        }

        if (n > 0 && tool_output_write(nc_from_net, n) != n)
        {
            tool_fault(IoErr());
            (VOID)tool_sock_close(sb, lsock);
            return -1;
        }

        /* Answer whoever spoke first. */
        if (tool_sock_connect(sb, lsock, &from) != 0)
        {
            tool_error("cannot answer that sender: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            (VOID)tool_sock_close(sb, lsock);
            return -1;
        }

        *accepted = lsock;
        return 0;
    }

    if (tool_sock_listen(sb, lsock, 1) != 0)
    {
        tool_error("cannot listen on port %ld: %s", (LONG)port,
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        (VOID)tool_sock_close(sb, lsock);
        return -1;
    }

    if (opt->verbose)
        tool_printf("listening on port %ld\n", (LONG)port);

    /*
     * accept() blocks, and a blocked accept() cannot see Ctrl-C.  So it is
     * armed through WaitSelect() first -- a listening socket becomes readable
     * exactly when there is a connection to take -- which gives the break a
     * place to be noticed and TIMEOUT somewhere to apply.
     */
    waited = 0;
    for (;;)
    {
        ToolFdSet   readfds;
        ToolTimeval tv;
        LONG        ready;

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            (VOID)tool_sock_close(sb, lsock);
            return -1;
        }

        tool_fd_zero(&readfds);
        tool_fd_add(&readfds, lsock);

        tv.tv_secs  = 0;
        tv.tv_micro = 200000;

        ready = tool_sock_select(sb, lsock + 1, &readfds, NULL, &tv);

        if (ready < 0)
        {
            if (tool_sock_errno(sb) == TOOL_EINTR)
                continue;

            tool_error("cannot wait for a connection: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            (VOID)tool_sock_close(sb, lsock);
            return -1;
        }

        if (ready > 0)
            break;

        waited++;
        if (opt->timeout != 0 && waited * 200UL >= opt->timeout * 1000UL)
        {
            tool_error("nobody connected within %lu seconds", opt->timeout);
            (VOID)tool_sock_close(sb, lsock);
            return -1;
        }
    }

    *accepted = tool_sock_accept(sb, lsock, &from);
    if (*accepted < 0)
    {
        tool_error("cannot accept a connection: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        (VOID)tool_sock_close(sb, lsock);
        return -1;
    }

    if (opt->verbose)
    {
        ami_config_format_ip(from.sin_addr, dotted, sizeof(dotted));
        tool_printf("connection from %s port %ld\n",
                    (LONG)dotted, (LONG)from.sin_port);
    }

    /* One caller, then done: the listening socket has no further use, and
       leaving it open would keep the port busy after this command exits. */
    (VOID)tool_sock_close(sb, lsock);

    return 0;
}


/* ----------------------------------------------------------------- scan --- */

static LONG nc_scan(struct Library *sb, const NcOptions *opt, ULONG address,
                    UWORD lo, UWORD hi)
{
    ULONG open_ports = 0;
    ULONG tried = 0;
    ULONG port;
    LONG  why = 0;
    char  dotted[16];

    ami_config_format_ip(address, dotted, sizeof(dotted));

    for (port = lo; port <= (ULONG)hi; port++)
    {
        ToolSockAddr sa;
        LONG         sock;
        LONG         result;

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            return RETURN_WARN;
        }

        tried++;

        sock = tool_sock_socket(sb, TOOL_AF_INET,
                                opt->udp ? TOOL_SOCK_DGRAM : TOOL_SOCK_STREAM,
                                0);
        if (sock < 0)
        {
            tool_error("no socket: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            return RETURN_FAIL;
        }

        tool_sock_addr(&sa, address, (UWORD)port);

        result = nc_connect(sb, sock, &sa, opt->timeout, &why);

        if (result == 0)
        {
            open_ports++;
            tool_printf("%s port %lu open\n", (LONG)dotted, port);
        }
        else if (opt->verbose)
        {
            if (result == NC_TIMED_OUT)
                tool_printf("%s port %lu no answer\n", (LONG)dotted, port);
            else
                tool_printf("%s port %lu %s\n", (LONG)dotted, port,
                            (LONG)tool_sock_errstr(why));
        }

        (VOID)tool_sock_close(sb, sock);
    }

    if (open_ports == 0)
    {
        tool_error("nothing is listening on %s", (LONG)dotted);
        return RETURN_ERROR;
    }

    return (open_ports == tried) ? RETURN_OK : RETURN_WARN;
}


/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    NcOptions       opt;
    const char     *host;
    const char     *portspec;
    ToolSockAddr    sa;
    ULONG           address = 0;
    UWORD           port = 0;
    LONG            sock = -1;
    LONG            why = 0;
    LONG            rc = RETURN_OK;
    ULONG           i;

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
        tool_usage("<host> <port>  |  -l <port>  |  -z <host> <port>[-<port>]",
                   "Copies standard input to a socket and the socket to "
                   "standard output.");
        return RETURN_ERROR;
    }

    opt.listen    = (args[ARG_LISTEN]  != 0) ? TRUE : FALSE;
    opt.udp       = (args[ARG_UDP]     != 0) ? TRUE : FALSE;
    opt.scan      = (args[ARG_SCAN]    != 0) ? TRUE : FALSE;
    opt.halfclose = (args[ARG_HALFCLOSE] != 0) ? TRUE : FALSE;
    opt.verbose   = (args[ARG_VERBOSE] != 0) ? TRUE : FALSE;
    opt.crlf      = (args[ARG_CRLF]    != 0) ? TRUE : FALSE;
    opt.timeout   = (args[ARG_TIMEOUT] != 0)
                        ? (ULONG)(*(LONG *)args[ARG_TIMEOUT]) : 0UL;
    opt.localport = (args[ARG_LOCALPORT] != 0)
                        ? (UWORD)(*(LONG *)args[ARG_LOCALPORT]) : 0;

    host     = (const char *)args[ARG_HOST];
    portspec = (const char *)args[ARG_PORT];

    /*
     * "nc -l 1234" is what everybody types, and it leaves the port sitting in
     * the HOST position.  Nothing else can be meant, so take it.
     */
    if (opt.listen && portspec == NULL)
    {
        portspec = host;
        host     = NULL;
    }

    if (opt.listen && portspec == NULL && opt.localport != 0)
    {
        port = opt.localport;
    }
    else if (portspec == NULL)
    {
        tool_error("which port?");
        tool_usage("<host> <port>  |  -l <port>  |  -z <host> <port>[-<port>]",
                   "Copies standard input to a socket and the socket to "
                   "standard output.");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!opt.listen && host == NULL)
    {
        tool_error("which host?");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (opt.listen && opt.scan)
    {
        tool_error("-l and -z do opposite things; pick one");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* ---- the port ------------------------------------------------------- */

    if (portspec != NULL && !opt.scan)
    {
        port = tool_sock_port(sb, portspec, opt.udp ? "udp" : "tcp");
        if (port == 0)
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    /* ---- and away ------------------------------------------------------- */

    if (opt.scan)
    {
        UWORD lo;
        UWORD hi;

        if (!tool_sock_resolve(sb, host, &address) ||
            !parse_range(portspec, &lo, &hi))
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        rc = nc_scan(sb, &opt, address, lo, hi);
        CloseLibrary(sb);
        FreeArgs(rda);
        return (int)rc;
    }

    if (opt.listen)
    {
        /* A host in listen mode is the local address to bind to, which is how
           a machine with two interfaces says "only this one". */
        if (host != NULL && !tool_sock_resolve(sb, host, &address))
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (nc_listen(sb, &opt, address, port, &sock) != 0)
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        rc = nc_shovel(sb, sock, &opt);
    }
    else
    {
        LONG result;

        if (!tool_sock_resolve(sb, host, &address))
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        sock = tool_sock_socket(sb, TOOL_AF_INET,
                                opt.udp ? TOOL_SOCK_DGRAM : TOOL_SOCK_STREAM,
                                0);
        if (sock < 0)
        {
            tool_error("no socket: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(sb)));
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        /* -p pins the local port, which is what a firewall on the far side
           may be written against. */
        if (opt.localport != 0)
        {
            ToolSockAddr local;
            LONG         one = 1;

            (VOID)tool_sock_setsockopt(sb, sock, TOOL_SOL_SOCKET,
                                       TOOL_SO_REUSEADDR, &one,
                                       (LONG)sizeof(one));
            tool_sock_addr(&local, 0, opt.localport);

            if (tool_sock_bind(sb, sock, &local) != 0)
            {
                tool_error("cannot use local port %ld: %s",
                           (LONG)opt.localport,
                           (LONG)tool_sock_errstr(tool_sock_errno(sb)));
                (VOID)tool_sock_close(sb, sock);
                CloseLibrary(sb);
                FreeArgs(rda);
                return RETURN_ERROR;
            }
        }

        tool_sock_addr(&sa, address, port);

        result = nc_connect(sb, sock, &sa, opt.timeout, &why);
        if (result != 0)
        {
            char dotted[16];

            ami_config_format_ip(address, dotted, sizeof(dotted));

            if (result == NC_TIMED_OUT)
                tool_error("%s port %ld did not answer within %lu seconds",
                           (LONG)dotted, (LONG)port, opt.timeout);
            else
                tool_error("cannot connect to %s port %ld: %s", (LONG)dotted,
                           (LONG)port, (LONG)tool_sock_errstr(why));

            (VOID)tool_sock_close(sb, sock);
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (opt.verbose)
        {
            char dotted[16];

            ami_config_format_ip(address, dotted, sizeof(dotted));
            tool_printf("connected to %s port %ld\n", (LONG)dotted, (LONG)port);
        }

        rc = nc_shovel(sb, sock, &opt);
    }

    if (sock >= 0)
        (VOID)tool_sock_close(sb, sock);

    CloseLibrary(sb);
    FreeArgs(rda);

    return (int)rc;
}
