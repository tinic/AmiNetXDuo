/*
 * amisslprobe, ask AmiSSL what it is, from inside the guest.
 *
 * A file on DH0: is not a working library.  The snapshot build checks the
 * bytes it copied and the launcher checks the bytes it copied, and both of
 * those are the host reading a file it wrote itself.  This runs on the Amiga
 * and performs the sequence a real application performs:
 *
 *   1. OpenLibrary("amisslmaster.library"), the proxy, ~5 KB;
 *   2. OpenAmiSSLTagList(), which is the proxy's OpenLibrary() of
 *      LIBS:AmiSSL/amissl_v362.library -- 3.5 MB of hunk file that AmigaDOS
 *      reads and relocates, and the reason this program is slow;
 *   3. InitAmiSSLA(), the per-process init;
 *   4. OpenSSL_version(OPENSSL_VERSION), which is a call INTO the library
 *      that returns the string OpenSSL was built with.
 *
 * Step 4 is the point.  Every earlier number could be got from a filename;
 * this one is the library speaking, through a jsr on its own base, and it
 * cannot be right unless the library loaded, relocated and initialised.
 * AmiSSLBase->lib_IdString is reported beside it because it names the CPU
 * build that was loaded, which a filename does not.
 *
 * The memory readings are the other half of the answer.  A versioned AmiSSL
 * library is 3.5 MB and an Amiga is not a machine with 3.5 MB to spare, so
 * "is it there" and "can this machine use it" are different questions and
 * this program answers both.  Free memory is read before the master opens,
 * after the versioned library is in, after per-process init, and after
 * everything closes; the last one against the first says whether the library
 * expunged.
 *
 * THE STACK.  A Kickstart 3.1 Shell gives a command 4,096 bytes and this
 * toolchain's crt0 exports no __stack hook to ask for more (RESEARCH 11.5),
 * which is why tests/compare/checkrunner.c launches its children with an
 * explicit one.  This program cannot launch itself, so it reads the stack it
 * was given and refuses on one too small rather than wandering off the end of
 * it: a stack overflow on this machine is a hang with nothing printed, which
 * is exactly the failure this program exists to distinguish from a slow open.
 *
 *     Stack 65536
 *     C:amisslprobe
 *
 * NO <stdio.h>.  Nothing else that runs in the guest links it, for the reason
 * above -- printf's own buffers do not fit in a Shell's stack -- and every
 * line here goes out through dos.library instead.
 *
 * Output is key=value on stdout and in DH0:amisslprobe.txt, one place because
 * the harness reads a file and one because a human runs it from a Shell.
 * Both are flushed line by line, and that is the whole point of them: the
 * step between two of these lines is the library open, which is minutes.
 * Buffered, the report would arrive at the end and neither the person
 * watching nor the host reading the file could tell a slow open from a
 * wedged one.
 *
 * Exit status is 0 only if every step above succeeded.
 *
 * Built against the AmiSSL SDK -- the same headers tests/crypto68k uses --
 * and linked against nothing of AmiSSL's: every call here is an inline macro
 * that jumps through a library base.
 *
 * WHAT A CONNECTION COSTS, added later, and the reason install/test/tlsprobe.c
 * exists beside this file.  A library's resident cost and a connection's are
 * two different numbers and were being quoted as one, so given a host, a port,
 * an address and a CA file this also opens two TLS connections over
 * bsdsocket.library, holds the FIRST one open while the second is made, and
 * samples memory at each:
 *
 *     Stack 65536
 *     C:amisslprobe HOST rsa2.test PORT 7301 ADDR 192.168.1.160 \
 *                   CAFILE DH0:testroots.pem REPORT DH0:amissl-r1.txt
 *
 * conn2 minus conn1 is the per-connection cost and conn1 minus socket_open is
 * the first one, which carries whatever the library allocates lazily.
 *
 * Run with no arguments it does exactly what it always did, samples the same
 * phases under the same keys, and writes DH0:amisslprobe.txt.  The one
 * difference in the argument form is that bsdsocket.library has to be open
 * before InitAmiSSLA -- AmiSSL_SocketBase is an init tag, not something that
 * can be handed over afterwards -- so `socket_open` is sampled between
 * `library_open` and `init` and each delta stays a delta on the phase before
 * it.  install/test/tlsprobe.c samples the same phase at the same place.
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

#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>

#include <proto/amisslmaster.h>
#include <proto/amissl.h>

#include <amissl/amissl.h>

#define TEMPLATE    "HOST/K,PORT/N/K,ADDR/K,CAFILE/K,REPORT/K"

#define ARG_HOST     0
#define ARG_PORT     1
#define ARG_ADDR     2
#define ARG_CAFILE   3
#define ARG_REPORT   4
#define ARG_COUNT    5

#define REPORT      "DH0:amisslprobe.txt"

/* Enough for the library open and AmiSSL's own per-process init to have room
   under them.  A Shell's 4,096 is not, and the failure it produces says
   nothing, so this is checked rather than hoped for.  Two live connections
   want more, so the argument form asks for the same 64 KB tlsprobe does. */
