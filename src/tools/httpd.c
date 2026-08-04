/*
 * httpd, an HTTP server with read-write WebDAV, so that a drawer on this
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
 *   MOVE, PROPPATCH, LOCK and UNLOCK, WebDAV class 2, which is the class
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
 *   is the part that would have to be rewritten, and none of them serves
 *   files, so the handler would be ours regardless.  What is left is the
 *   listen/accept idiom, which src/tools/nc.c already has.
 *
 * SEVERAL CONNECTIONS AT ONCE
 *
 *   `nc -l` takes one caller and exits, so before this the server half of the
 *   ABI had no user that ever held two connections open.  Every socket here is
 *   non-blocking and every wait is one WaitSelect() over the listener plus the
 *   live connections, so one client reading a file slowly cannot stop another
 *   from being answered, which matters because Finder and the Windows
 *   redirector both open several and expect all of them to progress.
 *
 * BOUNDED, BECAUSE THE FLOOR IS A 1 MB MACHINE
 *
 *   A connection that says nothing must not cost anything.  The request head,
 *   the count of headers, each line, the request body and the time a
 *   connection may spend making no progress are all capped, and the caps are
 *   the reason a client can open the connection limit's worth of sockets and
 *   stay silent without the machine noticing.  Everything is static: the
 *   buffers are HTTPD_CONN_MAX * ~7.5 KB, allocated once, and a Shell command
 *   gets 4 KB of stack on a stock Kickstart 3.1 (src/tools/nc.c says the same
 *   at its own buffers).  Writing added nothing to that: a tree walk carries
 *   two paths, one FileInfoBlock and two bytes a level rather than a stack of
 *   blocks, and the lock table is a fixed array.
 *
 * SPDX-License-Identifier: MIT
 */

#include "toolsock.h"
#include "httppath.h"
#include "httpif.h"
#include "httpframe.h"
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
 * a slot is 7.5 KB of buffers, so the default eight is 60 KB taken at startup
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

/* A walk that a client can ask for, DELETE and COPY of a drawer, has to
   stop somewhere, and a filesystem that keeps saying "not empty" is the shape
   a loop here would take.  A tree that needs more steps than this is refused
   half-done and said so in the multistatus. */
#define HTTPD_WALK_MAX     20000UL

/*
 * How much of a walk one pass of the event loop does.  This is the whole
 * reason the walks are a state machine: a DELETE of a thousand files is a
 * thousand-ish passes and every other connection is served between them, where
 * doing it in one go would stop the server for as long as the tree took.
 *
 * ONE entry, because a single AmigaDOS operation cannot be interrupted and is
 * already longer than a network round trip: DeleteFile() measured 150 ms on a
 * directory filesystem under emulation, so a slice of eight was 1.2 seconds
 * during which nothing else was answered.  What a slice costs on top is one
 * WaitSelect(), which is nothing beside the packet it is wrapped around.
 *
 * A file being copied is different, Read() and Write() of a scratch-full are
 * fast, so those go eight at a time.
 */
#define HTTPD_WALK_SLICE       1    /* directory entries acted on          */
#define HTTPD_COPY_SLICE       8    /* scratch-fulls of a file copied      */

/* How deep a walk may go below the request path.  24 levels is deeper than a
   path 256 bytes long can usefully reach. */
#define HTTPD_WALK_DEPTH      24

/*
 * What a partial DELETE may name.  RFC 4918 9.6.1 wants every failure in the
 * multistatus, and a drawer of a thousand delete-protected files would then be
 * a thousand-element answer on a machine with a megabyte.
 *
 * So the answer names the first eight, and at most 768 bytes of them,
 * whichever runs out first, which for a long name is the bytes.  Everything
 * past the bound is deleted or not deleted exactly the same way; it is only
 * unnamed, and a client that wants the rest asks again.
 */
#define HTTPD_FAILS_MAX        8
#define HTTPD_FAIL_MAX       768

/* Locks.  Fixed, like everything else: eight is more than the clients hold at
   once, Finder locks the file it is writing and nothing else, and a ninth
   asker is told the server is busy rather than costing memory. */
#define HTTPD_LOCK_MAX         8
#define HTTPD_LOCK_DEF      180UL   /* seconds granted when none was asked  */
#define HTTPD_LOCK_CAP     3600UL   /* the longest this server will hold one */
#define HTTPD_HOST_MAX        80    /* Host:, which decides a Destination   */
#define HTTPD_TOKEN_MAX       64    /* "opaquelocktoken:...", as text       */

/* An entity tag is four decimal numbers and three separators inside quotes. */
#define HTTPD_ETAG_MAX        48
/* The If: header, kept whole so it can be evaluated rather than skimmed. */
#define HTTPD_IF_MAX         256

/* The XML the write methods send is skimmed, not parsed: element names and
   the text between them, both bounded, and no tree.  PROPPATCH names the
   properties it wants and LOCK names an owner; nothing here needs more. */
#define HTTPD_PROPS_MAX        8    /* properties reported on in one 207    */

/*
 * What a PROPFIND body asked for, RFC 4918 9.1.  The body used to be read and
 * thrown away, so every request was answered as <allprop/>: a client asking
 * for two properties got all of them, one asking for <propname/> got values it
 * had said it did not want, and a property we do not have drew no 404 at all.
 */
#define HTTPD_PF_ALLPROP       0
#define HTTPD_PF_PROPNAME      1
#define HTTPD_PF_NAMED         2
#define HTTPD_QNAME_MAX       32    /* "Z:Win32LastModifiedTime" is 23      */
#define HTTPD_NS_MAX           3    /* xmlns: bindings carried to the reply */
#define HTTPD_NSURI_MAX       48
#define HTTPD_TEXT_MAX        48    /* an element's character data          */

/*
 * The lock owner is its own size because it is the one piece of client text
 * this server stores and hands back, and 47 characters did not hold the
 * "<D:owner><D:href>mailto:...</D:href></D:owner>" that clients actually
 * send, the address alone is longer than that.
 *
 * The cost is (128 - 48) bytes on each of HTTPD_LOCK_MAX locks and
 * HTTPD_CONN_MAX connections: about 2.5 KB, taken once, on a machine that may
 * have a megabyte.
 */
#define HTTPD_OWNER_MAX      128

/* How long WaitSelect() may sleep with nothing happening.  It is what makes
   Ctrl-C and the connection timeout noticed, and nothing else depends on it. */
#define HTTPD_TICK_MICROS  250000

/* And how long it may sleep with a tree walk outstanding.  Short, because the
   walk wants the processor, but NOT zero, which WaitSelect() reads as a poll
   and answers without yielding at all. */
#define HTTPD_WALK_MICROS    2000

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
    CONN_WALK,          /* a slice of a tree walk per pass; no socket work */
    CONN_SEND,          /* pushing out[], refilled by the producer         */
    CONN_DRAIN          /* reading away a refused request's body           */
};

/*
 * Where a tree walk has got to.  DELETE and COPY of a drawer are the only
 * unbounded work this server does, so they are the only things that carry a
 * position between passes of the loop rather than running to completion.
 */
enum
{
    WALK_NONE = 0,
    WALK_ENTER,         /* lock and examine what the path names            */
    WALK_SCAN,          /* one entry of the drawer being walked            */
    WALK_RMDIR,         /* the drawer itself, once it should be empty      */
    WALK_MKDIR,         /* the copy's matching drawer                      */
    WALK_OPEN,          /* a file copy: both ends                          */
    WALK_FILE,          /* a file copy: one slice of it                    */
    WALK_SHUT,          /* a file copy: close, and carry the date over     */
    WALK_UP,            /* out of this drawer, on with the one above       */
    WALK_DONE
};

/* What the walk was for, and so what happens when it ends. */
enum
{
    WALK_FOR_DELETE = 0,    /* the DELETE method's own walk                */
    WALK_FOR_CLEAR,         /* the destination of a COPY or MOVE           */
    WALK_FOR_COPY,          /* the copy itself                             */
    WALK_FOR_MOVED          /* a moved source, once the copy is in place   */
};

enum
{
    PROD_NONE = 0,      /* what is in out[] is the whole answer            */
    PROD_FILE,          /* the rest of an open file                        */
    PROD_INDEX,         /* a generated HTML directory listing              */
    PROD_PROPFIND       /* a generated 207 multistatus                     */
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
    UBYTE   framed;                 /* the whole head was read, so the     */
                                    /* body's length is known              */
    UBYTE   drain;                  /* a refused body still to be read away */
    HttpChunk chunk;                /* HTTP_CHUNK_OFF unless it is chunked  */
    ULONG   body_start;             /* seconds, when the body began         */
    ULONG   body_got;               /* bytes of it that have arrived        */
    ULONG   lock_secs;              /* Timeout: seconds asked for, 0 if none */
    char    host[HTTPD_HOST_MAX];   /* Host:, to tell a local Destination  */
    char    ifmatch[HTTPD_ETAG_MAX];        /* If-Match:                   */
    char    ifnone[HTTPD_ETAG_MAX];         /* If-None-Match:              */
    char    dest_url[HTTP_URL_MAX]; /* Destination:, still as it arrived   */
    HttpPath dest;                  /* and what it resolved to             */
    char    iftoken[2][HTTPD_TOKEN_MAX];    /* the tokens inside If:       */
    char    ifhdr[HTTPD_IF_MAX];            /* and the whole of it         */
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
    UBYTE   props_cut;              /* more named than there was room for  */
    UBYTE   pf_mode;
    UBYTE   prop_ok[HTTPD_PROPS_MAX];
    char    prop_name[HTTPD_PROPS_MAX][HTTPD_QNAME_MAX];
    UBYTE   nsdecls;
    char    nsdecl[HTTPD_NS_MAX][HTTPD_QNAME_MAX + HTTPD_NSURI_MAX];
    UBYTE   have_date;
    struct DateStamp prop_date;
    char    owner[HTTPD_OWNER_MAX];

    /* a tree walk, carried between passes of the loop */
    UBYTE   walk;
    UBYTE   walk_for;
    UBYTE   walk_copy;              /* copying rather than deleting        */
    UBYTE   walk_one;               /* a single file, not a drawer         */
    UBYTE   walk_depth;
    UBYTE   walk_new;               /* the destination was not there before */
    UBYTE   walk_move;              /* the copy is half of a MOVE          */
    UBYTE   fails;                  /* <D:response> elements recorded      */
    ULONG   walk_steps;
    ULONG   walk_dirty;             /* bit d: something under depth d stayed */
    ULONG   walk_status;            /* what stopped a copy                 */
    ULONG   fails_len;
    BPTR    walk_lock;              /* the drawer being scanned right now  */
    UWORD   walk_seen;              /* subdrawers counted in this scan     */
    UWORD   walk_skip[HTTPD_WALK_DEPTH];    /* and walked, per level       */
    char    walk_src[HTTP_PATH_MAX];
    char    walk_dst[HTTP_PATH_MAX];
    char    fail_xml[HTTPD_FAIL_MAX];

