/*
 * httpd -- an HTTP server with read-write WebDAV, so that a drawer on this
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
 * WHAT IT ANSWERS
 *
 *   OPTIONS, PROPFIND (Depth 0 and 1), GET, HEAD, PUT, DELETE, MKCOL, COPY,
 *   MOVE, PROPPATCH, LOCK and UNLOCK -- WebDAV class 2, which is the class
 *   that has locking.
 *
 *   The class matters more than the verbs do.  Finder mounts a `DAV: 1`
 *   server READ-ONLY however many write methods it answers, so a server that
 *   writes and does not lock is a server macOS will not write to; class 2 and
 *   a lock table is the price of a writable mount rather than an extra.
 *
 * WRITING, WITHOUT LOSING WHAT WAS THERE
 *
 *   A PUT goes to a temporary name in the destination drawer and is renamed
 *   over the target once the last byte has arrived, so a transfer that stops
 *   half way leaves the old file untouched.  Nothing else here writes into a
 *   file a client can already see.
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
 *   buffers are HTTPD_CONN_MAX * ~6 KB, allocated once, and a Shell command
 *   gets 4 KB of stack on a stock Kickstart 3.1 (src/tools/nc.c says the same
 *   at its own buffers).  Writing added nothing to that: a tree walk carries
 *   one path and one FileInfoBlock rather than a stack of them, and the lock
 *   table is a fixed array.
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
 * a slot is 6.2 KB of buffers, so the default eight is 50 KB taken at startup
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
#define HTTPD_BODY_MAX     65536UL  /* a BUFFERED request body: the XML     */
#define HTTPD_TIMEOUT_DEF     30UL  /* seconds of no progress               */
#define HTTPD_BACKLOG          8

/* A walk that a client can ask for -- DELETE and COPY of a drawer -- has to
   stop somewhere, and a filesystem that keeps saying "not empty" is the shape
   a loop here would take.  A tree that needs more passes than this is refused
   half-done and said so in the multistatus. */
#define HTTPD_WALK_MAX     20000UL

/* Locks.  Fixed, like everything else: eight is more than the clients hold at
   once -- Finder locks the file it is writing and nothing else -- and a ninth
   asker is told the server is busy rather than costing memory. */
#define HTTPD_LOCK_MAX         8
#define HTTPD_LOCK_DEF      180UL   /* seconds granted when none was asked  */
#define HTTPD_LOCK_CAP     3600UL   /* the longest this server will hold one */
#define HTTPD_TOKEN_MAX       64    /* "opaquelocktoken:...", as text       */

/* The XML the write methods send is skimmed, not parsed: element names and
   the text between them, both bounded, and no tree.  PROPPATCH names the
   properties it wants and LOCK names an owner; nothing here needs more. */
#define HTTPD_PROPS_MAX        8    /* properties reported on in one 207    */
#define HTTPD_QNAME_MAX       32    /* "Z:Win32LastModifiedTime" is 23      */
#define HTTPD_NS_MAX           3    /* xmlns: bindings carried to the reply */
#define HTTPD_NSURI_MAX       48
#define HTTPD_TEXT_MAX        48    /* an element's character data          */

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
    HTTPD_M_PROPFIND,
    HTTPD_M_PUT,
    HTTPD_M_DELETE,
    HTTPD_M_MKCOL,
    HTTPD_M_COPY,
    HTTPD_M_MOVE,
    HTTPD_M_PROPPATCH,
    HTTPD_M_LOCK,
    HTTPD_M_UNLOCK
};

/* A method may carry a request body. */
#define HTTPD_F_BODY    0x01
/* Changes the tree: the lock check and the Destination check hang off this,
   so a verb added to the table gets both by saying what it is. */
#define HTTPD_F_WRITE   0x02
/* The body is written out as it arrives rather than held, so its length is
   the client's business and not HTTPD_BODY_MAX's.  PUT, and nothing else. */
#define HTTPD_F_UPLOAD  0x04

typedef struct HttpConn HttpConn;

typedef struct HttpMethod
{
    const char *name;
    UBYTE       id;
    UBYTE       flags;
    VOID      (*handle)(HttpConn *c);
    /* Where the request body goes.  NULL discards it, which is what a method
       with nothing to read from one wants. */
    VOID      (*sink)(HttpConn *c, const UBYTE *data, LONG len);
    /* Run once the head has been read and before a byte reaches the sink, so
       a PUT that cannot be started is refused before the client uploads
       anything.  FALSE when it has already answered. */
    BOOL      (*begin)(HttpConn *c);
} HttpMethod;

/* ----------------------------------------------------------- connections --- */

enum
{
    CONN_FREE = 0,
    CONN_REQUEST,       /* reading the request head                        */
    CONN_CONTINUE,      /* pushing the interim 100 out before the body     */
    CONN_BODY,          /* reading the body through the sink               */
    CONN_SEND           /* pushing out[], refilled by the producer         */
};

enum
{
    PROD_NONE = 0,      /* what is in out[] is the whole answer            */
    PROD_FILE,          /* the rest of an open file                        */
    PROD_INDEX,         /* a generated HTML directory listing              */
    PROD_PROPFIND       /* a generated 207 multistatus                     */
};

/* Reading a chunked request body.  The framing is a size line, that many
   bytes, a CRLF, and a zero-sized chunk with optional trailers to end. */
enum
{
    CHUNK_OFF = 0,      /* the body is a Content-Length one                */
    CHUNK_SIZE,
    CHUNK_DATA,
    CHUNK_CRLF,
    CHUNK_TRAILER,
    CHUNK_DONE
};

/* The XML skimmer's position.  Not a parser: it finds element names and the
   text between them and has no opinion about anything else. */
enum
{
    XML_TEXT = 0,       /* between elements                                */
    XML_NAME,           /* inside a tag, reading its name                  */
    XML_ATTRS,          /* inside a tag, past the name                     */
    XML_QUOTE           /* inside an attribute value                       */
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
    UBYTE   expect;                 /* the client is waiting for a 100     */
    UBYTE   overwrite;              /* COPY/MOVE: Overwrite was not F      */
    UBYTE   had_body;               /* a body arrived, whatever its length */
    UBYTE   chunk_state;            /* CHUNK_OFF unless the body is chunked */
    ULONG   chunk_left;
    UBYTE   chunk_n;
    char    chunk_line[24];         /* the size line, as it arrives        */
    ULONG   lock_secs;              /* Timeout: seconds asked for, 0 if none */
    char    dest[HTTP_URL_MAX];     /* Destination:, still as it arrived   */
    char    iftoken[2][HTTPD_TOKEN_MAX];    /* the tokens inside If:       */
    char    unlock_token[HTTPD_TOKEN_MAX];  /* Lock-Token:                 */

    /* PUT: the temporary file the body goes to, until the rename */
    BPTR    put;
    char    put_temp[HTTP_PATH_MAX];
    LONG    put_err;                /* the DOS error that stopped it       */

