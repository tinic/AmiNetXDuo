/*
 * TlsLoop, tls.library shaking hands with itself.
 *
 * The server half of tls.library -- TLSA_Server, TLSA_Certificate and
 * TLSA_PrivateKey, and the branch _nx_secure_tls_session_start() takes when
 * the transport says "server" -- had never been run.  nx_secure's server state
 * machine had (tests/tls/tls_handshake.c drives it directly, over the in-tree
 * RAM driver); the DOOR to it, src/tlslib/tls_server.c and the server half of
 * tls_conn.c, had not.  This is the program that opens it: one binary, two
 * modes, two Shell processes in one guest, talking TLS to each other over
 * 127.0.0.1.
 *
 * TWO PROCESSES AND NOT TWO THREADS: TLSOpen() blocks for the whole handshake,
 * so one task cannot be both ends of it, and bsdsocket.library hands each
 * process its own descriptor table in any case.  tests/tls/run-tlsloop.sh
 * starts SERVER with ToolsSmoke's `&` and then runs CLIENT.
 *
 *   TlsLoop SERVER PORT 7443 CERT <der> KEY <der> [KEYTYPE RSA|EC] [HTTP]
 *   TlsLoop CLIENT PORT 7443 HOST <name> [STORE <path>] [NOVERIFY]
 *
 * HTTP makes the server answer one GET with an HTTP/1.1 response instead of
 * the fixed word below, so the client can be the SHIPPED `fetch` command
 * rather than this program's other half.  tests/tls/run-fetchtls.sh is that
 * arm: the whole point of it is that nothing but our own code is in the path,
 * so a red is a defect here and never a third party's outage.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include <proto/dos.h>

#include <stdarg.h>

#include "aminetxduo/tlslib.h"


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define L_LOG_SIZE  6144

static char  l_log_buffer[L_LOG_SIZE];
static ULONG l_log_used;

static VOID l_put(UBYTE c)
{
    RawPutChar(c);

    if (l_log_used < (ULONG)(L_LOG_SIZE - 1))
        l_log_buffer[l_log_used++] = (char)c;
}

static VOID l_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
        l_put(c);
}

static VOID l_log(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) l_put_char, NULL);
    va_end(args);

    l_put('\n');
}

static VOID l_flush(VOID)
{
    BPTR out = Output();

    if (out != (BPTR)0)
        (VOID)Write(out, (APTR)l_log_buffer, (LONG)l_log_used);
}

static ULONG l_checks;
static ULONG l_failures;

/* One line per check, "<name>=ok" or "<name>=fail:<detail>", so a harness
   greps for a key and never for prose. */
static BOOL l_check(BOOL ok, const char *what, ULONG detail)
{
    l_checks++;
    if (!ok)
    {
        l_failures++;
        l_log("%s=fail:%lu", (LONG)what, detail);
        return FALSE;
    }

    l_log("%s=ok", (LONG)what);
    return TRUE;
}


/* ------------------------------------------- bsdsocket.library, by hand --- */

/*
 * Declared here rather than pulled from <proto/bsdsocket.h> for the reason
 * tests/tls/tls_api.c gives: the inline header assumes one global SocketBase,
 * and the published ABI is part of what these tests exist to hold still.
 * Offsets from the Roadshow NDK pragmas.
 */
struct l_sockaddr_in
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

struct l_timeval
{
    LONG    tv_secs;
    LONG    tv_micro;
};

#define L_AF_INET       2
#define L_SOCK_STREAM   1
#define L_LOOPBACK      0x7F000001UL

static LONG bsd_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = domain;
    register LONG            d1 __asm("d1") = type;
    register LONG            d2 __asm("d2") = proto;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG bsd_bind(struct Library *base, LONG sock, APTR name, LONG len)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = sock;
    register APTR            a0 __asm("a0") = name;
    register LONG            d1 __asm("d1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-36:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG bsd_listen(struct Library *base, LONG sock, LONG backlog)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = sock;
    register LONG            d1 __asm("d1") = backlog;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-42:W)"
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG bsd_accept(struct Library *base, LONG sock, APTR addr, APTR len)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = sock;
    register APTR            a0 __asm("a0") = addr;
    register APTR            a1 __asm("a1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0),
                        "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1)
                      : "cc", "memory");
    return res;
}

