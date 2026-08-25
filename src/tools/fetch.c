/*
 * fetch, retrieve an http:// or https:// URL.
 *
 *     fetch URL/A,TO/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K,
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"

#include <exec/execbase.h>   /* SysBase->AttnFlags, AFF_68020 */
#include <exec/memory.h>
#include <exec/tasks.h>

#include "aminetxduo/tlslib.h"
#include "aminetxduo/version.h"

#include "fetchurl.h"

const char *const tool_name = "fetch";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("fetch");

#define TEMPLATE    "URL/A,TO/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K," \
                    "IPV4=-4/S,IPV6=-6/S"

enum
{
    ARG_URL = 0,
    ARG_TO,
    ARG_HEADERS,
    ARG_QUIET,
    ARG_NOVERIFY,
    ARG_TIMEOUT,
    ARG_IPV4,
    ARG_IPV6,
    ARG_COUNT
};

#define FETCH_DEFAULT_TIMEOUT   120UL       /* seconds */
#define FETCH_TIMEOUT_MAX       2147483L    /* signed milliseconds fit */
#define FETCH_MAX_HOPS          5

#define FETCH_CHUNK             4096

#define FETCH_MAX_INTERIM       8

/*
 * Static, not automatic: a Shell command gets whatever stack the Shell has,
 * 4 KB on a stock Kickstart 3.1, and these are 10 KB between them.  Same
 * reasoning as src/tools/toolssmoke.c.
 */
static UBYTE fetch_chunk[FETCH_CHUNK];
static char  fetch_head[FETCH_HEAD_MAX];
static char  fetch_request[FETCH_PATH_MAX + FETCH_AUTHORITY_MAX + 160];
static char  fetch_next[FETCH_URL_MAX];

/* fd_set, open-coded: <sys/types.h> is the socket world's and tools.h has
   already pulled in NetX Duo's.  256 descriptors is the published maximum. */
static ULONG fetch_readfds[8];


struct FetchTimeval
{
    LONG    tv_secs;
    LONG    tv_micro;
};

#define FETCH_SOCK_STREAM   1

static LONG sock_send(struct Library *base, LONG s, CONST_APTR buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register CONST_APTR      a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-66:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG sock_recv(struct Library *base, LONG s, APTR buf, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = buf;
    register LONG            d1  __asm("d1") = len;
    register LONG            d2  __asm("d2") = 0;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-78:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1), "r" (d2)
                      : "a1", "cc", "memory");
    return res;
}

static LONG sock_close(struct Library *base, LONG s)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG sock_waitselect(struct Library *base, LONG nfds, APTR readfds,
                            struct FetchTimeval *tv)
{
    register struct Library     *a6  __asm("a6") = base;
    register LONG                d0  __asm("d0") = nfds;
    register APTR                a0  __asm("a0") = readfds;
    register APTR                a1  __asm("a1") = NULL;
    register APTR                a2  __asm("a2") = NULL;
    register struct FetchTimeval *a3 __asm("a3") = tv;
    register ULONG              *d1  __asm("d1") = NULL;
    register LONG                res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

static LONG sock_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}


