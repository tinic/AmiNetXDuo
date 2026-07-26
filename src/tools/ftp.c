/*
 * ftp -- the File Transfer Protocol, RFC 959.
 *
 *     ftp HOST,PORT,USER/K,PASSWORD/K,ACTIVE/S,DATAPORT/N/K,
 *         TIMEOUT/N/K,DEBUG=-d/S
 *
 * Commands are read from standard input, one per line, so an interactive
 * session and a scripted one are the same program:
 *
 *     ftp ftp.gnu.org USER anonymous PASSWORD me@example.com <script.txt
 *
 * THE INTERESTING PART IS NOT THE FILE TRANSFER
 *
 *   FTP is the only command in this tree that runs TWO connections at once,
 *   in different states, from one program: a control connection that stays up
 *   for the whole session and a data connection built and torn down for every
 *   listing and every file.  And in ACTIVE mode -- the original design -- it
 *   is the CLIENT that listens and the SERVER that connects back to it, which
 *   makes this the only client-shaped program that exercises bind(), listen()
 *   and accept() as part of its ordinary work.
 *
 *   That is why both modes are here rather than just the one that works
 *   through a firewall.  Passive is the default because it is what works;
 *   active is what proves the stack.
 *
 * WHAT DATAPORT IS FOR
 *
 *   In active mode the client normally lets the stack pick the data port and
 *   tells the server about it in the PORT command.  Behind a NAT that is
 *   useless, because nothing outside can reach an unforwarded port.  DATAPORT
 *   pins it to a number a forwarding rule can name, which is the only way to
 *   run active FTP from behind one -- and the only way to test active mode
 *   under FS-UAE's SLIRP, which is a NAT.
 *
 * WHAT IT DOES NOT DO
 *
 *   No TLS (that is `fetch`'s department and FTPS is a different protocol
 *   again), no MLSD, no resume, no globbing, no recursive transfer.  ASCII
 *   mode does translate line endings in both directions, because a mode that
 *   is announced and not honoured is worse than one that is absent.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

const char *const tool_name = "ftp";

static const char version_tag[] __attribute__((used)) =
    "$VER: ftp 1.0 (25.7.2026)";

#define TEMPLATE                                                        \
    "HOST,PORT,USER/K,PASSWORD/K,ACTIVE/S,DATAPORT/N/K,TIMEOUT/N/K,"    \
    "DEBUG=-d/S"

enum
{
    ARG_HOST = 0,
    ARG_PORT,
    ARG_USER,
    ARG_PASSWORD,
    ARG_ACTIVE,
    ARG_DATAPORT,
    ARG_TIMEOUT,
    ARG_DEBUG,
    ARG_COUNT
};

#define FTP_DEFAULT_TIMEOUT     60UL        /* seconds waiting for a reply  */
#define FTP_LINE_MAX            512
#define FTP_CHUNK               4096

/*
 * Static, not automatic: a Shell command gets whatever stack the Shell has --
 * 4 KB on a stock Kickstart 3.1.  Same reasoning as src/tools/fetch.c.
 */
static UBYTE ftp_ctl_buf[1024];         /* control connection read-ahead    */
static UBYTE ftp_data[FTP_CHUNK];       /* one block of a transfer          */
static UBYTE ftp_ascii[FTP_CHUNK * 2];  /* ... after LF -> CR LF            */
static char  ftp_line[FTP_LINE_MAX];    /* one reply line                   */
static char  ftp_cmd[FTP_LINE_MAX];     /* one command being built          */
static char  ftp_input[FTP_LINE_MAX];   /* one line the user typed          */
static char  ftp_raw[FTP_LINE_MAX];     /* ... before ftp_split() cut it up */
static char  ftp_reply_text[FTP_LINE_MAX];


/* --------------------------------------------------------------- strings --- */