#define STACK_MIN       32768L
#define STACK_MIN_CONN  65536L

#define RESP_MAX    2048
#define BODY_WANT   16

static const char version_tag[] __attribute__((used)) =
    "$VER: amisslprobe 1.1 (16.8.2026) AmiNetXDuo";

/* The three bases AmiSSL's inline macros dereference. */
struct Library *AmiSSLMasterBase;
struct Library *AmiSSLBase;
struct Library *AmiSSLExtBase;

/* AmiSSL reaches for this when a socket or C-library call inside the library
   fails.  Nothing here uses the network, but the tag wants an address that
   outlives the call. */
static int      probe_errno;

static BPTR     out;


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
   every format string.  install/test/tlsprobe.c has the same helper. */
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
   question these answer is whether a machine has room for a 3.5 MB library,
   and that is a question about kilobytes. */
static void say_mem(const char *when)
{

    emit("mem_free_k_%s=%ld\n", (LONG)when,
         (LONG)(AvailMem(MEMF_ANY) / 1024), 0);
    emit("mem_largest_k_%s=%ld\n", (LONG)when,
         (LONG)(AvailMem(MEMF_ANY | MEMF_LARGEST) / 1024), 0);
}


/* Ticks since midnight, at 1/50 s.  DateStamp is enough: the interval being
   measured is tens of seconds to minutes, and opening timer.device to resolve
   it further would be measuring the wrong thing. */
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


/* A library node's own version, and its own ID string.  Read off the node
   exec built when the library loaded, not off the file it loaded from. */
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
 * The measurement rests on the baseline being clean.  A library resident from
 * an earlier run -- exec keeps one loaded until it needs the memory back --
 * would make `start` include it and its cost read as nothing, which is the
 * flattering answer.  So it is turned into a reported fact: the list is walked
 * and the open counts are printed, and a reader can see whether the number
 * below is a first load or a second opener paying nothing.  The same walk runs
 * after everything closes, which is the expunge question.
 *
 * Under Forbid(), because the list is exec's.  Nothing is printed inside it:
 * dos.library I/O breaks a Forbid() and the point of the Forbid() is that the
 * list does not change while it is walked.  The answers are copied out first.
 *
 * install/test/tlsprobe.c carries this same code, deliberately: the two
 * programs have to be able to say the same thing about the same machine.
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