static ULONG str_len(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* Append to a bounded buffer.  Returns FALSE when it would not fit.  Callers
   treat that as a hard error rather than truncating a URL. */
static BOOL str_append(char *dst, ULONG dstlen, ULONG *used, const char *src)
{
    ULONG srclen = str_len(src);
    ULONG i;

    if (*used + srclen + 1 > dstlen)
        return FALSE;

    for (i = 0; i < srclen; i++)
        dst[(*used)++] = src[i];

    dst[*used] = '\0';
    return TRUE;
}


static BOOL url_parse(const char *url, FetchUrl *out)
{
    FetchUrlResult why = fetch_url_parse(url, out);

    if (why == FETCH_URL_OK)
        return TRUE;

    tool_error("\"%s\": %s", (LONG)url, (LONG)fetch_url_error(why));

    return FALSE;
}

static BOOL url_resolve(const FetchUrl *base, const char *ref, FetchUrl *out)
{
    FetchUrlResult why = fetch_url_resolve(base, ref, out);

    if (why == FETCH_URL_OK)
        return TRUE;

    tool_error("\"%s\": %s", (LONG)ref, (LONG)fetch_url_error(why));

    return FALSE;
}


typedef struct FetchIO
{
    struct Library       *sbase;
    struct Library       *tbase;    /* NULL for a plain http: connection   */
    LONG                  sock;
    struct TLSConnection *tls;
    ULONG                 timeout;  /* seconds */
} FetchIO;

static LONG io_write(FetchIO *io, const char *buf, LONG len)
{
    if (io->tls != NULL)
        return TLSWrite(io->tbase, io->tls, (CONST_APTR)buf, len);

    return sock_send(io->sbase, io->sock, (CONST_APTR)buf, len);
}

/*
 * A read with a ceiling on how long it can block.  tls.library applies
 * TLSA_Timeout itself.  The plain socket has no portable NDK timeout, so this
 * polls with WaitSelect() first.  Returns 0 at end of stream, -1 on error, -2
 * on timeout.
 */
#define FETCH_TIMED_OUT (-2)

static LONG io_tls_error(FetchIO *io)
{
    struct TLSInfo ti;

    if (io->tls == NULL)
        return TLS_ERR_IO;

    memset(&ti, 0, sizeof(ti));
    ti.ti_Size = sizeof(ti);

    if (TLSInfo(io->tbase, io->tls, &ti) != TLS_OK)
        return TLS_ERR_IO;

    return ti.ti_Error;
}

static LONG io_read(FetchIO *io, UBYTE *buf, LONG len)
{
    struct FetchTimeval tv;
    LONG                ready;
    LONG                got;

    if (io->tls != NULL)
    {
        got = TLSRead(io->tbase, io->tls, (APTR)buf, len);

        if (got < 0 && io_tls_error(io) == TLS_ERR_TIMEOUT)
            return FETCH_TIMED_OUT;

        return got;
    }

    if (io->timeout > 0)
    {
        ULONG i;

        for (i = 0; i < 8UL; i++)
            fetch_readfds[i] = 0;
        fetch_readfds[(ULONG)io->sock / 32UL] |=
            1UL << ((ULONG)io->sock % 32UL);

        tv.tv_secs  = (LONG)io->timeout;
        tv.tv_micro = 0;

        ready = sock_waitselect(io->sbase, io->sock + 1,
                                (APTR)fetch_readfds, &tv);
        if (ready == 0)
            return FETCH_TIMED_OUT;
        if (ready < 0)
            return -1;
    }

    return sock_recv(io->sbase, io->sock, (APTR)buf, len);
}


/*
 * One request/response.  Returns the HTTP status, 0 on failure.  On a redirect
 * the new location is left in fetch_next.
 */
struct FetchState
{
    BPTR        out;                /* 0 until the first body byte        */
    const char *to;                 /* the TO argument, or NULL           */
    BOOL        headers;
    BOOL        quiet;
    BOOL        noverify;
    LONG        family;             /* -4/-6, else TOOL_AF_UNSPEC         */
    ULONG       total;              /* body bytes written                 */
    BOOL        failed;
};

static BOOL emit(struct FetchState *st, const UBYTE *data, LONG len)
{
    if (len <= 0)
        return TRUE;

    if (st->out == (BPTR)0)
    {
        if (st->to != NULL)
        {
            st->out = Open((CONST_STRPTR)st->to, MODE_NEWFILE);
            if (st->out == (BPTR)0)
            {
                tool_fault(IoErr());
                st->failed = TRUE;
                return FALSE;
            }
        }
        else
        {
            st->out = Output();
        }
    }

    if (Write(st->out, (APTR)data, len) != len)
    {
        tool_fault(IoErr());
        st->failed = TRUE;
        return FALSE;
    }

    return TRUE;
}

/*
 * What main() hands over to the transfer.  File-static because the transfer
 * runs on a different stack, see the StackSwap() note at the bottom.
 */
static struct FetchState fetch_init_state;
static FetchUrl          fetch_init_url;
static ULONG             fetch_init_timeout;

static LONG fetch_run(VOID)
{
    struct FetchState     st = fetch_init_state;
    FetchUrl              u  = fetch_init_url;
    ULONG                 timeout = fetch_init_timeout;
    struct Library       *sbase = NULL;
    struct Library       *tbase = NULL;
    ULONG                 hop;
    LONG                  rc = RETURN_OK;
    LONG                  status = 0;
    FetchHead             head;
    char                  line[128];

    fetch_head_start(&head, fetch_head, (ULONG)sizeof(fetch_head));

    /*
     * This open starts the stack.  The close at the end stops it again if
     * nobody else wants it.  See tool_stack_start().
     */
    sbase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (sbase == NULL)
    {
        if (tool_stack_installed())
            tool_error("the network did not start");
        else
            tool_error("no bsdsocket.library");
        tool_explain_no_stack();
        return RETURN_FAIL;
    }

    for (hop = 0; hop <= (ULONG)FETCH_MAX_HOPS; hop++)
    {
        FetchIO         io;
        ToolConnect     how;
        ToolAddr        address;
        LONG            cerr = 0;
        ULONG       interim = 0;
        LONG        n;
        LONG        why = TLS_OK;

        fetch_head_start(&head, fetch_head, (ULONG)sizeof(fetch_head));

        io.sbase   = sbase;
        io.tbase   = NULL;
        io.sock    = -1;
        io.tls     = NULL;
        io.timeout = timeout;

        if (hop == (ULONG)FETCH_MAX_HOPS)
        {
            tool_error("more than %ld redirects, so giving up",
                       (LONG)FETCH_MAX_HOPS);
            rc = RETURN_ERROR;
            break;
        }

        if (u.secure && tbase == NULL)
        {
            /*
             * Must open version 1, not TLS_LIB_VERSION: every vector used here
             * is original, and demanding the constant would break https:
             * against a working older library.
             */
            tbase = OpenLibrary((CONST_STRPTR)TLS_LIB_NAME, 1UL);
            if (tbase == NULL)
            {
                tool_error("https: needs LIBS:tls.library, and there is none");

                if ((SysBase->AttnFlags & AFF_68020) == 0)
                    tool_printf("  Install it with the full stack.  On a 68000 "
                                "the first handshake takes a minute or two.\n");

                rc = RETURN_FAIL;
                break;
            }
        }

        how.family    = st.family;
        how.socktype  = FETCH_SOCK_STREAM;
        how.port      = u.port;
        how.localport = 0;
        how.timeout   = timeout;
        how.announce  = FALSE;

        io.sock = tool_sock_connect_host(sbase, u.host, &how, &address, &cerr);
        if (io.sock < 0)
        {
            char dotted[TOOL_ADDR_STRLEN];

            tool_addr_text(sbase, &address, dotted, sizeof(dotted));

            rc = RETURN_ERROR;

            switch (io.sock)
            {
            case TOOL_CONNECT_NORESOLVE:
                break;                  /* the resolver has said why */
            case TOOL_CONNECT_NOSOCKET:
                tool_error("no socket (errno %ld)", (LONG)cerr);
                rc = RETURN_FAIL;
                break;
            case TOOL_CONNECT_TIMEDOUT:
                tool_error("%s port %ld did not answer within %lu seconds",
                           (LONG)dotted, (LONG)u.port, timeout);
                break;
            default:
                tool_error("cannot connect to %s port %ld (errno %ld)",
                           (LONG)dotted, (LONG)u.port, (LONG)cerr);
                break;
            }

            break;
        }

        if (u.secure)
        {
            io.tbase = tbase;

            if (st.noverify)
            {
                io.tls = TLSOpen(tbase, (APTR)sbase, io.sock,
                                 TLSA_HostName, (ULONG)u.host,
                                 TLSA_NoVerify, (ULONG)TRUE,
                                 TLSA_Timeout,  timeout * 1000UL,
                                 TLSA_Error,    (ULONG)&why);
            }
            else
            {
                io.tls = TLSOpen(tbase, (APTR)sbase, io.sock,
                                 TLSA_HostName, (ULONG)u.host,
                                 TLSA_Timeout,  timeout * 1000UL,
                                 TLSA_Error,    (ULONG)&why);
            }

            if (io.tls == NULL)
            {
                tool_error("%s: %s", (LONG)u.host,
                           (LONG)TLSErrorString(tbase, why));

                (VOID)sock_close(sbase, io.sock);
                rc = RETURN_ERROR;
                break;
            }

            if (!st.quiet)
            {
                struct TLSInfo info;
                UBYTE         *info_bytes = (UBYTE *)&info;
                ULONG          i;

                /*
                 * Zeroed first.  This command opens tls.library with version
                 * 1, so the library can predate ti_Resumed and leaves that
                 * field untouched.  Uninitialised stack would then decide
                 * whether " (resumed session)" prints.
                 */
                for (i = 0; i < (ULONG)sizeof(info); i++)
                    info_bytes[i] = 0;

                info.ti_Size = (ULONG)sizeof(info);
                if (TLSInfo(tbase, io.tls, &info) == 0)
                {
                    tool_printf("%s: TLS 0x%lx, ciphersuite 0x%lx, "
                                "%ld certificate(s), %lu.%lu s%s\n",
                                (LONG)u.host, info.ti_Version,
                                info.ti_CipherSuite, info.ti_ChainDepth,
                                info.ti_HandshakeMillis / 1000UL,
                                (info.ti_HandshakeMillis % 1000UL) / 100UL,
                                (LONG)(info.ti_Resumed
                                           ? " (resumed session)" : ""));
                    tool_printf("  chain %s, %s\n",
                                (LONG)(info.ti_Verified ? "verified"
                                                        : "NOT VERIFIED"),
                                (LONG)(info.ti_ExpiryChecked
                                           ? "validity dates checked"
                                           : "validity dates NOT checked "
                                             "(the clock is unset)"));
                }
            }
        }

        {
            ULONG used = 0;
            BOOL  ok = TRUE;
            char  authority[FETCH_AUTHORITY_MAX];

            /*
             * RFC 9112 section 3.2: "a client MUST send a field value for Host
             * that is identical to that authority component" of the target
             * URI.  A non-default port therefore belongs in it.  Without one,
             * "fetch http://host:8080/" reaches whichever virtual host the
             * server answers for port 80.
             */
            if (!fetch_url_authority(&u, authority, sizeof(authority)))
            {
                tool_error("that URL does not fit in a request");
                rc = RETURN_ERROR;
                goto hop_done;
            }

            fetch_request[0] = '\0';
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  "GET ");
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  u.path);
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  " HTTP/1.0\r\nHost: ");
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  authority);
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  "\r\nUser-Agent: AmiNetXDuo-fetch/1.0 (m68k)"
                                  "\r\nConnection: close\r\n\r\n");
            if (!ok)
            {
                tool_error("that URL does not fit in a request");
                rc = RETURN_ERROR;
                goto hop_done;
            }

            n = io_write(&io, fetch_request, (LONG)used);
            if (n != (LONG)used)
            {
                if (io.tls != NULL)
                {
                    LONG err = io_tls_error(&io);

                    tool_error("cannot send the request: %s"
                               " (%ld), %ld of %ld bytes",
                               (LONG)TLSErrorString(io.tbase, err),
                               (LONG)err, (LONG)n, (LONG)used);
                    rc = RETURN_ERROR;
                    goto hop_done;
                }
                tool_error("cannot send the request");
                rc = RETURN_ERROR;
                goto hop_done;
            }
        }

        fetch_next[0] = '\0';

        while ((n = io_read(&io, fetch_chunk, (LONG)sizeof(fetch_chunk))) > 0)
        {
            ULONG at = 0;

            if (tool_break())
            {
                tool_fault(ERROR_BREAK);
                rc = RETURN_WARN;
                goto hop_done;
            }

            for (;;)
            {
                if (!head.complete)
                {
                    if (at >= (ULONG)n)
                        break;              /* more of it is still on the wire */

                    at += fetch_head_feed(&head, &fetch_chunk[at],
                                          (ULONG)n - at);
                    if (!head.complete)
                        break;
                }

                if (status == 0)
                {
                    const char *loc;

                    status = (LONG)fetch_head_status(&head);

                    if (status == 0)
                    {
                        tool_error("%s did not answer with HTTP", (LONG)u.host);
                        rc = RETURN_ERROR;
                        goto hop_done;
                    }

                    /*
                     * RFC 9110 15.2: a 1xx is an interim response, terminated
                     * by the end of its header section and carrying no
                     * content.  Discard it and read the next block.  Taking
                     * it for the final answer wrote the real response into the
                     * user's file as body and reported success.
                     */
                    if (fetch_head_interim((ULONG)status))
                    {
                        if (++interim > (ULONG)FETCH_MAX_INTERIM)
                        {
                            tool_error("%s sent more than %ld interim "
                                       "responses and no answer",
                                       (LONG)u.host, (LONG)FETCH_MAX_INTERIM);
                            rc = RETURN_ERROR;
                            goto hop_done;
                        }

                        fetch_head_start(&head, fetch_head,
                                         (ULONG)sizeof(fetch_head));
                        status = 0;
                        continue;
                    }

                    /*
                     * Decided before any body byte is written, so the user's
                     * file is never opened for an answer we are not going to
                     * keep.
                     */
                    if (status >= 300 && status < 400)
                    {
                        loc = fetch_head_field(&head, "location:");
                        if (loc != NULL &&
                            fetch_head_value(loc, fetch_next,
                                             sizeof(fetch_next)))
                        {
                            goto hop_done;
                        }

                        if (head.truncated)
                        {
                            tool_error("the %ld redirect gave no Location: in "
                                       "the first %ld bytes of headers",
                                       (LONG)status, (LONG)FETCH_HEAD_MAX);
                            rc = RETURN_ERROR;
                            goto hop_done;
                        }
                    }

                    if (st.headers)
                    {
                        if (!emit(&st, (const UBYTE *)head.buf,
                                  (LONG)head.len))
                        {
                            rc = RETURN_FAIL;
                            goto hop_done;
                        }

                        /* Asked to show the headers, and they did not fit. */
                        if (head.truncated)
                            tool_error("showing the first %ld bytes of headers "
                                       "only", (LONG)FETCH_HEAD_MAX);
                    }
                }

                if (at < (ULONG)n)
                {
                    if (!emit(&st, &fetch_chunk[at], (LONG)((ULONG)n - at)))
                    {
                        rc = RETURN_FAIL;
                        goto hop_done;
                    }
                    st.total += (ULONG)n - at;
                    at = (ULONG)n;
                }

                break;
            }
        }

        if (n == FETCH_TIMED_OUT)
        {
            tool_error("%s stopped answering after %lu seconds",
                       (LONG)u.host, timeout);
            rc = RETURN_ERROR;
            goto hop_done;
        }

        if (n < 0)
        {
            if (io.tls != NULL)
            {
                LONG err = io_tls_error(&io);

                tool_error("the connection failed: %s (%ld)",
                           (LONG)TLSErrorString(tbase, err), (LONG)err);
            }
            else
                tool_error("the connection failed (errno %ld)",
                           (LONG)sock_errno(sbase));
            rc = RETURN_ERROR;
            goto hop_done;
        }

        if (!head.complete)
        {
            ULONG hs_ms = 0;

            if (io.tls != NULL)
            {
                struct TLSInfo  hi;
                UBYTE          *hb = (UBYTE *)&hi;
                ULONG           k;

                for (k = 0; k < (ULONG)sizeof(hi); k++)
                    hb[k] = 0;
                hi.ti_Size = (ULONG)sizeof(hi);

                if (TLSInfo(tbase, io.tls, &hi) == 0)
                    hs_ms = hi.ti_HandshakeMillis;
            }

            if (hs_ms >= 10000UL)
            {
                tool_error("%s closed the connection without answering.  The "
                           "handshake took %lu.%lu s.  Some servers allow "
                           "about 15 s for one.  This machine is slower than "
                           "that budget, and nothing here is misconfigured",
                           (LONG)u.host,
                           (LONG)(hs_ms / 1000UL),
                           (LONG)((hs_ms % 1000UL) / 100UL));
            }
            else if (hs_ms != 0)
            {
                tool_error("%s closed the connection without answering "
                           "(handshake %lu.%lu s)",
                           (LONG)u.host,
                           (LONG)(hs_ms / 1000UL),
                           (LONG)((hs_ms % 1000UL) / 100UL));
            }
            else
            {
                tool_error("%s closed the connection without answering",
                           (LONG)u.host);
            }

            rc = RETURN_ERROR;
        }

    hop_done:
        if (io.tls != NULL)
            TLSClose(tbase, io.tls);
        if (io.sock >= 0)
            (VOID)sock_close(sbase, io.sock);

        if (rc != RETURN_OK || fetch_next[0] == '\0')
            break;

        {
            FetchUrl next;
            BOOL     was_secure = u.secure;

            /*
             * RFC 9110 10.2.2: the Location value is a URI reference, and the
             * target is it resolved against the URI this request was made to.
             * Most of them are relative.  "about.html" and "docs/x.html" were
             * both handed to the absolute parser, which made them a host
             * called about.html and a host called docs.
             */
            if (!url_resolve(&u, fetch_next, &next))
            {
                rc = RETURN_ERROR;
                break;
            }

            if (was_secure && !next.secure)
            {
                tool_error("%s redirects to an unencrypted URL, which this "
                           "command does not follow", (LONG)u.host);
                rc = RETURN_ERROR;
                break;
            }

            u = next;
            status = 0;

            if (!st.quiet)
                tool_printf("  -> %s\n", (LONG)fetch_next);
        }
    }

    if (rc == RETURN_OK && !st.failed && status != 0)
    {
        fetch_head_first_line(&head, line, sizeof(line));

        if (!st.quiet)
        {
            tool_printf("%s\n", (LONG)line);
            tool_printf("%lu bytes -> %s\n", st.total, (LONG)st.to);
        }

        /* A 4xx or 5xx is a successful transfer of a bad answer: the body is
           the server's explanation, so it is written, but the return code says
           the fetch did not get what was asked for. */
        if (status >= 400)
        {
            tool_error("%s", (LONG)line);
            rc = RETURN_WARN;
        }
    }

    if (st.failed)
        rc = RETURN_FAIL;

    if (st.out != (BPTR)0 && st.to != NULL)
        Close(st.out);

    if (tbase != NULL)
        CloseLibrary(tbase);
    CloseLibrary(sbase);

    return rc;
}


