/*
 * fetch -- retrieve an http:// or https:// URL.
 *
 *     fetch URL/A,TO/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K
 *
 * The traveller for tls.library.  Everything else in this tree either builds
 * TLS or tests it; this is the command that uses it, and it is deliberately
 * the same program either way: the scheme decides at run time whether
 * LIBS:tls.library is opened at all, so a build made with -DAMINETXDUO_TLS=OFF
 * still ships a working `fetch` for http: URLs and says something legible if
 * it is asked for an https: one.
 *
 *   bsdsocket.library's published vectors, called by hand, and tls.library's
 *   eight (include/aminetxduo/tlslib.h).  Nothing here links our stack: this
 *   is an ordinary Amiga network application and it is written like one, so
 *   that it is also a worked example.
 *
 *   Opening bsdsocket.library is what starts the network -- the library is
 *   self-starting -- and closing it is what stops it again, unless something
 *   else (Online, AddNetInterface) is holding it open.  That is the normal
 *   contract for an application, and unlike ping or ShowNetStatus, a command
 *   whose whole job is to use the network has no business refusing to bring it
 *   up.
 *
 *   No chunked transfer decoding and no keep-alive: the request goes out as
 *   HTTP/1.0 with Connection: close, so the body is "everything until the
 *   other end hangs up" and there is no framing to get wrong.  No POST, no
 *   authentication, no cookies, no resume.  It follows up to five redirects
 *   and refuses to follow one that steps down from https: to http:, because
 *   silently dropping the encryption a user asked for is worse than stopping.
 *
 *   And there is one it cannot do anything about, though it is not the one an
 *   earlier version of this comment claimed: on a certificate chain of three
 *   or more, a 14 MHz 68020 spends longer verifying than a busy front end is
 *   willing to wait for a ClientKeyExchange, and the peer closes.  This
 *   command says "the connection is closed" and returns 10, which is the
 *   right answer; there is no third-party fix for it short of a faster
 *   handshake.  See docs/RESEARCH.md.  (The crash that used to be described
 *   here was the EMULATOR dying of SIGPIPE on the host, not the Amiga.)
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#include <exec/memory.h>
#include <exec/tasks.h>

#include "aminetxduo/tlslib.h"

const char *const tool_name = "fetch";

static const char version_tag[] __attribute__((used)) =
    "$VER: fetch 1.0 (25.7.2026)";

#define TEMPLATE    "URL/A,TO/K,HEADERS/S,QUIET/S,NOVERIFY/S,TIMEOUT/N/K"

enum
{
    ARG_URL = 0,
    ARG_TO,
    ARG_HEADERS,
    ARG_QUIET,
    ARG_NOVERIFY,
    ARG_TIMEOUT,
    ARG_COUNT
};

#define FETCH_DEFAULT_TIMEOUT   120UL       /* seconds */
#define FETCH_MAX_HOPS          5

#define FETCH_HOST_MAX          128
#define FETCH_PATH_MAX          512
#define FETCH_URL_MAX           640
/*
 * 3072 was too small for the real web: www.github.com answers a plain GET with
 * more than that in headers alone, so `fetch` completed the TLS handshake,
 * received a perfectly good 301, and then refused it. 16 KB is past anything
 * ordinary -- and running out is no longer fatal either, see head_trunc below.
 */
#define FETCH_HEAD_MAX          16384       /* status line + headers        */
#define FETCH_CHUNK             4096

/*
 * Static, not automatic: a Shell command gets whatever stack the Shell has --
 * 4 KB on a stock Kickstart 3.1 -- and these are 10 KB between them.  Same
 * reasoning as src/tools/toolssmoke.c.
 */
static UBYTE fetch_chunk[FETCH_CHUNK];
static char  fetch_head[FETCH_HEAD_MAX];
static char  fetch_request[FETCH_PATH_MAX + FETCH_HOST_MAX + 160];
static char  fetch_next[FETCH_URL_MAX];