    /* the answer */
    UBYTE   out[HTTPD_OUT_MAX];
    ULONG   out_len;
    ULONG   out_sent;
    ULONG   wrote;                  /* what send() has ACCEPTED, in total   */
    UBYTE   overflow;               /* the head did not fit, 500 instead */
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
 * a static one is the whole ceiling whether it is used or not: at 7.5 KB a
 * slot that is 120 KB of BSS in every copy of the command, which on a 1 MB
 * machine is an eighth of it reserved for connections nobody asked for.
 * Allocating it makes CONNECTIONS mean something, `-m 2` is 15 KB, and
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
 * produced per pass of the loop, the connections interleave between passes,
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
 * A path to ask a question about, and the one a Resource-Tag in an If: names.
 * Shared for the reason the scratches above are: both are filled and finished
 * with inside one dispatch, and the connections interleave between passes of
 * the loop and never inside one.
 *
 * A path that OUTLIVES its handler may not be here.  A walk spans passes, so
 * its two paths are in the connection, and so is a COPY or MOVE's resolved
 * destination, which the walk reads back after the handler has returned.  It
 * was here once, and two overlapping moves then wrote over each other's.
 */
static char     httpd_probe[HTTP_PATH_MAX];
static HttpPath httpd_ifpath;

/*
 * The lock table.  Static rather than allocated with the connections: it is
 * 3 KB whatever CONNECTIONS says, because a lock outlives the connection that
 * took it, that is the whole point of one.
 */
typedef struct HttpLock
{
    char  path[HTTP_PATH_MAX];      /* the AmigaOS path it covers          */
    char  token[HTTPD_TOKEN_MAX];
    char  owner[HTTPD_OWNER_MAX];
    ULONG expires;                  /* httpd_now() seconds                 */
    ULONG timeout;                  /* what was granted, for the reply     */
    UBYTE depth;                    /* 1 when it covers everything below   */
    UBYTE used;
} HttpLock;

static HttpLock httpd_locks[HTTPD_LOCK_MAX];
static ULONG    httpd_token_seed;
static ULONG    httpd_token_start;      /* the clock when the server started   */

static char        httpd_root_buf[HTTP_PATH_MAX];
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

/* Seconds of local time since the Amiga epoch, ds_Days is in the sum, so it
   does not wrap at midnight.  It can still step backwards if the clock is set,
   which the caller handles by treating any backwards step as "just now" rather
   than as an expiry.  Progress deadlines and lock expiry both read it. */
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

/* "Sun, 06 Nov 1994 08:49:37 GMT", RFC 1123, and not the machine's locale. */
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

/* "1994-11-06T08:49:37Z", ISO 8601, which is what creationdate takes. */
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
        case 424: return "Failed Dependency";
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
 * A body whose length nobody knows until it has been produced, a directory
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
   Content-Length at all, RFC 7230 8.1.2, and 201 after a PUT is the one
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

/* Declared here because the walk is defined below and the connection has to
   be able to end one, the same way the Allow header is declared above the
   table it is generated from. */
static VOID httpd_walk_release(HttpConn *c);

/*
 * A PUT that never finished.  The temporary is deleted rather than left, so an
 * abandoned upload costs nothing and does not appear in a listing, which is
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

    httpd_walk_release(c);
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

    httpd_walk_release(c);
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
     * refused here, so a client that omits it would get a 403 for asking
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
    c->framed      = 0;
    c->drain       = 0;
    http_chunk_off(&c->chunk);
    c->body_start  = 0;
    c->body_got    = 0;
    c->lock_secs   = 0;
    c->host[0]     = '\0';
    c->ifmatch[0]  = '\0';
    c->ifnone[0]   = '\0';
    c->dest_url[0] = '\0';
    c->dest.path[0] = '\0';
    c->iftoken[0][0] = '\0';
    c->iftoken[1][0] = '\0';
    c->ifhdr[0]    = '\0';
    c->unlock_token[0] = '\0';
    c->put_err     = 0;

    c->xml_state  = XML_TEXT;
    c->pf_mode    = (UBYTE)HTTPD_PF_ALLPROP;
    c->xml_name_n = 0;
    c->xml_close  = 0;
    c->xml_text_n = 0;
    c->xml_attr_n = 0;
    c->walk       = WALK_NONE;
    c->walk_move  = 0;
    c->walk_new   = 0;
    c->fails      = 0;
    c->fails_len  = 0;
    c->walk_status = 0;

    c->in_prop    = 0;
    c->in_owner   = 0;
    c->props      = 0;
    c->props_cut  = 0;
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
 * wants the same answer to the same failure, and it is the difference
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
        /* A name the filesystem will not carry, too long for OFS, or a
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

/*
 * TRUE when something is at `path` under a DIFFERENT name to the one asked
 * for, which means the filesystem did not refuse a name it cannot hold, it
 * TRUNCATED it, and the request is about to land on somebody else's file.
 *
 * Measured, not assumed: an OFS floppy took a 40-character name, cut it to 30
 * and answered success.  So a name that cannot be represented cannot be
 * caught by trusting the create; it has to be caught by asking what is really
 * there.  Two long names that differ only after the cut are the same file to
 * the filesystem, and without this a PUT of the second silently replaces the
 * first.
 */
static BOOL httpd_name_differs(const struct FileInfoBlock *fib,
                               const char *name)
{
    if (name[0] == '\0')
        return FALSE;               /* the root: no name was asked for     */

    return hs_equal((const char *)fib->fib_FileName, name) ? FALSE : TRUE;
}

static BOOL httpd_name_cut(const char *path, const char *name)
{
    BPTR lock;
    BOOL cut = FALSE;

    if (httpd_fib2 == NULL || name[0] == '\0')
        return FALSE;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return FALSE;               /* nothing there: nothing to collide   */

    if (Examine(lock, httpd_fib2))
        cut = httpd_name_differs(httpd_fib2, name);

    UnLock(lock);

    return cut;
}

/*
 * TRUE when creating something called `name` at `path` would leave it under a
 * different name, asked by doing it, because there is no call that says how
 * long a name a filesystem will carry and the answer differs per filesystem.
 *
 * httpd_name_cut() above catches a COLLISION: something already there under
 * the cut name.  It cannot catch the FIRST one, where nothing is there and the
 * create succeeds shortened, because it starts by locking a name that does not
 * exist yet and reads that as "nothing to collide with".
 *
 * PUT and MKCOL catch that afterwards and undo it, one file, one create.
 * COPY and MOVE cannot: by the time the destination exists the walk has copied
 * a tree into it, and undoing that is a second walk.  So they ask first.
 *
 * Only ever called with nothing at `path`; MODE_NEWFILE would truncate a file
 * that was.
 */
static BOOL httpd_name_survives(const char *path, const char *name)
{
    BPTR probe;
    BOOL cut;

    if (name[0] == '\0')
        return TRUE;

    probe = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (probe == (BPTR)0)
        return TRUE;                /* the operation itself will say why   */

    (VOID)Close(probe);

    cut = httpd_name_cut(path, name);

    (VOID)DeleteFile((CONST_STRPTR)path);

    return cut ? FALSE : TRUE;
}

/*
 * A directory entry that stands for something somewhere else.  ExNext() is the
 * only place one is visible: Lock() follows a link, so Examine() of the result
 * reports what it points AT and never that it was reached through one.
 *
 * ST_LINKFILE is deliberately not here.  A hard link to a file is one file,
 * the machine's owner put it in the served drawer on purpose, and copying it
 * copies bytes rather than walking anywhere.
 */
static BOOL httpd_entry_is_link(LONG type)
{
    return (type == ST_SOFTLINK || type == ST_LINKDIR) ? TRUE : FALSE;
}

/*
 * The entity tag for something the filesystem has already been asked about:
 * its size and the three fields of its DateStamp, which between them are
 * everything a FileInfoBlock knows that changes when the bytes do.  No I/O
 * and no state, the caller has the block in hand.
 *
 * Collections have none.  A listing's bytes change when a file inside it is
 * written, and the drawer's own date does not say so.
 */
static VOID httpd_etag(ULONG size, const struct DateStamp *ds,
                       char *out, ULONG outlen)
{
    ULONG used = 0;
    BOOL  ok;

    out[0] = '\0';

    ok = hs_append(out, outlen, &used, "\"");
    ok = ok && hs_append_num(out, outlen, &used, size);
    ok = ok && hs_append(out, outlen, &used, "-");
    ok = ok && hs_append_num(out, outlen, &used, (ULONG)ds->ds_Days);
    ok = ok && hs_append(out, outlen, &used, "-");
    ok = ok && hs_append_num(out, outlen, &used, (ULONG)ds->ds_Minute);
    ok = ok && hs_append(out, outlen, &used, "-");
    ok = ok && hs_append_num(out, outlen, &used, (ULONG)ds->ds_Tick);
    ok = ok && hs_append(out, outlen, &used, "\"");

    if (!ok)
        out[0] = '\0';
}

/* The same, for a path nothing has looked at yet.  "" when there is nothing
   there, or when it is a drawer. */
static VOID httpd_etag_of(const char *path, char *out, ULONG outlen)
{
    BPTR lock;

    out[0] = '\0';

    if (httpd_fib2 == NULL)
        return;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0)
        return;

    if (Examine(lock, httpd_fib2) && httpd_fib2->fib_DirEntryType <= 0)
        httpd_etag((ULONG)httpd_fib2->fib_Size, &httpd_fib2->fib_Date,
                   out, outlen);

    UnLock(lock);
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
 * The walk's three primitives, http_path_join(), http_path_up() and
 * http_path_within(), are in src/tools/httppath.c and not here, for the
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

/* --------------------------------------------------------------- walking --- */

/*
 * DELETE and COPY of a drawer are the only unbounded work this server does.
 * Everything else here is already incremental, a directory listing is
 * produced entry by entry across passes of the loop, and a walk that ran to
 * completion inside one dispatch would stop every other connection for as long
 * as the tree took, which is exactly the workload a file server sees.
 *
 * So a walk is a state machine that advances by a slice per pass.  Everything
 * it is in the middle of lives in the connection: the two paths, the depth,
 * and one open lock per level.
 *
 * KEEPING A PLACE IN A DRAWER WITHOUT KEEPING A FileInfoBlock
 *
 *   ExNext() carries its position in the FileInfoBlock, and there is one of
 *   those per connection, so descending into a subdrawer and examining THAT
 *   overwrites the position in the drawer above.  One block per level would be
 *   260 bytes a level, which is not a thing to spend on a machine with a
 *   megabyte; holding a lock per level does not help either, because it is the
 *   block and not the lock that remembers.
 *
 *   So a level remembers by ORDINAL: how many subdrawers of it have already
 *   been walked.  Coming back up re-reads the drawer from the start and takes
 *   the next one, which costs a rescan per subdrawer, ExNext() and no more,
 *   the files having already gone, and needs two bytes a level.
 *
 *   This is not a refinement.  Without it a walk descends once, comes back to
 *   a scan that reports the drawer as empty, and stops: the first version of
 *   this created every drawer of a tree and copied almost none of the files.
 *
 * WHAT ExNext() IS NOT TRUSTED FOR
 *
 *   No filesystem promises the scan survives the deletions it is making, so
 *   "is this drawer empty now" is asked of DeleteFile() and not of the scan.
 *   A drawer is finished when deleting it succeeds; ERROR_DIRECTORY_NOT_EMPTY
 *   with nothing having failed inside means the scan missed something, and
 *   sends it round again.  With something having failed inside it means what
 *   it says, and the walk goes up instead, which is what `walk_dirty` is
 *   for, one bit per level.
 */

/* The lock table is below, and a walk that removed something has to be able
   to say so, the same reason httpd_walk_release() is declared above. */
static VOID httpd_locks_drop(const char *path);

/* Everything below this level failed with it, so no drawer above can go and
   none of them may be retried. */
static VOID httpd_walk_mark(HttpConn *c)
{
    c->walk_dirty |= (1UL << (ULONG)(c->walk_depth + 1)) - 1UL;
}

/*
 * One <D:response> for something that would not go.  Bounded twice over,
 * HTTPD_FAILS_MAX of them and HTTPD_FAIL_MAX bytes of them, because a drawer
 * of a thousand delete-protected files must not become a thousand-element
 * answer on a machine with a megabyte.
 */
static VOID httpd_walk_failed(HttpConn *c, const char *path, ULONG status)
{
    ULONG used = c->fails_len;
    BOOL  ok;

    if (c->fails >= (UBYTE)HTTPD_FAILS_MAX)
        return;

    ok = hs_append(c->fail_xml, sizeof(c->fail_xml), &used,
                   "<D:response><D:href>");
    ok = ok && hs_append(c->fail_xml, sizeof(c->fail_xml), &used,
                         httpd_url_of(path));
    ok = ok && hs_append(c->fail_xml, sizeof(c->fail_xml), &used,
                         "</D:href><D:status>HTTP/1.1 ");
    ok = ok && hs_append_num(c->fail_xml, sizeof(c->fail_xml), &used, status);
    ok = ok && hs_append(c->fail_xml, sizeof(c->fail_xml), &used, " ");
    ok = ok && hs_append(c->fail_xml, sizeof(c->fail_xml), &used,
                         httpd_reason(status));
    ok = ok && hs_append(c->fail_xml, sizeof(c->fail_xml), &used,
                         "</D:status></D:response>\n");

    if (!ok)
    {
        /* It did not fit, so the one before it is the last.  The count goes
           to the ceiling so nothing tries to add another. */
        c->fail_xml[c->fails_len] = '\0';
        c->fails = (UBYTE)HTTPD_FAILS_MAX;
        return;
    }

    c->fails_len = used;
    c->fails++;
}

/* Every lock a walk is holding.  Called when it ends, and when the connection
   goes away under it. */
static VOID httpd_walk_release(HttpConn *c)
{
    if (c->walk_lock != (BPTR)0)
    {
        UnLock(c->walk_lock);
        c->walk_lock = (BPTR)0;
    }

    c->walk = WALK_NONE;
}

static VOID httpd_walk_begin(HttpConn *c, UBYTE what, BOOL copy, UBYTE first)
{
    httpd_walk_release(c);

    c->walk_skip[0] = 0;
    c->walk_seen    = 0;
    c->walk_for   = what;
    c->walk_copy  = copy ? 1 : 0;
    c->walk_one   = 0;
    c->walk_depth = 0;
    c->walk_dirty = 0;
    c->walk_steps = 0;
    c->walk       = first;
    c->state      = CONN_WALK;
}

/* The 204 or the 207, once a walk that was asked for by name has finished. */
static VOID httpd_walk_answer(HttpConn *c, ULONG plain)
{
    ULONG used = 0;
    BOOL  ok;

    if (c->fails == 0)
    {
        httpd_empty(c, plain);
        return;
    }

    ok = hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<D:multistatus xmlns:D=\"DAV:\">\n");
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         c->fail_xml);
    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:multistatus>\n");

    if (!ok)
    {
        httpd_error(c, 500, "that answer would not fit");
        return;
    }

    httpd_begin(c, 207);
    httpd_body_text(c, "text/xml; charset=utf-8", httpd_scratch);
}

