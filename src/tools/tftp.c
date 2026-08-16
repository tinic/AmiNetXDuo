/*
 * tftp, the trivial file transfer protocol, client half.
 *
 *     tftp HOST/A,GET/K,PUT/K,AS/K,PORT/N/K,TIMEOUT/N/K,QUIET/S,
 *          IPV4=-4/S,IPV6=-6/S
 *
 *   GET      fetch this file from the server.
 *   PUT      send this file to the server.
 *   AS       call it something else at the other end.
 *   PORT     the server's port. Default 69.
 *   TIMEOUT  seconds to wait for a block before asking again. Default 5.
 *   -4 / -6  pin the family the name resolves to. Without either, the library
 *            answers AF_UNSPEC and the selection rules pick, so on a dual
 *            stack there is otherwise no way to ask for the other one.
 *
 * One transfer per command: no interactive prompt and no `mget`, so it can be
 * driven from a script.
 *
 *   Octet mode only. RFC 1350's other mode, netascii, rewrites line endings in
 *   transit, which corrupts any binary moved with it. Every TFTP server
 *   accepts octet.
 *
 *   NetX Duo's TFTP add-on wants FileX, a filesystem this machine does not
 *   have and does not need, having AmigaDOS (docs/RESEARCH.md 5.4). Its client
 *   would build, but the protocol is a couple of hundred lines over the socket
 *   API, and written that way the command runs on Roadshow and AmiTCP too.
 *
 * Transfer identifiers: the request goes to port 69, but the answer comes from
 * a different port the server picked, and every later packet of the transfer
 * belongs to that port alone. The first reply fixes the peer; anything
 * arriving afterwards from elsewhere is sent an ERROR and otherwise ignored
 * (RFC 1350 section 4). That is how one server runs two transfers at once.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "aminetxduo/version.h"

const char *const tool_name = "tftp";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("tftp");

#define TEMPLATE    "HOST/A,GET/K,PUT/K,AS/K,PORT/N/K,TIMEOUT/N/K,QUIET/S," \
                    "IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_HOST = 0,
    ARG_GET,
    ARG_PUT,
    ARG_AS,
    ARG_PORT,
    ARG_TIMEOUT,
    ARG_QUIET,
    ARG_IPV4,
    ARG_IPV6,
    ARG_COUNT
};

#define TFTP_PORT               69
#define TFTP_BLOCK              512
#define TFTP_DEFAULT_TIMEOUT    5UL     /* seconds per attempt              */
#define TFTP_RETRIES            5       /* attempts before giving up        */

/* Opcodes. */
#define TFTP_RRQ        1
#define TFTP_WRQ        2
#define TFTP_DATA       3
#define TFTP_ACK        4
#define TFTP_ERROR      5

/* What tftp_wait() returns instead of a length. */
#define TFTP_BROKEN     (-1)
#define TFTP_BREAK      (-2)

/*
 * Static rather than automatic: a Shell command gets the Shell's stack, 4 KB
 * on a stock Kickstart 3.1, and these are a kilobyte and a half between them.
 * Same as nc and fetch.
 */
static UBYTE tftp_out[TFTP_BLOCK + 4];
static UBYTE tftp_in[TFTP_BLOCK + 4 + 64];

/* ---------------------------------------------------------------- helpers, */

static VOID tftp_put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)(v & 0xff);
}

static UWORD tftp_get16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

