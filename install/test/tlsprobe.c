/*
 * tlsprobe, ask tls.library what it COSTS, from inside the guest.
 *
 * install/test/amisslprobe.c does this for AmiSSL and this is its twin: same
 * helpers, same key=value output, same report file on DH0:, same refusal on a
 * stack too small, same RESULT line.  The two are meant to be diffed against
 * each other, so a key that means the same thing is spelled the same way.
 *
 * WHY IT EXISTS.  The only figure quoted for this library is the size of the
 * file, and a file size is not a resident cost.  What an application wants to
 * know before it puts TLS behind a browser is two different numbers: what the
 * library costs once, and what each open connection costs on top of it.  So
 * memory is sampled at five points --
 *
 *   start           nothing opened
 *   library_open    OpenLibrary("tls.library") has returned
 *   socket_open     bsdsocket.library is open too, the stack is up
 *   conn1           one TLS connection up: handshake done, request sent,
 *                   response read, connection STILL OPEN
 *   conn2           a second one up while the first is still open
 *   closed          both connections and both libraries closed
 *
 * -- and conn2 minus conn1 is the per-connection cost, which is the number the
 * per-library one keeps being confused with.
 *
 * RESIDENCY FIRST.  A library that is already resident when `start` is sampled
 * makes its own cost read as zero, and that error flatters us.  So before any
 * sample this walks SysBase->LibList under Forbid() and reports the open count
 * of tls.library, amisslmaster.library and the versioned AmiSSL library, or
 * that they are absent.  The same walk runs again after everything closes,
 * which is the expunge question: exec expunges a library only when it needs
 * the memory, so one still on the list after its last close has not gone.
 *
 * THE STACK.  A program that opens tls.library must not run on a Shell's
 * stack: the library brackets its caller into ThreadX and NetX Duo runs on the
 * caller's stack region (src/tools/fetch.c, at the bottom).  fetch brings its
 * own with StackSwap(); this one refuses instead, because a probe that quietly
 * moved the stack would be measuring a stack it allocated itself.  Two open
 * connections want more than one, so the floor here is 64 KB:
 *
 *     Stack 65536
 *     C:tlsprobe rsa2.test 7301 192.168.1.160 STORE DH0:teststore
 *
 * NO <stdio.h>, for the reason amisslprobe.c gives: nothing else that runs in
 * the guest links it and printf's own buffers do not fit in a Shell's stack.
 *
 * bsdsocket.library is called through its LVOs by hand, the way
 * src/tools/fetch.c does, so this links nothing of our stack.
 *
 * Exit status is 0 only if every step succeeded.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <aminetxduo/tlslib.h>

#define TEMPLATE    "HOST/A,PORT/N/A,ADDR/A,STORE/K,TIMEOUT/N/K,REPORT/K"

#define ARG_HOST     0
#define ARG_PORT     1
#define ARG_ADDR     2
#define ARG_STORE    3
#define ARG_TIMEOUT  4
#define ARG_REPORT   5
#define ARG_COUNT    6

#define REPORT_DEFAULT  "DH0:tlsprobe.txt"

/* Two live connections plus the handshake under them.  fetch asks for 64 KB
   for one; a Shell's 4,096 is not in the same country. */
#define STACK_MIN   65536L

/* A minute or two on a 68000 for a full RSA handshake, so the ceiling is not
   a timeout the slow arm trips over. */
#define TIMEOUT_DEFAULT  240UL

#define RESP_MAX    2048
#define BODY_WANT   16

static const char version_tag[] __attribute__((used)) =
    "$VER: tlsprobe 1.0 (16.8.2026) AmiNetXDuo";

static BPTR     out;


/* ------------------------------------------------------------ reporting --- */

/* Every report line goes through here, to both places, flushed.  dos.library
   takes its arguments as a LONG array, which is why the callers cast. */
