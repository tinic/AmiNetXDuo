/*
 * httpd -- an HTTP server with read-only WebDAV, so that a drawer on this
 * machine appears as a drive on Windows, macOS and Linux with nothing
 * installed at the far end.  Finder's Connect to Server, Explorer's Map
 * network drive and gvfs all speak WebDAV natively; none of them speaks
 * anything else an Amiga can serve without a client on the other machine.
 *
 *     httpd ROOT/A,PORT/N,ADDRESS=-a/K,CONNECTIONS=-m/N/K,TIMEOUT=-w/N/K,
 *           VERBOSE=-v/S,TRACE/S
 *
 *   httpd Work:Public            serve that drawer on port 80
 *   httpd DH0:Docs 8080 -v       another port, one log line per request
 *   httpd RAM: 8080 TRACE        every header, in the order it arrived
 *
 * WHAT IT ANSWERS, AND WHAT IT DOES NOT
 *
 *   OPTIONS, PROPFIND (Depth 0 and 1), GET, HEAD.  It advertises `DAV: 1`,
 *   which is the class that has no locking, and nothing here writes: there is
 *   no PUT, DELETE, MKCOL, COPY, MOVE or LOCK, and an attempt at one is a 405
 *   naming the methods there are.
 *
 *   Read-only first is a sequencing decision and not the destination.  The
 *   parts that a writing server cannot be retrofitted with safely are built to
 *   that standard now: the method table below takes new verbs by gaining rows,
 *   the request body is read through a per-method sink rather than skipped,
 *   and src/tools/httppath.c is written and tested as if DELETE already
 *   existed -- a path mistake leaks a file today and destroys one later.
 *
 * NOT PORTED FROM ANYTHING
 *
 *   The small C servers all assume a socket is a file descriptor and that
 *   select() is select().  Here a socket is an index into bsdsocket.library's
 *   own table and select() is WaitSelect() at LVO -0x0d2, so their event loop
 *   is the part that would have to be rewritten -- and none of them serves
 *   files, so the handler would be ours regardless.  What is left is the
 *   listen/accept idiom, which src/tools/nc.c already has.
 *
 * SEVERAL CONNECTIONS AT ONCE
 *
 *   `nc -l` takes one caller and exits, so before this the server half of the
 *   ABI had no user that ever held two connections open.  Every socket here is
 *   non-blocking and every wait is one WaitSelect() over the listener plus the
 *   live connections, so one client reading a file slowly cannot stop another
 *   from being answered -- which matters because Finder and the Windows
 *   redirector both open several and expect all of them to progress.
 *
 * BOUNDED, BECAUSE THE FLOOR IS A 1 MB MACHINE
 *
 *   A connection that says nothing must not cost anything.  The request head,
 *   the count of headers, each line, the request body and the time a
 *   connection may spend making no progress are all capped, and the caps are
 *   the reason a client can open the connection limit's worth of sockets and
 *   stay silent without the machine noticing.  Everything is static: the
 *   buffers are HTTPD_CONN_MAX * ~5 KB, allocated once, and a Shell command
 *   gets 4 KB of stack on a stock Kickstart 3.1 (src/tools/nc.c says the same
 *   at its own buffers).
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "httppath.h"
#include "aminetxduo/version.h"

#include <libraries/locale.h>
#include <proto/locale.h>

const char *const tool_name = "httpd";

/* proto/locale.h's inlines read this, the same way src/tools/sntp.c declares
   it for the same call. */
struct LocaleBase *LocaleBase;

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("httpd");

#define TEMPLATE                                                        \
    "ROOT/A,PORT/N,ADDRESS=-a/K,CONNECTIONS=-m/N/K,TIMEOUT=-w/N/K,"     \
    "VERBOSE=-v/S,TRACE/S"

enum
{
    ARG_ROOT = 0,
    ARG_PORT,
    ARG_ADDRESS,
    ARG_CONNECTIONS,
    ARG_TIMEOUT,
    ARG_VERBOSE,
    ARG_TRACE,
    ARG_COUNT
};

/* --------------------------------------------------------------- limits --- */

/*
 * The connection ceiling is a memory decision before it is a concurrency one:
 * a slot is 4.9 KB of buffers, so the default eight is 39 KB taken at startup
 * and CONNECTIONS is what a machine where that matters lowers.  Eight is what
 * the clients ask for: Finder opens four to six and the Windows redirector two
 * to four, and a client that finds no free slot waits in the listen backlog
 * rather than failing, so a lower number costs latency and not function.
 */
#define HTTPD_CONN_MAX      16
#define HTTPD_CONN_DEFAULT   8

#define HTTPD_IN_MAX        2048    /* request line and headers together    */
#define HTTPD_OUT_MAX       2048    /* one send() worth of answer           */
#define HTTPD_CHUNK_MAX     1400    /* one generated piece, framed into out */
#define HTTPD_HEADERS_MAX     48    /* header lines in one request          */
#define HTTPD_BODY_MAX     65536UL  /* a request body we will read at all   */
#define HTTPD_TIMEOUT_DEF     30UL  /* seconds of no progress               */
#define HTTPD_BACKLOG          8

/* How long WaitSelect() may sleep with nothing happening.  It is what makes
   Ctrl-C and the connection timeout noticed, and nothing else depends on it. */
#define HTTPD_TICK_MICROS  250000

/* 1978-01-01 to 1970-01-01, the same constant src/tlslib/tls_time.c uses. */
#define HTTPD_AMIGA_EPOCH  252460800UL

/* --------------------------------------------------------------- methods --- */

enum
{
    HTTPD_M_UNKNOWN = 0,
    HTTPD_M_GET,
    HTTPD_M_HEAD,
    HTTPD_M_OPTIONS,
    HTTPD_M_PROPFIND
};

/* A method may carry a request body.  Read-only never has to read one, but
   the frame that reads it is here so PUT is a row and a sink and not a
   restructuring of the loop. */
#define HTTPD_F_BODY    0x01
/* Changes the tree.  Nothing sets it yet; it is where an authorisation check
   will hang when there is one to make. */
#define HTTPD_F_WRITE   0x02

typedef struct HttpConn HttpConn;

typedef struct HttpMethod
{
    const char *name;
    UBYTE       id;
    UBYTE       flags;
    VOID      (*handle)(HttpConn *c);
    /* Where the request body goes.  NULL discards it, which is what every
       read-only method wants and what PUT will replace with a file write. */
    VOID      (*sink)(HttpConn *c, const UBYTE *data, LONG len);
} HttpMethod;

/* ----------------------------------------------------------- connections --- */

enum
{
    CONN_FREE = 0,
    CONN_REQUEST,       /* reading the request head                        */
    CONN_BODY,          /* reading Content-Length bytes through the sink   */
    CONN_SEND           /* pushing out[], refilled by the producer         */
};

enum
{
    PROD_NONE = 0,      /* what is in out[] is the whole answer            */
    PROD_FILE,          /* the rest of an open file                        */
    PROD_INDEX,         /* a generated HTML directory listing              */
    PROD_PROPFIND       /* a generated 207 multistatus                     */
};

enum
{
    DIR_SELF = 0,       /* the collection's own entry                      */
    DIR_CHILDREN,
    DIR_TRAILER,
    DIR_DONE
};

struct HttpConn
{
    LONG    sock;                   /* -1 when the slot is free            */
    UBYTE   state;
    UBYTE   producer;
    UBYTE   keepalive;
    UBYTE   head_only;              /* HEAD: the headers without the body  */

    ULONG   progress;               /* seconds, when this last did anything */
    ULONG   requests;               /* served on this connection            */
    char    peer[TOOL_ADDR_STRLEN];

    ULONG   in_len;
    UBYTE   in[HTTPD_IN_MAX];

    /* what the request said */
    const HttpMethod *method;
    HttpPath path;
    LONG    depth;                  /* 0, 1, or -1 for infinity            */
    ULONG   body_left;
    UBYTE   http11;
    UBYTE   has_range;
    ULONG   range_from;
    ULONG   range_to;               /* inclusive                           */

    /* the answer */
    UBYTE   out[HTTPD_OUT_MAX];
    ULONG   out_len;
    ULONG   out_sent;
    ULONG   wrote;                  /* what send() has ACCEPTED, in total   */
    UBYTE   overflow;               /* the head did not fit -- 500 instead */
    UBYTE   chunked;
    ULONG   status;

    /* where the body comes from */
    BPTR    file;
    ULONG   file_left;
    BPTR    dirlock;
    struct FileInfoBlock *fib;
    UBYTE   dir_stage;
};

/*
 * Taken from the heap and not declared as an array of HTTPD_CONN_MAX, because
 * a static one is the whole ceiling whether it is used or not: at 4.9 KB a
 * slot that was 78 KB of BSS in every copy of the command, which on a 1 MB
 * machine is most of a percent of the machine reserved for connections nobody
 * asked for.  Allocating it makes CONNECTIONS mean something -- `-m 2` is
 * 10 KB -- and takes the command's own BSS down to the shared scratches.
 */