/*
 * The copy half of a COPY or a MOVE, once the destination is out of the way.
 * A MOVE tries the rename first: within a volume nothing is copied, which is
 * what makes moving a 50 MB drawer instant.  Between volumes it cannot be, and
 * AmigaDOS says which it was rather than leaving it to be guessed from the
 * device names, an assign makes that guess wrong, and guessing wrong means
 * either copying a whole tree for a rename or renaming across a boundary.
 */
static VOID httpd_walk_copy_stage(HttpConn *c)
{
    LONG kind;

    if (c->walk_move)
    {
        LONG err;

        if (Rename((CONST_STRPTR)c->path.path, (CONST_STRPTR)c->dest.path))
        {
            /* The source name is gone, so nothing is holding it any more. */
            httpd_locks_drop(c->path.path);
            httpd_empty(c, c->walk_new ? 201 : 204);
            return;
        }

        err = IoErr();

        if (err != ERROR_RENAME_ACROSS_DEVICES)
        {
            httpd_error(c, httpd_dos_status(err), "that could not be moved");
            return;
        }
    }

    kind = httpd_kind(c->path.path);
    if (kind < 0)
    {
        httpd_error(c, 404, "there is nothing there to copy");
        return;
    }

    hs_copy(c->walk_src, sizeof(c->walk_src), c->path.path);
    hs_copy(c->walk_dst, sizeof(c->walk_dst), c->dest.path);

    c->fails     = 0;
    c->fails_len = 0;
    c->fail_xml[0] = '\0';
    c->walk_status = 0;

    if (kind == 0)
    {
        httpd_walk_begin(c, WALK_FOR_COPY, TRUE, WALK_OPEN);
        c->walk_one = 1;
        return;
    }

    /* Depth: 0 on a collection copies the collection and not what is in it,
       RFC 4918 9.8.3.  A MOVE is always infinity. */
    if (!c->walk_move && c->depth == 0)
    {
        BPTR made = CreateDir((CONST_STRPTR)c->dest.path);

        if (made == (BPTR)0)
        {
            httpd_error(c, httpd_dos_status(IoErr()),
                        "that drawer could not be made");
            return;
        }
        UnLock(made);

        httpd_empty(c, c->walk_new ? 201 : 204);
        return;
    }

    httpd_walk_begin(c, WALK_FOR_COPY, TRUE, WALK_MKDIR);
}

static VOID httpd_log_status(HttpConn *c);

/* A walk has reached WALK_DONE.  What happens next is what it was for. */
static VOID httpd_walk_end(HttpConn *c)
{
    httpd_walk_release(c);

    switch (c->walk_for)
    {
        case WALK_FOR_DELETE:
            /* RFC 4918 9.6.  Only when the whole tree went: a name that is
               still there is still the resource its lock was taken on. */
            if (c->fails == 0)
                httpd_locks_drop(c->path.path);
            httpd_walk_answer(c, 204);
            break;

        case WALK_FOR_CLEAR:
            if (c->fails != 0 || c->walk_status != 0)
            {
                httpd_error(c, (c->walk_status != 0) ? c->walk_status : 409,
                            "what was there already would not go");
                break;
            }
            httpd_walk_copy_stage(c);
            break;

        case WALK_FOR_COPY:
            if (c->walk_status != 0)
            {
                httpd_walk_answer(c, 500);
                break;
            }

            if (!c->walk_move)
            {
                httpd_walk_answer(c, c->walk_new ? 201 : 204);
                break;
            }

            /* The copy is in place, so the original goes, and only now, so
               that a copy that failed has cost the client nothing. */
            hs_copy(c->walk_src, sizeof(c->walk_src), c->path.path);
            httpd_walk_begin(c, WALK_FOR_MOVED, FALSE, WALK_ENTER);
            break;

        default:                        /* WALK_FOR_MOVED                  */
            if (c->fails == 0)
                httpd_locks_drop(c->path.path);
            httpd_walk_answer(c, c->walk_new ? 201 : 204);
            break;
    }

    /* A walk that has started another one has not answered yet. */
    if (c->state != CONN_WALK)
        httpd_log_status(c);
}

/*
 * One slice.  Called once per pass of the event loop for every connection that
 * is walking, and it does a bounded amount of AmigaDOS and returns.
 */