static void emit(const char *fmt, LONG a, LONG b, LONG c)
{

LONG    args[3];


    args[0] = a;
    args[1] = b;
    args[2] = c;

    VPrintf((STRPTR)fmt, args);
    Flush(Output());

    if (out != 0)
    {
        VFPrintf(out, (STRPTR)fmt, args);
        Flush(out);
    }
}


static void say(const char *key, const char *value)
{

    emit("%s=%s\n", (LONG)key, (LONG)value, 0);
}


static void say_num(const char *key, LONG value)
{

    emit("%s=%ld\n", (LONG)key, value, 0);
}


/* "conn1" + "_cipher".  emit() takes three arguments and a per-phase key needs
   one of them, so the key is built here instead of being spelled with a %s in
   every format string. */
static char key_buffer[64];

static const char *keyed(const char *prefix, const char *suffix)
{

ULONG   n = 0;


    while ((*prefix != '\0') && (n + 1UL < sizeof(key_buffer)))
    {
        key_buffer[n++] = *prefix++;
    }
    while ((*suffix != '\0') && (n + 1UL < sizeof(key_buffer)))
    {
        key_buffer[n++] = *suffix++;
    }
    key_buffer[n] = '\0';
    return(key_buffer);
}


/* Free memory, in kilobytes.  Bytes would be exact and unreadable; the
   question these answer is how much of a machine a library takes, and that is
   a question about kilobytes. */
static void say_mem(const char *when)
{

    emit("mem_free_k_%s=%ld\n", (LONG)when,
         (LONG)(AvailMem(MEMF_ANY) / 1024), 0);
    emit("mem_largest_k_%s=%ld\n", (LONG)when,
         (LONG)(AvailMem(MEMF_ANY | MEMF_LARGEST) / 1024), 0);
}


/* Ticks since midnight, at 1/50 s.  DateStamp is enough: a handshake on this
   hardware is seconds, and opening timer.device to resolve it further would be
   measuring the wrong thing. */
static LONG now_ticks(void)
{

struct DateStamp    ds;


    DateStamp(&ds);
    return((ds.ds_Minute * 3000L) + ds.ds_Tick);
}


static void say_elapsed(const char *key, LONG from, LONG to)
{

LONG    ticks;


    ticks = to - from;
    /* Midnight, once a day, and the number would otherwise be negative. */
    if (ticks < 0)
    {
        ticks += 24L * 60L * 3000L;
    }
    emit("%s=%ld.%02ld\n", (LONG)key, ticks / 50L, (ticks % 50L) * 2L);
}


/* A library node's own version, and its own ID string.  Read off the node exec
   built when the library loaded, not off the file it loaded from. */
static void say_library(const char *prefix, struct Library *base)
{

    emit("%s_version=%ld.%ld\n", (LONG)prefix,
         (LONG)base -> lib_Version, (LONG)base -> lib_Revision);
    emit("%s_id=%s\n", (LONG)prefix,
         (LONG)((base -> lib_IdString != NULL)
                ? (const char *)base -> lib_IdString : "none"), 0);
}


/* ------------------------------------------------------------- residency --- */

/*
 * WHAT IS ALREADY LOADED, before this program touches anything.
 *
 * The whole measurement rests on the baseline being clean.  tls.library
 * resident from an earlier run -- exec keeps a library loaded until it needs
 * the memory back -- would make `start` include it and the library's cost read
 * as nothing.  That is the flattering answer, so it is turned into a reported
 * fact: the list is walked and the open counts are printed, and a reader can
 * see for themselves whether the number below is a first load or a second
 * opener paying nothing.
 *
 * Under Forbid(), because the list is exec's.  Nothing is printed inside it:
 * dos.library I/O breaks a Forbid() and the point of the Forbid() is that the
 * list does not change while it is walked.  The answers are copied out first.
 */

static char resident_amissl_name[64];