static ULONG s_len(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

static BOOL s_add(char *dst, ULONG max, ULONG *used, const char *src)
{
    ULONG n = s_len(src);
    ULONG i;

    if (*used + n + 1 > max)
        return FALSE;

    for (i = 0; i < n; i++)
        dst[(*used)++] = src[i];

    dst[*used] = '\0';
    return TRUE;
}

static BOOL s_addnum(char *dst, ULONG max, ULONG *used, ULONG value)
{
    char  tmp[12];
    ULONG n = 0;

    if (value == 0)
        tmp[n++] = '0';

    while (value > 0)
    {
        tmp[n++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    }

    if (*used + n + 1 > max)
        return FALSE;

    while (n > 0)
        dst[(*used)++] = tmp[--n];

    dst[*used] = '\0';
    return TRUE;
}

static BOOL s_eq_ci(const char *a, const char *b)
{
    return (BOOL)(tool_stricmp(a, b) == 0);
}


/* ----------------------------------------------------------------- state --- */

typedef struct FtpState
{
    struct Library *sb;
    LONG            ctl;                /* -1 when not connected            */
    BOOL            passive;
    BOOL            binary;
    BOOL            debug;
    BOOL            interactive;
    ULONG           timeout;            /* seconds                          */
    UWORD           dataport;           /* 0 = let the stack choose         */
    ULONG           host_addr;
    UWORD           host_port;

    /* Read-ahead over the control connection: replies arrive in whatever
       lumps TCP feels like, and a line may be split across two of them. */
    LONG            buf_len;
    LONG            buf_pos;
} FtpState;


/* ----------------------------------------------------- control connection --- */

/*
 * One line off the control connection, without its CRLF.  FALSE at end of
 * connection or on error, having said which.
 *
 * The timeout is here rather than in the caller because every reply goes
 * through this and a server that stops answering mid-session must not wedge
 * the command forever.
 */
static BOOL ftp_getline(FtpState *st, char *out, ULONG max)
{
    ULONG o = 0;

    for (;;)
    {
        if (st->buf_pos >= st->buf_len)
        {
            ToolFdSet   readfds;
            ToolTimeval tv;
            LONG        ready;

            if (tool_break())
            {
                tool_fault(ERROR_BREAK);
                return FALSE;
            }

            tool_fd_zero(&readfds);
            tool_fd_add(&readfds, st->ctl);

            tv.tv_secs  = (LONG)st->timeout;
            tv.tv_micro = 0;

            ready = tool_sock_select(st->sb, st->ctl + 1, &readfds, NULL, &tv);
            if (ready == 0)
            {
                tool_error("the server stopped answering");
                return FALSE;
            }
            if (ready < 0)
            {
                if (tool_sock_errno(st->sb) == TOOL_EINTR)
                    continue;

                tool_error("the connection failed: %s",
                           (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
                return FALSE;
            }

            st->buf_len = tool_sock_recv(st->sb, st->ctl, ftp_ctl_buf,
                                         (LONG)sizeof(ftp_ctl_buf));
            st->buf_pos = 0;

            if (st->buf_len == 0)
            {
                tool_error("the server closed the connection");
                return FALSE;
            }
            if (st->buf_len < 0)
            {
                tool_error("the connection failed: %s",
                           (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
                st->buf_len = 0;
                return FALSE;
            }
        }

        while (st->buf_pos < st->buf_len)
        {
            UBYTE c = ftp_ctl_buf[st->buf_pos++];

            if (c == '\n')
            {
                out[o] = '\0';
                return TRUE;
            }
            if (c == '\r')
                continue;

            if (o + 1 < max)
                out[o++] = (char)c;
        }
    }
}

/*
 * A whole reply, which may be several lines: RFC 959 marks a continuation
 * with "220-" and the last line with "220 ", the same number and a space.
 * Returns the numeric code, or 0 when the connection failed.
 *
 * The text of the LAST line is kept in ftp_reply_text, because that is the
 * one PASV's numbers are in.
 */
static LONG ftp_reply(FtpState *st, BOOL show)
{
    LONG code = 0;
    BOOL first = TRUE;
    char marker[4];

    marker[0] = marker[1] = marker[2] = marker[3] = '\0';

    for (;;)
    {
        if (!ftp_getline(st, ftp_line, sizeof(ftp_line)))
            return 0;

        if (show || st->debug)
            tool_printf("%s\n", (LONG)ftp_line);

        if (first)
        {
            if (ftp_line[0] < '0' || ftp_line[0] > '9' ||
                ftp_line[1] < '0' || ftp_line[1] > '9' ||
                ftp_line[2] < '0' || ftp_line[2] > '9')
            {
                tool_error("the server did not answer with a reply code");
                return 0;
            }

            code = ((LONG)(ftp_line[0] - '0') * 100) +
                   ((LONG)(ftp_line[1] - '0') * 10) +
                    (LONG)(ftp_line[2] - '0');

            marker[0] = ftp_line[0];
            marker[1] = ftp_line[1];
            marker[2] = ftp_line[2];
            marker[3] = '\0';

            first = FALSE;

            if (ftp_line[3] != '-')
            {
                tool_copy_string(ftp_reply_text, sizeof(ftp_reply_text),
                                 ftp_line);
                return code;
            }

            continue;
        }

        /* A continuation ends at "<code> ", and only at that. */
        if (ftp_line[0] == marker[0] && ftp_line[1] == marker[1] &&
            ftp_line[2] == marker[2] && ftp_line[3] == ' ')
        {
            tool_copy_string(ftp_reply_text, sizeof(ftp_reply_text), ftp_line);
            return code;
        }
    }
}

/* Send one command line.  The password is never echoed, whatever DEBUG says. */
static BOOL ftp_send(FtpState *st, const char *text, BOOL secret)
{
    ULONG used = 0;
    LONG  len;

    ftp_cmd[0] = '\0';
    if (!s_add(ftp_cmd, sizeof(ftp_cmd), &used, text) ||
        !s_add(ftp_cmd, sizeof(ftp_cmd), &used, "\r\n"))
    {
        tool_error("that command is too long to send");
        return FALSE;
    }

    if (st->debug)
        tool_printf("---> %s\n", (LONG)(secret ? "PASS ********" : text));

    len = (LONG)used;
    if (tool_sock_send(st->sb, st->ctl, ftp_cmd, len) != len)
    {
        tool_error("cannot send: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
        return FALSE;
    }

    return TRUE;
}

/* Send "<verb> <arg>" (or just "<verb>") and read the reply. */
static LONG ftp_command(FtpState *st, const char *verb, const char *arg,
                        BOOL show)
{
    char  line[FTP_LINE_MAX];
    ULONG used = 0;

    line[0] = '\0';
    if (!s_add(line, sizeof(line), &used, verb))
        return 0;

    if (arg != NULL && arg[0] != '\0')
    {
        if (!s_add(line, sizeof(line), &used, " ") ||
            !s_add(line, sizeof(line), &used, arg))
        {
            tool_error("that argument is too long");
            return 0;
        }
    }

    if (!ftp_send(st, line, FALSE))
        return 0;

    return ftp_reply(st, show);
}


/* -------------------------------------------------------- data connection --- */

typedef struct FtpXfer
{
    LONG    listener;       /* active mode: the socket the server calls back */
    LONG    data;
} FtpXfer;

/* "227 Entering Passive Mode (10,0,2,2,200,42)." -> address and port. */
static BOOL ftp_parse_pasv(const char *text, ULONG *addr, UWORD *port)
{
    ULONG n[6];
    ULONG got = 0;
    ULONG i = 0;

    while (text[i] != '\0' && text[i] != '(')
        i++;

    if (text[i] != '(')
    {
        /*
         * Some servers answer 227 without the brackets.  The numbers are
         * still the last six comma-separated values on the line, so back off
         * to the first digit run rather than give up.
         */
        i = 0;
        while (text[i] != '\0' && (text[i] < '0' || text[i] > '9'))
            i++;
        /* skip the reply code itself */
        while (text[i] >= '0' && text[i] <= '9')
            i++;
    }
    else
    {
        i++;
    }

    while (got < 6UL && text[i] != '\0')
    {
        ULONG v = 0;
        BOOL  any = FALSE;

        while (text[i] != '\0' && (text[i] < '0' || text[i] > '9'))
        {
            if (text[i] == ')')
                break;
            i++;
        }

        while (text[i] >= '0' && text[i] <= '9')
        {
            v = (v * 10UL) + (ULONG)(text[i++] - '0');
            any = TRUE;
        }

        if (!any)
            break;

        if (v > 255UL)
            return FALSE;

        n[got++] = v;
    }

    if (got != 6UL)
        return FALSE;

    *addr = (n[0] << 24) | (n[1] << 16) | (n[2] << 8) | n[3];
    *port = (UWORD)((n[4] << 8) | n[5]);

    return TRUE;
}

/*
 * Build the data connection, in whichever direction the mode calls for.
 *
 * PASSIVE: the server opens a port and names it; we connect to it.  One
 * socket, no listening, works through any NAT -- which is why it is the
 * default and why it is also the mode that proves nothing about the server
 * half of the ABI.
 *
 * ACTIVE: we open a port, listen on it, and tell the server where with PORT.
 * The listen and the accept straddle the transfer command, because the server
 * does not connect back until it has been asked to transfer something.
 */
static BOOL ftp_data_prepare(FtpState *st, FtpXfer *x)
{
    ToolSockAddr sa;
    LONG         code;

    x->listener = -1;
    x->data     = -1;

    if (st->passive)
    {
        ULONG addr = 0;
        UWORD port = 0;

        code = ftp_command(st, "PASV", NULL, FALSE);
        if (code != 227)
        {
            if (code != 0)
                tool_error("the server will not go passive: %s",
                           (LONG)ftp_reply_text);
            return FALSE;
        }

        if (!ftp_parse_pasv(ftp_reply_text, &addr, &port))
        {
            tool_error("cannot read the address out of \"%s\"",
                       (LONG)ftp_reply_text);
            return FALSE;
        }

        /*
         * A server behind its own NAT can name an address it does not have.
         * The one it answered the control connection on is always reachable,
         * because we are already talking to it, so prefer that and keep only
         * the port.  Every modern client does this.
         */
        if (addr != st->host_addr && st->debug)
        {
            char a[16];
            char b[16];

            ami_config_format_ip(addr, a, sizeof(a));
            ami_config_format_ip(st->host_addr, b, sizeof(b));
            tool_printf("ftp: PASV named %s; using %s\n", (LONG)a, (LONG)b);
        }

        x->data = tool_sock_socket(st->sb, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
        if (x->data < 0)
        {
            tool_error("no socket: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
            return FALSE;
        }

        tool_sock_addr(&sa, st->host_addr, port);

        if (tool_sock_connect(st->sb, x->data, &sa) != 0)
        {
            tool_sock_fail(st->sb, "open a data connection to",
                           st->host_addr, port);
            (VOID)tool_sock_close(st->sb, x->data);
            x->data = -1;
            return FALSE;
        }

        return TRUE;
    }

    /* ---- active ---------------------------------------------------------- */

    {
        ToolSockAddr local;
        char         port_cmd[64];
        ULONG        used = 0;
        LONG         one = 1;
        ULONG        my_addr;
        UWORD        my_port;

        /* Our own address on this connection, as the server sees it: the
           control socket already knows it, so there is nothing to guess. */
        if (tool_sock_getsockname(st->sb, st->ctl, &local) != 0)
        {
            tool_error("cannot find this machine's own address: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
            return FALSE;
        }
        my_addr = local.sin_addr;

        x->listener = tool_sock_socket(st->sb, TOOL_AF_INET,
                                       TOOL_SOCK_STREAM, 0);
        if (x->listener < 0)
        {
            tool_error("no socket: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
            return FALSE;
        }

        (VOID)tool_sock_setsockopt(st->sb, x->listener, TOOL_SOL_SOCKET,
                                   TOOL_SO_REUSEADDR, &one, (LONG)sizeof(one));

        tool_sock_addr(&sa, 0, st->dataport);

        if (tool_sock_bind(st->sb, x->listener, &sa) != 0)
        {
            LONG err = tool_sock_errno(st->sb);

            tool_error("cannot open a data port: %s",
                       (LONG)tool_sock_errstr(err));

            if (err == TOOL_EADDRINUSE && st->dataport != 0)
            {
                tool_advise_blank();
                tool_advise("DATAPORT names one fixed port, so two transfers "
                            "cannot overlap and the last one has to have "
                            "finished closing.  Leave it out to let the stack "
                            "pick.");
            }

            (VOID)tool_sock_close(st->sb, x->listener);
            x->listener = -1;
            return FALSE;
        }

        if (tool_sock_listen(st->sb, x->listener, 1) != 0)
        {
            tool_error("cannot listen for the data connection: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
            (VOID)tool_sock_close(st->sb, x->listener);
            x->listener = -1;
            return FALSE;
        }

        /* Which port did we get?  With DATAPORT it is known; without it the
           stack chose and only getsockname() can say. */
        if (tool_sock_getsockname(st->sb, x->listener, &local) != 0)
        {
            tool_error("cannot find the data port: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
            (VOID)tool_sock_close(st->sb, x->listener);
            x->listener = -1;
            return FALSE;
        }
        my_port = local.sin_port;

        port_cmd[0] = '\0';
        (VOID)s_add(port_cmd, sizeof(port_cmd), &used, "PORT ");
        (VOID)s_addnum(port_cmd, sizeof(port_cmd), &used,
                       (my_addr >> 24) & 0xffUL);
        (VOID)s_add(port_cmd, sizeof(port_cmd), &used, ",");
        (VOID)s_addnum(port_cmd, sizeof(port_cmd), &used,
                       (my_addr >> 16) & 0xffUL);
        (VOID)s_add(port_cmd, sizeof(port_cmd), &used, ",");
        (VOID)s_addnum(port_cmd, sizeof(port_cmd), &used,
                       (my_addr >> 8) & 0xffUL);
        (VOID)s_add(port_cmd, sizeof(port_cmd), &used, ",");
        (VOID)s_addnum(port_cmd, sizeof(port_cmd), &used, my_addr & 0xffUL);
        (VOID)s_add(port_cmd, sizeof(port_cmd), &used, ",");
        (VOID)s_addnum(port_cmd, sizeof(port_cmd), &used,
                       ((ULONG)my_port >> 8) & 0xffUL);
        (VOID)s_add(port_cmd, sizeof(port_cmd), &used, ",");
        (VOID)s_addnum(port_cmd, sizeof(port_cmd), &used,
                       (ULONG)my_port & 0xffUL);

        if (!ftp_send(st, port_cmd, FALSE))
        {
            (VOID)tool_sock_close(st->sb, x->listener);
            x->listener = -1;
            return FALSE;
        }

        code = ftp_reply(st, FALSE);
        if (code < 200 || code >= 300)
        {
            if (code != 0)
                tool_error("the server refused the data port: %s",
                           (LONG)ftp_reply_text);
            (VOID)tool_sock_close(st->sb, x->listener);
            x->listener = -1;
            return FALSE;
        }

        return TRUE;
    }
}

/*
 * In active mode the server has now been told to transfer something and is
 * connecting back.  Waited for through WaitSelect() rather than a bare
 * accept(), so a server that never calls back gives up after TIMEOUT instead
 * of hanging the command forever -- which is the usual symptom of active mode
 * meeting a firewall.
 */
static BOOL ftp_data_accept(FtpState *st, FtpXfer *x)
{
    ToolSockAddr from;
    ToolFdSet    readfds;
    ToolTimeval  tv;
    LONG         ready;

    if (st->passive)
        return TRUE;

    tool_fd_zero(&readfds);
    tool_fd_add(&readfds, x->listener);

    tv.tv_secs  = (LONG)st->timeout;
    tv.tv_micro = 0;

    ready = tool_sock_select(st->sb, x->listener + 1, &readfds, NULL, &tv);

    if (ready == 0)
    {
        tool_error("the server never opened the data connection");
        tool_advise_blank();
        tool_advise("Active mode needs the server to be able to reach this "
                    "machine.  Behind a NAT it cannot, unless DATAPORT names "
                    "a forwarded port.  \"passive\" turns the connection "
                    "round.");
        return FALSE;
    }

    if (ready < 0)
    {
        tool_error("cannot wait for the data connection: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
        return FALSE;
    }

    x->data = tool_sock_accept(st->sb, x->listener, &from);
    if (x->data < 0)
    {
        tool_error("cannot accept the data connection: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
        return FALSE;
    }

    if (st->debug)
    {
        char dotted[16];

        ami_config_format_ip(from.sin_addr, dotted, sizeof(dotted));
        tool_printf("ftp: data connection from %s port %ld\n",
                    (LONG)dotted, (LONG)from.sin_port);
    }

    /* The listener has done its one job. */
    (VOID)tool_sock_close(st->sb, x->listener);
    x->listener = -1;

    return TRUE;
}

static VOID ftp_data_close(FtpState *st, FtpXfer *x)
{
    if (x->data >= 0)
    {
        (VOID)tool_sock_close(st->sb, x->data);
        x->data = -1;
    }
    if (x->listener >= 0)
    {
        (VOID)tool_sock_close(st->sb, x->listener);
        x->listener = -1;
    }
}


/* -------------------------------------------------------------- transfers --- */

/*
 * Everything that reads a data connection: a listing to the screen, or a file
 * to disk.  `out` of 0 means standard output.
 */
static BOOL ftp_read_data(FtpState *st, FtpXfer *x, BPTR out, ULONG *total)
{
    BOOL ok = TRUE;

    *total = 0;

    for (;;)
    {
        LONG n = tool_sock_recv(st->sb, x->data, ftp_data,
                                (LONG)sizeof(ftp_data));
        LONG len;
        APTR buf;

        if (n == 0)
            break;

        if (n < 0)
        {
            LONG err = tool_sock_errno(st->sb);

            if (err == TOOL_EINTR)
                continue;

            tool_error("the transfer failed: %s", (LONG)tool_sock_errstr(err));
            return FALSE;
        }

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            return FALSE;
        }

        buf = (APTR)ftp_data;
        len = n;

        /*
         * ASCII mode: the wire says CR LF and the Amiga says LF.  Dropping
         * the CR of a CR LF pair is the whole translation -- a lone CR is
         * left alone, because in a text file it is data.
         */
        if (!st->binary)
        {
            LONG i;
            LONG o = 0;

            for (i = 0; i < n; i++)
            {
                if (ftp_data[i] == '\r' && i + 1 < n && ftp_data[i + 1] == '\n')
                    continue;
                ftp_ascii[o++] = ftp_data[i];
            }

            buf = (APTR)ftp_ascii;
            len = o;
        }

        if (len > 0)
        {
            LONG written = (out != (BPTR)0)
                               ? Write(out, buf, len)
                               : tool_output_write((const UBYTE *)buf, len);

            if (written != len)
            {
                tool_fault(IoErr());
                ok = FALSE;
                break;
            }
        }

        *total += (ULONG)n;
    }

    return ok;
}

static BOOL ftp_write_data(FtpState *st, FtpXfer *x, BPTR in, ULONG *total)
{
    *total = 0;

    for (;;)
    {
        LONG n = Read(in, (APTR)ftp_data, (LONG)sizeof(ftp_data));
        LONG len;
        const UBYTE *buf;

        if (n == 0)
            break;

        if (n < 0)
        {
            tool_fault(IoErr());
            return FALSE;
        }

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            return FALSE;
        }

        buf = ftp_data;
        len = n;

        if (!st->binary)
        {
            LONG i;
            LONG o = 0;

            for (i = 0; i < n; i++)
            {
                if (ftp_data[i] == '\n')
                    ftp_ascii[o++] = '\r';
                ftp_ascii[o++] = ftp_data[i];
            }

            buf = ftp_ascii;
            len = o;
        }

        if (tool_sock_send(st->sb, x->data, buf, len) != len)
        {
            tool_error("the transfer failed: %s",
                       (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
            return FALSE;
        }

        *total += (ULONG)n;
    }

    return TRUE;
}

/*
 * The shape every transfer has: build the data connection, ask for the
 * transfer, take the server's 1xx, move the bytes, close, take the 2xx.
 *
 * The order matters and is not obvious: in active mode the accept() can only
 * happen AFTER the transfer command, because that is what makes the server
 * connect back, but the 1xx reply that acknowledges the command may arrive
 * before or after the data connection does.  Reading the reply first is what
 * every client does and what every server expects.
 */
static LONG ftp_transfer(FtpState *st, const char *verb, const char *arg,
                         BPTR file_out, BPTR file_in)
{
    FtpXfer x;
    LONG    code;
    ULONG   total = 0;
    BOOL    ok;

    if (!ftp_data_prepare(st, &x))
        return RETURN_ERROR;

    code = ftp_command(st, verb, arg, FALSE);

    if (code == 0)
    {
        ftp_data_close(st, &x);
        return RETURN_ERROR;
    }

    if (code >= 400)
    {
        tool_error("%s", (LONG)ftp_reply_text);
        ftp_data_close(st, &x);
        return RETURN_ERROR;
    }

    if (!ftp_data_accept(st, &x))
    {
        ftp_data_close(st, &x);
        /* The server is still expecting to finish the transfer; drain its
           final reply so the control connection stays usable. */
        (VOID)ftp_reply(st, FALSE);
        return RETURN_ERROR;
    }

    ok = (file_in != (BPTR)0)
             ? ftp_write_data(st, &x, file_in, &total)
             : ftp_read_data(st, &x, file_out, &total);

    ftp_data_close(st, &x);

    code = ftp_reply(st, FALSE);

    if (!ok)
        return RETURN_ERROR;

    if (code == 0)
        return RETURN_ERROR;

    if (code >= 400)
    {
        tool_error("%s", (LONG)ftp_reply_text);
        return RETURN_ERROR;
    }

    if (file_out != (BPTR)0 || file_in != (BPTR)0)
        tool_printf("%lu bytes transferred\n", total);

    return RETURN_OK;
}


/* -------------------------------------------------------------- the session --- */

static BOOL ftp_open(FtpState *st, const char *host, UWORD port)
{
    ToolSockAddr sa;
    LONG         code;

    if (!tool_sock_resolve(st->sb, host, &st->host_addr))
        return FALSE;

    st->host_port = port;

    st->ctl = tool_sock_socket(st->sb, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
    if (st->ctl < 0)
    {
        tool_error("no socket: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(st->sb)));
        return FALSE;
    }

    tool_sock_addr(&sa, st->host_addr, port);

    if (tool_sock_connect(st->sb, st->ctl, &sa) != 0)
    {
        tool_sock_fail(st->sb, "connect to", st->host_addr, port);
        (VOID)tool_sock_close(st->sb, st->ctl);
        st->ctl = -1;
        return FALSE;
    }

    st->buf_len = 0;
    st->buf_pos = 0;

    code = ftp_reply(st, TRUE);
    if (code == 0)
    {
        (VOID)tool_sock_close(st->sb, st->ctl);
        st->ctl = -1;
        return FALSE;
    }

    if (code >= 400)
    {
        tool_error("%s", (LONG)ftp_reply_text);
        (VOID)tool_sock_close(st->sb, st->ctl);
        st->ctl = -1;
        return FALSE;
    }

    return TRUE;
}

/* One line from the user, or FALSE at end of input. */
static BOOL ftp_prompt(FtpState *st, const char *prompt, char *out, ULONG max)
{
    ULONG n;

    if (st->interactive && prompt != NULL)
        tool_printf("%s", (LONG)prompt);

    if (FGets(Input(), (STRPTR)out, max) == NULL)
        return FALSE;

    n = s_len(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';

    return TRUE;
}

static BOOL ftp_login(FtpState *st, const char *user, const char *password)
{
    char  name[128];
    char  pass[128];
    LONG  code;

    if (user == NULL)
    {
        if (!ftp_prompt(st, "Name: ", name, sizeof(name)) || name[0] == '\0')
        {
            tool_error("no user name; not logged in");
            return FALSE;
        }
        user = name;
    }

    code = ftp_command(st, "USER", user, TRUE);
    if (code == 0)
        return FALSE;

    if (code == 230)
        return TRUE;                /* no password wanted */

    if (code == 331 || code == 332)
    {
        if (password == NULL)
        {
            if (!ftp_prompt(st, "Password: ", pass, sizeof(pass)))
                pass[0] = '\0';
            password = pass;
        }

        /* Not through ftp_command(): the password must not be echoed. */
        {
            char  line[FTP_LINE_MAX];
            ULONG used = 0;

            line[0] = '\0';
            (VOID)s_add(line, sizeof(line), &used, "PASS ");
            (VOID)s_add(line, sizeof(line), &used, password);

            if (!ftp_send(st, line, TRUE))
                return FALSE;
        }

        code = ftp_reply(st, TRUE);
        if (code == 0)
            return FALSE;
    }

    if (code >= 400)
    {
        tool_error("not logged in: %s", (LONG)ftp_reply_text);
        return FALSE;
    }

    return TRUE;
}

static VOID ftp_help(VOID)
{
    tool_printf("  open <host> [<port>]     close                 bye\n");
    tool_printf("  user <name> [<pass>]     pwd                   cd <dir>\n");
    tool_printf("  cdup                     ls [<dir>]            dir [<dir>]\n");
    tool_printf("  get <remote> [<local>]   put <local> [<remote>]\n");
    tool_printf("  binary                   ascii                 size <file>\n");
    tool_printf("  delete <file>            mkdir <dir>           rmdir <dir>\n");
    tool_printf("  rename <old> <new>       system                status\n");
    tool_printf("  passive                  active                debug\n");
    tool_printf("  quote <command...>       help\n");
}

static VOID ftp_status(FtpState *st)
{
    char dotted[16];

    if (st->ctl < 0)
    {
        tool_printf("Not connected.\n");
    }
    else
    {
        ami_config_format_ip(st->host_addr, dotted, sizeof(dotted));
        tool_printf("Connected to %s port %ld.\n",
                    (LONG)dotted, (LONG)st->host_port);
    }

    tool_printf("Mode: %s.  Type: %s.\n",
                (LONG)(st->passive ? "passive" : "active"),
                (LONG)(st->binary ? "binary" : "ascii"));

    if (!st->passive && st->dataport != 0)
        tool_printf("Data port pinned to %ld.\n", (LONG)st->dataport);
}


/* ------------------------------------------------------------------ main --- */

/* Everything after the first word of `line`, or NULL when there is nothing. */
static const char *ftp_tail(const char *line)
{
    ULONG i = 0;

    while (line[i] == ' ' || line[i] == '\t')
        i++;
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t')
        i++;
    while (line[i] == ' ' || line[i] == '\t')
        i++;

    return (line[i] != '\0') ? &line[i] : NULL;
}

/* Split a command line into at most four words, in place. */
static ULONG ftp_split(char *line, char *word[4])
{
    ULONG n = 0;
    ULONG i = 0;

    while (n < 4UL)
    {
        while (line[i] == ' ' || line[i] == '\t')
            i++;

        if (line[i] == '\0')
            break;

        word[n++] = &line[i];

        /*
         * The LAST word keeps its spaces, so `quote SITE CHMOD 755 file` and
         * a file name with a space in it both survive.
         */
        if (n == 4UL)
            break;

        while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t')
            i++;

        if (line[i] != '\0')
            line[i++] = '\0';
    }

    return n;
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    FtpState        st;
    const char     *user;
    const char     *password;
    UWORD           port = 21;
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
        tool_usage("[<host> [<port>]] [USER <name>] [PASSWORD <pass>]",
                   "Transfers files.  Commands are read from standard input; "
                   "\"help\" lists them.");
        return RETURN_ERROR;
    }

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    st.sb          = sb;
    st.ctl         = -1;
    st.passive     = (args[ARG_ACTIVE] != 0) ? FALSE : TRUE;
    st.binary      = TRUE;
    st.debug       = (args[ARG_DEBUG] != 0) ? TRUE : FALSE;
    st.interactive = (IsInteractive(Input()) != 0) ? TRUE : FALSE;
    st.timeout     = (args[ARG_TIMEOUT] != 0)
                         ? (ULONG)(*(LONG *)args[ARG_TIMEOUT])
                         : FTP_DEFAULT_TIMEOUT;
    st.dataport    = (args[ARG_DATAPORT] != 0)
                         ? (UWORD)(*(LONG *)args[ARG_DATAPORT]) : 0;
    st.host_addr   = 0;
    st.host_port   = 0;
    st.buf_len     = 0;
    st.buf_pos     = 0;

    if (st.timeout == 0)
        st.timeout = FTP_DEFAULT_TIMEOUT;

    user     = (const char *)args[ARG_USER];
    password = (const char *)args[ARG_PASSWORD];

    if (args[ARG_PORT] != 0)
    {
        port = tool_sock_port(sb, (const char *)args[ARG_PORT], "tcp");
        if (port == 0)
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    if (args[ARG_HOST] != 0)
    {
        if (!ftp_open(&st, (const char *)args[ARG_HOST], port))
        {
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (!ftp_login(&st, user, password))
        {
            (VOID)ftp_command(&st, "QUIT", NULL, FALSE);
            (VOID)tool_sock_close(sb, st.ctl);
            CloseLibrary(sb);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    /* ---- the command loop ------------------------------------------------ */

    for (;;)
    {
        char  *word[4];
        ULONG  n;
        const char *verb;

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            rc = RETURN_WARN;
            break;
        }

        if (!ftp_prompt(&st, "ftp> ", ftp_input, sizeof(ftp_input)))
            break;

        tool_copy_string(ftp_raw, sizeof(ftp_raw), ftp_input);

        n = ftp_split(ftp_input, word);
        if (n == 0)
            continue;

        verb = word[0];

        if (verb[0] == '#')
            continue;

        if (s_eq_ci(verb, "bye") || s_eq_ci(verb, "quit") ||
            s_eq_ci(verb, "exit"))
        {
            break;
        }

        if (s_eq_ci(verb, "help") || s_eq_ci(verb, "?"))
        {
            ftp_help();
            continue;
        }

        if (s_eq_ci(verb, "status"))
        {
            ftp_status(&st);
            continue;
        }

        if (s_eq_ci(verb, "debug"))
        {
            st.debug = (BOOL)(!st.debug);
            tool_printf("Debugging %s.\n", (LONG)(st.debug ? "on" : "off"));
            continue;
        }

        if (s_eq_ci(verb, "passive"))
        {
            st.passive = TRUE;
            tool_printf("Passive mode.\n");
            continue;
        }

        if (s_eq_ci(verb, "active"))
        {
            st.passive = FALSE;
            tool_printf("Active mode.\n");
            continue;
        }

        if (s_eq_ci(verb, "binary") || s_eq_ci(verb, "bin") ||
            s_eq_ci(verb, "image"))
        {
            if (st.ctl >= 0 && ftp_command(&st, "TYPE", "I", FALSE) == 0)
            {
                rc = RETURN_ERROR;
                break;
            }
            st.binary = TRUE;
            tool_printf("Type is binary.\n");
            continue;
        }

        if (s_eq_ci(verb, "ascii") || s_eq_ci(verb, "text"))
        {
            if (st.ctl >= 0 && ftp_command(&st, "TYPE", "A", FALSE) == 0)
            {
                rc = RETURN_ERROR;
                break;
            }
            st.binary = FALSE;
            tool_printf("Type is ascii.\n");
            continue;
        }

        if (s_eq_ci(verb, "open"))
        {
            UWORD p = 21;

            if (st.ctl >= 0)
            {
                tool_error("already connected; \"close\" first");
                continue;
            }
            if (n < 2)
            {
                tool_error("open which host?");
                continue;
            }
            if (n >= 3)
            {
                p = tool_sock_port(sb, word[2], "tcp");
                if (p == 0)
                    continue;
            }

            if (ftp_open(&st, word[1], p))
                (VOID)ftp_login(&st, NULL, NULL);

            continue;
        }

        if (s_eq_ci(verb, "close"))
        {
            if (st.ctl < 0)
            {
                tool_error("not connected");
                continue;
            }

            (VOID)ftp_command(&st, "QUIT", NULL, TRUE);
            (VOID)tool_sock_close(sb, st.ctl);
            st.ctl = -1;
            continue;
        }

        /* Everything past here needs a connection. */
        if (st.ctl < 0)
        {
            tool_error("not connected");
            continue;
        }

        if (s_eq_ci(verb, "user"))
        {
            if (n < 2)
            {
                tool_error("which user?");
                continue;
            }
            (VOID)ftp_login(&st, word[1], (n >= 3) ? word[2] : NULL);
            continue;
        }

        if (s_eq_ci(verb, "pwd"))
        {
            (VOID)ftp_command(&st, "PWD", NULL, TRUE);
            continue;
        }

        if (s_eq_ci(verb, "cd"))
        {
            if (n < 2)
            {
                tool_error("cd where?");
                continue;
            }
            (VOID)ftp_command(&st, "CWD", word[1], TRUE);
            continue;
        }

        if (s_eq_ci(verb, "cdup"))
        {
            (VOID)ftp_command(&st, "CDUP", NULL, TRUE);
            continue;
        }

        if (s_eq_ci(verb, "system") || s_eq_ci(verb, "syst"))
        {
            (VOID)ftp_command(&st, "SYST", NULL, TRUE);
            continue;
        }

        if (s_eq_ci(verb, "size"))
        {
            if (n < 2)
            {
                tool_error("the size of what?");
                continue;
            }
            (VOID)ftp_command(&st, "SIZE", word[1], TRUE);
            continue;
        }

        if (s_eq_ci(verb, "delete") || s_eq_ci(verb, "rm"))
        {
            if (n < 2)
            {
                tool_error("delete what?");
                continue;
            }
            (VOID)ftp_command(&st, "DELE", word[1], TRUE);
            continue;
        }

        if (s_eq_ci(verb, "mkdir"))
        {
            if (n < 2)
            {
                tool_error("make which directory?");
                continue;
            }
            (VOID)ftp_command(&st, "MKD", word[1], TRUE);
            continue;
        }

        if (s_eq_ci(verb, "rmdir"))
        {
            if (n < 2)
            {
                tool_error("remove which directory?");
                continue;
            }
            (VOID)ftp_command(&st, "RMD", word[1], TRUE);
            continue;
        }

        if (s_eq_ci(verb, "rename"))
        {
            if (n < 3)
            {
                tool_error("rename what to what?");
                continue;
            }
            if (ftp_command(&st, "RNFR", word[1], TRUE) == 350)
                (VOID)ftp_command(&st, "RNTO", word[2], TRUE);
            continue;
        }

        if (s_eq_ci(verb, "quote") || s_eq_ci(verb, "literal"))
        {
            /* Everything after the verb, spaces and all: the point of quote
               is to send a command this client has never heard of. */
            const char *rest = ftp_tail(ftp_raw);

            if (rest == NULL)
            {
                tool_error("send what?");
                continue;
            }
            if (ftp_send(&st, rest, FALSE))
                (VOID)ftp_reply(&st, TRUE);
            continue;
        }

        if (s_eq_ci(verb, "ls") || s_eq_ci(verb, "dir") ||
            s_eq_ci(verb, "nlst"))
        {
            const char *arg = (n >= 2) ? word[1] : NULL;

            (VOID)ftp_transfer(&st,
                               s_eq_ci(verb, "nlst") ? "NLST" : "LIST",
                               arg, (BPTR)0, (BPTR)0);
            continue;
        }

        if (s_eq_ci(verb, "get") || s_eq_ci(verb, "recv"))
        {
            const char *local;
            BPTR        out;

            if (n < 2)
            {
                tool_error("get what?");
                continue;
            }

            local = (n >= 3) ? word[2] : tool_basename(word[1]);

            out = Open((CONST_STRPTR)local, MODE_NEWFILE);
            if (out == (BPTR)0)
            {
                tool_fault(IoErr());
                continue;
            }

            if (ftp_transfer(&st, "RETR", word[1], out, (BPTR)0) != RETURN_OK)
                rc = RETURN_WARN;

            Close(out);
            continue;
        }

        if (s_eq_ci(verb, "put") || s_eq_ci(verb, "send"))
        {
            const char *remote;
            BPTR        in;

            if (n < 2)
            {
                tool_error("put what?");
                continue;
            }

            remote = (n >= 3) ? word[2] : tool_basename(word[1]);

            in = Open((CONST_STRPTR)word[1], MODE_OLDFILE);
            if (in == (BPTR)0)
            {
                tool_fault(IoErr());
                continue;
            }

            if (ftp_transfer(&st, "STOR", remote, (BPTR)0, in) != RETURN_OK)
                rc = RETURN_WARN;

            Close(in);
            continue;
        }

        tool_error("\"%s\" is not a command here; \"help\" lists them",
                   (LONG)verb);
    }

    if (st.ctl >= 0)
    {
        (VOID)ftp_command(&st, "QUIT", NULL, FALSE);
        (VOID)tool_sock_close(sb, st.ctl);
    }

    CloseLibrary(sb);
    FreeArgs(rda);

    return (int)rc;
}
