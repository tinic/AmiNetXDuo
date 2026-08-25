/*
 * whois, ask a registry what it knows about a name or an address.
 *
 *     whois QUERY/A,SERVER/K,PORT/N/K,FOLLOW/S,IPV4=-4/S,IPV6=-6/S
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "aminetxduo/version.h"

const char *const tool_name = "whois";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("whois");

#define TEMPLATE    "QUERY/A,SERVER/K,PORT/N/K,FOLLOW/S,IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_QUERY = 0,
    ARG_SERVER,
    ARG_PORT,
    ARG_FOLLOW,
    ARG_IPV4,
    ARG_IPV6,
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

/*
 * Matched case-insensitively at the start of a line, after any indentation.
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
 */
static BOOL whois_referral_from(const char *line, char *out, ULONG outlen)
{
    const char *p = NULL;
    ULONG       k;
    ULONG       o = 0;

    while (*line == ' ' || *line == '\t')
        line++;

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

static VOID whois_finish_line(ULONG *fill, BOOL *referred)
{
    if (*fill == 0)
        return;

    whois_line[*fill] = '\0';

    if (!*referred &&
        whois_referral_from(whois_line, whois_referral,
                            sizeof(whois_referral)))
        *referred = TRUE;

    *fill = 0;
}

/*
 * One server, one query. Everything that arrives goes straight to standard
 * output. The reply is also scanned a line at a time for a referral, which is
 * left in whois_referral. RETURN_OK, or a code after printing why not.
 */
static LONG whois_ask(struct Library *sb, const char *server, UWORD port,
                      LONG family, const char *query, BOOL *referred)
{
    ToolConnect     how;
    ToolAddr        address;
    LONG            why = 0;
    LONG            sock;
    LONG            len = 0;
    ULONG           fill = 0;
    ULONG           i;
    LONG            n;

    *referred = FALSE;
    whois_referral[0] = '\0';

    for (i = 0; query[i] != '\0'; i++)
    {
        if (len >= (LONG)sizeof(whois_request) - 2)
        {
            tool_error("the query is too long (at most %lu characters)",
                       (LONG)sizeof(whois_request) - 2);
            return RETURN_ERROR;
        }

        whois_request[len++] = query[i];
    }
    whois_request[len++] = '\r';
    whois_request[len++] = '\n';

    how.family    = family;
    how.socktype  = TOOL_SOCK_STREAM;
    how.port      = port;
    how.localport = 0;
    how.timeout   = 0;
    how.announce  = FALSE;

    sock = tool_sock_connect_host(sb, server, &how, &address, &why);
    if (sock < 0)
    {
        if (sock == TOOL_CONNECT_NOSOCKET)
        {
            tool_error("no socket: %s", (LONG)tool_sock_errstr(why));
            return RETURN_FAIL;
        }

        if (sock != TOOL_CONNECT_NORESOLVE)
            tool_sock_fail_why(sb, "reach", &address, port, why);

        return RETURN_ERROR;
    }

    if (tool_sock_send_full(sb, sock, whois_request, len) != len)
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

        for (i = 0; i < (ULONG)n; i++)
        {
            UBYTE c = whois_buf[i];

            if (c == '\n' || c == '\r')
            {
                whois_finish_line(&fill, referred);
            }
            else if (fill + 1 < (ULONG)sizeof(whois_line))
            {
                whois_line[fill++] = (char)c;
            }
        }
    }

    /* A close terminates the final line just as surely as CR or LF. */
    whois_finish_line(&fill, referred);

    (VOID)tool_sock_close(sb, sock);

    return RETURN_OK;
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    struct Library *sb;
    const char     *query;
    const char     *server;
    UWORD           port;
    BOOL            follow;
    LONG            family;
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
        tool_usage("[-4|-6] <name or address>",
                   "Asks a registry what it knows about a name or an address.");
        return RETURN_ERROR;
    }

    query  = (const char *)args[ARG_QUERY];
    server = (args[ARG_SERVER] != 0) ? (const char *)args[ARG_SERVER]
                                     : WHOIS_DEFAULT;
    follow = (args[ARG_FOLLOW] != 0) ? TRUE : FALSE;
    port   = (UWORD)WHOIS_PORT;

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

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6], &family))
    {
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
        rc = whois_ask(sb, server, port, family, query, &referred);

        if (rc != RETURN_OK || !referred)
            break;

        /* A server that refers to itself is a loop, not a referral. */
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
            /* The line to type next carries -4/-6 forward: a chain asked for
               over one family is meant to stay on it. */
            tool_printf("  whois %s%s SERVER %s\n",
                        (LONG)((family == TOOL_AF_INET)  ? "-4 " :
                               (family == TOOL_AF_INET6) ? "-6 " : ""),
                        (LONG)query, (LONG)next);
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