static BOOL str_eq(const char *a, const char *b)
{

    while ((*a != '\0') && (*a == *b))
    {
        a++;
        b++;
    }
    return((BOOL)(*a == *b));
}


static BOOL str_starts(const char *s, const char *prefix)
{

    while (*prefix != '\0')
    {
        if (*s != *prefix)
        {
            return(FALSE);
        }
        s++;
        prefix++;
    }
    return(TRUE);
}


static void str_copy(char *dst, const char *src, ULONG dstlen)
{

ULONG   i = 0;


    while ((src[i] != '\0') && (i + 1UL < dstlen))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}


/* -1 when the library is not on the list, otherwise its lib_OpenCnt. */
static void residency_scan(LONG *tls, LONG *master, LONG *versioned)
{

struct Node    *node;


    *tls       = -1;
    *master    = -1;
    *versioned = -1;
    resident_amissl_name[0] = '\0';

    Forbid();
    for (node = SysBase -> LibList.lh_Head;
         node -> ln_Succ != NULL;
         node = node -> ln_Succ)
    {
        const char     *name = (const char *)node -> ln_Name;
        struct Library *lib  = (struct Library *)node;

        if (name == NULL)
        {
            continue;
        }
        if (str_eq(name, "tls.library"))
        {
            *tls = (LONG)lib -> lib_OpenCnt;
        }
        else if (str_eq(name, "amisslmaster.library"))
        {
            *master = (LONG)lib -> lib_OpenCnt;
        }
        else if (str_starts(name, "amissl_v"))
        {
            *versioned = (LONG)lib -> lib_OpenCnt;
            str_copy(resident_amissl_name, name,
                     (ULONG)sizeof(resident_amissl_name));
        }
    }
    Permit();
}


/* $1 is "before" or "after".  Same three keys either side, so the pair reads
   as one answer to "was it there, and is it still there". */
static void say_residency(const char *when)
{

LONG    tls;
LONG    master;
LONG    versioned;


    residency_scan(&tls, &master, &versioned);

    if (tls >= 0)
    {
        emit("resident_%s_tls_library=%ld\n", (LONG)when, tls, 0);
    }
    else
    {
        emit("resident_%s_tls_library=absent\n", (LONG)when, 0, 0);
    }

    if (master >= 0)
    {
        emit("resident_%s_amisslmaster_library=%ld\n", (LONG)when, master, 0);
    }
    else
    {
        emit("resident_%s_amisslmaster_library=absent\n", (LONG)when, 0, 0);
    }

    if (versioned >= 0)
    {
        emit("resident_%s_amissl_versioned=%s:%ld\n", (LONG)when,
             (LONG)resident_amissl_name, versioned);
    }
    else
    {
        emit("resident_%s_amissl_versioned=absent\n", (LONG)when, 0, 0);
    }
}


/* --------------------------------------------------- bsdsocket, by hand --- */

/*
 * Through the LVOs, the way src/tools/fetch.c does it: a probe must not link
 * the whole socket surface to make two connections, and the NDK inline headers
 * assume a global SocketBase.
 */

#define SOCK_AF_INET        2
#define SOCK_STREAM_        1

struct ProbeSockAddr
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

static LONG sock_socket(struct Library *base, LONG domain, LONG type,
                        LONG proto)
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

static ULONG sock_inet_addr(struct Library *base, const char *dotted)
{
    register struct Library *a6  __asm("a6") = base;
    register const char     *a0  __asm("a0") = dotted;
    register ULONG           res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-180:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return res;
}


/* ------------------------------------------------------------ the round --- */

/*
 * The negotiated suite by the name the other end would print.  TLSInfo answers
 * an IANA number and AmiSSL answers an OpenSSL name, and a comparison whose
 * two halves are written in different alphabets is not a comparison.  The
 * table is the suites tests/peer/httppeer.py offers, which is the whole set
 * either side can pick from here.
 */