static ULONG str_len(const char *s)
{

ULONG   n = 0;


    while (s[n] != '\0')
    {
        n++;
    }
    return(n);
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
 * Through the LVOs, the way src/tools/fetch.c and install/test/tlsprobe.c do
 * it: nothing here links our stack, and the NDK inline headers assume a global
 * SocketBase that AmiSSL also wants a pointer to.
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


/* ------------------------------------------------------ the connections --- */

static struct Library  *SocketBase;

static SSL_CTX         *ctx;
static SSL             *ssl[2];
static LONG             sock[2];
static SSL_SESSION     *saved_session;

static char             request[256];
static UBYTE            response[RESP_MAX];


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
 * A request that leaves the connection open: HTTP/1.1 with no
 * "Connection: close", which is what tests/peer/httppeer.py's loop keeps
 * serving on.  An HTTP/1.0 request would have the peer hang up and conn1 would
 * not be live by the time conn2 is measured.  Byte for byte the request
 * install/test/tlsprobe.c sends.
 */
static BOOL build_request(const char *host, LONG port)
{

ULONG   used = 0;
char    portbuf[12];
LONG    p = port;
LONG    digits = 0;
LONG    i;


    request[0] = '\0';

    if (!str_append(request, sizeof(request), &used,
                    "GET /bytes/16 HTTP/1.1\r\nHost: "))
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


/* Read until the whole of a small response is in.  Bounded by the buffer and
   by a round count: the connection stays open, so there is no end of stream to
   wait for and a reader that waited for one would deadlock. */
static LONG read_response(SSL *s)
{

LONG    total = 0;
LONG    rounds;


    for (rounds = 0; rounds < 64; rounds++)
    {
        LONG got = (LONG)SSL_read(s, (void *)&response[total],
                                  (int)(RESP_MAX - (ULONG)total));

        if (got <= 0)
        {
            return((total > 0) ? total : -1);
        }
        total += got;

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
 *
 * The session from the first is offered to the second.  tls.library resumes by
 * itself and OpenSSL's client cache is off unless a program asks for it, so
 * without this the two libraries would be measured doing different work on
 * conn2 and the comparison would be about resumption instead of memory.
 */
static BOOL one_connection(LONG which, const char *host, LONG port,
                           ULONG addr)
{

struct ProbeSockAddr    sin;
const char             *key = (which == 0) ? "conn1" : "conn2";
const SSL_CIPHER       *cipher;
LONG                    n;
LONG                    t0;
LONG                    t1;
LONG                    verify;
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

    ssl[which] = SSL_new(ctx);
    if (ssl[which] == NULL)
    {
        say("error", "SSL_new failed");
        return(FALSE);
    }

    /* SNI and the name the certificate is checked against, which are two
       different things and both wanted.  Without the first a shared-address
       host answers with the wrong certificate. */
    (VOID)SSL_set_tlsext_host_name(ssl[which], (char *)host);
    (VOID)SSL_set1_host(ssl[which], (char *)host);

    if ((which == 1) && (saved_session != NULL))
    {
        (VOID)SSL_set_session(ssl[which], saved_session);
    }

    if (SSL_set_fd(ssl[which], (int)sock[which]) != 1)
    {
        say("error", "SSL_set_fd refused the bsdsocket descriptor");
        return(FALSE);
    }

    if (SSL_connect(ssl[which]) != 1)
    {
        emit("error=%s: SSL_connect failed (%ld)\n", (LONG)key,
             (LONG)SSL_get_error(ssl[which], -1), 0);
        return(FALSE);
    }

    n = (LONG)SSL_write(ssl[which], (const void *)request,
                        (int)str_len(request));
    if (n <= 0)
    {
        emit("error=%s: SSL_write answered %ld\n", (LONG)key, n, 0);
        return(FALSE);
    }

    n = read_response(ssl[which]);
    t1 = now_ticks();
    if (n <= 0)
    {
        emit("error=%s: SSL_read answered %ld\n", (LONG)key, n, 0);
        return(FALSE);
    }

    say_elapsed(keyed(key, "_seconds"), t0, t1);
    say_num(keyed(key, "_bytes"), n);

    say(keyed(key, "_tls_version"), (const char *)SSL_get_version(ssl[which]));
    cipher = SSL_get_current_cipher(ssl[which]);
    say(keyed(key, "_cipher"),
        (cipher != NULL) ? (const char *)SSL_CIPHER_get_name(cipher)
                         : "none");
    say(keyed(key, "_resumed"),
        (SSL_session_reused(ssl[which]) != 0) ? "yes" : "no");

    /* SSL_VERIFY_NONE is the client default, so the handshake completes and
       the verdict is read here rather than being turned into a failure the
       memory sample never gets to.  0 is X509_V_OK. */
    verify = (LONG)SSL_get_verify_result(ssl[which]);
    say(keyed(key, "_verified"), (verify == 0) ? "yes" : "no");
    say_num(keyed(key, "_verify_result"), verify);

    if (which == 0)
    {
        saved_session = SSL_get1_session(ssl[0]);
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
struct TagItem                  tags[6];
struct Process                 *self;
struct CommandLineInterface    *cli;
LONG                            stack;
LONG                t0;
LONG                t1;
const char         *version;
const char         *host = NULL;
const char         *addrtext = NULL;
const char         *cafile = NULL;
LONG                port = 0;
ULONG               addr = 0;
BOOL                measure_conns = FALSE;
LONG                i;
int                 rc = RETURN_FAIL;


    for (i = 0; i < ARG_COUNT; i++)
    {
        args[i] = 0;
    }
    sock[0] = -1;
    sock[1] = -1;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);

    out = Open((STRPTR)((args[ARG_REPORT] != 0)
                        ? (const char *)args[ARG_REPORT] : REPORT),
               MODE_NEWFILE);

    say("probe", "amissl");

    if (rda == NULL)
    {
        say("error", "usage: amisslprobe [HOST name PORT n ADDR dotted "
                     "CAFILE file] [REPORT file]");
        goto done;
    }

    host     = (const char *)args[ARG_HOST];
    addrtext = (const char *)args[ARG_ADDR];
    cafile   = (const char *)args[ARG_CAFILE];
    if (args[ARG_PORT] != 0)
    {
        port = *(LONG *)args[ARG_PORT];
    }

    measure_conns = (BOOL)((host != NULL) && (addrtext != NULL) &&
                           (cafile != NULL) && (port != 0));
    say("connections", measure_conns ? "yes" : "no");
    if (measure_conns)
    {
        say("host", host);
        say_num("port", port);
        say("address", addrtext);
        say("cafile", cafile);
    }

    /* Before anything else, because everything else depends on it.
     *
     * NOT pr_StackSize.  A Shell command does not get its own process: it
     * runs inside the Shell's, on a stack the Shell swaps in for it, and the
     * size of that one is cli_DefaultStack -- in LONGWORDS, which is the
     * field's documented and easily-missed unit.  pr_StackSize is the Shell's
     * own, it is 3,200 bytes in the web terminal, and reading it says a
     * command has too little stack when `Stack 65536` has just given it
     * plenty.  A program launched from Workbench has no CLI and pr_StackSize
     * is then the right and only answer.
     */
    self = (struct Process *)FindTask(NULL);
    cli = (struct CommandLineInterface *)BADDR(self -> pr_CLI);
    stack = (cli != NULL) ? ((LONG)cli -> cli_DefaultStack) * 4L
                          : (LONG)self -> pr_StackSize;
    say_num("stack_bytes", stack);
    if (stack < (measure_conns ? STACK_MIN_CONN : STACK_MIN))
    {
        say("error", "not enough stack: run `Stack 65536` in this Shell first");
        goto done;
    }

    if (measure_conns && !build_request(host, port))
    {
        say("error", "that host and port do not fit in a request");
        goto done;
    }

    /* Before any sample, so the reading below can be believed. */
    say_residency("before");

    say_mem("start");

    /* ------------------------------------------------------ the master -- */

    t0 = now_ticks();
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    t1 = now_ticks();
    if (AmiSSLMasterBase == NULL)
    {
        say("error", "LIBS:amisslmaster.library would not open");
        goto done;
    }
    say_elapsed("master_open_seconds", t0, t1);
    say_library("master", AmiSSLMasterBase);

    /* ---------------------------------------------- the versioned library --
     *
     * AmiSSL_InitAmiSSL FALSE splits the load from the per-process init, so
     * the two costs are reported apart.  They are paid at different times by
     * a real program: the library stays resident once loaded and the init
     * does not.
     */

    tags[0].ti_Tag = AmiSSL_UsesOpenSSLStructs; tags[0].ti_Data = (ULONG)FALSE;
    tags[1].ti_Tag = AmiSSL_InitAmiSSL;         tags[1].ti_Data = (ULONG)FALSE;
    tags[2].ti_Tag = AmiSSL_GetAmiSSLBase;      tags[2].ti_Data = (ULONG)&AmiSSLBase;
    tags[3].ti_Tag = TAG_DONE;                  tags[3].ti_Data = 0UL;

    say("opening", "LIBS:AmiSSL/amissl_v362.library");
    t0 = now_ticks();
    if (OpenAmiSSLTagList(AMISSL_CURRENT_VERSION, tags) != 0)
    {
        t1 = now_ticks();
        say_elapsed("library_open_seconds", t0, t1);
        say("error", "OpenAmiSSLTagList failed: is LIBS:AmiSSL/ populated?");
        goto done;
    }
    t1 = now_ticks();
    say_elapsed("library_open_seconds", t0, t1);
    say_library("library", AmiSSLBase);
    say_mem("library_open");

    /* ------------------------------------------------- the socket stack -- *
     *
     * Only in the argument form, and BEFORE the init below rather than after
     * it, because AmiSSL_SocketBase is an init tag: there is no way to hand
     * AmiSSL a socket base once InitAmiSSLA has run.  So it gets its own
     * sample and `init` stays a delta on the phase immediately before it.
     * install/test/tlsprobe.c opens bsdsocket.library at the same point.
     */

    if (measure_conns)
    {
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
    }

    /* -------------------------------------------------- per-process init -- */

    tags[0].ti_Tag = AmiSSL_GetAmiSSLBase;    tags[0].ti_Data = (ULONG)&AmiSSLBase;
    tags[1].ti_Tag = AmiSSL_GetAmiSSLExtBase; tags[1].ti_Data = (ULONG)&AmiSSLExtBase;
    tags[2].ti_Tag = AmiSSL_ErrNoPtr;         tags[2].ti_Data = (ULONG)&probe_errno;
    if (measure_conns)
    {
        tags[3].ti_Tag = AmiSSL_SocketBase;   tags[3].ti_Data = (ULONG)SocketBase;
        tags[4].ti_Tag = TAG_DONE;            tags[4].ti_Data = 0UL;
    }
    else
    {
        tags[3].ti_Tag = TAG_DONE;            tags[3].ti_Data = 0UL;
    }

    t0 = now_ticks();
    if (InitAmiSSLA(tags) != 0)
    {
        t1 = now_ticks();
        say_elapsed("init_seconds", t0, t1);
        say("error", "InitAmiSSLA failed");
        goto done;
    }
    t1 = now_ticks();
    say_elapsed("init_seconds", t0, t1);
    say_mem("init");

    /* ------------------------------------------------ the library speaks -- */

    version = (const char *)OpenSSL_version(OPENSSL_VERSION);
    say("openssl_version", (version != NULL) ? version : "none");

    /* --------------------------------------------------- the two rounds -- */

    if (measure_conns)
    {
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == NULL)
        {
            say("error", "SSL_CTX_new failed");
            goto done;
        }

        /* Pinned to 1.2 at both ends.  tests/peer/httppeer.py caps itself
           there unless AMINETXDUO_PEER_TLS13 is set, and tls.library speaks
           1.2, so a comparison that let AmiSSL negotiate 1.3 would be
           comparing two different protocols. */
        (VOID)SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        (VOID)SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);

        if (SSL_CTX_load_verify_locations(ctx, cafile, NULL) != 1)
        {
            say("error", "SSL_CTX_load_verify_locations would not read CAFILE");
            goto done;
        }

        if (!one_connection(0, host, port, addr))
        {
            goto done;
        }
        if (!one_connection(1, host, port, addr))
        {
            goto done;
        }
    }

    rc = RETURN_OK;

done:
    /* The connections, then the socket under each.  SSL_free does not close
       the descriptor it was given, so this closes it. */
    for (i = 1; i >= 0; i--)
    {
        if (ssl[i] != NULL)
        {
            (VOID)SSL_shutdown(ssl[i]);
            SSL_free(ssl[i]);
            ssl[i] = NULL;
        }
        if (sock[i] >= 0)
        {
            (VOID)sock_close(SocketBase, sock[i]);
            sock[i] = -1;
        }
    }
    if (saved_session != NULL)
    {
        SSL_SESSION_free(saved_session);
        saved_session = NULL;
    }
    if (ctx != NULL)
    {
        SSL_CTX_free(ctx);
        ctx = NULL;
    }

    if (AmiSSLBase != NULL)
    {
        CloseAmiSSL();
        AmiSSLBase    = NULL;
        AmiSSLExtBase = NULL;
    }
    if (AmiSSLMasterBase != NULL)
    {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
    if (SocketBase != NULL)
    {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }

    /* After the close, so the reading says whether the 3.5 MB came back.  It
       does not have to: exec expunges a library only when it needs the
       memory, so a machine with room keeps it loaded and the next opener
       pays nothing.  The residency walk beside it says which of those
       happened, which a free-memory figure on its own cannot. */
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