static HttpConn *httpd_conn;

/*
 * Static, not automatic, and for the reason src/tools/nc.c gives at its own
 * buffers: a Shell command gets whatever stack the Shell has, which is 4 KB on
 * a stock Kickstart 3.1.  Between them these are 4.7 KB, and the parse and the
 * answer would otherwise have most of that on the stack at once.
 *
 * Sharing them is safe because exactly one request is parsed and one answer
 * produced per pass of the loop -- the connections interleave between passes,
 * never inside one.
 */
static char httpd_scratch[HTTPD_CHUNK_MAX];
static char httpd_escape[(HTTP_URL_MAX + HTTP_NAME_MAX + 2) * 3];
static char httpd_text[HTTP_URL_MAX * 6];
static char httpd_href_buf[HTTP_URL_MAX + HTTP_NAME_MAX + 2];
static char httpd_target[HTTP_URL_MAX];
static char httpd_value[HTTP_URL_MAX];
static char httpd_page[256];

static const char *httpd_root = "";
static ULONG  httpd_conns   = HTTPD_CONN_DEFAULT;
static ULONG  httpd_timeout = HTTPD_TIMEOUT_DEF;
static BOOL   httpd_verbose = FALSE;
static BOOL   httpd_trace   = FALSE;
static LONG   httpd_gmt_west = 0;       /* minutes west of GMT, from locale */
static struct Library *httpd_sb = NULL;

/* --------------------------------------------------------------- the small */

static ULONG hs_len(const char *s)
{
    ULONG n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

/* Bounded append, in the shape src/tools/fetch.c builds its request with: one
   `ok = ok && ...` chain, and a single overflow fails the whole thing. */
static BOOL hs_append(char *dst, ULONG dstlen, ULONG *used, const char *src)
{
    ULONG n = *used;

    while (*src != '\0')
    {
        if (n + 1UL >= dstlen)
            return FALSE;
        dst[n++] = *src++;
    }

    dst[n] = '\0';
    *used  = n;

    return TRUE;
}

static BOOL hs_append_num(char *dst, ULONG dstlen, ULONG *used, ULONG value)
{
    char  text[12];
    ULONG n = 0;

    if (value == 0UL)
    {
        text[n++] = '0';
    }
    else
    {
        char  rev[12];
        ULONG r = 0;

        while (value > 0UL && r < sizeof(rev))
        {
            rev[r++] = (char)('0' + (value % 10UL));
            value /= 10UL;
        }
        while (r > 0UL)
            text[n++] = rev[--r];
    }

    text[n] = '\0';

    return hs_append(dst, dstlen, used, text);
}

/* Case-insensitive compare of `n` characters, so a header name matches
   however the client capitalised it. */
static int hs_nicmp(const char *a, const char *b, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++)
    {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;

        if (ca != cb || ca == 0)
            return ca - cb;
    }

    return 0;
}

static BOOL hs_equal(const char *a, const char *b)
{
    ULONG n = hs_len(b);

    return (hs_len(a) == n && hs_nicmp(a, b, n) == 0) ? TRUE : FALSE;
}

static VOID hs_copy(char *dst, ULONG dstlen, const char *src)
{
    ULONG n = 0;

    if (dstlen == 0UL)
        return;

    while (src[n] != '\0' && n + 1UL < dstlen)
    {
        dst[n] = src[n];
        n++;
    }

    dst[n] = '\0';
}

/* ------------------------------------------------------------------ time --- */

/*
 * AmigaOS keeps LOCAL time and HTTP dates are GMT, so the offset has to come
 * from somewhere.  locale.library has it and is the same source
 * src/tools/sntp.c uses; it is V38, so a 2.04 machine simply has none and the
 * dates are then local time labelled GMT.  That is the honest failure: a
 * client shows a file an hour out, which is what every Amiga HTTP server does
 * and is better than refusing to serve it.
 */
static VOID httpd_read_gmt_offset(VOID)
{
    struct Locale *locale;

    httpd_gmt_west = 0;

    LocaleBase = (struct LocaleBase *)
                 OpenLibrary((CONST_STRPTR)"locale.library", 38UL);
    if (LocaleBase == NULL)
        return;

    locale = OpenLocale(NULL);          /* NULL: the current preferences */
    if (locale != NULL)
    {
        httpd_gmt_west = (LONG)locale->loc_GMTOffset;
        CloseLocale(locale);
    }

    CloseLibrary((struct Library *)LocaleBase);
    LocaleBase = NULL;
}

/* A DateStamp as seconds since 1970, in GMT. */
static ULONG httpd_stamp_secs(const struct DateStamp *ds)
{
    ULONG secs;

    if (ds->ds_Days < 0 || ds->ds_Minute < 0 || ds->ds_Tick < 0)
        return HTTPD_AMIGA_EPOCH;

    secs  = (ULONG)ds->ds_Days * 86400UL;
    secs += (ULONG)ds->ds_Minute * 60UL;
    secs += (ULONG)ds->ds_Tick / (ULONG)TICKS_PER_SECOND;
    secs += HTTPD_AMIGA_EPOCH;

    /* loc_GMTOffset is minutes WEST, so GMT is later than local time here. */
    if (httpd_gmt_west > 0)
        secs += (ULONG)httpd_gmt_west * 60UL;
    else if (httpd_gmt_west < 0)
    {
        ULONG east = (ULONG)(-httpd_gmt_west) * 60UL;

        secs = (secs > east) ? (secs - east) : 0UL;
    }

    return secs;
}

/* Seconds of uptime-ish local time, for the progress deadlines only.  Wraps
   at midnight, which the caller handles by treating any backwards step as
   "just now" rather than as an expiry. */
static ULONG httpd_now(VOID)
{
    struct DateStamp ds;

    (VOID)DateStamp(&ds);

    return (ULONG)ds.ds_Days * 86400UL + (ULONG)ds.ds_Minute * 60UL +
           (ULONG)ds.ds_Tick / (ULONG)TICKS_PER_SECOND;
}

/* Seconds since 1970 to a calendar date.  Nothing in this tree did this: the
   only conversions are DateStamp to seconds and seconds to a LOCALISED string
   through DateToStr(), and an HTTP date must not be localised. */