static VOID httpd_walk_slice(HttpConn *c)
{
    ULONG n;

    for (n = 0; n < (ULONG)HTTPD_WALK_SLICE && c->walk != WALK_DONE; n++)
    {
        if (++c->walk_steps > HTTPD_WALK_MAX)
        {
            httpd_walk_failed(c, c->walk_src, 500);
            c->walk_status = 500;
            c->walk = WALK_DONE;
            break;
        }

        switch (c->walk)
        {
            case WALK_ENTER:
            {
                BPTR lock = Lock((CONST_STRPTR)c->walk_src, ACCESS_READ);

                if (lock == (BPTR)0)
                {
                    httpd_walk_failed(c, c->walk_src,
                                      httpd_dos_status(IoErr()));
                    httpd_walk_mark(c);
                    c->walk = WALK_UP;
                    break;
                }

                if (!Examine(lock, c->fib))
                {
                    UnLock(lock);
                    httpd_walk_failed(c, c->walk_src, 500);
                    httpd_walk_mark(c);
                    c->walk = WALK_UP;
                    break;
                }

                /* A file only reaches here as the whole of a DELETE: one
                   inside a drawer goes in the scan, and a file copy starts at
                   WALK_OPEN. */
                if (c->fib->fib_DirEntryType <= 0)
                {
                    UnLock(lock);

                    if (!DeleteFile((CONST_STRPTR)c->walk_src))
                    {
                        httpd_walk_failed(c, c->walk_src,
                                          httpd_dos_status(IoErr()));
                        httpd_walk_mark(c);
                    }

                    c->walk = WALK_UP;
                    break;
                }

                c->walk_lock = lock;
                c->walk_seen = 0;
                c->walk = WALK_SCAN;
                break;
            }

            case WALK_MKDIR:
            {
                BPTR made = CreateDir((CONST_STRPTR)c->walk_dst);

                if (made == (BPTR)0)
                {
                    /* Nothing below a drawer that cannot be made is going to
                       land either, so the copy stops rather than failing once
                       per file underneath it. */
                    c->walk_status = httpd_dos_status(IoErr());
                    httpd_walk_failed(c, c->walk_dst, c->walk_status);
                    c->walk = WALK_DONE;
                    break;
                }

                UnLock(made);

                /* A level of a copy is the drawer first and the source lock
                   second, so WALK_ENTER is the same step for both walks. */
                c->walk = WALK_ENTER;
                break;
            }

            case WALK_SCAN:
            {
                if (c->walk_lock == (BPTR)0 || !ExNext(c->walk_lock, c->fib))
                {
                    /* Nothing left, and no subdrawer this pass was going to
                       take, so this level is finished with. */
                    UnLock(c->walk_lock);
                    c->walk_lock = (BPTR)0;
                    c->walk = c->walk_copy ? WALK_UP : WALK_RMDIR;
                    break;
                }

                /*
                 * A link is not walked through, whatever it points at.  This
                 * is the only place one can be recognised, and it is the
                 * place that matters: a hard-linked drawer inside the served
                 * tree used to be descended into, so a DELETE of the tree
                 * deleted what was on the far side of it, outside the
                 * document root, which nothing else here can reach.
                 *
                 * The entry itself still goes on a DELETE.  Removing a link
                 * removes the link.
                 */
                if (httpd_entry_is_link((LONG)c->fib->fib_DirEntryType))
                {
                    if (!http_path_join(c->walk_src, sizeof(c->walk_src),
                                        (const char *)c->fib->fib_FileName))
                    {
                        httpd_walk_failed(c, c->walk_src, 414);
                        httpd_walk_mark(c);
                        break;
                    }

                    if (c->walk_copy)
                    {
                        /* Copying it would copy what it points at, which is
                           not what was asked for and may not be in the tree
                           at all. */
                        httpd_walk_failed(c, c->walk_src, 403);
                        httpd_walk_mark(c);
                    }
                    else if (!DeleteFile((CONST_STRPTR)c->walk_src))
                    {
                        httpd_walk_failed(c, c->walk_src,
                                          httpd_dos_status(IoErr()));
                        httpd_walk_mark(c);
                    }

                    http_path_up(c->walk_src);
                    break;
                }

                if (c->fib->fib_DirEntryType > 0)
                {
                    /* The (skip + 1)th subdrawer of this level is the one
                       that has not been walked yet.  Everything before it
                       has, and is either gone or was named as a failure. */
                    c->walk_seen++;

                    if (c->walk_seen <= c->walk_skip[c->walk_depth])
                        break;

                    if (c->walk_depth + 1 >= (UWORD)HTTPD_WALK_DEPTH)
                    {
                        httpd_walk_failed(c, c->walk_src, 409);
                        httpd_walk_mark(c);
                        c->walk_skip[c->walk_depth]++;
                        break;
                    }

                    if (!http_path_join(c->walk_src, sizeof(c->walk_src),
                                        (const char *)c->fib->fib_FileName) ||
                        (c->walk_copy &&
                         !http_path_join(c->walk_dst, sizeof(c->walk_dst),
                                         (const char *)c->fib->fib_FileName)))
                    {
                        httpd_walk_failed(c, c->walk_src, 414);
                        httpd_walk_mark(c);
                        c->walk_skip[c->walk_depth]++;
                        break;
                    }

                    /* The scan of this level is abandoned rather than kept:
                       the FileInfoBlock is about to be the subdrawer's, and
                       the ordinal is what brings this level back. */
                    UnLock(c->walk_lock);
                    c->walk_lock = (BPTR)0;

                    c->walk_skip[c->walk_depth]++;
                    c->walk_depth++;
                    c->walk_skip[c->walk_depth] = 0;
                    c->walk = c->walk_copy ? WALK_MKDIR : WALK_ENTER;
                    break;
                }

                if (!http_path_join(c->walk_src, sizeof(c->walk_src),
                                    (const char *)c->fib->fib_FileName))
                {
                    httpd_walk_failed(c, c->walk_src, 414);
                    httpd_walk_mark(c);
                    break;
                }

                if (c->walk_copy)
                {
                    if (!http_path_join(c->walk_dst, sizeof(c->walk_dst),
                                        (const char *)c->fib->fib_FileName))
                    {
                        http_path_up(c->walk_src);
                        c->walk_status = 414;
                        httpd_walk_failed(c, c->walk_src, 414);
                        c->walk = WALK_DONE;
                        break;
                    }

                    /* A drawer is read again once per subdrawer, so a file
                       that is already at the far end was copied on an earlier
                       pass over this level. */
                    if (httpd_kind(c->walk_dst) >= 0)
                    {
                        http_path_up(c->walk_src);
                        http_path_up(c->walk_dst);
                        break;
                    }

                    c->walk = WALK_OPEN;
                    break;
                }

                if (!DeleteFile((CONST_STRPTR)c->walk_src))
                {
                    httpd_walk_failed(c, c->walk_src,
                                      httpd_dos_status(IoErr()));
                    httpd_walk_mark(c);
                }

                http_path_up(c->walk_src);
                break;
            }

            case WALK_RMDIR:
            {
                LONG err;

                if (DeleteFile((CONST_STRPTR)c->walk_src))
                {
                    c->walk = WALK_UP;
                    break;
                }

                err = IoErr();

                if (err == ERROR_DIRECTORY_NOT_EMPTY &&
                    (c->walk_dirty & (1UL << (ULONG)c->walk_depth)) == 0UL)
                {
                    /* The scan said empty and the filesystem says otherwise,
                       so the scan is what is wrong, ExNext() is not promised
                       across the deletions it just made.  Nothing here failed,
                       so going round again terminates. */
                    c->walk_skip[c->walk_depth] = 0;
                    c->walk = WALK_ENTER;
                    break;
                }

                /* A drawer that is only still full has already had whatever
                   is in it named, and naming the drawer as well is noise the
                   client cannot act on. */
                if (err != ERROR_DIRECTORY_NOT_EMPTY)
                    httpd_walk_failed(c, c->walk_src, httpd_dos_status(err));

                httpd_walk_mark(c);
                c->walk = WALK_UP;
                break;
            }

            case WALK_OPEN:
                c->file = Open((CONST_STRPTR)c->walk_src, MODE_OLDFILE);
                if (c->file == (BPTR)0)
                {
                    c->walk_status = httpd_dos_status(IoErr());
                    httpd_walk_failed(c, c->walk_src, c->walk_status);
                    c->walk = WALK_DONE;
                    break;
                }

                c->put = Open((CONST_STRPTR)c->walk_dst, MODE_NEWFILE);
                if (c->put == (BPTR)0)
                {
                    c->walk_status = httpd_dos_status(IoErr());
                    httpd_walk_failed(c, c->walk_dst, c->walk_status);
                    (VOID)Close(c->file);
                    c->file = (BPTR)0;
                    c->walk = WALK_DONE;
                    break;
                }

                /* The half-written destination IS the temporary as far as
                   httpd_put_abandon() is concerned, so a client that hangs up
                   in the middle of a copy leaves no part-file behind. */
                hs_copy(c->put_temp, sizeof(c->put_temp), c->walk_dst);

                c->walk = WALK_FILE;
                break;

            case WALK_FILE:
            {
                ULONG k;

                for (k = 0; k < (ULONG)HTTPD_COPY_SLICE; k++)
                {
                    LONG got = Read(c->file, (APTR)httpd_scratch,
                                    (LONG)sizeof(httpd_scratch));

                    if (got < 0)
                    {
                        c->walk_status = 500;
                        httpd_walk_failed(c, c->walk_src, 500);
                        c->walk = WALK_DONE;
                        break;
                    }

                    if (got == 0)
                    {
                        c->walk = WALK_SHUT;
                        break;
                    }

                    if (Write(c->put, (APTR)httpd_scratch, got) != got)
                    {
                        c->walk_status = httpd_dos_status(IoErr());
                        httpd_walk_failed(c, c->walk_dst, c->walk_status);
                        c->walk = WALK_DONE;
                        break;
                    }
                }
                break;
            }

            case WALK_SHUT:
            {
                BPTR lock;

                (VOID)Close(c->put);
                c->put = (BPTR)0;
                (VOID)Close(c->file);
                c->file = (BPTR)0;
                c->put_temp[0] = '\0';

                /* The date and the protection bits follow the file: a copy
                   that loses them is a copy every client shows as changed.
                   httpd_fib2 and not c->fib, which is holding the place in
                   the drawer this file came out of. */
                lock = Lock((CONST_STRPTR)c->walk_src, ACCESS_READ);
                if (lock != (BPTR)0)
                {
                    if (httpd_fib2 != NULL && Examine(lock, httpd_fib2))
                    {
                        (VOID)SetFileDate((CONST_STRPTR)c->walk_dst,
                                          &httpd_fib2->fib_Date);
                        (VOID)SetProtection(
                            (CONST_STRPTR)c->walk_dst,
                            (LONG)httpd_fib2->fib_Protection);
                    }
                    UnLock(lock);
                }

                if (c->walk_one)
                {
                    c->walk = WALK_DONE;
                    break;
                }

                http_path_up(c->walk_src);
                http_path_up(c->walk_dst);
                c->walk = WALK_SCAN;
                break;
            }

            default:                    /* WALK_UP                         */
                if (c->walk_lock != (BPTR)0)
                {
                    UnLock(c->walk_lock);
                    c->walk_lock = (BPTR)0;
                }

                if (c->walk_depth == 0)
                {
                    c->walk = WALK_DONE;
                    break;
                }

                http_path_up(c->walk_src);
                if (c->walk_copy)
                    http_path_up(c->walk_dst);

                c->walk_depth--;
                c->walk = WALK_ENTER;   /* the ordinal takes it on from here */
                break;
        }
    }

    /* A walk that is getting somewhere is a connection that is getting
       somewhere, or the no-progress timeout would close it under itself. */
    c->progress = httpd_now();

    if (c->walk == WALK_DONE)
        httpd_walk_end(c);
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
 * "Tue, 05 Aug 2025 12:00:00 GMT" back to a DateStamp in local time, which
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
 * however many write methods the server answers, the lock table is what
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

/*
 * Every lock rooted at `path` or below it, gone.  RFC 4918 9.6: a DELETE that
 * succeeded MUST destroy them, and a MOVE's source is a DELETE of that name.
 *
 * Without this the name is unusable by anybody until the lock expires,
 * including the client that just deleted it, whose next PUT of the same name
 * is answered 423 for a lock on a resource that no longer exists.
 */
static VOID httpd_locks_drop(const char *path)
{
    ULONG i;

    for (i = 0; i < (ULONG)HTTPD_LOCK_MAX; i++)
    {
        HttpLock *l = &httpd_locks[i];

        if (l->used && http_path_within(path, l->path))
            l->used = 0;
    }
}

/*
 * Does `l` cover `path`?  Its own resource, or, when it was taken with
 * Depth: infinity, anything below it.
 *
 * Both UNLOCK and a LOCK refresh have to ask this.  Holding a token is not
 * enough on its own: the table is global, so a token alone lets any client
 * operate on any lock in it through any address.
 */
static BOOL httpd_lock_covers(const HttpLock *l, const char *path)
{
    if (l == NULL)
        return FALSE;

    if (hs_equal(l->path, path))
        return TRUE;

    return (l->depth != 0 && http_path_within(l->path, path)) ? TRUE : FALSE;
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
static VOID httpd_locked(HttpConn *c)
{
    httpd_begin(c, 423);
    httpd_body_text(c, "text/xml; charset=utf-8",
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<D:error xmlns:D=\"DAV:\">"
                    "<D:lock-token-submitted/></D:error>\n");
}

static BOOL httpd_lock_allows(HttpConn *c, const char *path)
{
    if (httpd_holds(c, httpd_lock_on(path)))
        return TRUE;

    httpd_locked(c);

    return FALSE;
}

/*
 * The same question asked DOWNWARDS as well.  DELETE and MOVE take everything
 * below the address with them, so a lock on something inside stops them,
 * RFC 4918 9.6.1, where a write to the address itself does not care what is
 * locked underneath it.
 */
static BOOL httpd_lock_allows_tree(HttpConn *c, const char *path)
{
    ULONG i;

    if (!httpd_lock_allows(c, path))
        return FALSE;

    httpd_locks_expire();

    for (i = 0; i < (ULONG)HTTPD_LOCK_MAX; i++)
    {
        HttpLock *l = &httpd_locks[i];

        if (!l->used || !http_path_within(path, l->path))
            continue;

        if (httpd_holds(c, l))
            continue;

        httpd_locked(c);

        return FALSE;
    }

    return TRUE;
}

/*
 * The lock on the drawer this address is IN.
 *
 * RFC 4918 7.1: a lock on a collection, at any depth including 0, locks that
 * collection's internal member namespace, so adding a member or taking one
 * away needs the token even when the member itself is not locked.  Changing an
 * existing member's content does not, which is why this is asked only by the
 * methods that create or remove a name.
 *
 * httpd_lock_on() answers about the address itself and about anything under a
 * deep lock; neither of those covers a Depth: 0 lock one level up, and without
 * this a client that locked a shared drawer to add a file to it watched
 * somebody else add one anyway.
 */
static BOOL httpd_lock_allows_parent(HttpConn *c, const char *path)
{
    char parent[HTTP_PATH_MAX];

    hs_copy(parent, (ULONG)sizeof(parent), path);
    http_path_up(parent);

    /* Unchanged means there is nothing above it: a device reference is the
       top and has no drawer to be a member of. */
    if (hs_equal(parent, path) || parent[0] == '\0')
        return TRUE;

    if (httpd_holds(c, httpd_lock_on(parent)))
        return TRUE;

    httpd_locked(c);

    return FALSE;
}

/*
 * "opaquelocktoken:" and 32 hex digits.
 *
 * WHAT A TOKEN IS AND IS NOT HERE
 *
 *   There is no authentication in this server, so anybody who can reach it can
 *   already write to anything that is not locked.  A lock is coordination
 *   between clients that are cooperating, not a boundary, and a guessed token
 *   buys an attacker nothing they did not already have.  RFC 4918 6.5 asks for
 *   unguessable anyway, and on a machine with no RTC, no entropy pool and no
 *   randomness in its timings there is nothing here to make one out of.
 *
 *   So the two properties that CAN be had are the two that are aimed at: a
 *   token is never issued twice in a run, the counter guarantees it, where
 *   eight hex digits out of one LCG word did not, and a token from a
 *   previous run does not name a lock in this one.
 *
 *   Eight digits was also short enough to collide by accident at 2^-32 per
 *   pair, which on a table of 16 is not a risk worth carrying for 24 bytes.
 */
static VOID httpd_make_token(char *out, ULONG outlen)
{
    static const char hex[] = "0123456789abcdef";
    static ULONG      count;

    ULONG used = 0;
    ULONG word[4];
    ULONG w;
    LONG  shift;

    count++;

    /* Four words, each stepped from the last, so the whole 128 bits move
       rather than the low ones repeating a pattern the high ones set. */
    word[0] = httpd_token_start ^ count;
    word[1] = httpd_now();
    word[2] = httpd_token_seed;
    word[3] = count;

    for (w = 0; w < 4UL; w++)
    {
        httpd_token_seed = (httpd_token_seed * 1103515245UL) + 12345UL;
        word[w] ^= httpd_token_seed;
        word[w] ^= word[w] >> 13;
        word[w]  = (word[w] * 2654435761UL) + w;
    }

    out[0] = '\0';
    (VOID)hs_append(out, outlen, &used, "opaquelocktoken:");

    for (w = 0; w < 4UL; w++)
    {
        for (shift = 28; shift >= 0; shift -= 4)
        {
            char one[2];

            one[0] = hex[(word[w] >> (ULONG)shift) & 0xfUL];
            one[1] = '\0';
            (VOID)hs_append(out, outlen, &used, one);
        }
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
    {
        /* Dropped silently, the answer reported on the ones that fitted, and
           the client read that as an answer about all of them. */
        c->props_cut = 1;
        return;
    }

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

            /* PROPPATCH's <prop> is a set of values and PROPFIND's is a list
               of names; only the second changes what the 207 reports on. */
            if (c->method == HTTPD_M_PROPFIND)
                c->pf_mode = (UBYTE)HTTPD_PF_NAMED;
        }
        else if (hs_equal(local, "allprop"))
        {
            c->pf_mode = (UBYTE)HTTPD_PF_ALLPROP;
        }
        else if (hs_equal(local, "propname"))
        {
            c->pf_mode = (UBYTE)HTTPD_PF_PROPNAME;
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
        {
            /* The collection stopped where the buffer did, which is not where
               a character ends. */
            http_utf8_trim(c->owner);
            c->in_owner = 0;
        }
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

                    /*
                     * Markup inside <owner> contributes nothing and its text
                     * does, which is what an <owner> holding an <href> needs:
                     * the address, and not the element around it.
                     *
                     * Everything from 0x20 up but DEL, so a UTF-8 owner keeps
                     * its bytes.  This used to stop at 0x7f, which turned
                     * every non-English name into the ASCII that was left of
                     * it, "Björn" became "Bjrn", and the body IS declared
                     * UTF-8 on the way back out.
                     */
                    if (n + 1UL < sizeof(c->owner) && ch >= 0x20 && ch != 0x7f)
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
/*
 * Whether this property belongs in the 207, and a note that it was asked for.
 *
 * <allprop/> and an empty body take everything.  <propname/> takes everything
 * too, because the names are the answer; the emitter leaves the values out.
 * A named list takes what it named, and marking each one here is what lets the
 * caller put the rest in a 404 propstat, which RFC 4918 9.1.1 requires and
 * which a client uses to learn what this server does not keep.
 */
static BOOL httpd_pf_want(HttpConn *c, const char *name)
{
    UBYTE i;

    if (c->pf_mode != (UBYTE)HTTPD_PF_NAMED)
        return TRUE;

    for (i = 0; i < c->props; i++)
    {
        if (hs_equal(httpd_local(c->prop_name[i]), name))
        {
            c->prop_ok[i] = 1;
            return TRUE;
        }
    }

    return FALSE;
}

static ULONG httpd_propfind_entry(HttpConn *c, const char *href, const char *name,
                                  const char *path,
                                  BOOL is_dir, ULONG size,
                                  const struct DateStamp *date)
{
    BOOL names_only = (c->pf_mode == (UBYTE)HTTPD_PF_PROPNAME) ? TRUE : FALSE;
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
                         "</D:href><D:propstat><D:prop>");

    if (httpd_pf_want(c, "displayname"))
    {
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:displayname>");
        if (!names_only)
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, httpd_text);
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:displayname>");
    }

    if (httpd_pf_want(c, "creationdate"))
    {
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:creationdate>");
        if (!names_only)
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, created);
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:creationdate>");
    }

    if (httpd_pf_want(c, "getlastmodified"))
    {
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:getlastmodified>");
        if (!names_only)
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, modified);
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:getlastmodified>");
    }

    if (httpd_pf_want(c, "resourcetype"))
    {
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:resourcetype>");
        if (is_dir && !names_only)
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:collection/>");
        ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:resourcetype>");
    }

    if (!is_dir)
    {
        char etag[HTTPD_ETAG_MAX];

        if (httpd_pf_want(c, "getcontentlength"))
        {
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:getcontentlength>");
            if (!names_only)
                ok = ok && hs_append_num(httpd_scratch, sizeof(httpd_scratch), &used, size);
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:getcontentlength>");
        }

        if (httpd_pf_want(c, "getcontenttype"))
        {
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:getcontenttype>");
            if (!names_only)
                ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, http_content_type(name));
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:getcontenttype>");
        }

        /* The same tag GET answers with, so a client can condition a write on
           what it last read without fetching the file again. */
        httpd_etag(size, date, etag, sizeof(etag));

        if (etag[0] != '\0' && httpd_pf_want(c, "getetag"))
        {
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:getetag>");
            if (!names_only)
                ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, etag);
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:getetag>");
        }
    }

    if (httpd_pf_want(c, "supportedlock"))
    {
        if (names_only)
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:supportedlock/>");
        else
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 "<D:supportedlock><D:lockentry>"
                                 "<D:lockscope><D:exclusive/></D:lockscope>"
                                 "<D:locktype><D:write/></D:locktype>"
                                 "</D:lockentry></D:supportedlock>");
    }

    if (httpd_pf_want(c, "lockdiscovery"))
    {
        if (l != NULL && !names_only)
        {
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:lockdiscovery>");
            ok = ok && httpd_activelock(l, httpd_scratch,
                                        sizeof(httpd_scratch), &used);
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:lockdiscovery>");
        }
        else
        {
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:lockdiscovery/>");
        }
    }

    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                         "</D:prop><D:status>HTTP/1.1 200 OK</D:status>"
                         "</D:propstat>");

    /*
     * RFC 4918 9.1.1: a named property this server does not keep is reported
     * in its own propstat with 404, not left out.  Leaving it out tells the
     * client the property is absent from the resource; saying 404 tells it the
     * property is absent from the server, which is the difference between
     * "this file has no author" and "ask somebody else".
     */
    if (c->pf_mode == (UBYTE)HTTPD_PF_NAMED)
    {
        UBYTE i;
        BOOL  any = FALSE;

        for (i = 0; i < c->props; i++)
        {
            if (c->prop_ok[i])
                continue;

            if (!any)
            {
                ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<D:propstat><D:prop>");
                any = TRUE;
            }

            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "<");
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, c->prop_name[i]);
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "/>");
        }

        if (any)
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 "</D:prop>"
                                 "<D:status>HTTP/1.1 404 Not Found</D:status>"
                                 "</D:propstat>");
    }

    ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used, "</D:response>\n");

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
 * generated answers have no length anybody can know in advance, a directory
 * is read entry by entry, and chunking is what lets them be streamed without
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

                            len = httpd_propfind_entry(c,
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

                            hs_copy(httpd_probe, sizeof(httpd_probe),
                                    c->path.path);

                            if (http_path_join(httpd_probe,
                                                sizeof(httpd_probe), name))
                                full = httpd_probe;

                            len = httpd_propfind_entry(c,
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
     * implemented, it is the thing that decides whether the drive is
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

    /* What the filesystem holds is not what was asked for, so the address
       truncated onto somebody else's file.  Reading it would serve that file
       under this name.  The block is already in hand, so this costs nothing. */
    if (httpd_name_differs(c->fib, c->path.name))
    {
        UnLock(lock);
        httpd_error(c, 400,
                    "that name is longer than this filesystem keeps");
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
        char etag[HTTPD_ETAG_MAX];

        httpd_rfc1123(httpd_stamp_secs(&c->fib->fib_Date), modified);
        httpd_header(c, "Last-Modified", modified);

        httpd_etag(size, &c->fib->fib_Date, etag, sizeof(etag));
        if (etag[0] != '\0')
            httpd_header(c, "ETag", etag);
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

    /*
     * On a filesystem that truncates, the address resolves to a file with a
     * different name, so a DELETE of one long name removes another, and the
     * loss is not recoverable.  Refused for every write rather than only for
     * the ones that create a name.
     */
    if (httpd_name_cut(c->path.path, c->path.name))
    {
        httpd_error(c, 400,
                    "that name is longer than this filesystem keeps");
        return FALSE;
    }

    return httpd_lock_allows(c, c->path.path);
}

/*
 * The temporary a PUT is written to.  It goes in the DESTINATION drawer and
 * not in T:, because a rename between volumes is a copy, and the point of
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

    /*
     * A PUT that creates the file adds a name to the drawer above and needs
     * that drawer's token; a PUT over a file that is already there does not,
     * because it changes content rather than membership.  RFC 4918 7.1.
     */
    if (httpd_kind(c->path.path) < 0 &&
        !httpd_lock_allows_parent(c, c->path.path))
        return FALSE;

    if (!httpd_parent(c->path.path, c->put_temp, sizeof(c->put_temp)) ||
        httpd_kind(c->put_temp) <= 0)
    {
        c->put_temp[0] = '\0';
        httpd_error(c, 409, "there is no drawer to put that in");
        return FALSE;
    }

    /*
     * Measured before the client sends a byte, which is the difference
     * between a 507 and a floppy full of a temporary file.
     *
     * A chunked upload cannot be measured here: it does not say how long it
     * is until it has finished, which is why a client chunks in the first
     * place.  It is bounded on the way in instead, httpd_sink_put() turns a
     * short write into a 507, and that is the whole of what can be done.
     */
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

    /* The name the client asked for has to be the name that would be used,
       or the rename below lands on a file somebody else put there. */
    if (httpd_name_cut(c->path.path, c->path.name))
    {
        httpd_put_abandon(c);
        httpd_error(c, 400,
                    "that name is longer than this filesystem keeps");
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
     * exists, so an overwrite is a delete and a rename, and the delete
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

    /*
     * What landed may not carry the name that was asked for.  The check above
     * catches a COLLISION, something already there under the cut name,
     * and this catches the first one, where nothing was there and the create
     * succeeded under a name the filesystem shortened.
     *
     * It has to be undone rather than reported: the file is now reachable
     * under two different addresses, and every read of it is refused, so
     * leaving it would be a 201 for a file nobody can GET.
     */
    if (httpd_name_cut(c->path.path, c->path.name))
    {
        (VOID)DeleteFile((CONST_STRPTR)c->path.path);
        httpd_error(c, 400,
                    "that name is longer than this filesystem keeps");
        return;
    }

    httpd_empty(c, existed ? 204 : 201);
}

/*
 * DELETE on a collection is Depth infinity and nothing else.  The walk starts
 * here and finishes some passes of the loop later, in httpd_walk_end(), which
 * is what answers, 204 when the whole tree went, and the multistatus RFC
 * 4918 9.6.1 asks for when part of it did not: the client's picture of the
 * tree is then wrong in a place it has no other way to find.
 */
static VOID httpd_do_delete(HttpConn *c)
{
    if (!httpd_may_write(c))
        return;

    /* Everything under the address goes too, so a lock on anything inside it
       stops this, RFC 4918 9.6.1. */
    if (!httpd_lock_allows_tree(c, c->path.path))
        return;

    /* And it takes a name out of the drawer above, which a lock on that
       drawer protects however deep it is, RFC 4918 7.1. */
    if (!httpd_lock_allows_parent(c, c->path.path))
        return;

    if (httpd_kind(c->path.path) < 0)
    {
        httpd_error(c, 404, "there is no such file to remove");
        return;
    }

    hs_copy(c->walk_src, sizeof(c->walk_src), c->path.path);

    c->fails       = 0;
    c->fails_len   = 0;
    c->fail_xml[0] = '\0';
    c->walk_status = 0;
    c->walk_move   = 0;

    httpd_walk_begin(c, WALK_FOR_DELETE, FALSE, WALK_ENTER);
}

static VOID httpd_do_mkcol(HttpConn *c)
{
    BPTR made;

    if (!httpd_may_write(c))
        return;

    /* A new name in the drawer above, which a lock on that drawer protects
       whatever its depth, RFC 4918 7.1. */
    if (!httpd_lock_allows_parent(c, c->path.path))
        return;

    /* RFC 4918 9.3: a body the server does not understand is a 415, and this
       server understands none. */
    if (c->had_body)
    {
        httpd_error(c, 415, "this server takes no body on MKCOL");
        return;
    }

    /* A name the filesystem would truncate onto something else has already
       been refused by httpd_may_write(), so what is here is a real clash. */
    if (httpd_kind(c->path.path) >= 0)
    {
        httpd_error(c, 405, "something of that name is there already");
        return;
    }

    if (!httpd_parent(c->path.path, httpd_probe, sizeof(httpd_probe)) ||
        httpd_kind(httpd_probe) <= 0)
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

    /* Made under a name the filesystem shortened, so it answers to two
       addresses and reads under both are refused.  Undone, as PUT's is. */
    if (httpd_name_cut(c->path.path, c->path.name))
    {
        (VOID)DeleteFile((CONST_STRPTR)c->path.path);
        httpd_error(c, 400,
                    "that name is longer than this filesystem keeps");
        return;
    }

    httpd_empty(c, 201);
}

/*
 * COPY and MOVE carry the other end of the operation in a header, so the
 * Destination goes through http_path_resolve() exactly as the request target
 * did, same decode, same colon check, same root.  A destination trusted any
 * less than the target is a way out of the document root that only writes.
 */
/*
 * The authority of an absolute-form URL, the "host:port" between "//" and
 * the path, or NULL when there is none, which is the ordinary case of a
 * Destination written as a bare path.
 */
static const char *httpd_authority(const char *url, ULONG *len)
{
    ULONG i;

    *len = 0;

    for (i = 0; i < 8UL && url[i] != '\0'; i++)
    {
        if (url[i] == ':')
            break;

        if (!((url[i] >= 'a' && url[i] <= 'z') ||
              (url[i] >= 'A' && url[i] <= 'Z')))
            return NULL;
    }

    if (i == 0UL || url[i] != ':' || url[i + 1] != '/' || url[i + 2] != '/')
        return NULL;

    url += i + 3;

    while (url[*len] != '\0' && url[*len] != '/')
        (*len)++;

    return url;
}

/*
 * Does the Destination name this server?
 *
 * The host is compared and the port is not.  A client reaching a machine on a
 * LAN writes the Host: and the Destination: with whatever name it resolved,
 * an mDNS name, a NetBIOS name, a dotted address, and the two agreeing is
 * the whole of what can be checked; whether they agree about ":80" says
 * nothing about whether they mean this machine.
 *
 * TRUE when there is no authority to compare, and TRUE when the client sent no
 * Host:, an HTTP/1.0 client need not, and refusing it would be refusing on
 * no evidence.
 */
static ULONG httpd_hostlen(const char *s, ULONG len)
{
    ULONG i = 0;

    /* An IPv6 literal is bracketed and full of colons; the one that ends the
       host is the one after the ']'. */
    if (len > 0UL && s[0] == '[')
    {
        while (i < len && s[i] != ']')
            i++;

        return (i < len) ? i + 1UL : len;
    }

    while (i < len && s[i] != ':')
        i++;

    return i;
}

static BOOL httpd_dest_is_local(const HttpConn *c)
{
    ULONG       dlen;
    ULONG       hlen;
    const char *dest = httpd_authority(c->dest_url, &dlen);

    if (dest == NULL || c->host[0] == '\0')
        return TRUE;

    dlen = httpd_hostlen(dest, dlen);
    hlen = httpd_hostlen(c->host, hs_len(c->host));

    if (dlen != hlen || dlen == 0UL)
        return FALSE;

    return (hs_nicmp(dest, c->host, dlen) == 0) ? TRUE : FALSE;
}

static BOOL httpd_resolve_dest(HttpConn *c)
{
    HttpPathResult why;

    if (c->dest_url[0] == '\0')
    {
        httpd_error(c, 400, "that method needs a Destination");
        return FALSE;
    }

    /*
     * A Destination naming another machine.  http_path_resolve() throws the
     * authority away, which is right for the request target, where the only
     * host it can name is this one, so a COPY to
     * "http://elsewhere/x" used to be answered 201 for a file written HERE,
     * and the client was told a copy it never asked for had succeeded.
     *
     * RFC 4918 9.8.4: this server does not copy between hosts, so it says so.
     */
    if (!httpd_dest_is_local(c))
    {
        httpd_error(c, 502, "that destination is on another server");
        return FALSE;
    }

    why = http_path_resolve(httpd_root, c->dest_url, &c->dest);
    if (why != HTTP_PATH_OK)
    {
        if (httpd_verbose || httpd_trace)
            httpd_log(c, "refused destination \"%s\": %s", (LONG)c->dest_url,
                      (LONG)http_path_error(why));

        httpd_error(c, 403,
                    "that destination is not one this server will open");
        return FALSE;
    }

    if (c->dest.segments == 0)
    {
        httpd_error(c, 403, "the served drawer itself is not a destination");
        return FALSE;
    }

    if (hs_equal(c->dest.path, c->path.path))
    {
        httpd_error(c, 403, "the source and the destination are the same");
        return FALSE;
    }

    /* Into itself is the one that would not stop: "a" to "a/b" is a walk that
       keeps finding what it has just copied. */
    if (http_path_within(c->path.path, c->dest.path))
    {
        httpd_error(c, 409, "that destination is inside the source");
        return FALSE;
    }

    return httpd_lock_allows(c, c->dest.path);
}

/*
 * COPY and MOVE do at most three things, and each of them is a walk that runs
 * across passes of the loop: clear the destination, put the source there, and
 * for a MOVE remove the original.  httpd_walk_end() is what strings them
 * together, so this only has to decide which of them are needed.
 */
static VOID httpd_copy_or_move(HttpConn *c, BOOL moving)
{
    LONG dst_kind;

    if (!httpd_may_write(c))
        return;

    /* A MOVE removes the source, so what is locked below it stops the move
       the way it stops a DELETE.  A COPY leaves the source where it is. */
    if (moving && !httpd_lock_allows_tree(c, c->path.path))
        return;

    /* A MOVE also takes the source's name out of the drawer above it. */
    if (moving && !httpd_lock_allows_parent(c, c->path.path))
        return;

    if (httpd_kind(c->path.path) < 0)
    {
        httpd_error(c, 404, "there is nothing there to copy");
        return;
    }

    if (!httpd_resolve_dest(c))
        return;

    if (!httpd_parent(c->dest.path, httpd_probe, sizeof(httpd_probe)) ||
        httpd_kind(httpd_probe) <= 0)
    {
        httpd_error(c, 409, "there is no drawer to put that in");
        return;
    }

    if (httpd_name_cut(c->dest.path, c->dest.name))
    {
        httpd_error(c, 400,
                    "that name is longer than this filesystem keeps");
        return;
    }

    dst_kind = httpd_kind(c->dest.path);

    /* The destination is a name in ITS drawer, so that drawer's lock has to
       allow it whether or not the destination itself is locked.  Asked after
       the destination resolves, because the drawer is not known before. */
    if (dst_kind < 0 && !httpd_lock_allows_parent(c, c->dest.path))
        return;

    /*
     * The check above catches a name the filesystem would shorten onto
     * something that is already there.  With nothing there it says nothing,
     * it locks a name that does not exist yet, and the destination is
     * created shortened, answering to two addresses with reads refused under
     * both.  PUT and MKCOL undo that afterwards; a copied tree is too much to
     * undo, so it is asked before the walk starts.
     */
    if (dst_kind < 0 && !httpd_name_survives(c->dest.path, c->dest.name))
    {
        httpd_error(c, 400,
                    "that name is longer than this filesystem keeps");
        return;
    }

    c->walk_move = moving ? 1 : 0;
    c->walk_new  = (dst_kind < 0) ? 1 : 0;

    c->fails       = 0;
    c->fails_len   = 0;
    c->fail_xml[0] = '\0';
    c->walk_status = 0;

    if (dst_kind >= 0)
    {
        if (!c->overwrite)
        {
            httpd_error(c, 412,
                        "something is there already and Overwrite said no");
            return;
        }

        hs_copy(c->walk_src, sizeof(c->walk_src), c->dest.path);
        httpd_walk_begin(c, WALK_FOR_CLEAR, FALSE, WALK_ENTER);
        return;
    }

    httpd_walk_copy_stage(c);
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
 * A file on this machine has one date and no other property anybody can set.
 *
 * ALL OR NONE, WHICH IS RFC 4918 9.2 AND WAS NOT WHAT THIS DID
 *
 *   "Instructions MUST either all be executed or none executed."  The date
 *   used to be set before the answer was built, so a request naming
 *   getlastmodified and one other property applied the timestamp AND refused
 *   its sibling, in one 207.  So the settable ones are counted first and the
 *   call is only made when every name in the request is one of them.
 *
 *   When one is not, the unsettable names get the 403 and the rest get 424:
 *   they would have gone through, and did not, because of another one.  This
 *   is the one place in 4918 where 424 belongs, 9.6.1 and 9.8.3 say it
 *   SHOULD NOT appear in a DELETE's or a COPY's multistatus.
 *
 * The namespace declarations come back out as they arrived, so the prefixes
 * in the answer mean what they meant in the question.
 */
static VOID httpd_do_proppatch(HttpConn *c)
{
    ULONG used = 0;
    ULONG pass;
    UBYTE i;
    BOOL  all = TRUE;
    BOOL  ok;

    if (!httpd_may_write(c))
        return;

    if (httpd_kind(c->path.path) < 0)
    {
        httpd_error(c, 404, "there is no such file");
        return;
    }

    /*
     * No property named at all.  The answer used to be a 207 holding a
     * <D:response> with an href and nothing else, which RFC 4918 14.24 does
     * not allow, a response carries either a status or at least one
     * propstat, and a client that validates the multistatus rejects it.
     *
     * There is nothing to report on, so this is a request that did not say
     * what to do: an empty body, a <propertyupdate> with no <prop>, or a body
     * the skimmer could not follow.  All three are 400.
     */
    if (c->props == 0)
    {
        httpd_error(c, 400, "that PROPPATCH names no property");
        return;
    }

    /*
     * More names than there is room to report on.  RFC 4918 9.2 wants all of
     * them executed or none, and a partial answer that does not mention the
     * ones it dropped reads as a complete one.
     */
    if (c->props_cut)
    {
        httpd_error(c, 400, "that PROPPATCH names more properties than this "
                            "server answers about");
        return;
    }

    for (i = 0; i < c->props; i++)
    {
        if (c->prop_ok[i] == 0)
            all = FALSE;
    }

    if (all && c->have_date &&
        !SetFileDate((CONST_STRPTR)c->path.path, &c->prop_date))
    {
        /* The one call there was did not go, so nothing was executed and
           every name is refused rather than blamed on another. */
        for (i = 0; i < c->props; i++)
            c->prop_ok[i] = 0;

        all = FALSE;
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
        {
            const char *status;

            if (wanted == 0)
                status = "HTTP/1.1 403 Forbidden";
            else if (all)
                status = "HTTP/1.1 200 OK";
            else
                status = "HTTP/1.1 424 Failed Dependency";

            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 "</D:prop><D:status>");
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 status);
            ok = ok && hs_append(httpd_scratch, sizeof(httpd_scratch), &used,
                                 "</D:status></D:propstat>");
        }
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

    /* The served drawer itself IS lockable, unlike everything else about it:
       a client may not replace or remove the root, but taking an exclusive
       lock on it is how one says "nobody else write in here while I work",
       and refusing that is refusing the thing locking is for.  LOCK therefore
       does not go through httpd_may_write(), so the truncation check that
       every other method gets from there is made here instead: a lock taken
       on a name the filesystem cut is a lock on a different file. */
    if (httpd_name_cut(c->path.path, c->path.name))
    {
        httpd_error(c, 400,
                    "that name is longer than this filesystem keeps");
        return;
    }

    secs = (c->lock_secs > 0UL) ? c->lock_secs : HTTPD_LOCK_DEF;
    if (secs > HTTPD_LOCK_CAP)
        secs = HTTPD_LOCK_CAP;

    /* A LOCK with no body is a refresh of the token in If:, RFC 4918 9.10.2,
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

        /*
         * And it has to be a lock on the resource this request is about.  The
         * token was the whole of the check, so any client holding any token
         * could keep any lock in the table alive by refreshing it through a
         * URL it had nothing to do with, including one whose owner had
         * stopped and left it to expire.  UNLOCK has asked this since it was
         * written; LOCK did not.
         */
        if (!httpd_lock_covers(l, c->path.path))
        {
            httpd_error(c, 412, "that lock is not on that resource");
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
            /* RFC 4918 9.10.6 names the precondition: a lock is already there
               and this one would conflict with it.  It used to be an HTML
               page, which a WebDAV client parses as nothing. */
            httpd_begin(c, 423);
            httpd_body_text(c, "text/xml; charset=utf-8",
                            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                            "<D:error xmlns:D=\"DAV:\">"
                            "<D:no-conflicting-lock/></D:error>\n");
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
    HttpLock *l;

    /* No header at all is a malformed request, RFC 4918 9.11: the Lock-Token
       is what an UNLOCK consists of.  It used to be indistinguishable from a
       token this server has never heard of, and both were 409, so a client
       whose header this server failed to parse was told the lock was gone. */
    if (c->unlock_token[0] == '\0')
    {
        httpd_error(c, 400, "UNLOCK needs a Lock-Token");
        return;
    }

    l = httpd_lock_by_token(c->unlock_token);

    if (l == NULL)
    {
        httpd_error(c, 409, "that is not a lock this server is holding");
        return;
    }

    /* RFC 4918 9.11.1: the token has to name a lock that covers the address
       the request was made against, or one client can unlock another's. */
    if (!httpd_lock_covers(l, c->path.path))
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
     * without the size.  Ignoring the header is a legal answer, the client
     * gets the whole file with a 200 and reads what it wanted, and no client
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
 * The tokens inside an If:, which is a different question from whether the
 * header HOLDS, RFC 4918 10.4.1 asks both.  These are the tokens the
 * request submits, and a lock whose token is among them is one this client
 * may write through; src/tools/httpif.c evaluates the conditions.  Two tokens
 * is as many as a request needs, which is a MOVE with both ends locked.
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
 * that adding one, Authorization is the obvious next, is a case in this
 * switch and not a second pass over the buffer.
 */
static BOOL httpd_parse(HttpConn *c, ULONG headlen)
{
    char   method[24];
    ULONG  i = 0;
    ULONG  n = 0;
    ULONG  headers = 0;
    BOOL   seen_len = FALSE;
    BOOL   seen_te  = FALSE;
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
        BOOL  cut   = FALSE;        /* the value did not fit in httpd_value */

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
                else
                    cut = TRUE;
                j++;
            }
            httpd_value[n] = '\0';
        }

        i++;

        if (hs_equal(name, "Content-Length"))
        {
            ULONG           len;
            HttpFrameResult bad = http_frame_length(httpd_value, &len);

            /* A length this server cannot read is not a length it may guess
               at: whatever it gets wrong stays in the socket and is parsed as
               the next request. */
            if (cut || bad != HTTP_FRAME_OK)
            {
                const char *said = cut ? "longer than this server reads"
                                       : http_frame_error(bad);

                if (httpd_verbose || httpd_trace)
                    httpd_log(c, "refused Content-Length: %s", (LONG)said, 0);

                httpd_error(c, 400, "that is not a Content-Length");
                return FALSE;
            }

            /* RFC 7230 3.3.3: two of them that disagree is the same hazard as
               one that overflowed, and for the same reason. */
            if (seen_len && len != c->body_left)
            {
                httpd_error(c, 400, "two Content-Lengths that disagree");
                return FALSE;
            }

            c->body_left = len;
            seen_len     = TRUE;
        }
        else if (hs_equal(name, "Depth"))
        {
            /* RFC 4918 10.2 has three values and no others.  "2" used to be
               read as 0, so a client asking for two levels got one and no
               indication that it had not been understood. */
            if (hs_nicmp(httpd_value, "infinity", 8) == 0 &&
                httpd_value[8] == '\0')
                c->depth = -1;
            else if (httpd_value[0] == '1' && httpd_value[1] == '\0')
                c->depth = 1;
            else if (httpd_value[0] == '0' && httpd_value[1] == '\0')
                c->depth = 0;
            else
            {
                httpd_error(c, 400, "that is not a Depth this server has");
                return FALSE;
            }
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
               it has sent it, so it chunks, which makes this the difference
               between PUT working from macOS and not working at all. */
            HttpFrameCoding te = http_frame_coding(httpd_value);

            /* Two of these is the same list written on two lines, and this
               server can apply one coding or none. */
            if (seen_te)
            {
                httpd_error(c, 400, "two Transfer-Encodings");
                return FALSE;
            }

            if (cut || te == HTTP_TE_UNSUPPORTED)
            {
                httpd_error(c, 501, "that is not a transfer encoding this "
                                    "server can undo");
                return FALSE;
            }

            if (te == HTTP_TE_CHUNKED)
                http_chunk_start(&c->chunk);
            else
                http_chunk_off(&c->chunk);

            seen_te = TRUE;
        }
        else if (hs_equal(name, "Expect"))
        {
            /* curl sends this on every PUT over a certain size and waits a
               second for the answer; the Windows redirector waits. */
            if (hs_nicmp(httpd_value, "100-continue", 12) == 0)
                c->expect = 1;
        }
        else if (hs_equal(name, "If-None-Match"))
        {
            hs_copy(c->ifnone, sizeof(c->ifnone), httpd_value);
        }
        else if (hs_equal(name, "If-Match"))
        {
            hs_copy(c->ifmatch, sizeof(c->ifmatch), httpd_value);
        }
        else if (hs_equal(name, "Host"))
        {
            /* Read for one thing only: telling a Destination that names this
               server from one that names another.  Nothing here is
               virtual-hosted, there is one document root. */
            hs_copy(c->host, sizeof(c->host), httpd_value);
        }
        else if (hs_equal(name, "Destination"))
        {
            /*
             * A Destination that did not fit used to be truncated into a
             * shorter path that still resolved, and Overwrite defaults to
             * T, so a COPY of a deep tree landed on, and replaced, whatever
             * happened to be at the cut.  Nothing is guessed here.
             */
            if (cut || hs_len(httpd_value) + 1UL >= sizeof(c->dest_url))
            {
                httpd_error(c, 414, "that destination is longer than this "
                                    "server will read");
                return FALSE;
            }

            hs_copy(c->dest_url, sizeof(c->dest_url), httpd_value);
        }
        else if (hs_equal(name, "Overwrite"))
        {
            c->overwrite = (httpd_value[0] == 'F' || httpd_value[0] == 'f')
                               ? 0 : 1;
        }
        else if (hs_equal(name, "If"))
        {
            /* Half an If: is not a weaker condition, it is a different one,
               and the half that survives the cut can be the one that says
               yes. */
            if (cut || hs_len(httpd_value) + 1UL >= sizeof(c->ifhdr))
            {
                httpd_error(c, 431, "that If: is longer than this server "
                                    "will read");
                return FALSE;
            }

            hs_copy(c->ifhdr, sizeof(c->ifhdr), httpd_value);
            httpd_parse_if(c, httpd_value);
        }
        else if (hs_equal(name, "Lock-Token"))
        {
            const char *p = httpd_value;

            if (cut)
            {
                httpd_error(c, 400, "that is not a lock token");
                return FALSE;
            }

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

    /*
     * RFC 7230 3.3.3: both together is a request whose length two ends of a
     * connection can read differently, which is the whole of request
     * smuggling.  The precedence rule, Transfer-Encoding wins, is what a
     * proxy in front of this server may not agree with, so the request is
     * refused rather than resolved.
     */
    if (seen_te && seen_len)
    {
        httpd_error(c, 400, "a body cannot have both a length and an "
                            "encoding");
        return FALSE;
    }

    /* Past this point Content-Length and Transfer-Encoding have been read, so
       a refusal knows whether there is a body and how long it is.  A refusal
       BEFORE it does not, and must close rather than guess. */
    c->framed = 1;

    c->method = httpd_lookup(method);

    if (c->method == NULL)
    {
        /* 405 and not 501: the address is fine, the verb is not, and the
           Allow header names what there is instead. */
        httpd_error(c, 405, "that is not a method this server has");
        return FALSE;
    }

    c->head_only = (c->method->id == HTTPD_M_HEAD) ? 1 : 0;
    c->had_body  = (c->body_left > 0UL ||
                    c->chunk.state != HTTP_CHUNK_OFF) ? 1 : 0;

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

/*
 * The state a condition in an If: is asking about: the lock this server is
 * holding on the resource and the tag its bytes have.  `tag` is a
 * Resource-Tag out of the header, or "" for the request target.
 *
 * A tag naming something outside this document root leaves both fields empty,
 * which is a list that cannot hold, the same answer as a resource with no
 * lock and no tag, and the right one: this server cannot speak for it.
 */
static VOID httpd_if_lookup(void *ctx, const char *tag, HttpIfState *out)
{
    HttpConn       *c = (HttpConn *)ctx;
    const char     *path = c->path.path;
    const HttpLock *l;

    if (tag[0] != '\0')
    {
        if (http_path_resolve(httpd_root, tag, &httpd_ifpath) != HTTP_PATH_OK)
            return;

        path = httpd_ifpath.path;
    }

    l = httpd_lock_on(path);
    if (l != NULL)
        hs_copy(out->token, sizeof(out->token), l->token);

    httpd_etag_of(path, out->etag, sizeof(out->etag));
}

/*
 * If-Match and If-None-Match, RFC 7232 3.1 and 3.2.
 *
 * WRITES ONLY, deliberately.  "If-None-Match: *" is how a client asks for a
 * PUT that creates and does not replace, an atomic create, and the reason
 * the header is on the list at all, and on a server with no authentication
 * it is the only way two clients can avoid overwriting each other without
 * taking a lock first.
 *
 * On a GET the same header means something else: it asks for a 304 and a
 * saved transfer, which this server does not do and which no client is harmed
 * by not getting.  Answering a read request 412 because a cache validator
 * matched would break every browser, so a read is left alone.
 *
 * FALSE when it has answered.
 */
static BOOL httpd_etag_listed(const char *list, const char *etag)
{
    ULONG n = hs_len(etag);

    if (n == 0UL)
        return FALSE;

    while (*list != '\0')
    {
        while (*list == ' ' || *list == '\t' || *list == ',')
            list++;

        /* A weak validator compares equal to a strong one for everything
           except a byte-range request, which this never is. */
        if (list[0] == 'W' && list[1] == '/')
            list += 2;

        if (*list == '\0')
            break;

        if (hs_nicmp(list, etag, n) == 0)
            return TRUE;

        while (*list != '\0' && *list != ',')
            list++;
    }

    return FALSE;
}

static BOOL httpd_preconditions(HttpConn *c)
{
    char  etag[HTTPD_ETAG_MAX];
    BOOL  exists;

    if (c->ifmatch[0] == '\0' && c->ifnone[0] == '\0')
        return TRUE;

    if ((c->method->flags & HTTPD_F_WRITE) == 0)
        return TRUE;

    exists = (httpd_kind(c->path.path) >= 0) ? TRUE : FALSE;

    if (c->ifnone[0] != '\0')
    {
        if (c->ifnone[0] == '*' && exists)
        {
            httpd_error(c, 412, "something of that name is there already");
            return FALSE;
        }

        if (c->ifnone[0] != '*' && exists)
        {
            httpd_etag_of(c->path.path, etag, sizeof(etag));

            if (httpd_etag_listed(c->ifnone, etag))
            {
                httpd_error(c, 412, "that is the version already there");
                return FALSE;
            }
        }
    }

    if (c->ifmatch[0] != '\0')
    {
        if (!exists)
        {
            httpd_error(c, 412, "there is nothing of that name here");
            return FALSE;
        }

        if (c->ifmatch[0] != '*')
        {
            httpd_etag_of(c->path.path, etag, sizeof(etag));

            if (!httpd_etag_listed(c->ifmatch, etag))
            {
                httpd_error(c, 412, "that is not the version that is there");
                return FALSE;
            }
        }
    }

    return TRUE;
}

static VOID httpd_dispatch(HttpConn *c)
{
    if (httpd_verbose && !httpd_trace)
        httpd_log(c, "%s %s", (LONG)c->method->name, (LONG)c->path.url);

    /*
     * RFC 4918 10.4.  The header used to be skimmed for tokens and never
     * evaluated, so `If: (<opaquelocktoken:deadbeef>)` on a file nobody had
     * locked was answered 201, the client's condition was not a condition
     * at all, and neither was `Not`.
     */
    if (c->ifhdr[0] != '\0' && !http_if_eval(c->ifhdr, httpd_if_lookup, c))
    {
        httpd_error(c, 412, "the If header's condition did not hold");
        httpd_log_status(c);
        return;
    }

    if (!httpd_preconditions(c))
    {
        httpd_log_status(c);
        return;
    }

    c->method->handle(c);

    /* A walk answers some passes of the loop later, and logs its own status
       when it does. */
    if (c->state != CONN_WALK)
        httpd_log_status(c);
}

/* http_chunk_feed() hands the decoded bytes here, and here is where they meet
   the method that asked for them. */
static VOID httpd_chunk_sink(void *ctx, const UBYTE *data, LONG len)
{
    HttpConn *c = (HttpConn *)ctx;

    if (c->method != NULL && c->method->sink != NULL)
        c->method->sink(c, data, len);
}

/*
 * A chunked request body, decoded as it arrives by httpframe.c.  Everything
 * the decoder is in the middle of lives in the connection, because a chunk
 * boundary falls wherever the network put it and not where the framing wanted
 * it.
 *
 * A ceiling applies to a body this server HOLDS, exactly as Content-Length's
 * does: the methods that read XML have fixed buffers, and a chunked body used
 * to reach them without being measured at all, Content-Length's 413 was on
 * `body_left`, which a chunked request never sets.  An upload is still the
 * client's business and is not measured.
 *
 * Returns how much of `data` belonged to the body; anything after that is the
 * next request.  A body that failed has answered by the time this returns, and
 * says so through HTTP_CHUNK_ERROR: the caller must stop reading rather than
 * ask whether the body is finished, because a failed one never will be.
 */
static LONG httpd_feed_chunked(HttpConn *c, const UBYTE *data, LONG len)
{
    LONG took = http_chunk_feed(&c->chunk, data, len, httpd_chunk_sink, c);

    if (c->chunk.state == HTTP_CHUNK_ERROR)
    {
        /* Nothing after a framing failure is known to be a request, so this
           answers and closes rather than resynchronising on the body's own
           bytes. */
        httpd_error(c, 400, "that is not a chunked body this server can read");
        c->keepalive = 0;
        return len;
    }

    if (c->method != NULL && (c->method->flags & HTTPD_F_UPLOAD) == 0 &&
        c->chunk.total > HTTPD_BODY_MAX)
    {
        httpd_error(c, 413, "that request body is larger than this server "
                            "will read");
        c->keepalive     = 0;
        c->chunk.state   = HTTP_CHUNK_ERROR;
        return len;
    }

    return took;
}

/*
 * Feed the body to the method's sink.  A method with no sink throws the bytes
 * away, but they are still READ, a request body left in the socket is the
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

    if (c->chunk.state != HTTP_CHUNK_OFF)
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
    if (c->chunk.state != HTTP_CHUNK_OFF)
        return (c->chunk.state == HTTP_CHUNK_DONE) ? TRUE : FALSE;

    return (c->body_left == 0UL) ? TRUE : FALSE;
}

/* A body that will never finish, because its framing did not hold.  It has
   been answered already; what is left is to stop reading. */
static BOOL httpd_body_failed(const HttpConn *c)
{
    return (c->chunk.state == HTTP_CHUNK_ERROR) ? TRUE : FALSE;
}

/*
 * A body still arriving, and whether it is arriving at all.
 *
 * The no-progress timeout asks whether ANYTHING came, and one byte answers it
 * so a client sending a byte every 29 seconds held its slot for as long as
 * it liked, and HTTPD_CONN_MAX of them held the whole table.  This asks the
 * other question: at what rate.
 *
 * The floor is far below any real transfer.  A 7 MHz machine writing to a
 * floppy manages some tens of kilobytes a second; the attack above is a
 * thirtieth of a byte.  The grace period is there so that a client which
 * pauses to think, Expect: 100-continue, or a Finder that opens the
 * connection before the user has chosen a file, is not counted against it.
 */
#define HTTPD_BODY_RATE     32UL    /* bytes a second, averaged             */
#define HTTPD_BODY_GRACE    60UL    /* seconds before the average is asked  */

static BOOL httpd_body_rate_ok(const HttpConn *c, ULONG now)
{
    ULONG elapsed;

    if (c->state != CONN_BODY || c->body_start == 0UL || now < c->body_start)
        return TRUE;

    elapsed = now - c->body_start;

    if (elapsed <= HTTPD_BODY_GRACE)
        return TRUE;

    return (c->body_got >= (elapsed - HTTPD_BODY_GRACE) * HTTPD_BODY_RATE)
               ? TRUE : FALSE;
}

/* The head, out of the buffer, leaving whatever the client pipelined behind
   it, the start of the body, or of the next request. */
static VOID httpd_drop_head(HttpConn *c, ULONG headlen)
{
    ULONG left = c->in_len - headlen;
    ULONG i;

    for (i = 0; i < left; i++)
        c->in[i] = c->in[headlen + i];

    c->in_len = left;
}

/*
 * A request refused before its body was read.
 *
 * WHY THIS IS NOT JUST "CLOSE"
 *
 *   A body left in the socket is the next request as far as the parser is
 *   concerned.  Three refusals, 405 for a verb this server has not, 501 for
 *   one too long to be one, and 403 for an address it will not open, used
 *   to answer with keep-alive still on and the body still there, so a POST
 *   refused 405 whose body held a `DELETE /file HTTP/1.1` line had that
 *   DELETE executed as the next request.  No attacker needed: a client that
 *   POSTs to a WebDAV share does this to itself.
 *
 *   Clearing keep-alive everywhere would have fixed it and cost a reconnect
 *   per 405, which is a request a client is meant to recover from.  So a body
 *   this server can bound is read away and the connection survives, and one
 *   it cannot bound closes:
 *
 *     no body            nothing to do, and anything pipelined behind it is
 *                        a real request that must NOT be thrown away.
 *     Content-Length     drained, up to the ceiling a held body would have
 *                        been measured against.
 *     larger than that   closed.  It is the same length the 413 refuses.
 *     chunked            closed.  The length is not knowable in advance, so
 *                        there is no bound on what draining would cost.
 *     Expect: 100        closed.  The client is waiting for a 100 that is not
 *                        coming and will not send the body, so a drain would
 *                        wait for bytes nobody is going to send.
 *     head not framed    closed.  A refusal before the headers were read does
 *                        not know whether there is a body at all, which is
 *                        what made the 501 path the worst of the three.
 */
static VOID httpd_refuse_drain(HttpConn *c)
{
    ULONG i;

    if (!c->keepalive || !c->framed || c->expect ||
        c->chunk.state != HTTP_CHUNK_OFF || c->body_left > HTTPD_BODY_MAX)
    {
        c->keepalive = 0;
        c->in_len    = 0;
        c->state     = CONN_SEND;
        return;
    }

    /* Whatever of the body already arrived is drained here rather than in the
       loop, so a body that fitted the head's buffer needs no drain at all. */
    if (c->body_left > 0UL)
    {
        ULONG take = (c->in_len < c->body_left) ? c->in_len : c->body_left;

        c->body_left -= take;

        for (i = 0; i + take < c->in_len; i++)
            c->in[i] = c->in[i + take];

        c->in_len -= take;
    }

    c->drain = (c->body_left > 0UL) ? 1 : 0;
    c->state = CONN_SEND;
}

/* The same, for a refusal made while the head is still in the buffer. */
static VOID httpd_refuse_body(HttpConn *c, ULONG headlen)
{
    httpd_drop_head(c, headlen);
    httpd_refuse_drain(c);
}

/* Anything the client pipelined after the head is the start of the body, or
   of the next request.  Either way it is already here and must not be lost. */
static VOID httpd_after_head(HttpConn *c, ULONG headlen)
{
    ULONG i;
    ULONG left;

    httpd_drop_head(c, headlen);
    left = c->in_len;

    /*
     * Nothing reaches the sink until the method has said it can take it.  A
     * PUT that cannot be started, no drawer, no room, somebody else's lock
     * is refused here, which is before the client has sent the file rather
     * than after.
     */
    if (c->method->begin != NULL && !c->method->begin(c))
    {
        /* What the client is about to send belongs to a request that will not
           be read.  A PUT is the only method with a begin(), so the body is
           usually a file and the connection ends; a small one is read away
           instead and the client keeps its socket. */
        httpd_refuse_drain(c);
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

    /* The framing failed inside what had already arrived.  It is answered;
       what is in the buffer behind it is not a request. */
    if (httpd_body_failed(c))
    {
        c->in_len = 0;
        c->state  = CONN_SEND;
        httpd_log_status(c);
        return;
    }

    if (!httpd_body_done(c))
    {
        /* The clock the arrival rate is measured against starts here, and not
           when the connection opened: what came before was the head. */
        c->body_start = httpd_now();
        c->body_got   = 0;

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

    /* The body of a request that was refused, on its way to nowhere.  The
       read is clamped to what is left of it, so nothing past the body is
       taken out of the socket and the next request is parsed normally. */
    if (c->state == CONN_DRAIN)
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

        c->body_left -= (ULONG)got;
        c->progress   = httpd_now();

        if (c->body_left == 0UL)
            httpd_reset(c);

        return TRUE;
    }

    if (c->state == CONN_BODY)
    {
        UBYTE scratch[512];
        LONG  want = (LONG)sizeof(scratch);
        LONG  took;

        /* A chunked body has no count to stop at, so the framing is what says
           where it ends and the read is whatever the socket has. */
        if (c->chunk.state == HTTP_CHUNK_OFF && (ULONG)want > c->body_left)
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
        c->progress  = httpd_now();
        c->body_got += (ULONG)got;

        if (httpd_body_failed(c))
        {
            c->in_len = 0;
            c->state  = CONN_SEND;
            httpd_log_status(c);
            return TRUE;
        }

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
                /* Answered already.  What the client sent as this request's
                   body must not be left in the socket for the parser to read
                   as the next one. */
                httpd_log_status(c);
                httpd_refuse_body(c, headlen);
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
             * was in send() rather than in the loop below,
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

        /* The answer has gone; the refused body has not.  Reading it away is
           what keeps this connection usable, so it happens before the reset
           that would forget how much of it there is. */
        if (c->drain)
        {
            c->state    = CONN_DRAIN;
            c->out_len  = 0;
            c->out_sent = 0;
            return TRUE;
        }

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
       last connection's TIME-WAIT and cannot bind for two minutes, which on
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
    httpd_conn[i].walk      = WALK_NONE;
    httpd_conn[i].walk_lock = (BPTR)0;
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
        ULONG       walking = 0;
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

            /* A walking connection wants neither half of the socket: it is
               busy with the filesystem, and the answer is not built until the
               walk ends. */
            if (c->state == CONN_WALK)
                walking++;
            else if (c->state == CONN_REQUEST || c->state == CONN_BODY ||
                     c->state == CONN_DRAIN)
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
         * a wait, so the backlog holds the caller instead and the machine
         * stays idle.
         */
        if (live < httpd_conns)
            tool_fd_add(&readfds, lsock);

        tv.tv_secs  = 0;
        /*
         * A pending walk shortens the wait to the smallest one there is
         * rather than removing it.  A zero timeout is a poll, and
         * WaitSelect() answers a poll without ever reaching Wait(), so the
         * loop then never yields, and measured on a real machine the other
         * connections went from being answered in 20 ms to being answered in
         * 1.7 SECONDS while a tree was walked.  The work was interleaved; the
         * stack simply never got the processor to notice what had arrived.
         *
         * So the walk gets a slice per pass and the pass still blocks, for
         * the shortest time the timer will carry.
         */
        tv.tv_micro = (walking > 0UL) ? HTTPD_WALK_MICROS : HTTPD_TICK_MICROS;

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

            if (c->state == CONN_WALK)
            {
                httpd_walk_slice(c);
                continue;
            }

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
             * says nothing from holding every slot on a 1 MB machine, the
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
            else if (!httpd_body_rate_ok(c, now))
            {
                if (httpd_verbose || httpd_trace)
                    httpd_log(c, "body arriving slower than %lu bytes a "
                                 "second; closing",
                              (LONG)HTTPD_BODY_RATE, 0);
                httpd_close(c);
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

    /* Trimmed before anything is resolved under it: the root is the one path
       here that does not go through http_path_resolve(), which is where every
       other doubled slash is prevented. */
    http_path_root((const char *)args[ARG_ROOT], httpd_root_buf,
                   sizeof(httpd_root_buf));
    httpd_root    = httpd_root_buf;
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
        httpd_conn[i].walk        = WALK_NONE;
        httpd_conn[i].walk_lock   = (BPTR)0;
    }

    /*
     * One FileInfoBlock per slot, taken once.  260 bytes each, and the
     * alternative, a stack one, does not fit: a Shell command has 4 KB of
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
     * handler is holding what it learned about another, a COPY examines the
     * source and then asks what is at the destination, so there is one of
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
    httpd_token_seed  = httpd_now();
    httpd_token_start = httpd_token_seed;

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