/*
 * Hazard: fetch_trampoline() must have no locals and no arguments, and must
 * stay noinline.  Between the two StackSwap() calls the stack pointer belongs
 * to the new stack, so a stack-based local of its own would read wrong memory.
 */
#define FETCH_STACK_SIZE    (64UL * 1024UL)

static struct StackSwapStruct fetch_sss;
static LONG                   fetch_result;

static __attribute__((noinline)) VOID fetch_trampoline(VOID)
{
    StackSwap(&fetch_sss);
    fetch_result = fetch_run();
    StackSwap(&fetch_sss);
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT];
    struct RDArgs *rda;
    APTR           stack;
    ULONG          timeout;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_URL]      = 0;
    args[ARG_TO]       = 0;
    args[ARG_HEADERS]  = 0;
    args[ARG_QUIET]    = 0;
    args[ARG_NOVERIFY] = 0;
    args[ARG_TIMEOUT]  = 0;
    args[ARG_IPV4]     = 0;
    args[ARG_IPV6]     = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("[-4|-6] <url> [TO <file>]",
                   "An http: or https: URL.  Without TO the body goes to "
                   "standard output.");
        return RETURN_ERROR;
    }

    fetch_init_state.out      = (BPTR)0;
    fetch_init_state.to       = (const char *)args[ARG_TO];
    fetch_init_state.headers  = (args[ARG_HEADERS]  != 0) ? TRUE : FALSE;
    fetch_init_state.quiet    = (args[ARG_QUIET]    != 0) ? TRUE : FALSE;
    fetch_init_state.noverify = (args[ARG_NOVERIFY] != 0) ? TRUE : FALSE;
    fetch_init_state.total    = 0;
    fetch_init_state.failed   = FALSE;

    if (!tool_arg_family(args[ARG_IPV4], args[ARG_IPV6],
                         &fetch_init_state.family))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * Without TO the body goes to standard output, so a progress line would
     * corrupt it.  With TO that channel is free and the summary can print.
     */
    if (fetch_init_state.to == NULL)
        fetch_init_state.quiet = TRUE;

    timeout = FETCH_DEFAULT_TIMEOUT;

    if (args[ARG_TIMEOUT] != 0)
    {
        LONG seconds = *(LONG *)args[ARG_TIMEOUT];

        if (seconds < 0 || seconds > FETCH_TIMEOUT_MAX)
        {
            tool_error("TIMEOUT must be between 0 and %ld seconds",
                       (LONG)FETCH_TIMEOUT_MAX);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        timeout = (ULONG)seconds;
    }

    if (timeout == 0)
        timeout = FETCH_DEFAULT_TIMEOUT;
    fetch_init_timeout = timeout;

    if (!url_parse((const char *)args[ARG_URL], &fetch_init_url))
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    stack = AllocMem(FETCH_STACK_SIZE, MEMF_ANY);
    if (stack != NULL)
    {
        fetch_sss.stk_Lower   = stack;
        fetch_sss.stk_Upper   = (ULONG)stack + FETCH_STACK_SIZE;
        fetch_sss.stk_Pointer = (APTR)((ULONG)stack + FETCH_STACK_SIZE);

        fetch_trampoline();

        FreeMem(stack, FETCH_STACK_SIZE);
    }
    else
    {
        /* Out of memory for a stack.  http: fits on the caller's, so try
           rather than refuse.  https: would not have worked on a machine this
           short of memory anyway. */
        fetch_result = fetch_run();
    }

    FreeArgs(rda);
    return fetch_result;
}