static VOID httpd_civil(ULONG secs, LONG *y, LONG *mo, LONG *d,
                        LONG *h, LONG *mi, LONG *s, LONG *dow)
{
    static const LONG mdays[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    ULONG days = secs / 86400UL;
    ULONG rem  = secs % 86400UL;
    LONG  year = 1970;
    LONG  month = 0;

    *h  = (LONG)(rem / 3600UL);
    *mi = (LONG)((rem % 3600UL) / 60UL);
    *s  = (LONG)(rem % 60UL);

    /* 1970-01-01 was a Thursday. */
    *dow = (LONG)((days + 4UL) % 7UL);

    for (;;)
    {
        ULONG len = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
                        ? 366UL : 365UL;

        if (days < len)
            break;

        days -= len;
        year++;
    }

    for (month = 0; month < 12; month++)
    {
        ULONG len = (ULONG)mdays[month];

        if (month == 1 &&
            ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
            len = 29UL;

        if (days < len)
            break;

        days -= len;
    }

    *y  = year;
    *mo = month + 1;
    *d  = (LONG)days + 1;
}

static VOID httpd_two(char *out, LONG value)
{
    out[0] = (char)('0' + ((value / 10) % 10));
    out[1] = (char)('0' + (value % 10));
}

/* "Sun, 06 Nov 1994 08:49:37 GMT" -- RFC 1123, and not the machine's locale. */
static VOID httpd_rfc1123(ULONG secs, char *out)
{
    static const char *const dows[7] =
        { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const months[12] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    LONG y, mo, d, h, mi, s, dow;
    ULONG n = 0;

    httpd_civil(secs, &y, &mo, &d, &h, &mi, &s, &dow);

    out[n++] = dows[dow][0];
    out[n++] = dows[dow][1];
    out[n++] = dows[dow][2];
    out[n++] = ',';
    out[n++] = ' ';
    httpd_two(&out[n], d);          n += 2;
    out[n++] = ' ';
    out[n++] = months[mo - 1][0];
    out[n++] = months[mo - 1][1];
    out[n++] = months[mo - 1][2];
    out[n++] = ' ';
    httpd_two(&out[n], y / 100);    n += 2;
    httpd_two(&out[n], y % 100);    n += 2;
    out[n++] = ' ';
    httpd_two(&out[n], h);          n += 2;
    out[n++] = ':';
    httpd_two(&out[n], mi);         n += 2;
    out[n++] = ':';
    httpd_two(&out[n], s);          n += 2;
    out[n++] = ' ';
    out[n++] = 'G';
    out[n++] = 'M';
    out[n++] = 'T';
    out[n]   = '\0';
}

/* "1994-11-06T08:49:37Z" -- ISO 8601, which is what creationdate takes. */
static VOID httpd_iso8601(ULONG secs, char *out)
{
    LONG y, mo, d, h, mi, s, dow;
    ULONG n = 0;

    httpd_civil(secs, &y, &mo, &d, &h, &mi, &s, &dow);

    httpd_two(&out[n], y / 100);    n += 2;
    httpd_two(&out[n], y % 100);    n += 2;
    out[n++] = '-';
    httpd_two(&out[n], mo);         n += 2;
    out[n++] = '-';
    httpd_two(&out[n], d);          n += 2;
    out[n++] = 'T';
    httpd_two(&out[n], h);          n += 2;
    out[n++] = ':';
    httpd_two(&out[n], mi);         n += 2;
    out[n++] = ':';
    httpd_two(&out[n], s);          n += 2;
    out[n++] = 'Z';
    out[n]   = '\0';
}

/* ------------------------------------------------------------------- log --- */

static VOID httpd_log(const HttpConn *c, const char *fmt, LONG a, LONG b)
{
    tool_printf("[%ld] ", (LONG)(c - httpd_conn));
    tool_printf(fmt, a, b);
    tool_printf("\n");
    (VOID)Flush(Output());
}

/* Every line the client sent, in the order it sent it.  This is the point of
   TRACE: the client quirks are the expensive part of WebDAV, and knowing
   exactly what Finder and the Windows redirector ask for is what decides
   whether writing is worth attempting. */
static VOID httpd_trace_head(const HttpConn *c, const UBYTE *head, ULONG len)
{
    ULONG i = 0;

    while (i < len)
    {
        char  line[200];
        ULONG n = 0;

        while (i < len && head[i] != '\n')
        {
            if (head[i] != '\r' && n + 1UL < sizeof(line))
                line[n++] = (char)head[i];
            i++;
        }
        i++;

        line[n] = '\0';

        if (n > 0UL)
            tool_printf("[%ld] < %s\n", (LONG)(c - httpd_conn), (LONG)line);
    }

    (VOID)Flush(Output());
}

/* ------------------------------------------------------------- responding --- */

static const char *httpd_reason(ULONG status)
{
    switch (status)
    {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 416: return "Range Not Satisfiable";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

static VOID httpd_out(HttpConn *c, const char *text)
{
    ULONG used = c->out_len;

    if (!hs_append((char *)c->out, sizeof(c->out), &used, text))
    {
        c->overflow = 1;
        return;
    }

    c->out_len = used;
}

static VOID httpd_out_num(HttpConn *c, ULONG value)
{
    ULONG used = c->out_len;

    if (!hs_append_num((char *)c->out, sizeof(c->out), &used, value))
    {
        c->overflow = 1;
        return;
    }

    c->out_len = used;
}

static VOID httpd_begin(HttpConn *c, ULONG status)
{
    char  date[40];
    struct DateStamp ds;

    c->out_len  = 0;
    c->out_sent = 0;
    c->overflow = 0;
    c->status   = status;

    (VOID)DateStamp(&ds);
    httpd_rfc1123(httpd_stamp_secs(&ds), date);

    httpd_out(c, "HTTP/1.1 ");
    httpd_out_num(c, status);
    httpd_out(c, " ");
    httpd_out(c, httpd_reason(status));
    httpd_out(c, "\r\nDate: ");
    httpd_out(c, date);
    httpd_out(c, "\r\nServer: AmiNetXDuo-httpd/" AMINETXDUO_VERSION "\r\n");
}

static VOID httpd_header(HttpConn *c, const char *name, const char *value)
{
    httpd_out(c, name);
    httpd_out(c, ": ");
    httpd_out(c, value);
    httpd_out(c, "\r\n");
}

static VOID httpd_header_num(HttpConn *c, const char *name, ULONG value)
{
    httpd_out(c, name);
    httpd_out(c, ": ");
    httpd_out_num(c, value);
    httpd_out(c, "\r\n");
}

/* The methods there are, built from the table so that the answer cannot drift
   from what is dispatched. */
static VOID httpd_allow_header(HttpConn *c);

static VOID httpd_finish_head(HttpConn *c)
{
    httpd_header(c, "Connection", c->keepalive ? "keep-alive" : "close");
    httpd_out(c, "\r\n");

    if (c->overflow)
    {
        /* The head did not fit, so nothing about the answer can be trusted.
           Say so in the only way that is still safe: a fixed reply, and the
           connection closed rather than left in an unknown frame. */
        static const char oops[] =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n";

        hs_copy((char *)c->out, sizeof(c->out), oops);
        c->out_len   = hs_len(oops);
        c->out_sent  = 0;
        c->status    = 500;
        c->keepalive = 0;
        c->producer  = PROD_NONE;
        c->chunked   = 0;
    }

    c->state = CONN_SEND;
}

/*
 * A body whose length nobody knows until it has been produced -- a directory
 * read entry by entry.  HTTP/1.1 gets chunked framing, which keeps the
 * connection usable afterwards; HTTP/1.0 has no chunked encoding, so the end
 * of the body has to be the end of the connection.
 */
static VOID httpd_begin_stream(HttpConn *c)
{
    c->chunked = c->http11;

    if (c->chunked)
        httpd_header(c, "Transfer-Encoding", "chunked");
    else
        c->keepalive = 0;

    httpd_finish_head(c);
}

/* A body that is already known in full: the errors, and the small answers. */
static VOID httpd_body_text(HttpConn *c, const char *type, const char *body)
{
    ULONG len = hs_len(body);

    httpd_header(c, "Content-Type", type);
    httpd_header_num(c, "Content-Length", len);
    httpd_finish_head(c);

    if (!c->head_only && !c->overflow)
        httpd_out(c, body);

    c->producer = PROD_NONE;
}

static VOID httpd_error(HttpConn *c, ULONG status, const char *detail)
{
    ULONG used = 0;
    BOOL  ok;

    httpd_begin(c, status);

    if (status == 405 || status == 501)
        httpd_allow_header(c);

    ok = hs_append(httpd_page, sizeof(httpd_page), &used,
                   "<html><head><title>");
    ok = ok && hs_append_num(httpd_page, sizeof(httpd_page), &used, status);
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used,
                         "</title></head><body><h1>");
    ok = ok && hs_append_num(httpd_page, sizeof(httpd_page), &used, status);
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used, " ");
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used,
                         httpd_reason(status));
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used, "</h1><p>");
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used,
                         (detail != NULL) ? detail : "");
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used,
                         "</p></body></html>\r\n");

    if (!ok)
        hs_copy(httpd_page, sizeof(httpd_page), "error\r\n");

    /* An error ends the conversation unless it is one the client can recover
       from mid-connection.  405 and 404 are recoverable; a framing failure is
       not, because what follows in the stream is no longer known to be a
       request. */
    if (status == 400 || status == 413 || status == 414 || status == 431 ||
        status == 500 || status == 408)
        c->keepalive = 0;

    httpd_body_text(c, "text/html; charset=utf-8", httpd_page);
}

/* ---------------------------------------------------------------- clients --- */

static VOID httpd_close(HttpConn *c)
{
    if (c->file != (BPTR)0)
    {
        (VOID)Close(c->file);
        c->file = (BPTR)0;
    }

    if (c->dirlock != (BPTR)0)
    {
        UnLock(c->dirlock);
        c->dirlock = (BPTR)0;
    }

    if (c->sock >= 0)
    {
        (VOID)tool_sock_close(httpd_sb, c->sock);
        c->sock = -1;
    }

    c->state    = CONN_FREE;
    c->producer = PROD_NONE;
    c->in_len   = 0;
    c->out_len  = 0;
    c->out_sent = 0;
}

/* Between requests on a kept-alive connection.  The socket and whatever the
   client has already pipelined survive; everything about the last request
   does not. */
static VOID httpd_reset(HttpConn *c)
{
    if (c->file != (BPTR)0)
    {
        (VOID)Close(c->file);
        c->file = (BPTR)0;
    }

    if (c->dirlock != (BPTR)0)
    {
        UnLock(c->dirlock);
        c->dirlock = (BPTR)0;
    }

    c->state     = CONN_REQUEST;
    c->producer  = PROD_NONE;
    c->method    = NULL;
    c->out_len   = 0;
    c->out_sent  = 0;
    c->overflow  = 0;
    c->chunked   = 0;
    c->head_only = 0;
    c->has_range = 0;
    /*
     * RFC 4918 says a PROPFIND with no Depth means infinity, and infinity is
     * refused here -- so a client that omits it would get a 403 for asking
     * the ordinary question.  Every client sends one; the ones that do not
     * mean "this collection", so that is what an absent header gets.
     */
    c->depth     = 1;
    c->body_left = 0;
    c->file_left = 0;
    c->dir_stage = DIR_SELF;
    c->wrote     = 0;
}

/* ------------------------------------------------------------- the answer --- */

/*
 * A collection's href always ends in a slash.  Finder and the Windows
 * redirector both treat the trailing slash as part of the identity of a
 * collection, and a multistatus whose self-href disagrees with the URL that
 * was asked about is where "the server returned an unexpected response" comes
 * from.
 */