static const struct
{
    ULONG       suite;
    const char *name;
} cipher_names[] =
{
    { 0xC02BUL, "ECDHE-ECDSA-AES128-GCM-SHA256" },
    { 0xC02FUL, "ECDHE-RSA-AES128-GCM-SHA256"   },
    { 0xC023UL, "ECDHE-ECDSA-AES128-SHA256"     },
    { 0xC027UL, "ECDHE-RSA-AES128-SHA256"       },
    { 0x009CUL, "AES128-GCM-SHA256"             },
    { 0x003CUL, "AES128-SHA256"                 },
    { 0x003DUL, "AES256-SHA256"                 },
    { 0UL,      NULL                            }
};

static const char *cipher_name(ULONG suite)
{

ULONG   i;


    for (i = 0; cipher_names[i].name != NULL; i++)
    {
        if (cipher_names[i].suite == suite)
        {
            return(cipher_names[i].name);
        }
    }
    return("unknown");
}


static const char *tls_version_name(ULONG version)
{

    switch (version)
    {
    case 0x0301UL: return("TLSv1");
    case 0x0302UL: return("TLSv1.1");
    case 0x0303UL: return("TLSv1.2");
    case 0x0304UL: return("TLSv1.3");
    default:       return("unknown");
    }
}


static struct Library       *SocketBase;
static struct Library       *TLSBase;

static struct TLSConnection *conn[2];
static LONG                  sock[2];

static char                  request[256];
static UBYTE                 response[RESP_MAX];


static ULONG str_len(const char *s)
{

ULONG   n = 0;


    while (s[n] != '\0')
    {
        n++;
    }
    return(n);
}


static BOOL str_append(char *dst, ULONG dstlen, ULONG *used, const char *src)
{

ULONG   n = str_len(src);
ULONG   i;


    if (*used + n + 1UL > dstlen)
    {
        return(FALSE);
    }
    for (i = 0; i < n; i++)
    {
        dst[(*used)++] = src[i];
    }
    dst[*used] = '\0';
    return(TRUE);
}


/*
 * A request that leaves the connection open.  HTTP/1.1 with no
 * "Connection: close", which is what tests/peer/httppeer.py's loop keeps
 * serving on: an HTTP/1.0 request or a close token would have the peer hang up
 * and conn1 would not be a live connection by the time conn2 is measured.
 */
static BOOL build_request(const char *host, LONG port)
{

ULONG   used = 0;
char    portbuf[12];
LONG    p = port;
LONG    digits = 0;
LONG    i;


    request[0] = '\0';

    if (!str_append(request, sizeof(request), &used, "GET /bytes/16 HTTP/1.1\r\nHost: "))
    {
        return(FALSE);
    }
    if (!str_append(request, sizeof(request), &used, host))
    {
        return(FALSE);
    }
    if (!str_append(request, sizeof(request), &used, ":"))
    {
        return(FALSE);
    }

    if (p == 0)
    {
        portbuf[digits++] = '0';
    }
    while (p > 0)
    {
        portbuf[digits++] = (char)('0' + (p % 10));
        p /= 10;
    }
    /* Written backwards above. */
    for (i = 0; i < digits / 2; i++)
    {
        char t = portbuf[i];

        portbuf[i] = portbuf[digits - 1 - i];
        portbuf[digits - 1 - i] = t;
    }
    portbuf[digits] = '\0';

    if (!str_append(request, sizeof(request), &used, portbuf))
    {
        return(FALSE);
    }
    return(str_append(request, sizeof(request), &used,
                      "\r\nAccept: */*\r\n\r\n"));
}


/*
 * Read until the whole of a small response is in.  Bounded twice: by the
 * buffer and by a read count, so a peer that dribbles cannot turn this into a
 * program that never finishes.  Returns the bytes read, or -1.
 */