    /* what the skimmer found in the body */
    UBYTE   xml_state;
    UBYTE   xml_name_n;
    char    xml_name[HTTPD_QNAME_MAX];
    UBYTE   xml_close;              /* the tag being read is a </close>    */
    UBYTE   xml_text_n;
    char    xml_text[HTTPD_TEXT_MAX];
    UBYTE   xml_attr_n;
    char    xml_attr[HTTPD_QNAME_MAX + HTTPD_NSURI_MAX];
    UBYTE   in_prop;                /* inside <prop>: children are names   */
    UBYTE   in_owner;               /* inside <owner>: text is the owner   */
    UBYTE   props;
    UBYTE   prop_ok[HTTPD_PROPS_MAX];
    char    prop_name[HTTPD_PROPS_MAX][HTTPD_QNAME_MAX];
    UBYTE   nsdecls;
    char    nsdecl[HTTPD_NS_MAX][HTTPD_QNAME_MAX + HTTPD_NSURI_MAX];
    UBYTE   have_date;
    struct DateStamp prop_date;
    char    owner[HTTPD_TEXT_MAX];

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
 * a static one is the whole ceiling whether it is used or not: at 6.2 KB a
 * slot that is 99 KB of BSS in every copy of the command, which on a 1 MB
 * machine is a tenth of the machine reserved for connections nobody asked for.
 * Allocating it makes CONNECTIONS mean something -- `-m 2` is 12 KB -- and
 * takes the command's own BSS down to the shared scratches and the locks.
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
static char httpd_page[512];

/*
 * The two paths a tree walk carries, and the destination a COPY or a MOVE
 * resolved to.  Shared for the reason the scratches above are: a walk runs to
 * completion inside one dispatch, so no second request can be inside one at
 * the same time -- the connections interleave between passes of the loop and
 * never inside a handler.
 */
static char     httpd_walk_src[HTTP_PATH_MAX];
static char     httpd_walk_dst[HTTP_PATH_MAX];
static char     httpd_child[HTTP_PATH_MAX];
static HttpPath httpd_dest;

/*
 * The lock table.  Static rather than allocated with the connections: it is
 * 3 KB whatever CONNECTIONS says, because a lock outlives the connection that
 * took it -- that is the whole point of one.
 */
typedef struct HttpLock
{
    char  path[HTTP_PATH_MAX];      /* the AmigaOS path it covers          */
    char  token[HTTPD_TOKEN_MAX];
    char  owner[HTTPD_TEXT_MAX];
    ULONG expires;                  /* httpd_now() seconds                 */
    ULONG timeout;                  /* what was granted, for the reply     */
    UBYTE depth;                    /* 1 when it covers everything below   */
    UBYTE used;
} HttpLock;

static HttpLock httpd_locks[HTTPD_LOCK_MAX];
static ULONG    httpd_token_seed;

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
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 423: return "Locked";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 507: return "Insufficient Storage";
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

/* An answer that is a status line and nothing else.  204 carries no
   Content-Length at all -- RFC 7230 8.1.2 -- and 201 after a PUT is the one
   answer a client reads before it will believe the file is there. */
static VOID httpd_empty(HttpConn *c, ULONG status)
{
    httpd_begin(c, status);

    if (status != 204)
        httpd_header_num(c, "Content-Length", 0);

    httpd_finish_head(c);
    c->producer = PROD_NONE;
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

/*
 * A PUT that never finished.  The temporary is deleted rather than left, so an
 * abandoned upload costs nothing and does not appear in a listing -- which is
 * the other half of writing to a temporary name: the client sees the old file
 * or the new one, and never a third thing.
 */
static VOID httpd_put_abandon(HttpConn *c)
{
    if (c->put != (BPTR)0)
    {
        (VOID)Close(c->put);
        c->put = (BPTR)0;
    }

    if (c->put_temp[0] != '\0')
    {
        (VOID)DeleteFile((CONST_STRPTR)c->put_temp);
        c->put_temp[0] = '\0';
    }
}

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

    httpd_put_abandon(c);

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

    httpd_put_abandon(c);

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

    c->expect      = 0;
    c->overwrite   = 1;             /* Overwrite defaults to T, RFC 4918 10.6 */
    c->had_body    = 0;
    c->chunk_state = CHUNK_OFF;
    c->chunk_left  = 0;
    c->chunk_n     = 0;
    c->lock_secs   = 0;
    c->dest[0]     = '\0';
    c->iftoken[0][0] = '\0';
    c->iftoken[1][0] = '\0';
    c->unlock_token[0] = '\0';
    c->put_err     = 0;

    c->xml_state  = XML_TEXT;
    c->xml_name_n = 0;
    c->xml_close  = 0;
    c->xml_text_n = 0;
    c->xml_attr_n = 0;
    c->in_prop    = 0;
    c->in_owner   = 0;
    c->props      = 0;
    c->nsdecls    = 0;
    c->have_date  = 0;
    c->owner[0]   = '\0';
}

/* ------------------------------------------------------------ the volume --- */

/* One extra FileInfoBlock and one InfoData, taken once at startup for the same
   reason the per-connection ones are: a Shell command has 4 KB of stack and
   these are 260 and 224 bytes of it.  Both are only ever used inside a
   handler, which runs to completion in one pass of the loop. */
static struct FileInfoBlock *httpd_fib2;
static struct InfoData      *httpd_info;

/*
 * An AmigaDOS error as an HTTP status.  One table, because every write method
 * wants the same answer to the same failure -- and it is the difference
 * between a client saying "the disk is full" and saying "forbidden".
 */
static ULONG httpd_dos_status(LONG err)
{
    switch (err)
    {
        case ERROR_OBJECT_NOT_FOUND:
        case ERROR_DIR_NOT_FOUND:           return 409;
        case ERROR_OBJECT_EXISTS:           return 405;
        case ERROR_DIRECTORY_NOT_EMPTY:
        case ERROR_OBJECT_IN_USE:           return 409;
        case ERROR_DISK_FULL:               return 507;
        /* A name the filesystem will not carry -- too long for OFS, or a
           character it reserves.  Refused, and not truncated into a name that
           would collide with a file that is already there. */
        case ERROR_INVALID_COMPONENT_NAME:
        case ERROR_BAD_STREAM_NAME:         return 400;
        default:                            return 403;
    }
}

/* The drawer a path lives in: "Work:Public/a/b" is "Work:Public/a" and
   "RAM:foo" is "RAM:".  FALSE when there is no separator at all, which is a
   path that names a device and has no parent here. */
static BOOL httpd_parent(const char *path, char *out, ULONG outlen)
{
    ULONG n = hs_len(path);
    ULONG cut = 0;
    ULONG i;
    BOOL  found = FALSE;

    for (i = 0; i < n; i++)
    {
        if (path[i] == '/')
        {
            cut   = i;              /* the separator is dropped            */
            found = TRUE;
        }
        else if (path[i] == ':')
        {
            cut   = i + 1UL;        /* the colon belongs to the device     */
            found = TRUE;
        }
    }

    if (!found || cut + 1UL >= outlen)
        return FALSE;

    for (i = 0; i < cut; i++)
        out[i] = path[i];
    out[cut] = '\0';

    return TRUE;
}

/* 1 a drawer, 0 a file, -1 nothing there. */
static LONG httpd_kind(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    LONG kind = 0;

    if (lock == (BPTR)0)
        return -1;

    if (httpd_fib2 != NULL && Examine(lock, httpd_fib2) &&
        httpd_fib2->fib_DirEntryType > 0)
        kind = 1;

    UnLock(lock);

    return kind;
}

/*
 * Free bytes on the volume `path` is on, or 0 when it cannot be told.  A PUT
 * that will not fit is refused before the upload rather than after it, which
 * is the difference between a 507 and a floppy full of a temporary file.
 */
static ULONG httpd_free_bytes(const char *path)
{
    BPTR  lock;
    ULONG blocks;
    ULONG per;

    if (httpd_info == NULL)
        return 0;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return 0;

    if (!Info(lock, httpd_info))
    {
        UnLock(lock);
        return 0;
    }
    UnLock(lock);

    if (httpd_info->id_NumBlocks <= httpd_info->id_NumBlocksUsed ||
        httpd_info->id_BytesPerBlock <= 0)
        return 0;

    blocks = (ULONG)(httpd_info->id_NumBlocks - httpd_info->id_NumBlocksUsed);
    per    = (ULONG)httpd_info->id_BytesPerBlock;

    /* Saturate rather than wrap: "more than anybody is about to ask for" is
       the only answer a big volume needs to give. */
    if (blocks > 0xffffffffUL / per)
        return 0xffffffffUL;

    return blocks * per;
}

/*
 * The walk's three primitives -- http_path_join(), http_path_up() and
 * http_path_within() -- are in src/tools/httppath.c and not here, for the
 * reason the resolver is: between them they decide which file a DELETE
 * removes, and there they are compiled for the host and driven by
 * src/tools/test/test_httppath.c.
 */

/*
 * The URL a walked path corresponds to, escaped for an href.  Every path a
 * walk produces was built by pushing onto the resolved one, so it begins with
 * the document root and the rest of it is the URL.
 */
static const char *httpd_url_of(const char *path)
{
    ULONG rootlen = hs_len(httpd_root);
    ULONG used = 0;

    if (hs_nicmp(path, httpd_root, rootlen) != 0)
        return "/";

    path += rootlen;

    httpd_href_buf[0] = '\0';

    if (*path != '/')
        (VOID)hs_append(httpd_href_buf, sizeof(httpd_href_buf), &used, "/");

    if (!hs_append(httpd_href_buf, sizeof(httpd_href_buf), &used, path))
        return "/";

    if (http_url_escape(httpd_href_buf, httpd_escape,
                        sizeof(httpd_escape)) == 0UL)
        return "/";

    return httpd_escape;
}

/*
 * One file to another, through the shared scratch.  The date and the
 * protection bits follow it: a copy that loses them is a copy every client
 * shows as a different file.
 */
static ULONG httpd_copy_file(const char *src, const char *dst)
{
    BPTR  in;
    BPTR  out;
    ULONG status = 0;

    in = Open((CONST_STRPTR)src, MODE_OLDFILE);
    if (in == (BPTR)0)
        return httpd_dos_status(IoErr());

    out = Open((CONST_STRPTR)dst, MODE_NEWFILE);
    if (out == (BPTR)0)
    {
        status = httpd_dos_status(IoErr());
        (VOID)Close(in);
        return status;
    }

    for (;;)
    {
        LONG got = Read(in, (APTR)httpd_scratch, (LONG)sizeof(httpd_scratch));

        if (got < 0)
        {
            status = 500;
            break;
        }

        if (got == 0)
            break;

        if (Write(out, (APTR)httpd_scratch, got) != got)
        {
            status = httpd_dos_status(IoErr());
            break;
        }
    }

    (VOID)Close(out);
    (VOID)Close(in);

    if (status != 0)
    {
        (VOID)DeleteFile((CONST_STRPTR)dst);
        return status;
    }

    if (httpd_fib2 != NULL)
    {
        BPTR lock = Lock((CONST_STRPTR)src, ACCESS_READ);

        if (lock != (BPTR)0)
        {
            if (Examine(lock, httpd_fib2))
            {
                (VOID)SetFileDate((CONST_STRPTR)dst, &httpd_fib2->fib_Date);
                (VOID)SetProtection((CONST_STRPTR)dst,
                                    (LONG)httpd_fib2->fib_Protection);
            }
            UnLock(lock);
        }
    }

    return 0;
}

/*
 * Everything under `path`, then `path` itself.  DELETE on a collection is
 * Depth infinity and nothing else, so this is what the method is.
 *
 * Iterative and by name rather than by lock: a Shell command has 4 KB of
 * stack and a FileInfoBlock is 260 bytes of it, so a walk that recursed would
 * run out at a depth a real drawer reaches.  One drawer is scanned at a time,
 * a subdrawer is descended into by appending to the path, and coming back up
 * is a truncation.
 *
 * ExNext() is not trusted across a deletion -- no filesystem promises it -- so
 * "is this drawer empty now" is asked of DeleteFile() and not of the scan: a
 * drawer is finished when deleting it succeeds, and ERROR_DIRECTORY_NOT_EMPTY
 * sends it round again.
 *
 * 0, or the status of the first failure with the path that failed left in
 * `path` for the multistatus to name.
 */
static ULONG httpd_delete_tree(char *path, ULONG pathlen,
                               struct FileInfoBlock *fib)
{
    ULONG depth = 0;
    ULONG steps = 0;