static const char *httpd_href(const HttpPath *p, const char *child, BOOL dir)
{
    ULONG used = 0;
    BOOL  ok;

    ok = hs_append(httpd_href_buf, sizeof(httpd_href_buf), &used, p->url);

    if (child != NULL)
    {
        if (used > 0UL && httpd_href_buf[used - 1] != '/')
            ok = ok && hs_append(httpd_href_buf, sizeof(httpd_href_buf), &used,
                                 "/");
        ok = ok && hs_append(httpd_href_buf, sizeof(httpd_href_buf), &used,
                             child);
    }

    if (dir && used > 0UL && httpd_href_buf[used - 1] != '/')
        ok = ok && hs_append(httpd_href_buf, sizeof(httpd_href_buf), &used, "/");

    if (!ok)
        return "/";

    /* Percent-encoding leaves no &, < or > behind, so an escaped href needs
       no XML escaping on top of it. */
    if (http_url_escape(httpd_href_buf, httpd_escape,
                        sizeof(httpd_escape)) == 0UL)
        return "/";

    return httpd_escape;
}

/* One <D:response> for a file or a collection, into the shared scratch. */
static ULONG httpd_propfind_entry(const char *href, const char *name,
                                  BOOL is_dir, ULONG size,
                                  const struct DateStamp *date)
{
    char  modified[40];
    char  created[32];
    ULONG used = 0;
    ULONG secs = httpd_stamp_secs(date);
    BOOL  ok;

    httpd_rfc1123(secs, modified);
    httpd_iso8601(secs, created);

    (VOID)http_xml_escape(name, httpd_text, sizeof(httpd_text));

    ok = hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                   "<D:response><D:href>");
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, href);
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:href><D:propstat><D:prop>"
                         "<D:displayname>");
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         httpd_text);
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:displayname><D:creationdate>");
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, created);
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:creationdate><D:getlastmodified>");
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, modified);
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:getlastmodified><D:resourcetype>");

    if (is_dir)
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             "<D:collection/>");

    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:resourcetype>");

    if (!is_dir)
    {
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             "<D:getcontentlength>");
        ok = ok && hs_append_num(httpd_scratch, sizeof(httpd_scratch), &used,
                                 size);
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             "</D:getcontentlength><D:getcontenttype>");
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             http_content_type(name));
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             "</D:getcontenttype>");
    }

    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:prop><D:status>HTTP/1.1 200 OK</D:status>"
                         "</D:propstat></D:response>\n");

    return ok ? used : 0UL;
}

/* One row of a generated directory index, for a browser rather than a client. */
static ULONG httpd_index_entry(const char *href, const char *name,
                               BOOL is_dir, ULONG size)
{
    ULONG used = 0;
    BOOL  ok;

    (VOID)http_xml_escape(name, httpd_text, sizeof(httpd_text));

    ok = hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<li><a href=\"");
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, href);
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "\">");
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, httpd_text);
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         is_dir ? "/</a></li>\n" : "</a> ");

    if (!is_dir)
    {
        ok = ok && hs_append_num(httpd_scratch, sizeof(httpd_scratch), &used,
                                 size);
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             " bytes</li>\n");
    }

    return ok ? used : 0UL;
}

/*
 * Frame whatever is in the scratch as one chunk of a chunked response.  The
 * generated answers have no length anybody can know in advance -- a directory
 * is read entry by entry -- and chunking is what lets them be streamed without
 * either buffering the whole thing or closing the connection to mark the end.
 */
static VOID httpd_emit_chunk(HttpConn *c, ULONG len)
{
    static const char hex[] = "0123456789abcdef";
    char  size[12];
    ULONG n = 0;
    ULONG shift;
    BOOL  seen = FALSE;

    if (len == 0UL)
        return;

    /*
     * An HTTP/1.0 client has no chunked encoding to decode, so for one the
     * body is sent as it comes and the end of it is the end of the connection.
     * Emitting chunk framing at a 1.0 client puts the hex counts in the file
     * it saves, which is the kind of failure that looks like a corrupt server.
     */
    if (!c->chunked)
    {
        httpd_out(c, httpd_scratch);
        return;
    }

    for (shift = 28; ; shift -= 4)
    {
        ULONG digit = (len >> shift) & 0xfUL;

        if (digit != 0UL || seen || shift == 0UL)
        {
            size[n++] = hex[digit];
            seen = TRUE;
        }

        if (shift == 0UL)
            break;
    }

    size[n] = '\0';

    httpd_out(c, size);
    httpd_out(c, "\r\n");
    httpd_out(c, httpd_scratch);
    httpd_out(c, "\r\n");
}

/*
 * The next piece of a streamed answer.  TRUE while there is more to come.
 *
 * Called only when out[] has been fully sent, so it may fill it from scratch
 * each time; the connection carries the position it left off at, which for a
 * directory is an open lock and a FileInfoBlock and for a file is an open
 * handle and a remaining count.
 */
static BOOL httpd_produce(HttpConn *c)
{
    c->out_len  = 0;
    c->out_sent = 0;

    switch (c->producer)
    {
        case PROD_FILE:
        {
            LONG want = (LONG)sizeof(c->out);
            LONG got;

            if (c->file_left == 0UL || c->file == (BPTR)0)
                return FALSE;

            if ((ULONG)want > c->file_left)
                want = (LONG)c->file_left;

            got = Read(c->file, (APTR)c->out, want);


            if (got <= 0)
            {
                /* Short of what Examine() said, which means the file changed
                   under us.  The frame is already committed to a length, so
                   the only truthful end is to stop sending and close. */
                c->file_left = 0;
                c->keepalive = 0;
                return FALSE;
            }

            c->out_len    = (ULONG)got;
            c->file_left -= (ULONG)got;

            return TRUE;
        }

        case PROD_INDEX:
        case PROD_PROPFIND:
        {
            BOOL propfind = (c->producer == PROD_PROPFIND) ? TRUE : FALSE;

            for (;;)
            {
                ULONG len = 0;

                switch (c->dir_stage)
                {
                    case DIR_SELF:
                    {
                        struct DateStamp date;
                        BOOL is_dir = (c->dirlock != (BPTR)0) ? TRUE : FALSE;
                        ULONG size = 0;

                        date.ds_Days   = 0;
                        date.ds_Minute = 0;
                        date.ds_Tick   = 0;

                        if (c->fib != NULL)
                        {
                            date = c->fib->fib_Date;
                            if (c->fib->fib_DirEntryType <= 0)
                                size = (ULONG)c->fib->fib_Size;
                        }

                        /* Depth is PROPFIND's question, not GET's: a listing
                           always lists, and Depth: 0 asks about the drawer
                           itself and not what is in it. */
                        c->dir_stage = (!is_dir || (propfind && c->depth == 0))
                                           ? DIR_TRAILER : DIR_CHILDREN;

                        if (propfind)
                        {
                            const char *name = (c->path.name[0] != '\0')
                                                   ? c->path.name : "/";

                            len = httpd_propfind_entry(
                                      httpd_href(&c->path, NULL, is_dir),
                                      name, is_dir, size, &date);
                        }
                        else
                        {
                            ULONG used = 0;

                            (VOID)hs_append(httpd_scratch,
                                            sizeof(httpd_scratch), &used,
                                            "<ul>\n");
                            len = used;
                        }
                        break;
                    }

                    case DIR_CHILDREN:
                    {
                        BOOL is_dir;
                        const char *name;

                        if (c->dirlock == (BPTR)0 || c->fib == NULL ||
                            !ExNext(c->dirlock, c->fib))
                        {
                            c->dir_stage = DIR_TRAILER;
                            continue;
                        }

                        name   = (const char *)c->fib->fib_FileName;
                        is_dir = (c->fib->fib_DirEntryType > 0) ? TRUE : FALSE;

                        if (propfind)
                            len = httpd_propfind_entry(
                                      httpd_href(&c->path, name, is_dir),
                                      name, is_dir,
                                      (ULONG)c->fib->fib_Size,
                                      &c->fib->fib_Date);
                        else
                            len = httpd_index_entry(
                                      httpd_href(&c->path, name, is_dir),
                                      name, is_dir,
                                      (ULONG)c->fib->fib_Size);

                        /* One entry that does not fit the scratch is skipped
                           rather than truncating the answer. */
                        if (len == 0UL)
                            continue;
                        break;
                    }

                    case DIR_TRAILER:
                    {
                        ULONG used = 0;

                        c->dir_stage = DIR_DONE;

                        (VOID)hs_append(httpd_scratch, sizeof(httpd_scratch),
                                        &used,
                                        propfind ? "</D:multistatus>\n"
                                                 : "</ul></body></html>\n");
                        len = used;
                        break;
                    }

                    default:
                        /* The zero chunk that ends a chunked body.  A body
                           that was not chunked ends by closing instead. */
                        if (c->chunked)
                            httpd_out(c, "0\r\n\r\n");
                        c->producer = PROD_NONE;
                        return (c->out_len > 0UL) ? TRUE : FALSE;
                }

                if (len > 0UL)
                {
                    httpd_emit_chunk(c, len);
                    return TRUE;
                }
            }
        }

        default:
            break;
    }

    return FALSE;
}

