/*
 * whois -- ask a registry what it knows about a name or an address.
 *
 *     whois QUERY/A,SERVER/K,PORT/N/K,FOLLOW/S
 *
 *   SERVER   ask this one instead of whois.iana.org.
 *   PORT     its port. Default 43.
 *   FOLLOW   chase the referral automatically instead of printing it.
 *
 * The protocol is one line long: connect, send the query, read until the other
 * end hangs up, print what came back. RFC 3912 is three pages and most of them
 * are about how little it specifies -- there is no request format, no reply
 * format and no character set, which is why what comes back is written out
 * exactly as it arrived rather than parsed.
 *
 * WHY THE DEFAULT IS whois.iana.org
 *
 *   There is no server that knows everything, and the one that used to be
 *   assumed -- whois.internic.net -- has known only .com and .net for twenty
 *   years. IANA's knows one thing about everything: which registry to ask.
 *   So the default answers usefully for any TLD, any IP range and any AS
 *   number, and the answer names the server that has the detail.
 *
 *   That referral is the whole reason FOLLOW exists. Without it the answer is
 *   still useful, because the line to type next is printed underneath it; a
 *   command that leaves you able to finish the job by hand is better than one
 *   that hides the second step.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

const char *const tool_name = "whois";

static const char version_tag[] __attribute__((used)) =
    "$VER: whois 1.0 (26.7.2026)";

#define TEMPLATE    "QUERY/A,SERVER/K,PORT/N/K,FOLLOW/S"

enum
{
    ARG_QUERY = 0,
    ARG_SERVER,
    ARG_PORT,
    ARG_FOLLOW,
    ARG_COUNT
};

#define WHOIS_PORT          43
#define WHOIS_DEFAULT       "whois.iana.org"
#define WHOIS_MAX_FOLLOW    3           /* registries do chain, but not far */
#define WHOIS_NAME_MAX      128
#define WHOIS_LINE_MAX      256

/* Static, not automatic: the Shell's stack is 4 KB on a stock 3.1. */
static UBYTE whois_buf[2048];
static char  whois_line[WHOIS_LINE_MAX];
static char  whois_referral[WHOIS_NAME_MAX];
static char  whois_request[WHOIS_NAME_MAX + 4];

/* ---------------------------------------------------------------- referrals */

/*
 * The four spellings a referral comes in.
 *
 *   refer:                   IANA
 *   whois:                   IANA, for a TLD record
 *   Registrar WHOIS Server:  the gTLD registries, since ICANN required it
 *   ReferralServer:          the RIRs, and it carries a whois:// scheme
 *
 * Matched case-insensitively at the start of a line, which is the only place
 * any of them appears.
 */
static const char *const whois_keys[] =
{
    "refer:",
    "whois:",
    "registrar whois server:",
    "referralserver:",
    NULL
};

static char whois_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static BOOL whois_starts_with(const char *line, const char *key)
{
    ULONG i;

    for (i = 0; key[i] != '\0'; i++)
    {
        if (whois_lower(line[i]) != key[i])
            return FALSE;
    }

    return TRUE;
}

/*
 * Pull a host name out of a referral line, if that is what it is.
 *
 * "ReferralServer: whois://whois.arin.net" and
 * "refer:          whois.verisign-grs.com" both reduce to a bare host: the
 * scheme is dropped, and so is any :port, because the port belongs to this
 * command's own PORT argument and a name with a colon in it would not resolve.
 */
static BOOL whois_referral_from(const char *line, char *out, ULONG outlen)
{
    const char *p = NULL;
    ULONG       k;
    ULONG       o = 0;

    for (k = 0; whois_keys[k] != NULL; k++)
    {
        if (whois_starts_with(line, whois_keys[k]))
        {
            ULONG len = 0;

            while (whois_keys[k][len] != '\0')
                len++;

            p = line + len;
            break;
        }
    }

    if (p == NULL)
        return FALSE;

    while (*p == ' ' || *p == '\t')
        p++;

    /* Any scheme, dropped: "whois://", "rwhois://", "http://" alike. */
    {
        const char *scheme = p;

        while (*scheme != '\0' && *scheme != ' ' && *scheme != ':')
            scheme++;

        if (scheme[0] == ':' && scheme[1] == '/' && scheme[2] == '/')
            p = scheme + 3;
    }

    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' &&
           *p != '\n' && *p != ':' && *p != '/')
    {
        if (o + 1 >= outlen)
            return FALSE;

        out[o++] = *p++;
    }

    out[o] = '\0';

    /* A bare "whois:" with nothing after it is a field, not a referral. */
    return (o > 0 && out[0] != '\0') ? TRUE : FALSE;
}

/* ------------------------------------------------------------ the exchange */

/*
 * One server, one query.
 *
 * Everything that arrives goes straight to standard output; the reply is also
 * scanned a line at a time for a referral, which is left in whois_referral.
 * RETURN_OK, or a code after printing why not.
 */