    for (;;)
    {
        BPTR lock;
        BOOL descend = FALSE;

        if (++steps > HTTPD_WALK_MAX)
            return 500;

        lock = Lock((CONST_STRPTR)path, ACCESS_READ);
        if (lock == (BPTR)0)
            return (depth == 0UL) ? 404 : httpd_dos_status(IoErr());

        if (!Examine(lock, fib))
        {
            UnLock(lock);
            return 500;
        }

        if (fib->fib_DirEntryType <= 0)
        {
            UnLock(lock);

            if (!DeleteFile((CONST_STRPTR)path))
                return httpd_dos_status(IoErr());
        }
        else
        {
            /* Every file in this drawer, and then the first subdrawer. */
            while (ExNext(lock, fib))
            {
                if (fib->fib_DirEntryType > 0)
                {
                    descend = TRUE;
                    break;
                }

                if (!http_path_join(path, pathlen,
                                     (const char *)fib->fib_FileName))
                {
                    UnLock(lock);
                    return 414;
                }

                if (!DeleteFile((CONST_STRPTR)path))
                {
                    ULONG why = httpd_dos_status(IoErr());

                    UnLock(lock);
                    return why;
                }

                http_path_up(path);
            }

            if (descend)
            {
                BOOL ok = http_path_join(path, pathlen,
                                          (const char *)fib->fib_FileName);

                UnLock(lock);

                if (!ok)
                    return 414;

                depth++;
                continue;
            }

            UnLock(lock);

            if (!DeleteFile((CONST_STRPTR)path))
            {
                LONG err = IoErr();

                /* The scan stopped early, or something appeared while it ran.
                   Round again rather than call it a failure. */
                if (err == ERROR_DIRECTORY_NOT_EMPTY)
                    continue;

                return httpd_dos_status(err);
            }
        }

        if (depth == 0UL)
            return 0;

        http_path_up(path);
        depth--;
    }
}

/*
 * `src` to `dst`, everything below it.  The delete walk's shape with the
 * destination doing the remembering: an entry that is already in the
 * destination has been copied, so a drawer can be picked up again on a later
 * pass without a stack of positions.  Nothing here modifies the source, so
 * the files all go in one scan and only a subdrawer costs a rescan.
 */
static ULONG httpd_copy_tree(char *src, ULONG srclen, char *dst, ULONG dstlen,
                             struct FileInfoBlock *fib)
{
    ULONG depth = 0;
    ULONG steps = 0;
    BPTR  made;

    made = CreateDir((CONST_STRPTR)dst);
    if (made == (BPTR)0)
        return httpd_dos_status(IoErr());
    UnLock(made);