static LONG read_response(struct TLSConnection *c)
{

LONG    total = 0;
LONG    rounds;


    for (rounds = 0; rounds < 64; rounds++)
    {
        LONG got = TLSRead(TLSBase, c, (APTR)&response[total],
                           (LONG)(RESP_MAX - (ULONG)total));

        if (got <= 0)
        {
            return((total > 0) ? total : -1);
        }
        total += got;

        /* Header terminator, then the sixteen bytes the peer promised.  The
           connection is left open, so there is no end of stream to wait for
           and a reader that waited for one would deadlock. */
        {
            LONG i;

            for (i = 0; i + 3 < total; i++)
            {
                if ((response[i] == '\r') && (response[i + 1] == '\n') &&
                    (response[i + 2] == '\r') && (response[i + 3] == '\n'))
                {
                    if (total - (i + 4) >= BODY_WANT)
                    {
                        return(total);
                    }
                    break;
                }
            }
        }

        if ((ULONG)total >= RESP_MAX)
        {
            return(total);
        }
    }
    return(total);
}


/*
 * One connection, end to end.  $1 is 0 or 1 and names both the slot and the
 * key prefix, so the two rounds cannot report each other's numbers.
 */
static BOOL one_connection(LONG which, const char *host, LONG port,
                           ULONG addr, const char *store, ULONG timeout_ms)
{

struct ProbeSockAddr    sin;
struct TLSInfo          info;
UBYTE                  *info_bytes = (UBYTE *)&info;
const char             *key = (which == 0) ? "conn1" : "conn2";
LONG                    why = TLS_OK;
LONG                    n;
LONG                    t0;
LONG                    t1;
ULONG                   i;


    sock[which] = sock_socket(SocketBase, SOCK_AF_INET, SOCK_STREAM_, 0);
    if (sock[which] < 0)
    {
        emit("error=%s: no socket (errno %ld)\n", (LONG)key,
             sock_errno(SocketBase), 0);
        return(FALSE);
    }

    for (i = 0; i < (ULONG)sizeof(sin.sin_zero); i++)
    {
        sin.sin_zero[i] = 0;
    }
    sin.sin_len    = (UBYTE)sizeof(sin);
    sin.sin_family = (UBYTE)SOCK_AF_INET;
    sin.sin_port   = (UWORD)port;       /* big-endian host: already network */
    sin.sin_addr   = addr;

    t0 = now_ticks();
    if (sock_connect(SocketBase, sock[which], (APTR)&sin,
                     (LONG)sizeof(sin)) < 0)
    {
        emit("error=%s: cannot connect (errno %ld)\n", (LONG)key,
             sock_errno(SocketBase), 0);
        (VOID)sock_close(SocketBase, sock[which]);
        sock[which] = -1;
        return(FALSE);
    }

    if (store != NULL)
    {
        conn[which] = TLSOpen(TLSBase, (APTR)SocketBase, sock[which],
                              TLSA_HostName,   (ULONG)host,
                              TLSA_TrustStore, (ULONG)store,
                              TLSA_Timeout,    timeout_ms,
                              TLSA_Error,      (ULONG)&why);
    }
    else
    {
        conn[which] = TLSOpen(TLSBase, (APTR)SocketBase, sock[which],
                              TLSA_HostName,   (ULONG)host,
                              TLSA_Timeout,    timeout_ms,
                              TLSA_Error,      (ULONG)&why);
    }
    if (conn[which] == NULL)
    {
        emit("error=%s: TLSOpen failed: %s\n", (LONG)key,
             (LONG)TLSErrorString(TLSBase, why), 0);
        (VOID)sock_close(SocketBase, sock[which]);
        sock[which] = -1;
        return(FALSE);
    }

    n = TLSWrite(TLSBase, conn[which], (CONST_APTR)request,
                 (LONG)str_len(request));
    if (n <= 0)
    {
        emit("error=%s: TLSWrite answered %ld\n", (LONG)key, n, 0);
        return(FALSE);
    }

    n = read_response(conn[which]);
    t1 = now_ticks();
    if (n <= 0)
    {
        emit("error=%s: TLSRead answered %ld\n", (LONG)key, n, 0);
        return(FALSE);
    }

    say_elapsed(keyed(key, "_seconds"), t0, t1);
    say_num(keyed(key, "_bytes"), n);

    /*
     * Zeroed first.  This probe opens the library with version 1, so a library
     * that predates ti_Resumed leaves that field untouched; uninitialised
     * stack would then decide what is reported.
     */
    for (i = 0; i < (ULONG)sizeof(info); i++)
    {
        info_bytes[i] = 0;
    }
    info.ti_Size = (ULONG)sizeof(info);

    if (TLSInfo(TLSBase, conn[which], &info) == TLS_OK)
    {
        say(keyed(key, "_tls_version"), tls_version_name(info.ti_Version));
        emit("%s=0x%lx\n", (LONG)keyed(key, "_tls_version_raw"),
             (LONG)info.ti_Version, 0);
        say(keyed(key, "_cipher"), cipher_name(info.ti_CipherSuite));
        emit("%s=0x%lx\n", (LONG)keyed(key, "_cipher_iana"),
             (LONG)info.ti_CipherSuite, 0);
        say(keyed(key, "_resumed"), info.ti_Resumed ? "yes" : "no");
        say(keyed(key, "_verified"), info.ti_Verified ? "yes" : "no");
        say_num(keyed(key, "_chain_depth"), (LONG)info.ti_ChainDepth);
        say_num(keyed(key, "_handshake_millis"),
                (LONG)info.ti_HandshakeMillis);
    }
    else
    {
        say(keyed(key, "_info"), "unavailable");
    }

    /* The connection is LEFT OPEN.  That is the whole point of the sample:
       conn2 is measured with conn1 still up, and the difference between them
       is what one connection costs. */
    say_mem(key);

    return(TRUE);
}