/* --------------------------------------------------------------- methods --- */

static VOID httpd_do_options(HttpConn *c)
{
    httpd_begin(c, 200);

    /*
     * DAV: 1 and not 1,2.  Class 2 is locking, and this server has none;
     * saying 2 without LOCK is what makes a client offer to write and then
     * fail at the first attempt rather than mount read-only cleanly.
     */
    httpd_header(c, "DAV", "1");
    httpd_allow_header(c);
    /* The Windows redirector looks for this before it will treat an http://
       URL as a WebDAV share at all rather than as a web page. */
    httpd_header(c, "MS-Author-Via", "DAV");
    httpd_header_num(c, "Content-Length", 0);
    httpd_finish_head(c);

    c->producer = PROD_NONE;
}

/*
 * The document root itself, or a file or drawer under it.  Locks it, and
 * leaves the lock open when the answer is going to be a listing, because the
 * listing is produced across later passes of the loop.
 *
 * FALSE when it has already answered with the reason.
 */
static BOOL httpd_examine(HttpConn *c, BOOL *is_dir, BOOL keep_lock)
{
    BPTR lock;

    if (c->fib == NULL)
    {
        httpd_error(c, 500, "no memory for a directory entry");
        return FALSE;
    }

    lock = Lock((CONST_STRPTR)c->path.path, ACCESS_READ);
    if (lock == (BPTR)0)
    {
        httpd_error(c, 404, "no such file on this machine");
        return FALSE;
    }

    if (!Examine(lock, c->fib))
    {
        UnLock(lock);
        httpd_error(c, 500, "that file could not be examined");
        return FALSE;
    }

    *is_dir = (c->fib->fib_DirEntryType > 0) ? TRUE : FALSE;

    if (keep_lock && *is_dir)
        c->dirlock = lock;
    else
        UnLock(lock);

    return TRUE;
}

static VOID httpd_do_propfind(HttpConn *c)
{
    BOOL is_dir = FALSE;

    /*
     * Depth: infinity is refused, as RFC 4918 8.1 allows.  A recursive
     * PROPFIND over a hard drive is an unbounded amount of work on a 14 MHz
     * machine and the client cannot cancel it; every real server refuses it
     * and every client copes.
     */
    if (c->depth < 0)
    {
        httpd_begin(c, 403);
        httpd_body_text(c, "text/xml; charset=utf-8",
                        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                        "<D:error xmlns:D=\"DAV:\">"
                        "<D:propfind-finite-depth/></D:error>\n");
        return;
    }

    if (!httpd_examine(c, &is_dir, TRUE))
        return;

    httpd_begin(c, 207);
    httpd_header(c, "DAV", "1");
    httpd_header(c, "Content-Type", "text/xml; charset=utf-8");

    if (c->head_only)
    {
        httpd_finish_head(c);
        c->producer = PROD_NONE;
        return;
    }

    httpd_begin_stream(c);

    if (c->overflow)
        return;

    c->dir_stage = DIR_SELF;
    c->producer  = PROD_PROPFIND;

    /* The preamble is the first chunk; everything after it comes from
       httpd_produce(). */
    {
        ULONG used = 0;

        (VOID)hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                        "<D:multistatus xmlns:D=\"DAV:\">\n");
        httpd_emit_chunk(c, used);
    }
}

static VOID httpd_do_get(HttpConn *c)
{
    BOOL  is_dir = FALSE;
    ULONG size;
    ULONG from = 0;
    ULONG to;

    if (!httpd_examine(c, &is_dir, TRUE))
        return;

    if (is_dir)
    {
        /*
         * A collection without the trailing slash is redirected rather than
         * served, because every relative link in the listing would otherwise
         * be resolved against the parent.
         */
        if (!c->path.trailing_slash && c->path.segments > 0)
        {
            const char *href = httpd_href(&c->path, NULL, TRUE);

            UnLock(c->dirlock);
            c->dirlock = (BPTR)0;

            httpd_begin(c, 301);
            httpd_header(c, "Location", href);
            httpd_body_text(c, "text/html; charset=utf-8",
                            "<html><body>It is a drawer.</body></html>\r\n");
            return;
        }

        httpd_begin(c, 200);
        httpd_header(c, "Content-Type", "text/html; charset=utf-8");

        if (c->head_only)
        {
            httpd_finish_head(c);
            c->producer = PROD_NONE;
            return;
        }

        httpd_begin_stream(c);

        if (c->overflow)
            return;

        c->dir_stage = DIR_SELF;
        c->producer  = PROD_INDEX;

        {
            ULONG used = 0;
            BOOL  ok;

            (VOID)http_xml_escape(c->path.url, httpd_text, sizeof(httpd_text));

            ok = hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                           "<html><head><title>");
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 httpd_text);
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 "</title></head><body><h1>");
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 httpd_text);
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 "</h1>\n");

            if (ok)
                httpd_emit_chunk(c, used);
        }

        return;
    }

    size = (ULONG)c->fib->fib_Size;
    to   = (size > 0UL) ? (size - 1UL) : 0UL;

    if (c->has_range)
    {
        from = c->range_from;
        to   = (c->range_to < size) ? c->range_to : ((size > 0UL) ? size - 1UL : 0UL);

        if (size == 0UL || from >= size || from > to)
        {
            httpd_begin(c, 416);
            httpd_out(c, "Content-Range: bytes */");
            httpd_out_num(c, size);
            httpd_out(c, "\r\n");
            httpd_body_text(c, "text/html; charset=utf-8",
                            "<html><body>Not that part.</body></html>\r\n");
            return;
        }
    }

    c->file = Open((CONST_STRPTR)c->path.path, MODE_OLDFILE);
    if (c->file == (BPTR)0)
    {
        httpd_error(c, 403, "that file will not open");
        return;
    }

    if (from > 0UL && Seek(c->file, (LONG)from, OFFSET_BEGINNING) < 0)
    {
        (VOID)Close(c->file);
        c->file = (BPTR)0;
        httpd_error(c, 416, "that file will not seek");
        return;
    }

    httpd_begin(c, c->has_range ? 206 : 200);
    httpd_header(c, "Content-Type", http_content_type(c->path.name));
    httpd_header_num(c, "Content-Length", (size > 0UL) ? (to - from + 1UL) : 0UL);
    /* A client that knows it may ask for a part will ask, which is how a
       50 MB ADF gets opened without being copied first. */
    httpd_header(c, "Accept-Ranges", "bytes");

    {
        char modified[40];

        httpd_rfc1123(httpd_stamp_secs(&c->fib->fib_Date), modified);
        httpd_header(c, "Last-Modified", modified);
    }

    if (c->has_range)
    {
        httpd_out(c, "Content-Range: bytes ");
        httpd_out_num(c, from);
        httpd_out(c, "-");
        httpd_out_num(c, to);
        httpd_out(c, "/");
        httpd_out_num(c, size);
        httpd_out(c, "\r\n");
    }

    httpd_finish_head(c);

    if (c->head_only || c->overflow || size == 0UL)
    {
        (VOID)Close(c->file);
        c->file     = (BPTR)0;
        c->producer = PROD_NONE;
        return;
    }

    c->file_left = to - from + 1UL;
    c->producer  = PROD_FILE;
}

/*
 * The table.  A ladder of strcmp would have been shorter today and would have
 * to be unpicked the moment PUT lands; this is the shape the destination
 * needs, and the Allow header is generated from it so the two cannot disagree.
 */
static const HttpMethod httpd_methods[] =
{
    { "GET",      HTTPD_M_GET,      0,            httpd_do_get,      NULL },
    { "HEAD",     HTTPD_M_HEAD,     0,            httpd_do_get,      NULL },
    { "OPTIONS",  HTTPD_M_OPTIONS,  0,            httpd_do_options,  NULL },
    /* PROPFIND's body is read and thrown away: an empty body and <allprop/>
       mean the same thing, and a body asking for named properties gets the
       ones there are, which RFC 4918 permits and every client tolerates.  No
       XML parser is worth the size on this machine. */
    { "PROPFIND", HTTPD_M_PROPFIND, HTTPD_F_BODY, httpd_do_propfind, NULL },
    { NULL,       HTTPD_M_UNKNOWN,  0,            NULL,              NULL }
};

static VOID httpd_allow_header(HttpConn *c)
{
    ULONG i;

    httpd_out(c, "Allow: ");

    for (i = 0; httpd_methods[i].name != NULL; i++)
    {
        if (i > 0UL)
            httpd_out(c, ", ");
        httpd_out(c, httpd_methods[i].name);
    }

    httpd_out(c, "\r\n");
}

static const HttpMethod *httpd_lookup(const char *name)
{
    ULONG i;

    for (i = 0; httpd_methods[i].name != NULL; i++)
    {
        if (hs_equal(name, httpd_methods[i].name))
            return &httpd_methods[i];
    }

    return NULL;
}

/* --------------------------------------------------------------- parsing --- */