    for (;;)
    {
        BPTR lock;
        BOOL descend = FALSE;

        if (++steps > HTTPD_WALK_MAX)
            return 500;

        lock = Lock((CONST_STRPTR)src, ACCESS_READ);
        if (lock == (BPTR)0)
            return httpd_dos_status(IoErr());

        if (!Examine(lock, fib))
        {
            UnLock(lock);
            return 500;
        }

        while (ExNext(lock, fib))
        {
            ULONG why;

            if (!http_path_join(src, srclen,
                                 (const char *)fib->fib_FileName) ||
                !http_path_join(dst, dstlen,
                                 (const char *)fib->fib_FileName))
            {
                UnLock(lock);
                return 414;
            }

            if (httpd_kind(dst) >= 0)       /* done on an earlier pass     */
            {
                http_path_up(src);
                http_path_up(dst);
                continue;
            }

            if (fib->fib_DirEntryType > 0)
            {
                descend = TRUE;             /* both paths stay pushed      */
                break;
            }

            why = httpd_copy_file(src, dst);

            http_path_up(src);
            http_path_up(dst);

            if (why != 0UL)
            {
                UnLock(lock);
                return why;
            }
        }

        UnLock(lock);

        if (descend)
        {
            made = CreateDir((CONST_STRPTR)dst);
            if (made == (BPTR)0)
                return httpd_dos_status(IoErr());
            UnLock(made);

            depth++;
            continue;
        }

        if (depth == 0UL)
            return 0;

        http_path_up(src);
        http_path_up(dst);
        depth--;
    }
}

/* ------------------------------------------------------------------ dates --- */

static BOOL httpd_leap(LONG year)
{
    return ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
               ? TRUE : FALSE;
}

/* The inverse of httpd_civil(), for the timestamps a client sends back. */
static ULONG httpd_days_from_civil(LONG y, LONG mo, LONG d)
{
    static const ULONG cum[12] =
        { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

    ULONG days = 0;
    LONG  year;

    for (year = 1970; year < y; year++)
        days += httpd_leap(year) ? 366UL : 365UL;

    days += cum[mo - 1];

    if (mo > 2 && httpd_leap(y))
        days++;

    return days + (ULONG)(d - 1);
}

static LONG httpd_month(const char *name)
{
    static const char *const months[12] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    LONG i;

    for (i = 0; i < 12; i++)
    {
        if (hs_nicmp(name, months[i], 3) == 0)
            return i + 1;
    }

    return 0;
}

static ULONG httpd_digits(const char **p, ULONG count)
{
    const char *s = *p;
    ULONG value = 0;
    ULONG i;

    for (i = 0; i < count && s[i] >= '0' && s[i] <= '9'; i++)
        value = (value * 10UL) + (ULONG)(s[i] - '0');

    *p = s + i;

    return value;
}

/*
 * "Tue, 05 Aug 2025 12:00:00 GMT" back to a DateStamp in local time -- which
 * is what SetFileDate() takes, and the reason httpd_read_gmt_offset() is read
 * at startup rather than only on the way out.
 */
static BOOL httpd_parse_rfc1123(const char *text, struct DateStamp *ds)
{
    LONG  day;
    LONG  month;
    LONG  year;
    ULONG h;
    ULONG mi;
    ULONG s;
    ULONG secs;

    while (*text == ' ')
        text++;

    /* The day name is optional here: "05 Aug 2025 ..." is what some clients
       send and nothing downstream reads the name anyway. */
    if (text[0] != '\0' && text[1] != '\0' && text[2] != '\0' &&
        text[3] == ',')
        text += 4;

    while (*text == ' ')
        text++;

    day = (LONG)httpd_digits(&text, 2);
    while (*text == ' ' || *text == '-')
        text++;

    month = httpd_month(text);
    if (month == 0 || day < 1 || day > 31)
        return FALSE;

    text += 3;
    while (*text == ' ' || *text == '-')
        text++;

    year = (LONG)httpd_digits(&text, 4);
    if (year < 1978 || year > 2100)
        return FALSE;

    while (*text == ' ')
        text++;

    h = httpd_digits(&text, 2);
    if (*text == ':') text++;
    mi = httpd_digits(&text, 2);
    if (*text == ':') text++;
    s = httpd_digits(&text, 2);

    if (h > 23UL || mi > 59UL || s > 60UL)
        return FALSE;

    secs = httpd_days_from_civil(year, month, day) * 86400UL;
    secs += (h * 3600UL) + (mi * 60UL) + s;

    /* The stamp is GMT and a DateStamp is local, so this undoes exactly what
       httpd_stamp_secs() does on the way out. */
    if (httpd_gmt_west > 0)
    {
        ULONG west = (ULONG)httpd_gmt_west * 60UL;

        if (secs < west)
            return FALSE;
        secs -= west;
    }
    else if (httpd_gmt_west < 0)
    {
        secs += (ULONG)(-httpd_gmt_west) * 60UL;
    }

    if (secs < HTTPD_AMIGA_EPOCH)
        return FALSE;

    secs -= HTTPD_AMIGA_EPOCH;

    ds->ds_Days   = (LONG)(secs / 86400UL);
    ds->ds_Minute = (LONG)((secs % 86400UL) / 60UL);
    ds->ds_Tick   = (LONG)((secs % 60UL) * (ULONG)TICKS_PER_SECOND);

    return TRUE;
}

/* --------------------------------------------------------------- locking --- */

/*
 * Class 2 is not an extra.  Finder asks for a lock before it writes and reads
 * a `DAV: 1` answer as "this share cannot be written", so it mounts read-only
 * however many write methods the server answers -- the lock table is what
 * makes macOS write at all, and stopping two clients writing one file is the
 * second thing it does rather than the first.
 *
 * Exclusive write locks only.  Nothing that mounts a drive asks for a shared
 * one, and granting an exclusive lock to a client that did is the safe half.
 */

/* Expiry is lazy, on the next question anybody asks: there is no timer here
   and a lock nobody asks about costs nothing to leave lying. */
static VOID httpd_locks_expire(VOID)
{
    ULONG now = httpd_now();
    ULONG i;

    for (i = 0; i < (ULONG)HTTPD_LOCK_MAX; i++)
    {
        if (httpd_locks[i].used && now >= httpd_locks[i].expires)
            httpd_locks[i].used = 0;
    }
}

/* The lock covering `path`: its own, or a drawer's above it. */
static HttpLock *httpd_lock_on(const char *path)
{
    ULONG i;

    httpd_locks_expire();

    for (i = 0; i < (ULONG)HTTPD_LOCK_MAX; i++)
    {
        HttpLock *l = &httpd_locks[i];

        if (!l->used)
            continue;

        if (hs_equal(l->path, path))
            return l;

        if (l->depth != 0 && http_path_within(l->path, path))
            return l;
    }

    return NULL;
}

static HttpLock *httpd_lock_by_token(const char *token)
{
    ULONG i;

    if (token == NULL || token[0] == '\0')
        return NULL;

    httpd_locks_expire();

    for (i = 0; i < (ULONG)HTTPD_LOCK_MAX; i++)
    {
        if (httpd_locks[i].used && hs_equal(httpd_locks[i].token, token))
            return &httpd_locks[i];
    }

    return NULL;
}

/* Does this request carry the token for `l`?  No lock is everybody's. */
static BOOL httpd_holds(const HttpConn *c, const HttpLock *l)
{
    if (l == NULL)
        return TRUE;

    return (hs_equal(c->iftoken[0], l->token) ||
            hs_equal(c->iftoken[1], l->token)) ? TRUE : FALSE;
}

/*
 * The check every write goes through.  FALSE when it has answered with the
 * 423 that tells a client to take a lock rather than to keep retrying.
 */
static BOOL httpd_lock_allows(HttpConn *c, const char *path)
{
    if (httpd_holds(c, httpd_lock_on(path)))
        return TRUE;

    httpd_begin(c, 423);
    httpd_body_text(c, "text/xml; charset=utf-8",
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<D:error xmlns:D=\"DAV:\">"
                    "<D:lock-token-submitted/></D:error>\n");

    return FALSE;
}

/*
 * "opaquelocktoken:" and eight hex digits.  The seed is the clock at startup,
 * so a token from a previous run of the server does not unlock a file in this
 * one -- which is the only thing an easily-guessed token could do here.
 */
static VOID httpd_make_token(char *out, ULONG outlen)
{
    static const char hex[] = "0123456789abcdef";
    ULONG used = 0;
    ULONG value;
    LONG  shift;

    httpd_token_seed = (httpd_token_seed * 1103515245UL) + 12345UL;
    value = httpd_token_seed ^ (httpd_now() << 8);

    out[0] = '\0';
    (VOID)hs_append(out, outlen, &used, "opaquelocktoken:");

    for (shift = 28; shift >= 0; shift -= 4)
    {
        char one[2];

        one[0] = hex[(value >> (ULONG)shift) & 0xfUL];
        one[1] = '\0';
        (VOID)hs_append(out, outlen, &used, one);
    }
}

/*
 * One <D:activelock>: what LOCK answers with and what PROPFIND reports about
 * a resource somebody is holding.  Appended rather than returned, so the
 * caller decides what element it goes inside.
 */
static BOOL httpd_activelock(const HttpLock *l, char *out, ULONG outlen,
                             ULONG *used)
{
    BOOL ok;

    ok = hs_append(out, outlen, used,
                   "<D:activelock><D:locktype><D:write/></D:locktype>"
                   "<D:lockscope><D:exclusive/></D:lockscope><D:depth>");
    ok = ok && hs_append(out, outlen, used,
                         (l->depth != 0) ? "infinity" : "0");
    ok = ok && hs_append(out, outlen, used, "</D:depth>");

    if (l->owner[0] != '\0')
    {
        (VOID)http_xml_escape(l->owner, httpd_text, sizeof(httpd_text));
        ok = ok && hs_append(out, outlen, used, "<D:owner>");
        ok = ok && hs_append(out, outlen, used, httpd_text);
        ok = ok && hs_append(out, outlen, used, "</D:owner>");
    }

    ok = ok && hs_append(out, outlen, used, "<D:timeout>Second-");
    ok = ok && hs_append_num(out, outlen, used, l->timeout);
    ok = ok && hs_append(out, outlen, used,
                         "</D:timeout><D:locktoken><D:href>");
    ok = ok && hs_append(out, outlen, used, l->token);
    ok = ok && hs_append(out, outlen, used,
                         "</D:href></D:locktoken><D:lockroot><D:href>");
    ok = ok && hs_append(out, outlen, used, httpd_url_of(l->path));
    ok = ok && hs_append(out, outlen, used,
                         "</D:href></D:lockroot></D:activelock>");

    return ok;
}

/* -------------------------------------------------------------- skimming --- */

/*
 * What the write methods need out of a request body, without an XML parser.
 * PROPPATCH names the properties it wants and LOCK names an owner: both are
 * element names and the text between them, both are bounded, and a tree is
 * the right answer on a machine with room for one.
 *
 * The namespace declarations are carried through as they arrived rather than
 * resolved, because the 207 has to name the properties back and a prefix
 * rebound to the wrong URI is a property the client does not recognise as
 * the one it asked about.
 */

static const char *httpd_local(const char *qname)
{
    ULONG i;

    for (i = 0; qname[i] != '\0'; i++)
    {
        if (qname[i] == ':')
            return &qname[i + 1];
    }

    return qname;
}

static VOID httpd_note_property(HttpConn *c)
{
    if (c->props >= (UBYTE)HTTPD_PROPS_MAX)
        return;

    hs_copy(c->prop_name[c->props], (ULONG)HTTPD_QNAME_MAX, c->xml_name);
    c->prop_ok[c->props] = 0;
    c->props++;
}

/* The property just closed, with whatever text it held.  AmigaOS keeps one
   date per file, so the modification time is the only thing here that maps to
   a call; everything else is answered 403 in the 207. */
static VOID httpd_set_property(HttpConn *c)
{
    const char *local = httpd_local(c->xml_name);
    UBYTE       i;

    if (!hs_equal(local, "Win32LastModifiedTime") &&
        !hs_equal(local, "getlastmodified"))
        return;

    c->xml_text[c->xml_text_n] = '\0';

    if (!httpd_parse_rfc1123(c->xml_text, &c->prop_date))
        return;

    c->have_date = 1;

    for (i = 0; i < c->props; i++)
    {
        if (hs_equal(c->prop_name[i], c->xml_name))
            c->prop_ok[i] = 1;
    }
}

static VOID httpd_note_nsdecl(HttpConn *c)
{
    UBYTE i;

    if (hs_nicmp(c->xml_attr, "xmlns", 5) != 0)
        return;

    if (c->nsdecls >= (UBYTE)HTTPD_NS_MAX)
        return;

    for (i = 0; i < c->nsdecls; i++)
    {
        if (hs_equal(c->nsdecl[i], c->xml_attr))
            return;
    }

    hs_copy(c->nsdecl[c->nsdecls],
            (ULONG)(HTTPD_QNAME_MAX + HTTPD_NSURI_MAX), c->xml_attr);
    c->nsdecls++;
}

/* A complete start or end tag.  `selfclose` means both at once. */
static VOID httpd_xml_tag(HttpConn *c, BOOL closing, BOOL selfclose)
{
    const char *local = httpd_local(c->xml_name);

    if (!closing)
    {
        if (hs_equal(local, "prop"))
        {
            c->in_prop = 1;
        }
        else if (hs_equal(local, "owner"))
        {
            c->in_owner  = 1;
            c->owner[0]  = '\0';
        }
        else if (c->in_prop)
        {
            httpd_note_property(c);
        }

        c->xml_text_n = 0;
    }

    if (closing || selfclose)
    {
        if (hs_equal(local, "prop"))
            c->in_prop = 0;
        else if (hs_equal(local, "owner"))
            c->in_owner = 0;
        else if (c->in_prop)
            httpd_set_property(c);
    }
}

/*
 * Feed the body through.  Called from the sinks, so it must survive being
 * handed one byte at a time: everything it is in the middle of is in the
 * connection and not on the stack.
 */
static VOID httpd_xml_feed(HttpConn *c, const UBYTE *data, LONG len)
{
    LONG i;

    for (i = 0; i < len; i++)
    {
        int ch = data[i];

        switch (c->xml_state)
        {
            case XML_TEXT:
                if (ch == '<')
                {
                    c->xml_state  = XML_NAME;
                    c->xml_name_n = 0;
                    c->xml_close  = 0;
                    c->xml_attr_n = 0;
                }
                else if (c->in_owner)
                {
                    ULONG n = hs_len(c->owner);

                    /* Markup inside <owner> contributes nothing and its text
                       does, which is what an <owner> holding an <href> needs:
                       the address, and not the element around it. */
                    if (n + 1UL < sizeof(c->owner) && ch >= 0x20 && ch < 0x7f)
                    {
                        c->owner[n]     = (char)ch;
                        c->owner[n + 1] = '\0';
                    }
                }
                else if (c->xml_text_n + 1U < sizeof(c->xml_text))
                {
                    c->xml_text[c->xml_text_n++] = (char)ch;
                }
                break;

            case XML_NAME:
                if (ch == '/' && c->xml_name_n == 0U)
                {
                    c->xml_close = 1;
                }
                else if (ch == '>' || ch == ' ' || ch == '\t' ||
                         ch == '\r' || ch == '\n' || ch == '/')
                {
                    c->xml_name[c->xml_name_n] = '\0';

                    if (ch == '>')
                    {
                        httpd_xml_tag(c, c->xml_close ? TRUE : FALSE, FALSE);
                        c->xml_state = XML_TEXT;
                        if (!c->xml_close)
                            c->xml_text_n = 0;
                    }
                    else if (ch == '/')
                    {
                        httpd_xml_tag(c, FALSE, TRUE);
                        c->xml_state = XML_ATTRS;
                        c->xml_close = 1;   /* the '>' has nothing left to do */
                    }
                    else
                    {
                        c->xml_state  = XML_ATTRS;
                        c->xml_attr_n = 0;
                    }
                }
                else if (c->xml_name_n + 1U < sizeof(c->xml_name))
                {
                    c->xml_name[c->xml_name_n++] = (char)ch;
                }
                break;

            case XML_ATTRS:
                if (ch == '"' || ch == '\'')
                {
                    if (c->xml_attr_n + 1U <
                        sizeof(c->xml_attr))
                        c->xml_attr[c->xml_attr_n++] = '"';
                    c->xml_state = XML_QUOTE;
                }
                else if (ch == '>')
                {
                    if (c->xml_close)
                        c->xml_state = XML_TEXT;
                    else
                    {
                        httpd_xml_tag(c, FALSE, FALSE);
                        c->xml_state  = XML_TEXT;
                        c->xml_text_n = 0;
                    }
                }
                else if (ch == '/')
                {
                    /* "<x a=1/>": a start and an end with nothing between. */
                    if (!c->xml_close)
                    {
                        httpd_xml_tag(c, FALSE, TRUE);
                        c->xml_close = 1;
                    }
                }
                else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
                {
                    c->xml_attr[c->xml_attr_n] = '\0';
                    httpd_note_nsdecl(c);
                    c->xml_attr_n = 0;
                }
                else if (c->xml_attr_n + 1U < sizeof(c->xml_attr))
                {
                    c->xml_attr[c->xml_attr_n++] = (char)ch;
                }
                break;

            default:                        /* XML_QUOTE                   */
                if (ch == '"' || ch == '\'')
                {
                    if (c->xml_attr_n + 1U < sizeof(c->xml_attr))
                        c->xml_attr[c->xml_attr_n++] = '"';
                    c->xml_attr[c->xml_attr_n] = '\0';
                    httpd_note_nsdecl(c);
                    c->xml_attr_n = 0;
                    c->xml_state  = XML_ATTRS;
                }
                else if (c->xml_attr_n + 1U < sizeof(c->xml_attr))
                {
                    c->xml_attr[c->xml_attr_n++] = (char)ch;
                }
                break;
        }
    }
}

/* Every method that reads XML reads it the same way. */
static VOID httpd_sink_xml(HttpConn *c, const UBYTE *data, LONG len)
{
    httpd_xml_feed(c, data, len);
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

/*
 * One <D:response> for a file or a collection, into the shared scratch.
 *
 * `path` is the AmigaOS path, which is here only so the lock can be looked
 * up: a client that has taken a lock reads lockdiscovery to check it is still
 * held, and one that has not reads supportedlock to decide whether to ask.
 */
static ULONG httpd_propfind_entry(const char *href, const char *name,
                                  const char *path,
                                  BOOL is_dir, ULONG size,
                                  const struct DateStamp *date)
{
    const HttpLock *l = (path != NULL) ? httpd_lock_on(path) : NULL;
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
                         "<D:supportedlock><D:lockentry>"
                         "<D:lockscope><D:exclusive/></D:lockscope>"
                         "<D:locktype><D:write/></D:locktype>"
                         "</D:lockentry></D:supportedlock>");

    if (l != NULL)
    {
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             "<D:lockdiscovery>");
        ok = ok && httpd_activelock(l, httpd_scratch, sizeof(httpd_scratch),
                                    &used);
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             "</D:lockdiscovery>");
    }
    else
    {
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             "<D:lockdiscovery/>");
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
                                      name, c->path.path, is_dir, size,
                                      &date);
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
                        {
                            const char *full = NULL;

                            hs_copy(httpd_child, sizeof(httpd_child),
                                    c->path.path);

                            if (http_path_join(httpd_child,
                                                sizeof(httpd_child), name))
                                full = httpd_child;

                            len = httpd_propfind_entry(
                                      httpd_href(&c->path, name, is_dir),
                                      name, full, is_dir,
                                      (ULONG)c->fib->fib_Size,
                                      &c->fib->fib_Date);
                        }
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
     * DAV: 1,2.  Class 2 is locking, and the reason it is here rather than
     * left out is macOS: Finder mounts a class 1 share READ-ONLY whatever
     * else the server answers, because it asks for a lock before it writes
     * and reads the absence of the class as "this share cannot be written".
     * The 2 is therefore not a claim about how much of RFC 4918 is
     * implemented -- it is the thing that decides whether the drive is
     * writable, so LOCK has to exist for it to be true.
     */
    httpd_header(c, "DAV", "1,2");
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
    httpd_header(c, "DAV", "1,2");
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