int main(void)
{

struct RDArgs                  *rda = NULL;
LONG                            args[ARG_COUNT];
struct Process                 *self;
struct CommandLineInterface    *cli;
LONG                            stack;
LONG                            t0;
LONG                            t1;
const char                     *host;
const char                     *addrtext;
const char                     *store;
LONG                            port;
ULONG                           addr;
ULONG                           timeout_ms;
int                             rc = RETURN_FAIL;
LONG                            i;


    for (i = 0; i < ARG_COUNT; i++)
    {
        args[i] = 0;
    }
    sock[0] = -1;
    sock[1] = -1;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);

    out = Open((STRPTR)((args[ARG_REPORT] != 0)
                        ? (const char *)args[ARG_REPORT] : REPORT_DEFAULT),
               MODE_NEWFILE);

    say("probe", "tls");

    if (rda == NULL)
    {
        say("error", "usage: tlsprobe HOST PORT ADDR [STORE file] "
                     "[TIMEOUT n] [REPORT file]");
        goto done;
    }

    host       = (const char *)args[ARG_HOST];
    addrtext   = (const char *)args[ARG_ADDR];
    store      = (const char *)args[ARG_STORE];
    port       = *(LONG *)args[ARG_PORT];
    timeout_ms = ((args[ARG_TIMEOUT] != 0)
                  ? (ULONG)*(LONG *)args[ARG_TIMEOUT]
                  : TIMEOUT_DEFAULT) * 1000UL;

    say("host", host);
    say_num("port", port);
    say("address", addrtext);
    say("truststore", (store != NULL) ? store : "DEVS:Internet/certificates");

    /*
     * NOT pr_StackSize.  A Shell command does not get its own process: it runs
     * inside the Shell's, on a stack the Shell swaps in for it, and the size
     * of that one is cli_DefaultStack -- in LONGWORDS, which is the field's
     * documented and easily-missed unit.  A program launched from Workbench
     * has no CLI and pr_StackSize is then the right and only answer.
     */
    self = (struct Process *)FindTask(NULL);
    cli = (struct CommandLineInterface *)BADDR(self -> pr_CLI);
    stack = (cli != NULL) ? ((LONG)cli -> cli_DefaultStack) * 4L
                          : (LONG)self -> pr_StackSize;
    say_num("stack_bytes", stack);
    if (stack < STACK_MIN)
    {
        say("error", "not enough stack: run `Stack 65536` in this Shell first");
        goto done;
    }

    if (!build_request(host, port))
    {
        say("error", "that host and port do not fit in a request");
        goto done;
    }

    /* Before any sample, so the reading below can be believed. */
    say_residency("before");

    say_mem("start");

    /* --------------------------------------------------- the library ----- */

    /*
     * Version 1 and not TLS_LIB_VERSION, for the reason src/tools/fetch.c
     * gives: everything used here is an original vector, so asking for the
     * constant would make a recompile demand a library this probe does not
     * need.  ti_Resumed is a version-2 TLSInfo field and zeroing the structure
     * is what makes reading it from a version-1 library safe.
     */
    t0 = now_ticks();
    TLSBase = OpenLibrary((CONST_STRPTR)TLS_LIB_NAME, 1UL);
    t1 = now_ticks();
    if (TLSBase == NULL)
    {
        say_elapsed("library_open_seconds", t0, t1);
        say("error", "LIBS:tls.library would not open");
        goto done;
    }
    say_elapsed("library_open_seconds", t0, t1);
    say_library("library", TLSBase);
    say_mem("library_open");

    /* ------------------------------------------------- the socket stack -- */

    /*
     * bsdsocket.library AFTER the library sample, so `library_open` is the
     * library and nothing else.  This open starts the whole netstack, which is
     * a large allocation that belongs to neither TLS library and would swamp
     * both if it sat inside the number being compared.  amisslprobe.c opens it
     * at the same point for the same reason.
     */
    t0 = now_ticks();
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    t1 = now_ticks();
    if (SocketBase == NULL)
    {
        say("error", "no bsdsocket.library, or the network did not start");
        goto done;
    }
    say_elapsed("socket_open_seconds", t0, t1);
    say_mem("socket_open");

    addr = sock_inet_addr(SocketBase, addrtext);
    if (addr == 0xFFFFFFFFUL)
    {
        say("error", "ADDR is not a dotted IPv4 address");
        goto done;
    }

    /* --------------------------------------------------- the two rounds -- */

    if (!one_connection(0, host, port, addr, store, timeout_ms))
    {
        goto done;
    }
    if (!one_connection(1, host, port, addr, store, timeout_ms))
    {
        goto done;
    }

    rc = RETURN_OK;

done:
    /* Both connections, then the socket under each, then the libraries.  The
       order matters: TLSClose hands the descriptor back and closing it first
       would have the library shutting down a socket somebody else may have
       been given. */
    for (i = 1; i >= 0; i--)
    {
        if (conn[i] != NULL)
        {
            TLSClose(TLSBase, conn[i]);
            conn[i] = NULL;
        }
        if (sock[i] >= 0)
        {
            (VOID)sock_close(SocketBase, sock[i]);
            sock[i] = -1;
        }
    }
    if (TLSBase != NULL)
    {
        CloseLibrary(TLSBase);
        TLSBase = NULL;
    }
    if (SocketBase != NULL)
    {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }

    /* After the close, so the reading says whether the library came back.  It
       does not have to: exec expunges a library only when it needs the memory,
       so a machine with room keeps it loaded and the next opener pays
       nothing.  The residency walk beside it says which of those happened. */
    say_mem("closed");
    say_residency("after");
    say("RESULT", (rc == RETURN_OK) ? "PASS" : "FAIL");

    if (rda != NULL)
    {
        FreeArgs(rda);
    }
    if (out != 0)
    {
        Close(out);
        out = 0;
    }
    return(rc);
}