/* "bytes=0-1023", "bytes=1024-", "bytes=-512".  One range only: a multipart
   answer is a different framing and no client needs it to open a file. */
static BOOL httpd_parse_range(HttpConn *c, const char *value)
{
    ULONG from = 0;
    ULONG to   = 0xffffffffUL;
    BOOL  have_from = FALSE;
    BOOL  suffix = FALSE;

    if (hs_nicmp(value, "bytes=", 6) != 0)
        return FALSE;

    value += 6;

    if (*value == '-')
    {
        suffix = TRUE;
        value++;
    }

    while (*value >= '0' && *value <= '9')
    {
        from = (from * 10UL) + (ULONG)(*value++ - '0');
        have_from = TRUE;
    }

    if (!have_from)
        return FALSE;

    /*
     * "bytes=-500" is the last 500 bytes, which cannot be turned into a start
     * without the size.  Ignoring the header is a legal answer -- the client
     * gets the whole file with a 200 and reads what it wanted -- and no client
     * that mounts a drive asks this way; the ones that do are media players
     * reading a trailer.
     */
    if (suffix)
        return FALSE;

    if (*value == '-')
    {
        value++;

        if (*value >= '0' && *value <= '9')
        {
            to = 0;
            while (*value >= '0' && *value <= '9')
                to = (to * 10UL) + (ULONG)(*value++ - '0');
        }
    }

    c->has_range  = 1;
    c->range_from = from;
    c->range_to   = to;

    return TRUE;
}

/*
 * The request head, from the first byte to the blank line.  Returns FALSE
 * having already answered.
 *
 * Everything a header can say that this server acts on is picked out here, so
 * that adding one -- Authorization is the obvious next -- is a case in this
 * switch and not a second pass over the buffer.
 */
static BOOL httpd_parse(HttpConn *c, ULONG headlen)
{
    char   method[24];
    ULONG  i = 0;
    ULONG  n = 0;
    ULONG  headers = 0;
    HttpPathResult why;

    /* ---- the request line ------------------------------------------- */

    while (i < headlen && c->in[i] != ' ' && c->in[i] != '\r' &&
           c->in[i] != '\n')
    {
        if (n + 1UL >= sizeof(method))
        {
            httpd_error(c, 501, "that is not a method this server has");
            return FALSE;
        }
        method[n++] = (char)c->in[i++];
    }
    method[n] = '\0';

    while (i < headlen && c->in[i] == ' ')
        i++;

    n = 0;
    while (i < headlen && c->in[i] != ' ' && c->in[i] != '\r' &&
           c->in[i] != '\n')
    {
        if (n + 1UL >= sizeof(httpd_target))
        {
            httpd_error(c, 414, "that address is longer than this server "
                                "will read");
            return FALSE;
        }
        httpd_target[n++] = (char)c->in[i++];
    }
    httpd_target[n] = '\0';

    while (i < headlen && c->in[i] == ' ')
        i++;

    /* HTTP/1.1 keeps the connection open unless told otherwise; 1.0 is the
       other way round.  Anything unrecognised is treated as 1.0, which is the
       conservative half of the two. */
    c->http11    = (headlen - i >= 8UL &&
                    hs_nicmp((const char *)&c->in[i], "HTTP/1.1", 8) == 0)
                       ? 1 : 0;
    c->keepalive = c->http11;

    while (i < headlen && c->in[i] != '\n')
        i++;
    i++;

    if (method[0] == '\0' || httpd_target[0] == '\0')
    {
        httpd_error(c, 400, "that is not a request line");
        return FALSE;
    }

    /* ---- the headers ------------------------------------------------- */

    while (i < headlen)
    {
        char  name[40];
        ULONG start = i;

        while (i < headlen && c->in[i] != '\n')
            i++;

        /* An empty line is the end of the head. */
        if (i == start || (i == start + 1UL && c->in[start] == '\r'))
        {
            i++;
            break;
        }

        if (++headers > (ULONG)HTTPD_HEADERS_MAX)
        {
            httpd_error(c, 431, "too many headers");
            return FALSE;
        }

        {
            ULONG j = start;

            n = 0;
            while (j < i && c->in[j] != ':')
            {
                if (n + 1UL < sizeof(name))
                    name[n++] = (char)c->in[j];
                j++;
            }
            name[n] = '\0';

            if (j < i)
                j++;                        /* the colon                  */
            while (j < i && (c->in[j] == ' ' || c->in[j] == '\t'))
                j++;

            n = 0;
            while (j < i && c->in[j] != '\r')
            {
                if (n + 1UL < sizeof(httpd_value))
                    httpd_value[n++] = (char)c->in[j];
                j++;
            }
            httpd_value[n] = '\0';
        }

        i++;

        if (hs_equal(name, "Content-Length"))
        {
            ULONG len = 0;
            const char *p = httpd_value;

            while (*p >= '0' && *p <= '9')
                len = (len * 10UL) + (ULONG)(*p++ - '0');

            c->body_left = len;
        }
        else if (hs_equal(name, "Depth"))
        {
            if (hs_nicmp(httpd_value, "infinity", 8) == 0)
                c->depth = -1;
            else if (httpd_value[0] == '1')
                c->depth = 1;
            else
                c->depth = 0;
        }
        else if (hs_equal(name, "Connection"))
        {
            if (hs_nicmp(httpd_value, "close", 5) == 0)
                c->keepalive = 0;
            else if (hs_nicmp(httpd_value, "keep-alive", 10) == 0)
                c->keepalive = 1;
        }
        else if (hs_equal(name, "Range"))
        {
            (VOID)httpd_parse_range(c, httpd_value);
        }
        else if (hs_equal(name, "Transfer-Encoding"))
        {
            /* A chunked request body is a second framing to implement and
               nothing read-only can receive one; PUT is where it earns its
               keep, and until then saying so is better than mis-reading it. */
            if (hs_nicmp(httpd_value, "chunked", 7) == 0)
            {
                httpd_error(c, 501, "this server does not read a chunked "
                                    "request body");
                return FALSE;
            }
        }
    }

    /* ---- what to do with it ------------------------------------------ */

    c->method = httpd_lookup(method);

    if (c->method == NULL)
    {
        /* 405 and not 501: the address is fine, the verb is not, and the
           Allow header is what tells a client to mount read-only rather than
           to keep trying.  This is the answer Finder and Explorer get for
           LOCK and PUT. */
        httpd_error(c, 405, "this server is read-only");
        return FALSE;
    }

    c->head_only = (c->method->id == HTTPD_M_HEAD) ? 1 : 0;

    if (c->body_left > HTTPD_BODY_MAX)
    {
        httpd_error(c, 413, "that request body is larger than this server "
                            "will read");
        return FALSE;
    }

    if (c->body_left > 0UL && (c->method->flags & HTTPD_F_BODY) == 0)
    {
        httpd_error(c, 400, "that method takes no request body");
        return FALSE;
    }

    /* "OPTIONS *" asks about the server rather than about a resource. */
    if (c->method->id == HTTPD_M_OPTIONS && httpd_target[0] == '*')
    {
        hs_copy(c->path.path, sizeof(c->path.path), httpd_root);
        hs_copy(c->path.url, sizeof(c->path.url), "/");
        c->path.name[0]      = '\0';
        c->path.segments     = 0;
        c->path.trailing_slash = 1;
        return TRUE;
    }

    why = http_path_resolve(httpd_root, httpd_target, &c->path);
    if (why != HTTP_PATH_OK)
    {
        if (httpd_verbose || httpd_trace)
            httpd_log(c, "refused \"%s\": %s", (LONG)httpd_target,
                      (LONG)http_path_error(why));

        /* 403 rather than 404 for every one of them: whether the path exists
           is exactly what a caller probing for an escape wants told, and the
           answer is the same for a device reference that resolves to a real
           drawer and one that does not. */
        httpd_error(c, 403, "that address is not one this server will open");
        return FALSE;
    }

    return TRUE;
}

/* --------------------------------------------------------------- driving --- */

/* What was answered, whatever answered it.  Refusals go through here too:
   they used to be the one thing the log did not record, so a transcript
   showed a client's PUT and then nothing, and the 405 that made it give up
   had to be inferred from the byte count. */
static VOID httpd_log_status(HttpConn *c)
{
    if (httpd_verbose || httpd_trace)
        httpd_log(c, "> %lu %s", (LONG)c->status,
                  (LONG)httpd_reason(c->status));
}

static VOID httpd_dispatch(HttpConn *c)
{
    if (httpd_verbose && !httpd_trace)
        httpd_log(c, "%s %s", (LONG)c->method->name, (LONG)c->path.url);

    c->method->handle(c);

    httpd_log_status(c);
}

/*
 * Feed the body to the method's sink.  Read-only has no sink and the bytes go
 * nowhere, but they are still READ -- a request body left in the socket is the
 * next request as far as the parser is concerned, which is how a server that
 * ignores bodies answers the wrong question on a kept-alive connection.
 */