/* --------------------------------------------------------------- writing --- */

/*
 * What every write goes through first.  The document root itself is not a
 * resource a client may replace or remove, and a lock somebody else holds
 * stops the request here rather than half way through it.
 */
static BOOL httpd_may_write(HttpConn *c)
{
    if (c->path.segments == 0)
    {
        httpd_error(c, 403, "the served drawer itself is not writable");
        return FALSE;
    }

    return httpd_lock_allows(c, c->path.path);
}

/*
 * The temporary a PUT is written to.  It goes in the DESTINATION drawer and
 * not in T:, because a rename between volumes is a copy -- and the point of
 * the temporary is that the last step is a rename and nothing else.
 *
 * Everything that can refuse the transfer refuses it HERE, before the client
 * has sent a byte: a 507 now stops the upload, and a 507 at the end means the
 * file crossed the network for nothing.
 */
static BOOL httpd_begin_put(HttpConn *c)
{
    ULONG used;

    if (!httpd_may_write(c))
        return FALSE;

    if (httpd_kind(c->path.path) > 0 || c->path.trailing_slash)
    {
        httpd_error(c, 405, "that address is a drawer");
        return FALSE;
    }

    if (!httpd_parent(c->path.path, c->put_temp, sizeof(c->put_temp)) ||
        httpd_kind(c->put_temp) <= 0)
    {
        c->put_temp[0] = '\0';
        httpd_error(c, 409, "there is no drawer to put that in");
        return FALSE;
    }

    if (c->body_left > 0UL)
    {
        ULONG room = httpd_free_bytes(c->put_temp);

        if (room > 0UL && c->body_left > room)
        {
            c->put_temp[0] = '\0';
            httpd_error(c, 507, "there is not enough room on that volume");
            return FALSE;
        }
    }

    /* One name per connection slot, so two uploads into one drawer cannot
       collide, and short enough for a filesystem that stops at 30
       characters. */
    used = hs_len(c->put_temp);

    if (used > 0UL && c->put_temp[used - 1] != ':' &&
        c->put_temp[used - 1] != '/')
        (VOID)hs_append(c->put_temp, sizeof(c->put_temp), &used, "/");

    (VOID)hs_append(c->put_temp, sizeof(c->put_temp), &used, ".httpd-put-");
    (VOID)hs_append_num(c->put_temp, sizeof(c->put_temp), &used,
                        (ULONG)(c - httpd_conn));

    c->put = Open((CONST_STRPTR)c->put_temp, MODE_NEWFILE);
    if (c->put == (BPTR)0)
    {
        ULONG why = httpd_dos_status(IoErr());

        c->put_temp[0] = '\0';
        httpd_error(c, why, "that file cannot be written here");
        return FALSE;
    }

    return TRUE;
}

static VOID httpd_sink_put(HttpConn *c, const UBYTE *data, LONG len)
{
    if (c->put == (BPTR)0 || c->put_err != 0)
        return;

    if (Write(c->put, (APTR)data, len) != len)
    {
        c->put_err = IoErr();

        /* A short write with nothing in IoErr() is still a full disk as far
           as the client needs to know. */
        if (c->put_err == 0)
            c->put_err = ERROR_DISK_FULL;
    }
}

static VOID httpd_do_put(HttpConn *c)
{
    BOOL existed;

    if (c->put == (BPTR)0)
        return;                     /* begin() answered already            */

    (VOID)Close(c->put);
    c->put = (BPTR)0;

    if (c->put_err != 0)
    {
        ULONG why = httpd_dos_status(c->put_err);

        httpd_put_abandon(c);
        httpd_error(c, why, "that file could not be written");
        return;
    }

    /* A client is free to PUT a file called ".httpd-put-0", and then the
       temporary IS the target and the rename below would delete it. */
    if (hs_equal(c->put_temp, c->path.path))
    {
        c->put_temp[0] = '\0';
        httpd_empty(c, 201);
        return;
    }

    existed = (httpd_kind(c->path.path) >= 0) ? TRUE : FALSE;

    /*
     * The rename is the transfer, as far as everything else on this machine
     * is concerned: until it happens the old file is untouched, and after it
     * the new one is whole.  AmigaDOS will not rename onto a name that
     * exists, so an overwrite is a delete and a rename -- and the delete
     * happens after the last byte has landed, so an upload that failed has
     * cost nothing.
     */
    if (existed && !DeleteFile((CONST_STRPTR)c->path.path))
    {
        ULONG why = httpd_dos_status(IoErr());

        httpd_put_abandon(c);
        httpd_error(c, why, "the file already there will not go");
        return;
    }

    if (!Rename((CONST_STRPTR)c->put_temp, (CONST_STRPTR)c->path.path))
    {
        ULONG why = httpd_dos_status(IoErr());

        httpd_put_abandon(c);
        httpd_error(c, why, "that file could not be put in place");
        return;
    }

    c->put_temp[0] = '\0';

    httpd_empty(c, existed ? 204 : 201);
}

/* One <D:response> saying that one path failed, for the walks that stop part
   way through.  Built whole, because it is short and the alternative is a
   producer for four elements. */
static BOOL httpd_one_status(const char *href, ULONG status)
{
    ULONG used = 0;
    BOOL  ok;

    ok = hs_append(httpd_page, sizeof(httpd_page), &used,
                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<D:multistatus xmlns:D=\"DAV:\">\n"
                   "<D:response><D:href>");
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used, href);
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used,
                         "</D:href><D:status>HTTP/1.1 ");
    ok = ok && hs_append_num(httpd_page, sizeof(httpd_page), &used, status);
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used, " ");
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used,
                         httpd_reason(status));
    ok = ok && hs_append(httpd_page, sizeof(httpd_page), &used,
                         "</D:status></D:response>\n</D:multistatus>\n");

    return ok;
}

static VOID httpd_do_delete(HttpConn *c)
{
    ULONG why;

    if (!httpd_may_write(c))
        return;

    if (httpd_kind(c->path.path) < 0)
    {
        httpd_error(c, 404, "there is no such file to remove");
        return;
    }

    hs_copy(httpd_walk_src, sizeof(httpd_walk_src), c->path.path);

    why = httpd_delete_tree(httpd_walk_src, sizeof(httpd_walk_src), c->fib);

    if (why == 0UL)
    {
        httpd_empty(c, 204);
        return;
    }

    /*
     * Something under it would not go, so some of it did.  RFC 4918 9.6.1
     * wants that said as a multistatus naming what is left rather than as a
     * bare status: the client's picture of the tree is now wrong in one
     * place and it has no other way to find out which.
     */
    if (!hs_equal(httpd_walk_src, c->path.path) &&
        httpd_one_status(httpd_url_of(httpd_walk_src), why))
    {
        httpd_begin(c, 207);
        httpd_body_text(c, "text/xml; charset=utf-8", httpd_page);
        return;
    }

    httpd_error(c, why, "that could not be removed");
}