/* fd_set, open-coded.  <sys/types.h> is the socket world's and tools.h has
   already pulled in NetX Duo's; 256 descriptors is the published maximum. */
static ULONG fetch_readfds[8];


/* ---------------------------------------------------- bsdsocket, by hand --- */

/*
 * Called through the LVOs rather than the NDK inlines, for the reason
 * src/tools/tool_diag.c gives: a command must not link the whole socket
 * surface to make one connection, and an inline header that assumes a global
 * SocketBase hides the very ABI this program exists to demonstrate.
 */

struct FetchSockAddrIn
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;               /* network order == our order on m68k */
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

struct FetchHostEnt
{
    char   *h_name;
    char  **h_aliases;
    LONG    h_addrtype;
    LONG    h_length;
    char  **h_addr_list;
};

struct FetchTimeval
{
    LONG    tv_secs;
    LONG    tv_micro;
};

#define FETCH_AF_INET       2
#define FETCH_SOCK_STREAM   1

static LONG sock_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = domain;
    register LONG            d1  __asm("d1") = type;
    register LONG            d2  __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG sock_connect(struct Library *base, LONG s, APTR name, LONG len)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            d0  __asm("d0") = s;
    register APTR            a0  __asm("a0") = name;
    register LONG            d1  __asm("d1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

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

static struct FetchHostEnt *sock_gethostbyname(struct Library *base,
                                               const char *name)
{
    register struct Library     *a6  __asm("a6") = base;
    register const char         *a0  __asm("a0") = name;
    register struct FetchHostEnt *res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
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


/* --------------------------------------------------------------- strings --- */

static ULONG str_len(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* Append to a bounded buffer.  Returns FALSE when it would not fit, which
   every caller treats as a hard error rather than truncating a URL. */
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


/* ------------------------------------------------------------------- URL --- */

typedef struct FetchUrl
{
    BOOL    secure;
    UWORD   port;
    char    host[FETCH_HOST_MAX];
    char    path[FETCH_PATH_MAX];
} FetchUrl;

/*
 * scheme://host[:port][/path][#fragment], plus the courtesy of accepting a
 * bare "host/path" as http.  A fragment is dropped: it is the browser's, and
 * sending it would be wrong.
 */
static BOOL url_parse(const char *url, FetchUrl *out)
{
    const char *p = url;
    const char *sep;
    ULONG       i;

    out->secure = FALSE;
    out->port   = 80;

    for (sep = p; sep[0] != '\0'; sep++)
    {
        if (sep[0] == '/' && sep[1] == '/' && sep > p && sep[-1] == ':')
            break;
        if (sep[0] == '/')          /* a path before any "://": no scheme */
        {
            sep = NULL;
            break;
        }
    }

    if (sep != NULL && sep[0] != '\0')
    {
        ULONG scheme_len = (ULONG)(sep - p) - 1;

        if (scheme_len == 5 && tool_stricmp_n(p, "https", 5) == 0)
        {
            out->secure = TRUE;
            out->port   = 443;
        }
        else if (scheme_len == 4 && tool_stricmp_n(p, "http", 4) == 0)
        {
            out->secure = FALSE;
            out->port   = 80;
        }
        else
        {
            tool_error("\"%s\" is not an http: or https: URL", (LONG)url);
            return FALSE;
        }

        p = sep + 2;
    }

    /* host[:port] */
    i = 0;
    while (p[0] != '\0' && p[0] != '/' && p[0] != ':' &&
           p[0] != '?' && p[0] != '#')
    {
        if (i + 1 >= (ULONG)FETCH_HOST_MAX)
        {
            tool_error("the host name in that URL is too long");
            return FALSE;
        }
        out->host[i++] = p[0];
        p++;
    }
    out->host[i] = '\0';

    if (out->host[0] == '\0')
    {
        tool_error("\"%s\" has no host name in it", (LONG)url);
        return FALSE;
    }

    if (p[0] == ':')
    {
        ULONG port = 0;

        p++;
        if (p[0] < '0' || p[0] > '9')
        {
            tool_error("the port number in that URL is not a number");
            return FALSE;
        }
        while (p[0] >= '0' && p[0] <= '9')
        {
            port = (port * 10UL) + (ULONG)(p[0] - '0');
            if (port > 65535UL)
            {
                tool_error("the port number in that URL is out of range");
                return FALSE;
            }
            p++;
        }
        out->port = (UWORD)port;
    }

    /* path */
    i = 0;
    if (p[0] != '/')
        out->path[i++] = '/';

    while (p[0] != '\0' && p[0] != '#')
    {
        if (i + 1 >= (ULONG)FETCH_PATH_MAX)
        {
            tool_error("the path in that URL is too long");
            return FALSE;
        }
        out->path[i++] = p[0];
        p++;
    }
    out->path[i] = '\0';

    return TRUE;
}


/* -------------------------------------------------------------- transfer --- */

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
 * A read with a ceiling on how long it may block.
 *
 * tls.library has TLSA_Timeout and applies it itself.  The plain socket has
 * no timeout the NDK spells portably, so this polls with WaitSelect() first --
 * a published vector, and the one every Amiga network program already uses.
 * 0 is end of stream, -1 is an error, -2 is the timeout.
 */
#define FETCH_TIMED_OUT (-2)

static LONG io_read(FetchIO *io, UBYTE *buf, LONG len)
{
    struct FetchTimeval tv;
    LONG                ready;

    if (io->tls != NULL)
        return TLSRead(io->tbase, io->tls, (APTR)buf, len);

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


/* ------------------------------------------------------------- responses --- */

/* The numeric status out of "HTTP/1.1 301 Moved Permanently", or 0. */
static ULONG head_status(const char *head)
{
    const char *p = head;
    ULONG       code = 0;
    ULONG       i;

    while (p[0] != '\0' && p[0] != ' ')
        p++;
    while (p[0] == ' ')
        p++;

    for (i = 0; i < 3UL; i++)
    {
        if (p[i] < '0' || p[i] > '9')
            return 0;
        code = (code * 10UL) + (ULONG)(p[i] - '0');
    }

    return code;
}

/* The value of a header, case-insensitively, or NULL.  `name` includes the
   colon: "location:". */
static const char *head_field(const char *head, const char *name)
{
    ULONG namelen = str_len(name);
    ULONG i;

    /* Skip the status line -- a header never appears on it. */
    for (i = 0; head[i] != '\0'; i++)
    {
        if (head[i] != '\n')
            continue;

        i++;
        if (tool_stricmp_n(&head[i], name, namelen) != 0)
            continue;

        i += namelen;
        while (head[i] == ' ' || head[i] == '\t')
            i++;
        return &head[i];
    }

    return NULL;
}

/* Copy one header value, stopping at the end of its line. */
static BOOL head_value(const char *value, char *dst, ULONG dstlen)
{
    ULONG i = 0;

    while (value[i] != '\0' && value[i] != '\r' && value[i] != '\n')
    {
        if (i + 1 >= dstlen)
            return FALSE;
        dst[i] = value[i];
        i++;
    }

    dst[i] = '\0';
    return (BOOL)(i > 0);
}

/* The status line on its own, for the summary. */
static VOID head_first_line(const char *head, char *dst, ULONG dstlen)
{
    ULONG i = 0;

    while (head[i] != '\0' && head[i] != '\r' && head[i] != '\n' &&
           i + 1 < dstlen)
    {
        dst[i] = head[i];
        i++;
    }

    dst[i] = '\0';
}


/* ------------------------------------------------------------------ main --- */

/*
 * One request/response.  Returns the HTTP status, 0 on failure.  When the
 * answer is a redirect the new location is left in fetch_next.
 *
 * `out` is opened lazily by the caller through open_output(), so a redirect
 * never truncates the user's file and a failure before the first body byte
 * leaves it alone.
 */
struct FetchState
{
    BPTR        out;                /* 0 until the first body byte        */
    const char *to;                 /* the TO argument, or NULL           */
    BOOL        headers;
    BOOL        quiet;
    BOOL        noverify;
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
 * runs on a different stack from main() -- see the StackSwap() note at the
 * bottom of this file.
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
    struct FetchHostEnt  *he;
    ULONG                 hop;
    LONG                  rc = RETURN_OK;
    LONG                  status = 0;
    char                  line[128];

    /* ---- the network ---------------------------------------------------- */

    /*
     * This open is what starts the stack, and the close at the end is what
     * stops it again if nobody else wants it.  See tool_stack_start().
     */
    sbase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (sbase == NULL)
    {
        if (tool_stack_installed())
            tool_error("the network would not start");
        else
            tool_error("there is no bsdsocket.library on this machine");
        tool_explain_no_stack();
        return RETURN_FAIL;
    }

    for (hop = 0; hop <= (ULONG)FETCH_MAX_HOPS; hop++)
    {
        FetchIO     io;
        struct FetchSockAddrIn sin;
        ULONG       address = 0;
        ULONG       head_len = 0;
        BOOL        in_head = TRUE;
        BOOL        head_trunc = FALSE;
        char        head_tail[4] = { 0, 0, 0, 0 };
        LONG        n;
        LONG        why = TLS_OK;
        ULONG       i;

        io.sbase   = sbase;
        io.tbase   = NULL;
        io.sock    = -1;
        io.tls     = NULL;
        io.timeout = timeout;

        if (hop == (ULONG)FETCH_MAX_HOPS)
        {
            tool_error("more than %ld redirects; giving up",
                       (LONG)FETCH_MAX_HOPS);
            rc = RETURN_ERROR;
            break;
        }

        /* ---- tls.library, only when the URL asks for it ------------------ */

        if (u.secure && tbase == NULL)
        {
            /*
             * VERSION 1, not TLS_LIB_VERSION, because 1 is what this command
             * uses: TLSOpen, TLSWrite, TLSRead, TLSClose, TLSInfo and
             * TLSErrorString are all original vectors.  Asking for the
             * constant would mean a recompile silently demanded a newer
             * library than the transfer needs, and a user with a working
             * older pair would lose https: for nothing.  ti_Resumed is a
             * version-2 TLSInfo field and is handled by zeroing the structure
             * below, which is what makes asking for 1 safe rather than lucky.
             */
            tbase = OpenLibrary((CONST_STRPTR)TLS_LIB_NAME, 1UL);
            if (tbase == NULL)
            {
                tool_error("https: needs LIBS:tls.library, and there is none");
                tool_advise_blank();

                /*
                 * Why the CPU is tested here. On a 68000 there is no
                 * tls.library to install -- the archive deliberately carries
                 * none, because the encryption needs a 68020 -- so telling
                 * that user to go and install it sends them looking for a
                 * file that does not exist. One binary serves every CPU, so
                 * the only place that difference can be known is at run time,
                 * and the only moment it matters is this one.
                 *
                 * It also puts the explanation where the person who hit the
                 * problem is standing, which is why the installer no longer
                 * says any of this on its way out.
                 */
                if ((SysBase->AttnFlags & AFF_68020) == 0)
                {
                    tool_advise("This machine is a 68000, and there is no "
                                "tls.library for one -- the encryption needs "
                                "a 68020. Nothing is missing from your "
                                "installation.");
                }
                else
                {
                    tool_advise("Install it from the same archive as this "
                                "bsdsocket.library -- the two are a pair and "
                                "are not interchangeable between builds.");
                }

                tool_advise("http: URLs work without it.");
                rc = RETURN_FAIL;
                break;
            }
        }

        /* ---- resolve ----------------------------------------------------- */

        if (!ami_config_parse_ip(u.host, &address))
        {
            he = sock_gethostbyname(sbase, u.host);
            if (he == NULL || he->h_addr_list == NULL ||
                he->h_addr_list[0] == NULL || he->h_length != 4)
            {
                tool_error("cannot resolve \"%s\"", (LONG)u.host);
                tool_explain_resolve(u.host, AMI_NET_ERR_NONAME);
                rc = RETURN_ERROR;
                break;
            }

            for (i = 0; i < 4UL; i++)
                address = (address << 8) |
                          (ULONG)(UBYTE)he->h_addr_list[0][i];
        }

        /* ---- connect ----------------------------------------------------- */

        for (i = 0; i < (ULONG)sizeof(sin.sin_zero); i++)
            sin.sin_zero[i] = 0;

        sin.sin_len    = (UBYTE)sizeof(sin);
        sin.sin_family = (UBYTE)FETCH_AF_INET;
        sin.sin_port   = u.port;
        sin.sin_addr   = address;

        io.sock = sock_socket(sbase, FETCH_AF_INET, FETCH_SOCK_STREAM, 0);
        if (io.sock < 0)
        {
            tool_error("no socket (errno %ld)", (LONG)sock_errno(sbase));
            rc = RETURN_FAIL;
            break;
        }

        if (sock_connect(sbase, io.sock, (APTR)&sin, (LONG)sizeof(sin)) != 0)
        {
            char dotted[16];

            ami_config_format_ip(address, dotted, sizeof(dotted));
            tool_error("cannot connect to %s port %ld (errno %ld)",
                       (LONG)dotted, (LONG)u.port, (LONG)sock_errno(sbase));
            (VOID)sock_close(sbase, io.sock);
            rc = RETURN_ERROR;
            break;
        }

        /* ---- the handshake, for an https: URL ----------------------------- */

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

                if (why == TLS_ERR_TRUSTSTORE)
                {
                    tool_advise_blank();
                    tool_advise("DEVS:Internet/certificates is the list of "
                                "certificate authorities this machine trusts. "
                                "Install it from the archive, or build one "
                                "with tools/mkcertstore.py.");
                }
                else if (why == TLS_ERR_UNTRUSTED || why == TLS_ERR_HOSTNAME ||
                         why == TLS_ERR_EXPIRED)
                {
                    tool_advise_blank();
                    tool_advise("NOVERIFY connects anyway, encrypted but NOT "
                                "authenticated -- anyone in the path can be "
                                "the other end.  Do not use it to reach a "
                                "site you do not control.");
                }

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
                 * Zeroed first, and that is load-bearing rather than tidy.
                 * This command opens tls.library with version 1, so it can be
                 * talking to a library that predates ti_Resumed -- which will
                 * fill the fields it knows about and leave the rest untouched.
                 * Uninitialised stack would then decide whether this prints
                 * "(resumed session)".  Zeroed, an older library simply says
                 * no, which is the truth: it cannot resume.
                 */
                for (i = 0; i < (ULONG)sizeof(info); i++)
                    info_bytes[i] = 0;

                info.ti_Size = (ULONG)sizeof(info);
                if (TLSInfo(tbase, io.tls, &info) == 0)
                {
                    /*
                     * "resumed" is worth a word of its own rather than being
                     * left to be inferred from the time.  A resumed handshake
                     * sends no certificate and verifies no signature, so
                     * ti_ChainDepth reads 0 and the seconds read 0.2 -- both
                     * of which look like something went wrong until you know
                     * that they are the point.
                     */
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

        /* ---- the request -------------------------------------------------- */

        {
            ULONG used = 0;
            BOOL  ok = TRUE;

            fetch_request[0] = '\0';
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  "GET ");
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  u.path);
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  " HTTP/1.0\r\nHost: ");
            ok = ok && str_append(fetch_request, sizeof(fetch_request), &used,
                                  u.host);
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
                tool_error("the request could not be sent");
                rc = RETURN_ERROR;
                goto hop_done;
            }
        }

        /* ---- the response -------------------------------------------------- */

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

            /*
             * Running past the buffer stops us KEEPING headers; it does not
             * stop us reading them. The end of the block is found from the
             * last four bytes seen rather than from the last four bytes
             * stored, so an unusually chatty server costs us the tail of its
             * headers instead of the whole transfer. What we actually need --
             * the status line, and Location: on a redirect -- is at the front,
             * and the two places that care check head_trunc before complaining
             * about not finding something.
             */
            while (in_head && at < (ULONG)n)
            {
                char c = (char)fetch_chunk[at++];

                if (head_len + 1 < (ULONG)FETCH_HEAD_MAX)
                    fetch_head[head_len++] = c;
                else
                    head_trunc = TRUE;

                head_tail[0] = head_tail[1];
                head_tail[1] = head_tail[2];
                head_tail[2] = head_tail[3];
                head_tail[3] = c;

                if ((head_tail[0] == '\r' && head_tail[1] == '\n' &&
                     head_tail[2] == '\r' && head_tail[3] == '\n') ||
                    (head_tail[2] == '\n' && head_tail[3] == '\n'))
                {
                    in_head = FALSE;
                }
            }

            if (in_head)
                continue;

            if (status == 0)
            {
                const char *loc;

                fetch_head[head_len] = '\0';
                status = (LONG)head_status(fetch_head);

                if (status == 0)
                {
                    tool_error("%s did not answer with HTTP", (LONG)u.host);
                    rc = RETURN_ERROR;
                    goto hop_done;
                }

                /*
                 * A redirect is decided here, before a single body byte is
                 * written, so the user's file is never opened for an answer
                 * that turns out not to be the answer.
                 */
                if (status >= 300 && status < 400)
                {
                    loc = head_field(fetch_head, "location:");
                    if (loc != NULL &&
                        head_value(loc, fetch_next, sizeof(fetch_next)))
                    {
                        goto hop_done;
                    }

                    /*
                     * The one case where losing the tail of the headers costs
                     * something. Say so, rather than reporting a redirect
                     * with nowhere to go as though the server sent none.
                     */
                    if (head_trunc)
                    {
                        tool_error("the %ld redirect gave no Location: in the "
                                   "first %ld bytes of headers",
                                   (LONG)status, (LONG)FETCH_HEAD_MAX);
                        rc = RETURN_ERROR;
                        goto hop_done;
                    }
                }

                if (st.headers)
                {
                    if (!emit(&st, (const UBYTE *)fetch_head, (LONG)head_len))
                    {
                        rc = RETURN_FAIL;
                        goto hop_done;
                    }

                    /* Asked to show the headers, and they did not all fit. */
                    if (head_trunc)
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
                tool_error("the connection failed: %s",
                           (LONG)TLSErrorString(tbase, TLS_ERR_IO));
            else
                tool_error("the connection failed (errno %ld)",
                           (LONG)sock_errno(sbase));
            rc = RETURN_ERROR;
            goto hop_done;
        }

        if (in_head)
        {
            tool_error("%s closed the connection without answering",
                       (LONG)u.host);
            rc = RETURN_ERROR;
        }

    hop_done:
        if (io.tls != NULL)
            TLSClose(tbase, io.tls);
        if (io.sock >= 0)
            (VOID)sock_close(sbase, io.sock);

        if (rc != RETURN_OK || fetch_next[0] == '\0')
            break;

        /* ---- follow the redirect ------------------------------------------ */

        {
            FetchUrl next;
            BOOL     was_secure = u.secure;

            if (fetch_next[0] == '/' && fetch_next[1] == '/')
            {
                /* Scheme-relative.  RFC 7231 asks for an absolute URI here and
                   nothing on the public web sends this; say so rather than
                   guess a scheme on the user's behalf. */
                tool_error("%s redirects to \"%s\", which this command does "
                           "not follow", (LONG)u.host, (LONG)fetch_next);
                rc = RETURN_ERROR;
                break;
            }
            else if (fetch_next[0] == '/')
            {
                /* Same host and scheme, new path. */
                next = u;
                tool_copy_string(next.path, sizeof(next.path), fetch_next);
            }
            else if (!url_parse(fetch_next, &next))
            {
                rc = RETURN_ERROR;
                break;
            }

            if (was_secure && !next.secure)
            {
                tool_error("%s redirects to an unencrypted URL; not following",
                           (LONG)u.host);
                tool_advise_blank();
                tool_advise("Ask for that http: URL directly if you meant it.");
                rc = RETURN_ERROR;
                break;
            }

            u = next;
            status = 0;

            if (!st.quiet)
                tool_printf("  -> %s\n", (LONG)fetch_next);
        }
    }

    /* ---- the summary ---------------------------------------------------- */

    if (rc == RETURN_OK && !st.failed && status != 0)
    {
        head_first_line(fetch_head, line, sizeof(line));

        if (!st.quiet)
        {
            tool_printf("%s\n", (LONG)line);
            tool_printf("%lu bytes -> %s\n", st.total, (LONG)st.to);
        }

        /* A 4xx or 5xx is a successful transfer of a bad answer.  The body is
           the server's explanation and is worth keeping, so it is written --
           but the return code says the fetch did not get what was asked for. */
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


/* ------------------------------------------------------------- the stack --- */

/*
 * A program that opens tls.library must not run on a Shell's stack.
 *
 * Two measurements, on the emulated 68020.  A command started by the Kickstart
 * 3.1 Shell gets 4,096 bytes (tc_SPUpper - tc_SPLower), and by the time this
 * one reaches TLSOpen() 2,736 of them are left.  And tls.library brackets its
 * caller into ThreadX to reach the stack, which -- see
 * port/threadx-amiga/src/tx_amiga_adopt.c -- hands _tx_thread_create() the
 * CALLER'S stack region as the ThreadX thread stack.  So those 2,736 bytes are
 * not just this command's: NetX Duo, nx_secure and the bignum code all run on
 * them.  On a machine with no memory protection, running off the bottom is not
 * an error message, it is an illegal instruction at a random address some
 * seconds later.
 *
 * So this command brings its own stack rather than expecting the user to type
 * `stack 65536` first, and any program that opens tls.library should do the
 * same.  StackSwap() is the documented way (exec V36, so it is there on the
 * 3.1 floor).  64 KB is a deliberate over-provision -- it is against the
 * ~40 KB the connection itself allocates on a machine assumed to have four
 * megabytes, and it costs nothing when no connection is open.
 *
 * fetch_trampoline() has NO locals and NO arguments on purpose, and is
 * noinline for the same reason.  Between the two StackSwap() calls the stack
 * pointer belongs to the new stack, so a function reading a stack-based local
 * of its own in there would be reading the wrong memory.  Everything it needs
 * is static; the transfer's own locals are fine, because they live on the new
 * stack.  (Verified in the disassembly: the function's only stack traffic is
 * one push of a6 before the first swap and its matching pop after the second,
 * which balances because StackSwap() restores the pointer exactly.)
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

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<url> [TO <file>]",
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

    /*
     * The body goes to standard output unless TO names a file, so the only
     * place a progress line could go without corrupting it is nowhere.  With
     * TO there is a free channel and the summary is worth having.
     */
    if (fetch_init_state.to == NULL)
        fetch_init_state.quiet = TRUE;

    timeout = (args[ARG_TIMEOUT] != 0)
                  ? (ULONG)(*(LONG *)args[ARG_TIMEOUT])
                  : FETCH_DEFAULT_TIMEOUT;
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
           rather than refuse -- and https: was not going to work anyway on a
           machine this short of memory. */
        fetch_result = fetch_run();
    }

    FreeArgs(rda);
    return fetch_result;
}