static LONG bsd_connect(struct Library *base, LONG sock, APTR name, LONG len)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = sock;
    register APTR            a0 __asm("a0") = name;
    register LONG            d1 __asm("d1") = len;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
                      : "a1", "cc", "memory");
    return res;
}

static LONG bsd_setsockopt(struct Library *base, LONG sock, LONG level,
                           LONG optname, APTR optval, LONG optlen)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = sock;
    register LONG            d1 __asm("d1") = level;
    register LONG            d2 __asm("d2") = optname;
    register APTR            a0 __asm("a0") = optval;
    register LONG            d3 __asm("d3") = optlen;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-90:W)"
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2), "r" (a0),
                        "r" (d3)
                      : "a1", "cc", "memory");
    return res;
}

static LONG bsd_close_socket(struct Library *base, LONG sock)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = sock;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-120:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static LONG bsd_wait_select(struct Library *base, LONG nfds, APTR readfds,
                            APTR writefds, APTR exceptfds, APTR timeout)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = nfds;
    register APTR            a0 __asm("a0") = readfds;
    register APTR            a1 __asm("a1") = writefds;
    register APTR            a2 __asm("a2") = exceptfds;
    register APTR            a3 __asm("a3") = timeout;
    register ULONG          *d1 __asm("d1") = NULL;
    register LONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");
    register LONG _clob_a1 __asm("a1");

    __asm __volatile ("jsr a6@(-126:W)"
                      : "=r" (res), "=r" (_clob_a0), "=r" (_clob_a1)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (a1), "r" (a2),
                        "r" (a3), "r" (d1)
                      : "cc", "memory");
    return res;
}

static LONG bsd_errno(struct Library *base)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* SOL_SOCKET / SO_REUSEADDR, 4.2BSD numbers, unchanged everywhere. */
#define L_SOL_SOCKET    0xFFFF
#define L_SO_REUSEADDR  0x0004


/* ----------------------------------------------------------- the words --- */

/* What the client sends and what the server answers.  Both are checked at the
   far end, so a handshake that completes but moves the wrong bytes fails. */
static const char l_request[]  = "AmiNetXDuo TlsLoop request\n";
static const char l_response[] = "AmiNetXDuo TlsLoop response\n";

/* What the HTTP arm answers.  Content-Length is a literal, so the two are
   held together by the size check below rather than by anyone's attention. */