static VOID httpd_do_mkcol(HttpConn *c)
{
    BPTR made;

    if (!httpd_may_write(c))
        return;

    /* RFC 4918 9.3: a body the server does not understand is a 415, and this
       server understands none. */
    if (c->had_body)
    {
        httpd_error(c, 415, "this server takes no body on MKCOL");
        return;
    }

    if (httpd_kind(c->path.path) >= 0)
    {
        httpd_error(c, 405, "something of that name is there already");
        return;
    }

    if (!httpd_parent(c->path.path, httpd_walk_src,
                      sizeof(httpd_walk_src)) ||
        httpd_kind(httpd_walk_src) <= 0)
    {
        httpd_error(c, 409, "there is no drawer to make that in");
        return;
    }

    made = CreateDir((CONST_STRPTR)c->path.path);
    if (made == (BPTR)0)
    {
        httpd_error(c, httpd_dos_status(IoErr()),
                    "that drawer could not be made");
        return;
    }
    UnLock(made);

    httpd_empty(c, 201);
}

/*
 * COPY and MOVE carry the other end of the operation in a header, so the
 * Destination goes through http_path_resolve() exactly as the request target
 * did -- same decode, same colon check, same root.  A destination trusted any
 * less than the target is a way out of the document root that only writes.
 */
static BOOL httpd_resolve_dest(HttpConn *c)
{
    HttpPathResult why;

    if (c->dest[0] == '\0')
    {
        httpd_error(c, 400, "that method needs a Destination");
        return FALSE;
    }

    why = http_path_resolve(httpd_root, c->dest, &httpd_dest);
    if (why != HTTP_PATH_OK)
    {
        if (httpd_verbose || httpd_trace)
            httpd_log(c, "refused destination \"%s\": %s", (LONG)c->dest,
                      (LONG)http_path_error(why));

        httpd_error(c, 403,
                    "that destination is not one this server will open");
        return FALSE;
    }

    if (httpd_dest.segments == 0)
    {
        httpd_error(c, 403, "the served drawer itself is not a destination");
        return FALSE;
    }

    if (hs_equal(httpd_dest.path, c->path.path))
    {
        httpd_error(c, 403, "the source and the destination are the same");
        return FALSE;
    }

    /* Into itself is the one that would not stop: "a" to "a/b" is a walk that
       keeps finding what it has just copied. */
    if (http_path_within(c->path.path, httpd_dest.path))
    {
        httpd_error(c, 409, "that destination is inside the source");
        return FALSE;
    }

    return httpd_lock_allows(c, httpd_dest.path);
}

static VOID httpd_copy_or_move(HttpConn *c, BOOL moving)
{
    LONG  src_kind;
    LONG  dst_kind;
    ULONG why;

    if (!httpd_may_write(c))
        return;

    src_kind = httpd_kind(c->path.path);
    if (src_kind < 0)
    {
        httpd_error(c, 404, "there is nothing there to copy");
        return;
    }

    if (!httpd_resolve_dest(c))
        return;

    if (!httpd_parent(httpd_dest.path, httpd_walk_dst,
                      sizeof(httpd_walk_dst)) ||
        httpd_kind(httpd_walk_dst) <= 0)
    {
        httpd_error(c, 409, "there is no drawer to put that in");
        return;
    }

    dst_kind = httpd_kind(httpd_dest.path);

    if (dst_kind >= 0)
    {
        if (!c->overwrite)
        {
            httpd_error(c, 412,
                        "something is there already and Overwrite said no");
            return;
        }

        hs_copy(httpd_walk_dst, sizeof(httpd_walk_dst), httpd_dest.path);

        why = httpd_delete_tree(httpd_walk_dst, sizeof(httpd_walk_dst),
                                c->fib);
        if (why != 0UL)
        {
            httpd_error(c, why, "what was there already would not go");
            return;
        }
    }

    if (moving)
    {
        /*
         * Within a volume this is a rename and nothing is copied, which is
         * what makes moving a 50 MB drawer instant.  Between volumes it
         * cannot be, and AmigaDOS says which it was rather than leaving it to
         * be guessed from the device names -- an assign makes that guess
         * wrong, and guessing wrong here means copying a whole tree for a
         * rename or renaming across a boundary and losing it.
         */
        LONG err;

        if (Rename((CONST_STRPTR)c->path.path, (CONST_STRPTR)httpd_dest.path))
        {
            httpd_empty(c, (dst_kind >= 0) ? 204 : 201);
            return;
        }

        err = IoErr();

        if (err != ERROR_RENAME_ACROSS_DEVICES)
        {
            httpd_error(c, httpd_dos_status(err), "that could not be moved");
            return;
        }
    }

    hs_copy(httpd_walk_src, sizeof(httpd_walk_src), c->path.path);
    hs_copy(httpd_walk_dst, sizeof(httpd_walk_dst), httpd_dest.path);

    if (src_kind > 0)
    {
        /* Depth: 0 on a collection copies the collection and not what is in
           it, RFC 4918 9.8.3.  A MOVE is always infinity. */
        if (!moving && c->depth == 0)
        {
            BPTR made = CreateDir((CONST_STRPTR)httpd_dest.path);

            if (made == (BPTR)0)
            {
                httpd_error(c, httpd_dos_status(IoErr()),
                            "that drawer could not be made");
                return;
            }
            UnLock(made);
            why = 0;
        }
        else
        {
            why = httpd_copy_tree(httpd_walk_src, sizeof(httpd_walk_src),
                                  httpd_walk_dst, sizeof(httpd_walk_dst),
                                  c->fib);
        }
    }
    else
    {
        why = httpd_copy_file(httpd_walk_src, httpd_walk_dst);
    }

    if (why != 0UL)
    {
        if (httpd_one_status(httpd_url_of(httpd_walk_src), why))
        {
            httpd_begin(c, 207);
            httpd_body_text(c, "text/xml; charset=utf-8", httpd_page);
            return;
        }

        httpd_error(c, why, "that could not be copied");
        return;
    }

    if (moving)
    {
        hs_copy(httpd_walk_src, sizeof(httpd_walk_src), c->path.path);

        why = httpd_delete_tree(httpd_walk_src, sizeof(httpd_walk_src),
                                c->fib);
        if (why != 0UL)
        {
            httpd_error(c, why,
                        "the copy is in place and the original would not go");
            return;
        }
    }

    httpd_empty(c, (dst_kind >= 0) ? 204 : 201);
}

static VOID httpd_do_copy(HttpConn *c)
{
    httpd_copy_or_move(c, FALSE);
}

static VOID httpd_do_move(HttpConn *c)
{
    httpd_copy_or_move(c, TRUE);
}

/*
 * A file on this machine has one date and no other property anybody can set,
 * so the modification time is honoured and everything else is answered 403 in
 * a propstat of its own -- which is what RFC 4918 9.2.1 asks for and what
 * stops Explorer treating a timestamp it could not set as a failed copy.
 *
 * The namespace declarations come back out as they arrived, so the prefixes
 * in the answer mean what they meant in the question.
 */
static VOID httpd_do_proppatch(HttpConn *c)
{
    ULONG used = 0;
    ULONG pass;
    UBYTE i;
    BOOL  ok;

    if (!httpd_may_write(c))
        return;

    if (httpd_kind(c->path.path) < 0)
    {
        httpd_error(c, 404, "there is no such file");
        return;
    }

    if (c->have_date &&
        !SetFileDate((CONST_STRPTR)c->path.path, &c->prop_date))
    {
        for (i = 0; i < c->props; i++)
            c->prop_ok[i] = 0;
    }

    ok = hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<D:multistatus xmlns:D=\"DAV:\"");

    for (i = 0; i < c->nsdecls; i++)
    {
        /* xmlns:D is ours already; declaring it twice is not well formed. */
        if (hs_nicmp(c->nsdecl[i], "xmlns:D=", 8) == 0)
            continue;

        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, " ");
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                             c->nsdecl[i]);
    }

    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         ">\n<D:response><D:href>");
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         httpd_href(&c->path, NULL, FALSE));
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:href>");

    /* One propstat for what was set and one for what was not, rather than one
       each: the client reads the status off the group. */
    for (pass = 0; pass < 2UL; pass++)
    {
        UBYTE wanted = (pass == 0UL) ? 1 : 0;
        BOOL  any = FALSE;

        for (i = 0; i < c->props; i++)
        {
            if (c->prop_ok[i] != wanted)
                continue;

            if (!any)
            {
                ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch),
                                     &used, "<D:propstat><D:prop>");
                any = TRUE;
            }

            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 "<");
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 c->prop_name[i]);
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 "/>");
        }

        if (any)
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 (wanted != 0)
                                     ? "</D:prop><D:status>HTTP/1.1 200 OK"
                                       "</D:status></D:propstat>"
                                     : "</D:prop><D:status>HTTP/1.1 403 "
                                       "Forbidden</D:status></D:propstat>");
    }

    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:response>\n</D:multistatus>\n");

    if (!ok)
    {
        httpd_error(c, 500, "that answer would not fit");
        return;
    }

    httpd_begin(c, 207);
    httpd_body_text(c, "text/xml; charset=utf-8", httpd_scratch);
}