static VOID httpd_consume_body(HttpConn *c, const UBYTE *data, LONG len)
{
    if (len <= 0)
        return;

    if (c->method != NULL && c->method->sink != NULL)
        c->method->sink(c, data, len);
    else if (httpd_trace)
    {
        char  text[200];
        ULONG n = 0;

        while (n + 1UL < sizeof(text) && (LONG)n < len)
        {
            UBYTE ch = data[n];

            text[n] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
            n++;
        }
        text[n] = '\0';

        tool_printf("[%ld] body %s\n", (LONG)(c - httpd_conn), (LONG)text);
        (VOID)Flush(Output());
    }

    c->body_left -= (ULONG)len;
}

/* Anything the client pipelined after the head is the start of the body, or
   of the next request.  Either way it is already here and must not be lost. */
static VOID httpd_after_head(HttpConn *c, ULONG headlen)
{
    ULONG left = c->in_len - headlen;
    ULONG i;

    for (i = 0; i < left; i++)
        c->in[i] = c->in[headlen + i];

    c->in_len = left;

    if (c->body_left > 0UL)
    {
        ULONG take = (left < c->body_left) ? left : c->body_left;

        if (take > 0UL)
        {
            httpd_consume_body(c, c->in, (LONG)take);

            for (i = 0; i + take < c->in_len; i++)
                c->in[i] = c->in[i + take];

            c->in_len -= take;
        }
    }

    c->state = (c->body_left > 0UL) ? CONN_BODY : CONN_SEND;

    if (c->state == CONN_SEND)
        httpd_dispatch(c);
}

/* One readable connection.  FALSE when it is finished with. */
static BOOL httpd_readable(HttpConn *c)
{
    LONG got;

    if (c->state == CONN_BODY)
    {
        UBYTE scratch[512];
        LONG  want = (LONG)sizeof(scratch);

        if ((ULONG)want > c->body_left)
            want = (LONG)c->body_left;

        got = tool_sock_recv(httpd_sb, c->sock, scratch, want);

        if (got == 0)
            return FALSE;

        if (got < 0)
        {
            LONG err = tool_sock_errno(httpd_sb);

            return (err == TOOL_EWOULDBLOCK || err == TOOL_EINTR) ? TRUE : FALSE;
        }

        httpd_consume_body(c, scratch, got);
        c->progress = httpd_now();

        if (c->body_left == 0UL)
        {
            c->state = CONN_SEND;
            httpd_dispatch(c);
        }

        return TRUE;
    }

    if (c->in_len >= sizeof(c->in))
    {
        httpd_error(c, 431, "that request head is larger than this server "
                            "will read");
        return TRUE;
    }

    got = tool_sock_recv(httpd_sb, c->sock, &c->in[c->in_len],
                         (LONG)(sizeof(c->in) - c->in_len));

    if (got == 0)
        return FALSE;                   /* the client hung up: normal      */

    if (got < 0)
    {
        LONG err = tool_sock_errno(httpd_sb);

        return (err == TOOL_EWOULDBLOCK || err == TOOL_EINTR) ? TRUE : FALSE;
    }

    c->in_len   += (ULONG)got;
    c->progress  = httpd_now();

    /*
     * The end of the head, found from the bytes in the buffer.  Bare LF is
     * accepted as well as CRLF, the same way src/tools/fetch.c accepts it on
     * the answering side: something always sends it.
     */
    {
        ULONG i;

        for (i = 0; i + 1UL < c->in_len; i++)
        {
            ULONG headlen = 0;

            if (c->in[i] == '\n' && c->in[i + 1] == '\n')
                headlen = i + 2UL;
            else if (i + 3UL < c->in_len &&
                     c->in[i] == '\r' && c->in[i + 1] == '\n' &&
                     c->in[i + 2] == '\r' && c->in[i + 3] == '\n')
                headlen = i + 4UL;

            if (headlen == 0UL)
                continue;

            c->requests++;

            if (httpd_trace)
                httpd_trace_head(c, c->in, headlen);

            if (!httpd_parse(c, headlen))
            {
                /* Answered already.  What is left in the buffer belongs to a
                   request that will not be read, so the connection ends after
                   the answer goes out. */
                httpd_log_status(c);
                c->in_len = 0;
                c->state  = CONN_SEND;
                return TRUE;
            }

            httpd_after_head(c, headlen);
            return TRUE;
        }
    }

    if (c->in_len >= sizeof(c->in))
    {
        httpd_error(c, 431, "that request head is larger than this server "
                            "will read");
        c->in_len = 0;
    }

    return TRUE;
}

/* One writable connection.  FALSE when it is finished with. */
static BOOL httpd_writable(HttpConn *c)
{
    for (;;)
    {
        LONG sent;

        if (c->out_sent < c->out_len)
        {
            LONG want = (LONG)(c->out_len - c->out_sent);

            sent = tool_sock_send(httpd_sb, c->sock, &c->out[c->out_sent],
                                  want);

            /*
             * What send() says it took, totalled per answer.  It is one line
             * under TRACE and it is here because it is the only way to tell
             * this program's mistakes from the library's: on a 512 KB file
             * this counter reads exactly Content-Length while the wire
             * carries several thousand bytes more, which is a fault in
             * send() and not in the loop below.  docs/BACKLOG.md has the
             * measurement.
             */
            if (sent > 0)
                c->wrote += (ULONG)sent;

            if (sent < 0)
            {
                LONG err = tool_sock_errno(httpd_sb);

                return (err == TOOL_EWOULDBLOCK || err == TOOL_EINTR)
                           ? TRUE : FALSE;
            }

            c->out_sent += (ULONG)sent;
            c->progress  = httpd_now();

            if (c->out_sent < c->out_len)
                return TRUE;            /* the socket is full for now      */
        }

        if (c->producer != PROD_NONE && !c->head_only)
        {
            if (httpd_produce(c))
                continue;
        }

        /* Nothing left to say.  The counter is reported here, before the two
           paths part: a client sending `Connection: close` is the one that
           downloads a large file, and it used to be the one whose total was
           never printed. */
        if (httpd_trace)
            httpd_log(c, "send() accepted %lu bytes for this answer",
                      (LONG)c->wrote, 0);

        if (!c->keepalive)
            return FALSE;

        {
            ULONG pipelined = c->in_len;

            httpd_reset(c);

            /* A request that was already in the buffer behind the last one is
               parsed on the next pass rather than here, so one client cannot
               hold the loop by pipelining. */
            c->in_len = pipelined;
        }

        return TRUE;
    }
}

/* ------------------------------------------------------------------ main --- */

static LONG httpd_listen(const ToolAddr *bindaddr, UWORD port)
{
    ToolSockAddrAny sa;
    LONG            lsock;
    LONG            one = 1;

    lsock = tool_sock_socket(httpd_sb, (LONG)bindaddr->ta_Family,
                             TOOL_SOCK_STREAM, 0);
    if (lsock < 0)
    {
        tool_error("no socket: %s",
                   (LONG)tool_sock_errstr(tool_sock_errno(httpd_sb)));
        return -1;
    }

    /* Without it, a server restarted after serving anything meets its own
       last connection's TIME-WAIT and cannot bind for two minutes -- which on
       a machine being tested is every single run. */
    (VOID)tool_sock_setsockopt(httpd_sb, lsock, TOOL_SOL_SOCKET,
                               TOOL_SO_REUSEADDR, &one, (LONG)sizeof(one));

    (VOID)tool_sock_addr(&sa, bindaddr, port);

    if (tool_sock_bind(httpd_sb, lsock, &sa) != 0)
    {
        LONG err = tool_sock_errno(httpd_sb);

        tool_error("cannot listen on port %ld: %s", (LONG)port,
                   (LONG)tool_sock_errstr(err));

        if (err == TOOL_EADDRINUSE)
        {
            tool_advise_blank();
            tool_advise("Something else has that port, or the last program to "
                        "use it has not finished closing it down.  netstat -a "
                        "says which.");
        }

        (VOID)tool_sock_close(httpd_sb, lsock);
        return -1;
    }

    if (tool_sock_listen(httpd_sb, lsock, HTTPD_BACKLOG) != 0)
    {
        tool_error("cannot listen on port %ld: %s", (LONG)port,
                   (LONG)tool_sock_errstr(tool_sock_errno(httpd_sb)));
        (VOID)tool_sock_close(httpd_sb, lsock);
        return -1;
    }

    return lsock;
}