static LONG whois_ask(struct Library *sb, const char *server, UWORD port,
                      const char *query, BOOL *referred)
{
    ToolSockAddr sa;
    ULONG        address = 0;
    LONG         sock;
    LONG         len = 0;
    ULONG        fill = 0;
    ULONG        i;
    LONG         n;

    *referred = FALSE;
    whois_referral[0] = '\0';

    if (!tool_sock_resolve(sb, server, &address))
        return RETURN_ERROR;

    sock = tool_sock_socket(sb, TOOL_AF_INET, TOOL_SOCK_STREAM, 0);
    if (sock < 0)
    {
        tool_error("no socket: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        return RETURN_FAIL;
    }

    tool_sock_addr(&sa, address, port);

    if (tool_sock_connect(sb, sock, &sa) != 0)
    {
        tool_sock_fail(sb, "reach", address, port);
        (VOID)tool_sock_close(sb, sock);
        return RETURN_ERROR;
    }

    /* The request is the query and a CRLF. That is the entire protocol. */
    for (i = 0; query[i] != '\0' && len < (LONG)sizeof(whois_request) - 3; i++)
        whois_request[len++] = query[i];
    whois_request[len++] = '\r';
    whois_request[len++] = '\n';

    if (tool_sock_send(sb, sock, whois_request, len) != len)
    {
        tool_error("cannot send the query: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(sb)));
        (VOID)tool_sock_close(sb, sock);
        return RETURN_ERROR;
    }

    for (;;)
    {
        if (tool_break())
        {
            (VOID)tool_sock_close(sb, sock);
            tool_fault(ERROR_BREAK);
            return RETURN_WARN;
        }

        n = tool_sock_recv(sb, sock, whois_buf, (LONG)sizeof(whois_buf));

        if (n == 0)
            break;                      /* the server has said everything */

        if (n < 0)
        {
            LONG err = tool_sock_errno(sb);

            if (err == TOOL_EINTR || err == TOOL_EWOULDBLOCK)
                continue;

            tool_error("the connection failed: %s",
                       (LONG)tool_sock_errstr(err));
            (VOID)tool_sock_close(sb, sock);
            return RETURN_ERROR;
        }

        if (tool_output_write(whois_buf, n) != n)
        {
            tool_fault(IoErr());
            (VOID)tool_sock_close(sb, sock);
            return RETURN_FAIL;
        }

        /*
         * The same bytes again, a line at a time, looking for the referral.
         * Only the FIRST one is kept: a registry answer that mentions several
         * servers means the one nearest the top, and chasing the last would
         * follow whatever happened to be in the free-text legal notice.
         */
        for (i = 0; i < (ULONG)n; i++)
        {
            UBYTE c = whois_buf[i];

            if (c == '\n' || c == '\r')
            {
                if (fill > 0)
                {
                    whois_line[fill] = '\0';

                    if (!*referred &&
                        whois_referral_from(whois_line, whois_referral,
                                            sizeof(whois_referral)))
                        *referred = TRUE;
                }
                fill = 0;
            }
            else if (fill + 1 < (ULONG)sizeof(whois_line))
            {
                whois_line[fill++] = (char)c;
            }
        }
    }

    (VOID)tool_sock_close(sb, sock);

    return RETURN_OK;
}

/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    const char     *query;
    const char     *server;
    UWORD           port;
    BOOL            follow;
    BOOL            referred = FALSE;
    LONG            rc;
    ULONG           hops = 0;
    ULONG           i;
    char            next[WHOIS_NAME_MAX];

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
        tool_usage("<name or address>",
                   "Asks a registry what it knows about a name or an address.");
        return RETURN_ERROR;
    }

    query  = (const char *)args[ARG_QUERY];
    server = (args[ARG_SERVER] != 0) ? (const char *)args[ARG_SERVER]
                                     : WHOIS_DEFAULT;
    follow = (args[ARG_FOLLOW] != 0) ? TRUE : FALSE;
    port   = (args[ARG_PORT] != 0)
                 ? (UWORD)(*(LONG *)args[ARG_PORT]) : (UWORD)WHOIS_PORT;

    if (port == 0)
    {
        tool_error("port 0 is not a port anything listens on");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    sb = tool_socket_open();
    if (sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    for (;;)
    {
        rc = whois_ask(sb, server, port, query, &referred);

        if (rc != RETURN_OK || !referred)
            break;

        /* A server that refers you to itself is a loop, not a referral. */
        if (tool_stricmp(whois_referral, server) == 0)
            break;

        for (i = 0; i < (ULONG)sizeof(next); i++)
        {
            next[i] = whois_referral[i];
            if (next[i] == '\0')
                break;
        }
        next[sizeof(next) - 1] = '\0';

        if (!follow || ++hops > WHOIS_MAX_FOLLOW)
        {
            tool_printf("\n");
            tool_printf("%s has the detail:\n", (LONG)next);
            tool_printf("  whois %s SERVER %s\n", (LONG)query, (LONG)next);
            break;
        }

        server = next;
        tool_printf("\n");
        tool_printf("--- %s ---\n", (LONG)server);
    }

    CloseLibrary(sb);
    FreeArgs(rda);

    return (int)rc;
}