static VOID httpd_do_lock(HttpConn *c)
{
    HttpLock *exact = NULL;
    HttpLock *held;
    HttpLock *l;
    ULONG     secs;
    ULONG     used = 0;
    ULONG     i;
    BOOL      created;
    BOOL      ok;

    if (c->path.segments == 0)
    {
        httpd_error(c, 403, "the served drawer itself is not lockable");
        return;
    }

    secs = (c->lock_secs > 0UL) ? c->lock_secs : HTTPD_LOCK_DEF;
    if (secs > HTTPD_LOCK_CAP)
        secs = HTTPD_LOCK_CAP;

    /* A LOCK with no body is a refresh of the token in If:, RFC 4918 9.10.2 --
       and it is what a client sends every few minutes to keep a long copy
       alive, so answering it wrong ends the copy. */
    if (!c->had_body)
    {
        l = httpd_lock_by_token(c->iftoken[0]);
        if (l == NULL)
            l = httpd_lock_by_token(c->iftoken[1]);

        if (l == NULL)
        {
            httpd_error(c, 412, "that is not a lock this server is holding");
            return;
        }

        l->timeout = secs;
        l->expires = httpd_now() + secs;
    }
    else
    {
        held = httpd_lock_on(c->path.path);

        if (held != NULL && !httpd_holds(c, held))
        {
            httpd_error(c, 423, "somebody else is holding that");
            return;
        }

        /* Only a lock on this exact path is refreshed in place.  One
           inherited from a drawer above belongs to that drawer, and writing
           this path into it would move it. */
        for (i = 0; i < (ULONG)HTTPD_LOCK_MAX; i++)
        {
            if (httpd_locks[i].used &&
                hs_equal(httpd_locks[i].path, c->path.path))
            {
                exact = &httpd_locks[i];
                break;
            }
        }

        l = exact;

        if (l == NULL)
        {
            for (i = 0; i < (ULONG)HTTPD_LOCK_MAX; i++)
            {
                if (!httpd_locks[i].used)
                {
                    l = &httpd_locks[i];
                    break;
                }
            }

            if (l == NULL)
            {
                httpd_error(c, 503,
                            "this server is holding as many locks as it can");
                return;
            }

            httpd_make_token(l->token, sizeof(l->token));
        }

        l->used    = 1;
        l->depth   = (c->depth != 0) ? 1 : 0;
        l->timeout = secs;
        l->expires = httpd_now() + secs;
        hs_copy(l->path, sizeof(l->path), c->path.path);
        hs_copy(l->owner, sizeof(l->owner), c->owner);
    }

    /*
     * A LOCK on a name that is not there yet is how Finder starts an upload,
     * and RFC 4918 7.3 answers it 201 with a lock and no resource: the PUT
     * that follows carries the token and creates the file.  Nothing is
     * created here, so a lock the client abandons leaves no empty file.
     */
    created = (httpd_kind(c->path.path) < 0) ? TRUE : FALSE;

    ok = hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery>");
    ok = ok && httpd_activelock(l, httpd_scratch, sizeof(httpd_scratch),
                                &used);
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:lockdiscovery></D:prop>\n");

    if (!ok)
    {
        httpd_error(c, 500, "that answer would not fit");
        return;
    }

    httpd_begin(c, created ? 201 : 200);
    httpd_out(c, "Lock-Token: <");
    httpd_out(c, l->token);
    httpd_out(c, ">\r\n");
    httpd_body_text(c, "text/xml; charset=utf-8", httpd_scratch);
}

static VOID httpd_do_unlock(HttpConn *c)
{
    HttpLock *l = httpd_lock_by_token(c->unlock_token);

    if (l == NULL)
    {
        httpd_error(c, 409, "that is not a lock this server is holding");
        return;
    }

    /* RFC 4918 9.11.1: the token has to name a lock that covers the address
       the request was made against, or one client can unlock another's. */
    if (!hs_equal(l->path, c->path.path) &&
        !(l->depth != 0 && http_path_within(l->path, c->path.path)))
    {
        httpd_error(c, 409, "that lock is not on that address");
        return;
    }

    l->used = 0;

    httpd_empty(c, 204);
}

/*
 * The table.  A ladder of strcmp would have been shorter and would have had
 * to be unpicked the moment PUT landed; this is the shape, and the Allow
 * header is generated from it so the two cannot disagree.
 */
static const HttpMethod httpd_methods[] =
{
    { "GET",      HTTPD_M_GET,      0,            httpd_do_get,      NULL,
      NULL },
    { "HEAD",     HTTPD_M_HEAD,     0,            httpd_do_get,      NULL,
      NULL },
    { "OPTIONS",  HTTPD_M_OPTIONS,  0,            httpd_do_options,  NULL,
      NULL },
    /* PROPFIND's body is read and thrown away: an empty body and <allprop/>
       mean the same thing, and a body asking for named properties gets the
       ones there are, which RFC 4918 permits and every client tolerates. */
    { "PROPFIND", HTTPD_M_PROPFIND, HTTPD_F_BODY, httpd_do_propfind, NULL,
      NULL },
    { "PUT",      HTTPD_M_PUT,      HTTPD_F_BODY | HTTPD_F_WRITE |
                                    HTTPD_F_UPLOAD,
                                                  httpd_do_put,
      httpd_sink_put, httpd_begin_put },
    { "DELETE",   HTTPD_M_DELETE,   HTTPD_F_WRITE, httpd_do_delete,  NULL,
      NULL },
    { "MKCOL",    HTTPD_M_MKCOL,    HTTPD_F_BODY | HTTPD_F_WRITE,
                                                   httpd_do_mkcol,   NULL,
      NULL },
    { "COPY",     HTTPD_M_COPY,     HTTPD_F_WRITE, httpd_do_copy,    NULL,
      NULL },
    { "MOVE",     HTTPD_M_MOVE,     HTTPD_F_WRITE, httpd_do_move,    NULL,
      NULL },
    { "PROPPATCH", HTTPD_M_PROPPATCH, HTTPD_F_BODY | HTTPD_F_WRITE,
                                                   httpd_do_proppatch,
      httpd_sink_xml, NULL },
    { "LOCK",     HTTPD_M_LOCK,     HTTPD_F_BODY | HTTPD_F_WRITE,
                                                   httpd_do_lock,
      httpd_sink_xml, NULL },
    { "UNLOCK",   HTTPD_M_UNLOCK,   HTTPD_F_WRITE, httpd_do_unlock,  NULL,
      NULL },
    { NULL,       HTTPD_M_UNKNOWN,  0,             NULL,             NULL,
      NULL }
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
 * The tokens inside an If:.  The header is a small language -- tagged lists,
 * entity tags, Not -- and none of it applies here except the state tokens:
 * this server has no ETags to compare and nothing to condition a write on but
 * a lock.  Two tokens is as many as a request needs, which is a MOVE with
 * both ends locked.
 */
static VOID httpd_parse_if(HttpConn *c, const char *value)
{
    ULONG n = 0;

    while (*value != '\0' && n < 2UL)
    {
        if (*value == '<')
        {
            const char *start = ++value;
            ULONG       len   = 0;

            while (*value != '\0' && *value != '>')
            {
                value++;
                len++;
            }

            /* A tagged list names a URL the same way, so only the ones that
               look like a token are taken. */
            if (hs_nicmp(start, "opaquelocktoken:", 16) == 0 &&
                len + 1UL < (ULONG)HTTPD_TOKEN_MAX)
            {
                ULONG k;

                for (k = 0; k < len; k++)
                    c->iftoken[n][k] = start[k];
                c->iftoken[n][len] = '\0';
                n++;
            }

            if (*value == '>')
                value++;
        }
        else
        {
            value++;
        }
    }
}

/* "Second-3600", or "Infinite".  What is granted is capped here whatever was
   asked for, and the answer says what it was. */
static ULONG httpd_parse_timeout(const char *value)
{
    while (*value == ' ')
        value++;

    if (hs_nicmp(value, "infinite", 8) == 0)
        return HTTPD_LOCK_CAP;

    if (hs_nicmp(value, "second-", 7) == 0)
    {
        ULONG secs = 0;

        value += 7;
        while (*value >= '0' && *value <= '9')
            secs = (secs * 10UL) + (ULONG)(*value++ - '0');

        return secs;
    }

    return 0;
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
            /* Finder does not know how long a file it is uploading is until
               it has sent it, so it chunks -- which makes this the difference
               between PUT working from macOS and not working at all. */
            if (hs_nicmp(httpd_value, "chunked", 7) == 0)
                c->chunk_state = CHUNK_SIZE;
        }
        else if (hs_equal(name, "Expect"))
        {
            /* curl sends this on every PUT over a certain size and waits a
               second for the answer; the Windows redirector waits. */
            if (hs_nicmp(httpd_value, "100-continue", 12) == 0)
                c->expect = 1;
        }
        else if (hs_equal(name, "Destination"))
        {
            hs_copy(c->dest, sizeof(c->dest), httpd_value);
        }
        else if (hs_equal(name, "Overwrite"))
        {
            c->overwrite = (httpd_value[0] == 'F' || httpd_value[0] == 'f')
                               ? 0 : 1;
        }
        else if (hs_equal(name, "If"))
        {
            httpd_parse_if(c, httpd_value);
        }
        else if (hs_equal(name, "Lock-Token"))
        {
            const char *p = httpd_value;

            while (*p != '\0' && *p != '<')
                p++;
            if (*p == '<')
                p++;

            hs_copy(c->unlock_token, sizeof(c->unlock_token), p);

            {
                ULONG n2 = hs_len(c->unlock_token);

                while (n2 > 0UL && c->unlock_token[n2 - 1] != '>')
                    n2--;
                if (n2 > 0UL)
                    c->unlock_token[n2 - 1] = '\0';
            }
        }
        else if (hs_equal(name, "Timeout"))
        {
            c->lock_secs = httpd_parse_timeout(httpd_value);
        }
    }

    /* ---- what to do with it ------------------------------------------ */

    c->method = httpd_lookup(method);

    if (c->method == NULL)
    {
        /* 405 and not 501: the address is fine, the verb is not, and the
           Allow header names what there is instead. */
        httpd_error(c, 405, "that is not a method this server has");
        return FALSE;
    }

    c->head_only = (c->method->id == HTTPD_M_HEAD) ? 1 : 0;
    c->had_body  = (c->body_left > 0UL || c->chunk_state != CHUNK_OFF) ? 1 : 0;

    /*
     * The ceiling is on a body this server HOLDS.  A PUT's goes to a file as
     * it arrives and is the client's business, so an upload is not measured
     * against a buffer it never occupies.
     */
    if (c->body_left > HTTPD_BODY_MAX &&
        (c->method->flags & HTTPD_F_UPLOAD) == 0)
    {
        httpd_error(c, 413, "that request body is larger than this server "
                            "will read");
        return FALSE;
    }

    if (c->had_body && (c->method->flags & HTTPD_F_BODY) == 0)
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
 * A chunked request body, decoded as it arrives.  Everything the decoder is in
 * the middle of lives in the connection, because a chunk boundary falls
 * wherever the network put it and not where the framing wanted it.
 *
 * There is no ceiling on what a chunked body may total.  The sinks are what
 * bound it: PUT's writes to a file, and the ones that read XML have fixed
 * buffers, so an endless body costs time -- which the connection timeout
 * already bounds -- and not memory.
 *
 * Returns how much of `data` belonged to the body; anything after that is the
 * next request.
 */