static ULONG tftp_strlen(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/*
 * "DH0:roms/kick.rom" -> "kick.rom". Both separators, since one end of the
 * transfer names files the AmigaDOS way and the other the Unix way.
 */
static const char *tftp_basename(const char *path)
{
    const char *last = path;
    ULONG       i;

    for (i = 0; path[i] != '\0'; i++)
    {
        if (path[i] == '/' || path[i] == ':')
            last = &path[i + 1];
    }

    return (last[0] != '\0') ? last : path;
}

/*
 * The server's own error, as a sentence. The server's text is printed where
 * there is any, since it is the only thing that says which file or why, but
 * the code is named as well: plenty of servers send an empty string.
 */
static const char *tftp_error_name(UWORD code)
{
    switch (code)
    {
        case 1:  return "there is no such file on the server";
        case 2:  return "the server will not allow that";
        case 3:  return "the server has no room for it";
        case 4:  return "the server did not understand the request";
        case 5:  return "the server does not know that transfer";
        case 6:  return "there is already a file of that name there";
        case 7:  return "the server has no such user";
        default: return "the server refused the transfer";
    }
}

static VOID tftp_report_error(const UBYTE *buf, LONG len)
{
    UWORD       code = (len >= 4) ? tftp_get16(&buf[2]) : 0;
    const char *text = NULL;
    LONG        i;

    for (i = 4; i < len; i++)
    {
        if (buf[i] == '\0')
        {
            text = (const char *)&buf[4];
            break;
        }
    }

    if (text != NULL && text[0] != '\0')
        tool_error("%s (%s)", (LONG)tftp_error_name(code), (LONG)text);
    else
        tool_error("%s", (LONG)tftp_error_name(code));
}

/* RFC 1350's answer to a packet from the wrong port. */
static VOID tftp_stray(struct Library *sb, LONG sock,
                       const ToolSockAddrAny *who)
{
    static const char msg[] = "unknown transfer ID";
    UBYTE packet[4 + sizeof(msg)];
    ULONG i;

    tftp_put16(&packet[0], TFTP_ERROR);
    tftp_put16(&packet[2], 5);
    for (i = 0; i < (ULONG)sizeof(msg); i++)
        packet[4 + i] = (UBYTE)msg[i];

    (VOID)tool_sock_sendto(sb, sock, packet, (LONG)sizeof(packet), who);
}

/*
 * A request packet, opcode, filename, 0, "octet", 0, built into tftp_out.
 * Returns its length, or 0 when the name will not fit.
 */
static ULONG tftp_build_request(UWORD opcode, const char *name)
{
    static const char mode[] = "octet";
    ULONG n = tftp_strlen(name);
    ULONG o = 2;
    ULONG i;

    if (n == 0 || n + sizeof(mode) + 3 > sizeof(tftp_out))
        return 0;

    tftp_put16(&tftp_out[0], opcode);

    for (i = 0; i < n; i++)
        tftp_out[o++] = (UBYTE)name[i];
    tftp_out[o++] = 0;

    for (i = 0; i + 1 < (ULONG)sizeof(mode); i++)
        tftp_out[o++] = (UBYTE)mode[i];
    tftp_out[o++] = 0;

    return o;
}

/*
 * Wait for one packet, at most `secs` seconds. Returns bytes received, 0 on
 * timeout, TFTP_BROKEN on a failed socket, TFTP_BREAK on Ctrl-C. The wait is
 * cut into 200 ms slices so a break is noticed while waiting on a dead server.
 */
static LONG tftp_wait(struct Library *sb, LONG sock, ULONG secs,
                      ToolSockAddrAny *from)
{
    ULONG slices = (secs * 1000UL) / 200UL;
    ULONG i;

    if (slices == 0)
        slices = 1;

    for (i = 0; i < slices; i++)
    {
        ToolFdSet   readfds;
        ToolTimeval tv;
        LONG        ready;

        if (tool_break())
            return TFTP_BREAK;

        tool_fd_zero(&readfds);
        tool_fd_add(&readfds, sock);

        tv.tv_secs  = 0;
        tv.tv_micro = 200000;

        ready = tool_sock_select(sb, sock + 1, &readfds, NULL, &tv);
        if (ready < 0)
        {
            if (tool_sock_errno(sb) == TOOL_EINTR)
                continue;
            return TFTP_BROKEN;
        }
        if (ready == 0)
            continue;

        return tool_sock_recvfrom(sb, sock, tftp_in, (LONG)sizeof(tftp_in),
                                  from);
    }

    return 0;
}

/*
 * Everything both directions share. `last` is whatever was last sent, the
 * request, an ACK or a DATA block, since TFTP's entire reliability mechanism
 * is sending that again.
 */
typedef struct TftpXfer
{
    struct Library *sb;
    LONG            sock;
    ToolSockAddrAny peer;               /* server:69 until the first reply  */
    BOOL            have_peer;
    ULONG           timeout;
    UBYTE           last[TFTP_BLOCK + 4];
    LONG            last_len;
    ULONG           total;              /* payload bytes moved              */
    ULONG           blocks;
    ULONG           dots;               /* progress marks printed so far    */
    BOOL            quiet;
} TftpXfer;

static VOID tftp_remember(TftpXfer *x, LONG len)
{
    LONG i;

    if (len > (LONG)sizeof(x->last))
        len = (LONG)sizeof(x->last);

    for (i = 0; i < len; i++)
        x->last[i] = tftp_out[i];

    x->last_len = len;
}

static BOOL tftp_send_last(TftpXfer *x)
{
    return (tool_sock_sendto(x->sb, x->sock, x->last, x->last_len, &x->peer)
                == x->last_len) ? TRUE : FALSE;
}

/*
 * One dot per 32 KB, so a big transfer over a slow link looks alive. Nothing
 * at all for a small one.
 */
static VOID tftp_tick(TftpXfer *x)
{
    x->blocks++;

    if (!x->quiet && (x->blocks % 64UL) == 0)
    {
        tool_printf(".");
        (VOID)Flush(Output());
        x->dots++;
    }
}

/*
 * Wait for the reply this side is expecting, re-sending on each timeout.
 * `want_op` is TFTP_DATA or TFTP_ACK and `want_block` the block number.
 * Returns the length of the accepted packet in tftp_in, one of the negative
 * codes, or 0 when the server stopped answering.
 */
static LONG tftp_await(TftpXfer *x, UWORD want_op, UWORD want_block)
{
    ULONG tries = 0;

    for (;;)
    {
        ToolSockAddrAny from;
        LONG            n = tftp_wait(x->sb, x->sock, x->timeout, &from);
        UWORD           op;

        if (n == TFTP_BREAK || n == TFTP_BROKEN)
            return n;

        if (n == 0)
        {
            if (++tries >= TFTP_RETRIES)
                return 0;

            if (!tftp_send_last(x))
                return TFTP_BROKEN;

            continue;
        }

        /* Somebody else's transfer, or a stray from an old one. */
        if (x->have_peer && !tool_sock_addr_same(&from, &x->peer))
        {
            tftp_stray(x->sb, x->sock, &from);
            continue;
        }

        if (n < 4)
            continue;

        op = tftp_get16(&tftp_in[0]);

        if (op == TFTP_ERROR)
        {
            tftp_report_error(tftp_in, n);
            return TFTP_BROKEN;
        }

        if (op != want_op)
            continue;

        /* The first packet of the transfer names the port the rest uses. */
        if (!x->have_peer)
        {
            x->peer      = from;
            x->have_peer = TRUE;
        }

        if (tftp_get16(&tftp_in[2]) == want_block)
            return n;

        /*
         * A block already dealt with, whose acknowledgement was lost. On a GET
         * it is acknowledged again without advancing; advancing is the
         * "sorcerer's apprentice" bug, which doubles every packet on the wire
         * for the rest of the transfer. On a PUT an old ACK is ignored.
         */
        if (want_op == TFTP_DATA)
        {
            tftp_put16(&tftp_out[0], TFTP_ACK);
            tftp_put16(&tftp_out[2], tftp_get16(&tftp_in[2]));
            (VOID)tool_sock_sendto(x->sb, x->sock, tftp_out, 4L, &x->peer);
        }
    }
}

/* ------------------------------------------------------------------- get --- */

static LONG tftp_get(TftpXfer *x, const char *remote, const char *local)
{
    BPTR  out;
    ULONG len;
    UWORD block = 1;

    len = tftp_build_request(TFTP_RRQ, remote);
    if (len == 0)
    {
        tool_error("\"%s\" is too long a name for TFTP", (LONG)remote);
        return RETURN_ERROR;
    }

    out = Open((CONST_STRPTR)local, MODE_NEWFILE);
    if (out == 0)
    {
        tool_error("cannot write \"%s\"", (LONG)local);
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    tftp_remember(x, (LONG)len);
    if (!tftp_send_last(x))
    {
        tool_error("cannot send the request: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(x->sb)));
        Close(out);
        DeleteFile((CONST_STRPTR)local);
        return RETURN_ERROR;
    }

    for (;;)
    {
        LONG n = tftp_await(x, TFTP_DATA, block);
        LONG payload;

        if (n == TFTP_BREAK)
        {
            Close(out);
            DeleteFile((CONST_STRPTR)local);
            return TFTP_BREAK;
        }
        if (n == TFTP_BROKEN)
        {
            Close(out);
            DeleteFile((CONST_STRPTR)local);
            return RETURN_ERROR;
        }
        if (n == 0)
        {
            tool_error("the server stopped answering after %lu bytes",
                       x->total);
            Close(out);
            return RETURN_ERROR;
        }

        payload = n - 4;

        if (payload > 0 && Write(out, (APTR)&tftp_in[4], payload) != payload)
        {
            tool_error("cannot write \"%s\"", (LONG)local);
            tool_fault(IoErr());
            Close(out);
            return RETURN_FAIL;
        }

        x->total += (ULONG)payload;

        /* Acknowledge it, and keep the acknowledgement to re-send. */
        tftp_put16(&tftp_out[0], TFTP_ACK);
        tftp_put16(&tftp_out[2], block);
        tftp_remember(x, 4L);
        (VOID)tftp_send_last(x);

        tftp_tick(x);

        /* Short block: that was the last of the file. */
        if (payload < TFTP_BLOCK)
            break;

        block++;                        /* UWORD, so 65535 rolls to 0 */
    }

    if (Close(out) == 0)
    {
        tool_error("cannot finish writing \"%s\"", (LONG)local);
        tool_fault(IoErr());
        return RETURN_FAIL;
    }

    return RETURN_OK;
}

/* ------------------------------------------------------------------- put --- */

static LONG tftp_put(TftpXfer *x, const char *local, const char *remote)
{
    BPTR  in;
    ULONG len;
    UWORD block = 0;
    BOOL  final = FALSE;

    in = Open((CONST_STRPTR)local, MODE_OLDFILE);
    if (in == 0)
    {
        tool_error("cannot read \"%s\"", (LONG)local);
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    len = tftp_build_request(TFTP_WRQ, remote);
    if (len == 0)
    {
        tool_error("\"%s\" is too long a name for TFTP", (LONG)remote);
        Close(in);
        return RETURN_ERROR;
    }

    tftp_remember(x, (LONG)len);
    if (!tftp_send_last(x))
    {
        tool_error("cannot send the request: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(x->sb)));
        Close(in);
        return RETURN_ERROR;
    }

    for (;;)
    {
        LONG n = tftp_await(x, TFTP_ACK, block);
        LONG got;

        if (n == TFTP_BREAK)
        {
            Close(in);
            return TFTP_BREAK;
        }
        if (n == TFTP_BROKEN)
        {
            Close(in);
            return RETURN_ERROR;
        }
        if (n == 0)
        {
            tool_error("the server stopped answering after %lu bytes",
                       x->total);
            Close(in);
            return RETURN_ERROR;
        }

        if (final)
            break;                      /* the last block is acknowledged */

        got = Read(in, (APTR)&tftp_out[4], (LONG)TFTP_BLOCK);
        if (got < 0)
        {
            tool_error("cannot read \"%s\"", (LONG)local);
            tool_fault(IoErr());
            Close(in);
            return RETURN_FAIL;
        }

        block++;
        tftp_put16(&tftp_out[0], TFTP_DATA);
        tftp_put16(&tftp_out[2], block);
        tftp_remember(x, got + 4);

        if (!tftp_send_last(x))
        {
            tool_error("cannot send: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(x->sb)));
            Close(in);
            return RETURN_ERROR;
        }

        x->total += (ULONG)got;
        tftp_tick(x);

        /*
         * A file that is an exact multiple of 512 bytes ends with an empty
         * DATA block, so this is a flag rather than a `break`: that block
         * still has to be sent and acknowledged.
         */
        final = (got < TFTP_BLOCK) ? TRUE : FALSE;
    }

    Close(in);

    return RETURN_OK;
}

/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    TftpXfer        x;
    const char     *host;
    const char     *get;
    const char     *put;
    const char     *as;
    const char     *local;
    const char     *remote;
    ToolAddr        address;
    ULONG           port;
    ULONG           t0;
    ULONG           elapsed;
    LONG            sock = -1;
    LONG            family;
    LONG            rc;
    ULONG           i;
    char            dotted[TOOL_ADDR_STRLEN];

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
        tool_usage("[-4|-6] <host> GET <file>  |  <host> PUT <file>",
                   "Fetches a file from a TFTP server, or sends one to it.");
        return RETURN_ERROR;
    }

    host = (const char *)args[ARG_HOST];
    get  = (const char *)args[ARG_GET];
    put  = (const char *)args[ARG_PUT];
    as   = (const char *)args[ARG_AS];

    if ((get == NULL) == (put == NULL))
    {
        tool_error((get == NULL) ? "GET or PUT must name a file"
                                 : "GET and PUT are opposite directions. "
                                   "Use one of them");
        tool_usage("<host> GET <file>  |  <host> PUT <file>",
                   "Fetches a file from a TFTP server, or sends one to it.");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    port = (args[ARG_PORT] != 0)
               ? (ULONG)(*(LONG *)args[ARG_PORT]) : (ULONG)TFTP_PORT;
    if (port == 0 || port > 65535UL)
    {
        tool_error("port %lu is not a port anything listens on", port);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    x.timeout = (args[ARG_TIMEOUT] != 0)
                    ? (ULONG)(*(LONG *)args[ARG_TIMEOUT])
                    : TFTP_DEFAULT_TIMEOUT;
    if (x.timeout == 0)
        x.timeout = TFTP_DEFAULT_TIMEOUT;

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6], &family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    x.quiet     = (args[ARG_QUIET] != 0) ? TRUE : FALSE;
    x.have_peer = FALSE;
    x.total     = 0;
    x.blocks    = 0;
    x.dots      = 0;
    x.last_len  = 0;

    /*
     * The two names. Only one is ever given, so the other is that one without
     * its directory: `tftp box GET pub/kick.rom` writes kick.rom into the
     * current drawer. AS overrides it.
     */
    if (get != NULL)
    {
        remote = get;
        local  = (as != NULL) ? as : tftp_basename(get);
    }
    else
    {
        local  = put;
        remote = (as != NULL) ? as : tftp_basename(put);
    }

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!tool_sock_resolve_af(sb, host, family, &address))
    {
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    sock = tool_sock_socket(sb, (LONG)address.ta_Family, TOOL_SOCK_DGRAM, 0);
    if (sock < 0)
    {
        tool_error("no socket: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        CloseLibrary(sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    x.sb   = sb;
    x.sock = sock;
    (VOID)tool_sock_addr(&x.peer, &address, (UWORD)port);

    tool_addr_text(sb, &address, dotted, sizeof(dotted));

    if (!x.quiet)
    {
        tool_printf("%s %s %s %s\n",
                    (LONG)((get != NULL) ? "getting" : "sending"),
                    (LONG)((get != NULL) ? remote : local),
                    (LONG)((get != NULL) ? "from" : "to"), (LONG)dotted);
        (VOID)Flush(Output());
    }

    t0 = ami_millis();

    rc = (get != NULL) ? tftp_get(&x, remote, local)
                       : tftp_put(&x, local, remote);

    elapsed = ami_millis() - t0;

    if (x.dots > 0)
        tool_printf("\n");

    if (rc == TFTP_BREAK)
    {
        (VOID)tool_sock_close(sb, sock);
        CloseLibrary(sb);
        FreeArgs(rda);
        tool_fault(ERROR_BREAK);
        return RETURN_WARN;
    }

    if (rc == RETURN_OK && !x.quiet)
    {
        if (elapsed >= 1000UL)
        {
            tool_printf("%lu bytes in %lu.%lu seconds (%lu bytes/s)\n",
                        x.total, elapsed / 1000UL, (elapsed / 100UL) % 10UL,
                        x.total / (elapsed / 1000UL));
        }
        else
        {
            tool_printf("%lu bytes\n", x.total);
        }
    }

    (VOID)tool_sock_close(sb, sock);
    CloseLibrary(sb);
    FreeArgs(rda);

    return (int)rc;
}