static const char l_http_body[]  = "AmiNetXDuo fetch over TLS\n";
static const char l_http_reply[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 26\r\n"
    "Connection: close\r\n"
    "\r\n"
    "AmiNetXDuo fetch over TLS\n";

typedef char l_http_length_agrees[(sizeof(l_http_body) - 1 == 26) ? 1 : -1];

static UBYTE l_buffer[1024];

/* fd_set, open-coded. */
static ULONG l_readfds[8];

static BOOL l_same(const UBYTE *got, LONG got_len, const char *want)
{
    LONG i;

    for (i = 0; want[i] != '\0'; i++)
    {
        if (i >= got_len || got[i] != (UBYTE)want[i])
            return FALSE;
    }

    return (BOOL)(i == got_len);
}

/* Read exactly `want` bytes, or fail.  TLSRead() answers a record at a time
   and a short read is not an error. */
static LONG l_read_all(struct Library *tbase, struct TLSConnection *tls,
                       UBYTE *buffer, LONG want)
{
    LONG got = 0;

    while (got < want)
    {
        LONG n = TLSRead(tbase, tls, (APTR)&buffer[got], want - got);

        if (n <= 0)
            return (got > 0) ? got : n;

        got += n;
    }

    return got;
}

static VOID l_report_info(struct Library *tbase, struct TLSConnection *tls,
                          const char *side)
{
    struct TLSInfo info;
    ULONG          i;
    UBYTE         *p = (UBYTE *)&info;

    for (i = 0; i < (ULONG)sizeof(info); i++)
        p[i] = 0;

    info.ti_Size = (ULONG)sizeof(info);

    if (TLSInfo(tbase, tls, &info) != TLS_OK)
    {
        l_log("%s_info=fail", (LONG)side);
        return;
    }

    l_log("%s_version=0x%lx", (LONG)side, info.ti_Version);
    l_log("%s_suite=0x%lx", (LONG)side, info.ti_CipherSuite);
    l_log("%s_handshake_ms=%lu", (LONG)side, info.ti_HandshakeMillis);
    l_log("%s_verified=%lu", (LONG)side, (ULONG)info.ti_Verified);
    l_log("%s_chain_depth=%lu", (LONG)side, info.ti_ChainDepth);
}


/* ------------------------------------------------------------- the arms --- */

/*
 * Up to the blank line, and not l_read_all(): a TLS record boundary is not a
 * message boundary, so a request that fits in one write() can still arrive in
 * two records, and its length is not known in advance.  Anything past the
 * head is left in the session; this arm answers one GET and closes.
 */
static LONG l_read_head(struct Library *tbase, struct TLSConnection *tls,
                        UBYTE *buffer, LONG size)
{
    LONG got = 0;

    while (got < size)
    {
        LONG n = TLSRead(tbase, tls, (APTR)&buffer[got], size - got);

        if (n <= 0)
            return (got > 0) ? got : n;

        got += n;

        if (got >= 4 && buffer[got - 4] == '\r' && buffer[got - 3] == '\n' &&
            buffer[got - 2] == '\r' && buffer[got - 1] == '\n')
            return got;
    }

    return got;
}

static VOID l_server(struct Library *sbase, struct Library *tbase, LONG port,
                     const char *cert, const char *key, ULONG key_type,
                     BOOL http, LONG accept_secs)
{
    struct l_sockaddr_in  sa;
    struct l_timeval      tv;
    struct TLSConnection *tls;
    LONG                  listener;
    LONG                  fd;
    LONG                  one = 1;
    LONG                  rc;
    LONG                  n;
    LONG                  error = 0;
    ULONG                 i;

    listener = bsd_socket(sbase, L_AF_INET, L_SOCK_STREAM, 0);
    if (!l_check((BOOL)(listener >= 0), "server_socket",
                 (ULONG)bsd_errno(sbase)))
        return;

    (VOID)bsd_setsockopt(sbase, listener, L_SOL_SOCKET, L_SO_REUSEADDR,
                         (APTR)&one, (LONG)sizeof(one));

    for (i = 0; i < (ULONG)sizeof(sa); i++)
        ((UBYTE *)&sa)[i] = 0;

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = L_AF_INET;
    sa.sin_port   = (UWORD)port;
    sa.sin_addr   = L_LOOPBACK;

    rc = bsd_bind(sbase, listener, (APTR)&sa, (LONG)sizeof(sa));
    if (!l_check((BOOL)(rc == 0), "server_bind", (ULONG)bsd_errno(sbase)))
    {
        (VOID)bsd_close_socket(sbase, listener);
        return;
    }

    rc = bsd_listen(sbase, listener, 1);
    if (!l_check((BOOL)(rc == 0), "server_listen", (ULONG)bsd_errno(sbase)))
    {
        (VOID)bsd_close_socket(sbase, listener);
        return;
    }

    /*
     * WaitSelect() and not a bare accept(): a client that never arrives would
     * otherwise hang this process until the emulator's own timeout, and a
     * timeout tells a reader nothing about which half failed.
     */
    for (i = 0; i < 8; i++)
        l_readfds[i] = 0;
    l_readfds[(ULONG)listener / 32UL] |= 1UL << ((ULONG)listener % 32UL);

    tv.tv_secs  = accept_secs;
    tv.tv_micro = 0;

    rc = bsd_wait_select(sbase, listener + 1, (APTR)l_readfds, NULL, NULL,
                         (APTR)&tv);
    if (!l_check((BOOL)(rc == 1), "server_client_arrived", (ULONG)rc))
    {
        (VOID)bsd_close_socket(sbase, listener);
        return;
    }

    fd = bsd_accept(sbase, listener, NULL, NULL);
    if (!l_check((BOOL)(fd >= 0), "server_accept", (ULONG)bsd_errno(sbase)))
    {
        (VOID)bsd_close_socket(sbase, listener);
        return;
    }

    /* The listener has done its work; the handshake is on `fd`. */
    (VOID)bsd_close_socket(sbase, listener);

    tls = TLSOpen(tbase, (APTR)sbase, fd,
                  TLSA_Server,      (ULONG)TRUE,
                  TLSA_Certificate, (ULONG)cert,
                  TLSA_PrivateKey,  (ULONG)key,
                  TLSA_KeyType,     key_type,
                  TLSA_Timeout,     240000UL,
                  TLSA_Error,       (ULONG)&error,
                  TAG_DONE);

    if (!l_check((BOOL)(tls != NULL), "server_handshake", (ULONG)error))
    {
        (VOID)bsd_close_socket(sbase, fd);
        return;
    }

    l_report_info(tbase, tls, "server");

    if (http)
    {
        n = l_read_head(tbase, tls, l_buffer, (LONG)sizeof(l_buffer));
        (VOID)l_check((BOOL)(n > 0), "server_read", (ULONG)n);
        (VOID)l_check((BOOL)(n >= 4 && l_buffer[0] == 'G' &&
                             l_buffer[1] == 'E' && l_buffer[2] == 'T' &&
                             l_buffer[3] == ' '),
                      "server_read_matches", (ULONG)n);

        n = TLSWrite(tbase, tls, (CONST_APTR)l_http_reply,
                     (LONG)sizeof(l_http_reply) - 1);
        (VOID)l_check((BOOL)(n == (LONG)sizeof(l_http_reply) - 1),
                      "server_write", (ULONG)n);
    }
    else
    {
        n = l_read_all(tbase, tls, l_buffer, (LONG)sizeof(l_request) - 1);
        (VOID)l_check((BOOL)(n == (LONG)sizeof(l_request) - 1), "server_read",
                      (ULONG)n);
        (VOID)l_check(l_same(l_buffer, n, l_request), "server_read_matches",
                      (ULONG)n);

        n = TLSWrite(tbase, tls, (CONST_APTR)l_response,
                     (LONG)sizeof(l_response) - 1);
        (VOID)l_check((BOOL)(n == (LONG)sizeof(l_response) - 1), "server_write",
                      (ULONG)n);
    }

    TLSClose(tbase, tls);
    (VOID)bsd_close_socket(sbase, fd);

    (VOID)l_check(TRUE, "server_closed", 0);
}

static VOID l_client(struct Library *sbase, struct Library *tbase, LONG port,
                     const char *host, const char *store, BOOL noverify,
                     LONG connect_secs)
{
    struct l_sockaddr_in  sa;
    struct TLSConnection *tls;
    LONG                  fd = -1;
    LONG                  rc = -1;
    LONG                  n;
    LONG                  error = 0;
    LONG                  tries;
    ULONG                 i;

    for (i = 0; i < (ULONG)sizeof(sa); i++)
        ((UBYTE *)&sa)[i] = 0;

    sa.sin_len    = (UBYTE)sizeof(sa);
    sa.sin_family = L_AF_INET;
    sa.sin_port   = (UWORD)port;
    sa.sin_addr   = L_LOOPBACK;

    /*
     * The server is a separate Shell process started moments ago, so a first
     * connect() can beat its listen().  Half-second retries rather than one
     * long sleep: the common case then costs nothing.
     */
    for (tries = 0; tries < connect_secs * 2; tries++)
    {
        fd = bsd_socket(sbase, L_AF_INET, L_SOCK_STREAM, 0);
        if (fd < 0)
            break;

        rc = bsd_connect(sbase, fd, (APTR)&sa, (LONG)sizeof(sa));
        if (rc == 0)
            break;

        (VOID)bsd_close_socket(sbase, fd);
        fd = -1;
        Delay(25);
    }

    if (!l_check((BOOL)(fd >= 0), "client_socket", (ULONG)bsd_errno(sbase)))
        return;

    if (!l_check((BOOL)(rc == 0), "client_connect", (ULONG)bsd_errno(sbase)))
    {
        (VOID)bsd_close_socket(sbase, fd);
        return;
    }

    if (noverify)
    {
        tls = TLSOpen(tbase, (APTR)sbase, fd,
                      TLSA_NoVerify, (ULONG)TRUE,
                      TLSA_NoResume, (ULONG)TRUE,
                      TLSA_Timeout,  240000UL,
                      TLSA_Error,    (ULONG)&error,
                      TAG_DONE);
    }
    else
    {
        tls = TLSOpen(tbase, (APTR)sbase, fd,
                      TLSA_HostName,   (ULONG)host,
                      TLSA_TrustStore, (ULONG)store,
                      TLSA_NoResume,   (ULONG)TRUE,
                      TLSA_Timeout,    240000UL,
                      TLSA_Error,      (ULONG)&error,
                      TAG_DONE);
    }

    if (!l_check((BOOL)(tls != NULL), "client_handshake", (ULONG)error))
    {
        (VOID)bsd_close_socket(sbase, fd);
        return;
    }

    l_report_info(tbase, tls, "client");

    n = TLSWrite(tbase, tls, (CONST_APTR)l_request, (LONG)sizeof(l_request) - 1);
    (VOID)l_check((BOOL)(n == (LONG)sizeof(l_request) - 1), "client_write",
                  (ULONG)n);

    n = l_read_all(tbase, tls, l_buffer, (LONG)sizeof(l_response) - 1);
    (VOID)l_check((BOOL)(n == (LONG)sizeof(l_response) - 1), "client_read",
                  (ULONG)n);
    (VOID)l_check(l_same(l_buffer, n, l_response), "client_read_matches",
                  (ULONG)n);

    TLSClose(tbase, tls);
    (VOID)bsd_close_socket(sbase, fd);

    (VOID)l_check(TRUE, "client_closed", 0);
}


/* --------------------------------------------------------------- entry --- */

#define L_TEMPLATE  "MODE/A,PORT/K/N,CERT/K,KEY/K,KEYTYPE/K,HOST/K,STORE/K," \
                    "NOVERIFY/S,HTTP/S,WAIT/K/N"

enum
{
    L_ARG_MODE = 0,
    L_ARG_PORT,
    L_ARG_CERT,
    L_ARG_KEY,
    L_ARG_KEYTYPE,
    L_ARG_HOST,
    L_ARG_STORE,
    L_ARG_NOVERIFY,
    L_ARG_HTTP,
    L_ARG_WAIT,
    L_ARG_COUNT
};

static LONG l_args[L_ARG_COUNT];

static BOOL l_is(const char *text, const char *want)
{
    ULONG i;

    if (text == NULL)
        return FALSE;

    for (i = 0; want[i] != '\0'; i++)
    {
        char c = text[i];

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');

        if (c != want[i])
            return FALSE;
    }

    return (BOOL)(text[i] == '\0');
}

#define L_STACK_SIZE    (48UL * 1024UL)

static struct StackSwapStruct l_sss;
static int                    l_result;

static int l_run(VOID);

static __attribute__((noinline)) VOID l_trampoline(VOID)
{
    StackSwap(&l_sss);
    l_result = l_run();
    StackSwap(&l_sss);
}

int main(VOID)
{
    APTR stack = AllocMem(L_STACK_SIZE, MEMF_ANY);

    if (stack != NULL)
    {
        l_sss.stk_Lower   = stack;
        l_sss.stk_Upper   = (ULONG)stack + L_STACK_SIZE;
        l_sss.stk_Pointer = (APTR)((ULONG)stack + L_STACK_SIZE);

        l_trampoline();

        FreeMem(stack, L_STACK_SIZE);
    }
    else
    {
        l_result = l_run();
    }

    return l_result;
}

static int l_run(VOID)
{
    struct RDArgs  *rdargs;
    struct Library *sbase;
    struct Library *tbase;
    const char     *mode;
    LONG            port = 7443;
    LONG            wait_secs = 90;
    ULONG           key_type = TLS_KEY_RSA;
    BOOL            server_mode;

    /* A guest program started from a Shell sees argc == 1, so the command line
       comes from ReadArgs() and never from argv. */
    rdargs = ReadArgs((CONST_STRPTR)L_TEMPLATE, l_args, NULL);
    if (rdargs == NULL)
    {
        l_log("tlsloop=fail:badargs:%lu", (ULONG)IoErr());
        l_flush();
        return RETURN_FAIL;
    }

    mode = (const char *)l_args[L_ARG_MODE];

    if (l_args[L_ARG_PORT] != 0)
        port = *(LONG *)l_args[L_ARG_PORT];
    if (l_args[L_ARG_WAIT] != 0)
        wait_secs = *(LONG *)l_args[L_ARG_WAIT];
    if (l_is((const char *)l_args[L_ARG_KEYTYPE], "EC"))
        key_type = TLS_KEY_EC;

    server_mode = l_is(mode, "SERVER");

    if (!server_mode && !l_is(mode, "CLIENT"))
    {
        l_log("tlsloop=fail:mode");
        FreeArgs(rdargs);
        l_flush();
        return RETURN_FAIL;
    }

    l_log("tlsloop_mode=%s", (LONG)(server_mode ? "server" : "client"));
    l_log("tlsloop_port=%lu", (ULONG)port);

    sbase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (!l_check((BOOL)(sbase != NULL), "open_bsdsocket", 0))
        goto done;

    /* The jump table this library gates on: TLSOpen() refuses a base whose
       lib_NegSize does not reach Errno() at -0x0a2, and a run on a stack
       that is not ours wants the number in the transcript. */
    l_log("bsdsocket_negsize=%lu", (ULONG)((struct Library *)sbase)->lib_NegSize);

    tbase = OpenLibrary((CONST_STRPTR)TLS_LIB_NAME, (ULONG)TLS_LIB_VERSION);
    if (!l_check((BOOL)(tbase != NULL), "open_tls", 0))
    {
        CloseLibrary(sbase);
        goto done;
    }

    if (server_mode)
    {
        if (l_args[L_ARG_CERT] == 0 || l_args[L_ARG_KEY] == 0)
        {
            (VOID)l_check(FALSE, "server_identity_given", 0);
        }
        else
        {
            l_server(sbase, tbase, port,
                     (const char *)l_args[L_ARG_CERT],
                     (const char *)l_args[L_ARG_KEY],
                     key_type, (BOOL)(l_args[L_ARG_HTTP] != 0), wait_secs);
        }
    }
    else
    {
        const char *store = (const char *)l_args[L_ARG_STORE];
        const char *host  = (const char *)l_args[L_ARG_HOST];

        l_client(sbase, tbase, port, host, store,
                 (BOOL)(l_args[L_ARG_NOVERIFY] != 0), wait_secs);
    }

    CloseLibrary(tbase);
    CloseLibrary(sbase);

done:
    FreeArgs(rdargs);

    l_log("tlsloop_checks=%lu", l_checks);
    l_log("tlsloop_failures=%lu", l_failures);
    l_flush();

    return (l_failures == 0 && l_checks > 0) ? RETURN_OK : RETURN_FAIL;
}