static LONG httpd_feed_chunked(HttpConn *c, const UBYTE *data, LONG len)
{
    LONG i = 0;

    while (i < len && c->chunk_state != CHUNK_DONE)
    {
        switch (c->chunk_state)
        {
            case CHUNK_SIZE:
            {
                int ch = data[i++];

                if (ch != '\n')
                {
                    if (ch != '\r' &&
                        c->chunk_n + 1U < sizeof(c->chunk_line))
                        c->chunk_line[c->chunk_n++] = (char)ch;
                    break;
                }

                {
                    const char *p = c->chunk_line;

                    c->chunk_line[c->chunk_n] = '\0';
                    c->chunk_n    = 0;
                    c->chunk_left = 0;

                    /* Hex, and it stops at the ';' of a chunk extension. */
                    for (;;)
                    {
                        int d = *p;

                        if (d >= '0' && d <= '9')      d -= '0';
                        else if (d >= 'a' && d <= 'f') d -= 'a' - 10;
                        else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
                        else                           break;

                        c->chunk_left = (c->chunk_left << 4) | (ULONG)d;
                        p++;
                    }
                }

                c->chunk_state = (c->chunk_left > 0UL)
                                     ? CHUNK_DATA : CHUNK_TRAILER;
                break;
            }

            case CHUNK_DATA:
            {
                ULONG take = (ULONG)(len - i);

                if (take > c->chunk_left)
                    take = c->chunk_left;

                if (c->method != NULL && c->method->sink != NULL)
                    c->method->sink(c, &data[i], (LONG)take);

                i += (LONG)take;
                c->chunk_left -= take;

                if (c->chunk_left == 0UL)
                    c->chunk_state = CHUNK_CRLF;
                break;
            }

            case CHUNK_CRLF:
                if (data[i++] == '\n')
                    c->chunk_state = CHUNK_SIZE;
                break;

            default:                    /* CHUNK_TRAILER                   */
            {
                int ch = data[i++];

                /* Trailer lines, then a blank one.  Nothing here reads a
                   trailer; they are counted so the blank line is found. */
                if (ch == '\n')
                {
                    if (c->chunk_n == 0U)
                        c->chunk_state = CHUNK_DONE;
                    c->chunk_n = 0;
                }
                else if (ch != '\r')
                {
                    c->chunk_n = 1;
                }
                break;
            }
        }
    }

    return i;
}

/*
 * Feed the body to the method's sink.  A method with no sink throws the bytes
 * away, but they are still READ -- a request body left in the socket is the
 * next request as far as the parser is concerned, which is how a server that
 * ignores bodies answers the wrong question on a kept-alive connection.
 *
 * Returns how much was taken, which is not `len` when the body ended inside
 * it and the rest is another request.
 */
static LONG httpd_consume_body(HttpConn *c, const UBYTE *data, LONG len)
{
    LONG take;

    if (len <= 0)
        return 0;

    if (c->chunk_state != CHUNK_OFF)
        return httpd_feed_chunked(c, data, len);

    take = ((ULONG)len > c->body_left) ? (LONG)c->body_left : len;

    if (take <= 0)
        return 0;

    if (c->method != NULL && c->method->sink != NULL)
    {
        c->method->sink(c, data, take);
    }
    else if (httpd_trace)
    {
        char  text[200];
        ULONG n = 0;

        while (n + 1UL < sizeof(text) && (LONG)n < take)
        {
            UBYTE ch = data[n];

            text[n] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
            n++;
        }
        text[n] = '\0';

        tool_printf("[%ld] body %s\n", (LONG)(c - httpd_conn), (LONG)text);
        (VOID)Flush(Output());
    }

    c->body_left -= (ULONG)take;

    return take;
}

static BOOL httpd_body_done(const HttpConn *c)
{
    if (c->chunk_state != CHUNK_OFF)
        return (c->chunk_state == CHUNK_DONE) ? TRUE : FALSE;

    return (c->body_left == 0UL) ? TRUE : FALSE;
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

    /*
     * Nothing reaches the sink until the method has said it can take it.  A
     * PUT that cannot be started -- no drawer, no room, somebody else's lock
     * -- is refused here, which is before the client has sent the file rather
     * than after.
     */
    if (c->method->begin != NULL && !c->method->begin(c))
    {
        /* What the client is about to send belongs to a request that will not
           be read, so this answer ends the connection. */
        c->keepalive = 0;
        c->in_len    = 0;
        c->state     = CONN_SEND;
        httpd_log_status(c);
        return;
    }

    if (left > 0UL && !httpd_body_done(c))
    {
        LONG took = httpd_consume_body(c, c->in, (LONG)left);

        if (took > 0)
        {
            for (i = 0; i + (ULONG)took < c->in_len; i++)
                c->in[i] = c->in[i + (ULONG)took];

            c->in_len -= (ULONG)took;
        }
    }

    if (!httpd_body_done(c))
    {
        if (c->expect)
        {
            /* The client is waiting for this before it sends anything: curl
               waits a second and then sends regardless, the Windows
               redirector waits.  It goes out through the ordinary write path
               rather than being pushed at the socket here, because a blocking
               write is the one thing this loop may not do. */
            static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";

            hs_copy((char *)c->out, sizeof(c->out), go);
            c->out_len  = hs_len(go);
            c->out_sent = 0;
            c->expect   = 0;
            c->state    = CONN_CONTINUE;
        }
        else
        {
            c->state = CONN_BODY;
        }

        return;
    }

    c->state = CONN_SEND;
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
        LONG  took;

        /* A chunked body has no count to stop at, so the framing is what says
           where it ends and the read is whatever the socket has. */
        if (c->chunk_state == CHUNK_OFF && (ULONG)want > c->body_left)
            want = (LONG)c->body_left;

        got = tool_sock_recv(httpd_sb, c->sock, scratch, want);

        if (got == 0)
            return FALSE;

        if (got < 0)
        {
            LONG err = tool_sock_errno(httpd_sb);

            return (err == TOOL_EWOULDBLOCK || err == TOOL_EINTR) ? TRUE : FALSE;
        }

        took = httpd_consume_body(c, scratch, got);
        c->progress = httpd_now();

        /* Anything past the end of the body is the next request, and it has
           already been taken out of the socket. */
        while (took < got && c->in_len < sizeof(c->in))
            c->in[c->in_len++] = scratch[took++];

        if (httpd_body_done(c))
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
             * What send() says it took, totalled per answer.  One line under
             * TRACE, and the only way to tell this program's mistakes from
             * the library's: put it next to the distinct sequence space in a
             * capture and the two must agree.  They did not, and the fault
             * was in send() rather than in the loop below --
             * bsd_send_consumed() in src/bsdsocket/transfer.c is what that
             * cost and why the counter stays.
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

        /* The interim 100 has gone out, so the client will send the body
           now and this connection goes back to reading. */
        if (c->state == CONN_CONTINUE)
        {
            c->state    = CONN_BODY;
            c->out_len  = 0;
            c->out_sent = 0;
            return TRUE;
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
    httpd_conn[i].put       = (BPTR)0;
    httpd_conn[i].put_temp[0] = '\0';
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

            if (keep && (c->state == CONN_SEND || c->state == CONN_CONTINUE) &&
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
                   "Serves a drawer over HTTP and WebDAV, so this machine can "
                   "be mounted as a writable drive.");
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
        httpd_conn[i].sock        = -1;
        httpd_conn[i].state       = CONN_FREE;
        httpd_conn[i].fib         = NULL;
        httpd_conn[i].put         = (BPTR)0;
        httpd_conn[i].put_temp[0] = '\0';
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

    /*
     * The two the write methods share.  A walk asks about one path while a
     * handler is holding what it learned about another -- a COPY examines the
     * source and then asks what is at the destination -- so there is one of
     * each here rather than one borrowed from the connection.
     */
    httpd_fib2 = (struct FileInfoBlock *)
        ami_alloc((ULONG)sizeof(struct FileInfoBlock));
    httpd_info = (struct InfoData *)ami_alloc((ULONG)sizeof(struct InfoData));

    if (httpd_fib2 == NULL || httpd_info == NULL)
    {
        tool_error("not enough memory to start");
        ami_free(httpd_info);
        ami_free(httpd_fib2);
        for (i = 0; i < httpd_conns; i++)
            ami_free(httpd_conn[i].fib);
        ami_free(httpd_conn);
        CloseLibrary(httpd_sb);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /* The lock tokens have to differ between runs of the server, or a token a
       client kept from the last one unlocks a file in this one. */
    httpd_token_seed = httpd_now();

    lsock = httpd_listen(&address, port);
    if (lsock < 0)
    {
        ami_free(httpd_info);
        ami_free(httpd_fib2);
        for (i = 0; i < httpd_conns; i++)
            ami_free(httpd_conn[i].fib);
        ami_free(httpd_conn);
        CloseLibrary(httpd_sb);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    tool_addr_text(httpd_sb, &address, dotted, sizeof(dotted));
    tool_printf("Serving %s read-write on http://%s:%ld/  (Ctrl-C to stop)\n",
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
    ami_free(httpd_info);
    ami_free(httpd_fib2);

    (VOID)tool_sock_close(httpd_sb, lsock);
    CloseLibrary(httpd_sb);
    FreeArgs(rda);

    return RETURN_OK;
}