static VOID httpd_accept(LONG lsock)
{
    ToolSockAddrAny from;
    ULONG i;
    LONG  sock;
    LONG  nonblock = 1;

    for (i = 0; i < httpd_conns; i++)
    {
        if (httpd_conn[i].state == CONN_FREE)
            break;
    }

    if (i >= httpd_conns)
        return;                         /* the caller does not offer then  */

    sock = tool_sock_accept(httpd_sb, lsock, &from);
    if (sock < 0)
        return;

    /*
     * Every socket here is non-blocking.  A blocking send() to a client that
     * has stopped reading would stop the whole server, which is the failure
     * this design exists to avoid.
     */
    (VOID)tool_sock_ioctl(httpd_sb, sock, TOOL_FIONBIO, &nonblock);

    httpd_conn[i].sock      = sock;
    httpd_conn[i].file      = (BPTR)0;
    httpd_conn[i].dirlock   = (BPTR)0;
    httpd_conn[i].keepalive = 0;
    httpd_conn[i].progress  = httpd_now();
    httpd_conn[i].requests  = 0;

    /* The same reset a kept-alive connection gets between requests, so there
       is one place where a per-request field's starting value is decided. */
    httpd_reset(&httpd_conn[i]);
    httpd_conn[i].in_len = 0;

    tool_sock_addr_text(httpd_sb, &from, httpd_conn[i].peer,
                        sizeof(httpd_conn[i].peer));

    if (httpd_verbose || httpd_trace)
        httpd_log(&httpd_conn[i], "connected from %s", (LONG)httpd_conn[i].peer,
                  0);
}

static VOID httpd_serve(LONG lsock)
{
    for (;;)
    {
        ToolFdSet   readfds;
        ToolFdSet   writefds;
        ToolTimeval tv;
        LONG        nfds = lsock + 1;
        LONG        ready;
        ULONG       i;
        ULONG       live = 0;
        ULONG       now;

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            return;
        }

        tool_fd_zero(&readfds);
        tool_fd_zero(&writefds);

        for (i = 0; i < httpd_conns; i++)
        {
            HttpConn *c = &httpd_conn[i];

            if (c->state == CONN_FREE)
                continue;

            live++;

            if (c->state == CONN_REQUEST || c->state == CONN_BODY)
                tool_fd_add(&readfds, c->sock);
            else
                tool_fd_add(&writefds, c->sock);

            if (c->sock + 1 > nfds)
                nfds = c->sock + 1;
        }

        /*
         * The listener is offered only when there is somewhere to put the
         * caller.  Leaving it in the set with every slot busy would make
         * WaitSelect() return immediately and for ever, which is a spin, not
         * a wait -- so the backlog holds the caller instead and the machine
         * stays idle.
         */
        if (live < httpd_conns)
            tool_fd_add(&readfds, lsock);

        tv.tv_secs  = 0;
        tv.tv_micro = HTTPD_TICK_MICROS;

        ready = tool_sock_select(httpd_sb, nfds, &readfds, &writefds, &tv);

        if (ready < 0)
        {
            LONG err = tool_sock_errno(httpd_sb);

            if (err == TOOL_EINTR)
                continue;

            tool_error("cannot wait for a connection: %s",
                       (LONG)tool_sock_errstr(err));
            return;
        }

        now = httpd_now();

        if (ready > 0 && live < httpd_conns && tool_fd_isset(&readfds, lsock))
            httpd_accept(lsock);

        for (i = 0; i < httpd_conns; i++)
        {
            HttpConn *c = &httpd_conn[i];
            BOOL      keep = TRUE;

            if (c->state == CONN_FREE)
                continue;

            if (ready > 0 && tool_fd_isset(&readfds, c->sock))
                keep = httpd_readable(c);

            if (keep && c->state == CONN_SEND &&
                ready > 0 && tool_fd_isset(&writefds, c->sock))
                keep = httpd_writable(c);

            if (!keep)
            {
                if (httpd_verbose || httpd_trace)
                    httpd_log(c, "closed after %lu request(s)",
                              (LONG)c->requests, 0);
                httpd_close(c);
                continue;
            }

            /*
             * A connection that has made no progress for TIMEOUT seconds is
             * dropped.  This is what stops a client that opens sockets and
             * says nothing from holding every slot on a 1 MB machine -- the
             * cheapest denial there is, and the one a server with a fixed
             * connection table is most exposed to.
             *
             * `now` going backwards is midnight, not an expiry.
             */
            if (httpd_timeout > 0UL && now >= c->progress &&
                now - c->progress >= httpd_timeout)
            {
                if (httpd_verbose || httpd_trace)
                    httpd_log(c, "no progress for %lu seconds; closing",
                              (LONG)httpd_timeout, 0);
                httpd_close(c);
            }
            else if (now < c->progress)
            {
                c->progress = now;
            }
        }
    }
}

int main(int argc, char **argv)
{
    LONG            args[ARG_COUNT];
    struct RDArgs  *rda;
    ToolAddr        address;
    UWORD           port = 80;
    LONG            lsock;
    ULONG           i;
    char            dotted[TOOL_ADDR_STRLEN];

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    tool_addr_v4(&address, 0);           /* every address, as `nc -l` does */

    for (i = 0; i < (ULONG)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        tool_usage("<drawer> [<port>] [-v] [TRACE]",
                   "Serves a drawer over HTTP and read-only WebDAV, so this "
                   "machine can be mounted as a drive.");
        return RETURN_ERROR;
    }

    httpd_root    = (const char *)args[ARG_ROOT];
    httpd_verbose = (args[ARG_VERBOSE] != 0) ? TRUE : FALSE;
    httpd_trace   = (args[ARG_TRACE]   != 0) ? TRUE : FALSE;

    if (httpd_trace)
        httpd_verbose = TRUE;

    if (args[ARG_PORT] != 0)
    {
        LONG value = *(LONG *)args[ARG_PORT];

        if (value <= 0 || value > 65535)
        {
            tool_error("port %ld is not a port anything listens on", value);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        port = (UWORD)value;
    }

    if (args[ARG_CONNECTIONS] != 0)
    {
        LONG value = *(LONG *)args[ARG_CONNECTIONS];

        if (value < 1 || value > HTTPD_CONN_MAX)
        {
            tool_error("CONNECTIONS has to be between 1 and %ld",
                       (LONG)HTTPD_CONN_MAX);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        httpd_conns = (ULONG)value;
    }

    if (args[ARG_TIMEOUT] != 0)
    {
        LONG value = *(LONG *)args[ARG_TIMEOUT];

        if (value < 0)
        {
            tool_error("TIMEOUT cannot be negative");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        httpd_timeout = (ULONG)value;
    }

    /* The document root has to be there before anything is served from it: a
       server that starts on a misspelled drawer answers 404 to everything and
       looks like a network fault. */
    {
        BPTR lock = Lock((CONST_STRPTR)httpd_root, ACCESS_READ);

        if (lock == (BPTR)0)
        {
            tool_error("there is no \"%s\" to serve", (LONG)httpd_root);
            tool_fault(IoErr());
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        UnLock(lock);
    }

    httpd_read_gmt_offset();

    httpd_sb = tool_socket_open();
    if (httpd_sb == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (args[ARG_ADDRESS] != 0 &&
        !tool_sock_resolve_af(httpd_sb, (const char *)args[ARG_ADDRESS],
                              TOOL_AF_UNSPEC, &address))
    {
        CloseLibrary(httpd_sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    httpd_conn = (HttpConn *)ami_alloc(httpd_conns * (ULONG)sizeof(HttpConn));
    if (httpd_conn == NULL)
    {
        tool_error("not enough memory for %lu connections", httpd_conns);
        CloseLibrary(httpd_sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    for (i = 0; i < httpd_conns; i++)
    {
        httpd_conn[i].sock  = -1;
        httpd_conn[i].state = CONN_FREE;
        httpd_conn[i].fib   = NULL;
    }

    /*
     * One FileInfoBlock per slot, taken once.  260 bytes each, and the
     * alternative -- a stack one -- does not fit: a Shell command has 4 KB of
     * stack and this is not the only thing on it (src/tools/tool_diag.c makes
     * the same call for the same reason).
     */
    for (i = 0; i < httpd_conns; i++)
    {
        httpd_conn[i].fib = (struct FileInfoBlock *)
            ami_alloc((ULONG)sizeof(struct FileInfoBlock));

        if (httpd_conn[i].fib == NULL)
        {
            tool_error("not enough memory for %lu connections", httpd_conns);
            while (i-- > 0UL)
                ami_free(httpd_conn[i].fib);
            ami_free(httpd_conn);
            CloseLibrary(httpd_sb);
            FreeArgs(rda);
            return RETURN_FAIL;
        }
    }

    lsock = httpd_listen(&address, port);
    if (lsock < 0)
    {
        for (i = 0; i < httpd_conns; i++)
            ami_free(httpd_conn[i].fib);
        ami_free(httpd_conn);
        CloseLibrary(httpd_sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    tool_addr_text(httpd_sb, &address, dotted, sizeof(dotted));
    tool_printf("Serving %s on http://%s:%ld/  (Ctrl-C to stop)\n",
                (LONG)httpd_root, (LONG)dotted, (LONG)port);
    (VOID)Flush(Output());

    httpd_serve(lsock);

    for (i = 0; i < httpd_conns; i++)
    {
        if (httpd_conn[i].state != CONN_FREE)
            httpd_close(&httpd_conn[i]);
        ami_free(httpd_conn[i].fib);
    }
    ami_free(httpd_conn);

    (VOID)tool_sock_close(httpd_sb, lsock);
    CloseLibrary(httpd_sb);
    FreeArgs(rda);

    return RETURN_OK;
}
