/*
 * AmiNetXDuo -- fetching a real HTTPS URL through tls.library, and nothing else.
 *
 * WHAT MAKES THIS DIFFERENT FROM tests/tls/tls_https.c
 *
 *   tls_https is linked against our whole stack: it calls netstack_startup(),
 *   nx_tcp_socket_create(), nx_secure_tls_session_create(), and it verifies the
 *   chain against ONE root CA compiled into the test.  It proves the crypto
 *   works.  It does not prove anybody could use it.
 *
 *   This program is linked against NOTHING of ours.  It opens two shared
 *   libraries by name, calls their published vectors, and verifies the chain
 *   against DEVS:Internet/certificates -- 119 Mozilla roots on disk, of which
 *   it parses the one the chain actually needs.  It is the program an ordinary
 *   Amiga application would be.
 *
 *   Everything below the TLSOpen() call is therefore evidence about the
 *   design, not about the arithmetic: that a separate library can borrow
 *   bsdsocket.library's NetX Duo through one private vector, that a trust
 *   store on disk can be searched lazily by issuer, and that
 *   WaitSelect()-shaped code does not hang when the plaintext is already
 *   decrypted.
 *
 * STAGING
 *
 *   LIBS:bsdsocket.library, LIBS:tls.library, DEVS:Internet/certificates,
 *   DEVS:NetInterfaces/eth0 and DEVS:a2065.device -- see tests/tls/run-api.sh.
 *
 * NOT A BASELINE.  It depends on the internet, on FS-UAE's SLIRP NAT, on a
 * third party's server, and on a certificate that rotates every ninety days.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>

#include "aminetxduo/tlslib.h"


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define A_LOG_SIZE  8192

static char  a_log_buffer[A_LOG_SIZE];
static ULONG a_log_used;

static VOID a_put(UBYTE c)
{
    RawPutChar(c);

    if (a_log_used < (ULONG)(A_LOG_SIZE - 1))
        a_log_buffer[a_log_used++] = (char)c;
}

static VOID a_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
        a_put(c);
}

static VOID a_log(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) a_put_char, NULL);
    va_end(args);

    a_put('\n');
}

static VOID a_flush(VOID)
{
    BPTR out = Output();

    if (out != (BPTR)0)
        (VOID)Write(out, (APTR)a_log_buffer, (LONG)a_log_used);
}

static ULONG a_checks;
static ULONG a_failures;

static BOOL a_check(BOOL ok, const char *what, ULONG detail)
{
    a_checks++;
    if (!ok)
    {
        a_failures++;
        a_log("  FAIL %s (0x%lx)", (LONG)what, detail);
        return FALSE;
    }

    a_log("  ok   %s", (LONG)what);
    return TRUE;
}


/* ------------------------------------------- bsdsocket.library, by hand --- */

/*
 * Declared here rather than pulled from <proto/bsdsocket.h> for the same
 * reason tests/libraries/library_test.c does it: the point is to exercise the
 * published ABI, and an inline header that assumes a global SocketBase hides
 * exactly the thing under test.  Offsets from the Roadshow NDK pragmas.
 */
struct t_sockaddr_in
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

struct t_hostent
{
    char   *h_name;
    char  **h_aliases;
    LONG    h_addrtype;
    LONG    h_length;
    char  **h_addr_list;
};

#define T_AF_INET       2
#define T_SOCK_STREAM   1

static LONG bsd_socket(struct Library *base, LONG domain, LONG type, LONG proto)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = domain;
    register LONG            d1 __asm("d1") = type;
    register LONG            d2 __asm("d2") = proto;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (d1), "r" (d2)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG bsd_connect(struct Library *base, LONG sock, APTR name, LONG len)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = sock;
    register APTR            a0 __asm("a0") = name;
    register LONG            d1 __asm("d1") = len;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0), "r" (a0), "r" (d1)
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

static struct t_hostent *bsd_gethostbyname(struct Library *base, const char *name)
{
    register struct Library   *a6 __asm("a6") = base;
    register const char       *a0 __asm("a0") = name;
    register struct t_hostent *res __asm("d0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
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


/* ------------------------------------------------------------- the run --- */

#define A_HOST      "tls-v1-2.badssl.com"
#define A_PORT      443
#define A_WRONGNAME "example.invalid"

static const char a_request[] =
    "GET / HTTP/1.0\r\n"
    "Host: " A_HOST "\r\n"
    "Connection: close\r\n"
    "User-Agent: AmiNetXDuo-tls.library/1 (m68k)\r\n"
    "\r\n";

static UBYTE a_body[2048];

/* fd_set, open-coded -- see the note in src/tlslib/tls_conn.c. */
static ULONG a_readfds[8];

struct t_timeval
{
    LONG    tv_secs;
    LONG    tv_micro;
};

/* -------------------------------------------------- the machine's clock --- */

/*
 * Enough of timer.device to move the system clock and put it back.  Only
 * exec.library calls are used (DoIO on a timerequest), so there is no
 * TimerBase to conflict with anything.
 */
static struct MsgPort     *a_clock_port;
static struct timerequest *a_clock_req;
static struct timeval      a_clock_saved;

static BOOL a_clock_open(VOID)
{
    a_clock_port = CreateMsgPort();
    if (a_clock_port == NULL)
        return FALSE;

    a_clock_req = (struct timerequest *)
                  CreateIORequest(a_clock_port, (ULONG)sizeof(*a_clock_req));
    if (a_clock_req == NULL)
    {
        DeleteMsgPort(a_clock_port);
        a_clock_port = NULL;
        return FALSE;
    }

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)a_clock_req, 0) != 0)
    {
        DeleteIORequest((struct IORequest *)a_clock_req);
        DeleteMsgPort(a_clock_port);
        a_clock_req  = NULL;
        a_clock_port = NULL;
        return FALSE;
    }

    a_clock_req->tr_node.io_Command = TR_GETSYSTIME;
    (VOID)DoIO((struct IORequest *)a_clock_req);
    a_clock_saved = a_clock_req->tr_time;

    return TRUE;
}

static VOID a_clock_set(ULONG secs, ULONG micro)
{
    if (a_clock_req == NULL)
        return;

    a_clock_req->tr_node.io_Command = TR_SETSYSTIME;
    a_clock_req->tr_time.tv_secs    = secs;
    a_clock_req->tr_time.tv_micro   = micro;
    (VOID)DoIO((struct IORequest *)a_clock_req);
}

static VOID a_clock_restore(VOID)
{
    if (a_clock_req == NULL)
        return;

    a_clock_req->tr_node.io_Command = TR_SETSYSTIME;
    a_clock_req->tr_time            = a_clock_saved;
    (VOID)DoIO((struct IORequest *)a_clock_req);
}

static VOID a_clock_close(VOID)
{
    if (a_clock_req == NULL)
        return;

    CloseDevice((struct IORequest *)a_clock_req);
    DeleteIORequest((struct IORequest *)a_clock_req);
    DeleteMsgPort(a_clock_port);
    a_clock_req  = NULL;
    a_clock_port = NULL;
}

static LONG a_connect(struct Library *sbase, ULONG address)
{
    struct t_sockaddr_in sin;
    LONG                 sock;
    ULONG                i;

    for (i = 0; i < sizeof(sin.sin_zero); i++)
        sin.sin_zero[i] = 0;

    sin.sin_len    = (UBYTE)sizeof(sin);
    sin.sin_family = (UBYTE)T_AF_INET;
    sin.sin_port   = (UWORD)A_PORT;
    sin.sin_addr   = address;

    sock = bsd_socket(sbase, T_AF_INET, T_SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    if (bsd_connect(sbase, sock, (APTR)&sin, (LONG)sizeof(sin)) != 0)
    {
        (VOID)bsd_close_socket(sbase, sock);
        return -1;
    }

    return sock;
}


int main(VOID)
{
    struct Library        *sbase;
    struct Library        *tbase;
    struct t_hostent      *he;
    struct TLSConnection  *tls;
    struct TLSConnection  *conns[2];
    struct TLSInfo         info;
    struct TLSSelect       sel;
    struct t_timeval       poll;
    ULONG                  address = 0;
    ULONG                  total = 0;
    LONG                   sock;
    LONG                   why = 0;
    LONG                   n;
    LONG                   rc;
    ULONG                  i;
    BOOL                   eol;
    char                   line[96];

    a_log("AmiNetXDuo -- HTTPS through tls.library, over the published ABI");

    /* ---- the two libraries ---------------------------------------------- */

    sbase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (!a_check((BOOL)(sbase != NULL), "OpenLibrary(bsdsocket.library)", 0))
        goto done;

    tbase = OpenLibrary((CONST_STRPTR)TLS_LIB_NAME, (ULONG)TLS_LIB_VERSION);
    if (!a_check((BOOL)(tbase != NULL), "OpenLibrary(tls.library)", 0))
    {
        CloseLibrary(sbase);
        goto done;
    }

    a_log("  %s %ld.%ld", (LONG)TLS_LIB_NAME,
          (ULONG)tbase->lib_Version, (ULONG)tbase->lib_Revision);

    /* ---- DNS ------------------------------------------------------------ */

    he = bsd_gethostbyname(sbase, A_HOST);
    if (!a_check((BOOL)(he != NULL && he->h_addr_list != NULL &&
                        he->h_addr_list[0] != NULL),
                 "gethostbyname(" A_HOST ")", 0))
    {
        goto close_libs;
    }

    for (i = 0; i < 4; i++)
        address = (address << 8) | (ULONG)(UBYTE)he->h_addr_list[0][i];

    a_log("  %s is %lu.%lu.%lu.%lu", (LONG)A_HOST,
          (address >> 24) & 0xFF, (address >> 16) & 0xFF,
          (address >> 8) & 0xFF, address & 0xFF);

    /* ---- the wrong name must be refused --------------------------------- */

    /*
     * Before the positive test, because a passing positive test proves nothing
     * about verification unless the negative one also passes: a library that
     * accepts every certificate passes the fetch and fails here.
     */
    sock = a_connect(sbase, address);
    if (a_check((BOOL)(sock >= 0), "connect for the negative test",
                (ULONG)bsd_errno(sbase)))
    {
        why = TLS_OK;
        tls = TLSOpen(tbase, (APTR)sbase, sock,
                      TLSA_HostName, (ULONG)A_WRONGNAME,
                      TLSA_Error,    (ULONG)&why);

        (VOID)a_check((BOOL)(tls == NULL),
                      "a certificate for another host is REFUSED", 0);

        /*
         * WHICH refusal is the server's business, not ours.  Asked for a name
         * it does not serve, this host answers with whatever its default vhost
         * has -- and badssl.com's exists to be broken in assorted ways, so the
         * observed reason has been "issued to another host" and "expired" on
         * different days.  Pinning the test to one of them would make it fail
         * when somebody else changes their configuration.  What must hold is
         * that the refusal is a VERIFICATION refusal and not, say, a timeout.
         */
        (VOID)a_check((BOOL)(why == TLS_ERR_HOSTNAME ||
                             why == TLS_ERR_HANDSHAKE ||
                             why == TLS_ERR_EXPIRED ||
                             why == TLS_ERR_UNTRUSTED),
                      "and for a certificate-verification reason", (ULONG)why);
        a_log("  refused with: %s", (LONG)TLSErrorString(tbase, why));

        if (tls != NULL)
            TLSClose(tbase, tls);
        (VOID)bsd_close_socket(sbase, sock);
    }

    /* ---- and no trust store is a legible failure, not a mystery ---------- */

    sock = a_connect(sbase, address);
    if (a_check((BOOL)(sock >= 0), "connect for the missing-store test",
                (ULONG)bsd_errno(sbase)))
    {
        why = TLS_OK;
        tls = TLSOpen(tbase, (APTR)sbase, sock,
                      TLSA_HostName,   (ULONG)A_HOST,
                      TLSA_TrustStore, (ULONG)"DEVS:Internet/no-such-store",
                      TLSA_Error,      (ULONG)&why);

        (VOID)a_check((BOOL)(tls == NULL && why == TLS_ERR_TRUSTSTORE),
                      "a missing trust store refuses, and says which",
                      (ULONG)why);

        if (tls != NULL)
            TLSClose(tbase, tls);
        (VOID)bsd_close_socket(sbase, sock);
    }

    /* ---- the real thing -------------------------------------------------- */

    sock = a_connect(sbase, address);
    if (!a_check((BOOL)(sock >= 0), "connect to port 443",
                 (ULONG)bsd_errno(sbase)))
    {
        goto close_libs;
    }

    why = TLS_OK;
    tls = TLSOpen(tbase, (APTR)sbase, sock,
                  TLSA_HostName, (ULONG)A_HOST,
                  TLSA_Error,    (ULONG)&why);

    if (!a_check((BOOL)(tls != NULL), "TLSOpen() against a public server",
                 (ULONG)why))
    {
        a_log("  %s", (LONG)TLSErrorString(tbase, why));
        (VOID)bsd_close_socket(sbase, sock);
        goto close_libs;
    }

    info.ti_Size = (ULONG)sizeof(info);
    if (a_check((BOOL)(TLSInfo(tbase, tls, &info) == 0), "TLSInfo()", 0))
    {
        a_log("  TLS 0x%lx, ciphersuite 0x%lx, %ld certificates, %lu.%lu s",
              info.ti_Version, info.ti_CipherSuite, info.ti_ChainDepth,
              info.ti_HandshakeMillis / 1000UL,
              (info.ti_HandshakeMillis % 1000UL) / 100UL);
        a_log("  trust store: %ld roots on disk, %ld parsed for this chain",
              info.ti_TrustRoots, info.ti_RootsLoaded);
        a_log("  clock %s (unix %lu); certificate dates %s",
              (LONG)(info.ti_ExpiryChecked ? "believed" : "UNSET"),
              info.ti_UnixTime,
              (LONG)(info.ti_ExpiryChecked ? "checked" : "NOT checked"));

        (VOID)a_check((BOOL)(info.ti_Verified),
                      "the chain was verified", 0);
        (VOID)a_check((BOOL)(info.ti_TrustRoots > 100),
                      "the store holds a full root set", info.ti_TrustRoots);
        (VOID)a_check((BOOL)(info.ti_RootsLoaded >= 1 &&
                             info.ti_RootsLoaded <= 2),
                      "exactly the roots this chain needed were parsed",
                      info.ti_RootsLoaded);
    }

    /* ---- the request ---------------------------------------------------- */

    n = TLSWrite(tbase, tls, (CONST_APTR)a_request,
                 (LONG)(sizeof(a_request) - 1));
    (VOID)a_check((BOOL)(n == (LONG)(sizeof(a_request) - 1)),
                  "TLSWrite() sent the request", (ULONG)n);

    /* One byte first, so that the rest of the record is provably sitting in
       the library when TLSPending() and TLSWaitSelect() are asked about it. */
    n = TLSRead(tbase, tls, (APTR)a_body, 1);
    (VOID)a_check((BOOL)(n == 1), "TLSRead() of one byte", (ULONG)n);
    total += (ULONG)((n > 0) ? n : 0);

    /* ---- what WaitSelect() would have got wrong -------------------------- */

    n = TLSPending(tbase, tls);
    (VOID)a_check((BOOL)(n > 0),
                  "TLSPending() sees decrypted plaintext the socket does not",
                  (ULONG)n);

    for (i = 0; i < 8; i++)
        a_readfds[i] = 0;
    a_readfds[(ULONG)sock / 32UL] |= 1UL << ((ULONG)sock % 32UL);

    conns[0] = tls;
    conns[1] = NULL;

    poll.tv_secs  = 0;      /* a poll: plain WaitSelect() would answer 0 */
    poll.tv_micro = 0;

    sel.ts_Size        = (ULONG)sizeof(sel);
    sel.ts_SocketBase  = (APTR)sbase;
    sel.ts_NFds        = sock + 1;
    sel.ts_Read        = (APTR)a_readfds;
    sel.ts_Write       = NULL;
    sel.ts_Except      = NULL;
    sel.ts_Timeout     = (APTR)&poll;
    sel.ts_SignalMask  = NULL;
    sel.ts_Connections = conns;

    rc = TLSWaitSelect(tbase, &sel);

    (VOID)a_check((BOOL)(rc == 1),
                  "TLSWaitSelect() reports the connection readable", (ULONG)rc);
    (VOID)a_check((BOOL)((a_readfds[(ULONG)sock / 32UL] &
                          (1UL << ((ULONG)sock % 32UL))) != 0),
                  "and it is the right descriptor", a_readfds[0]);

    /* ---- the rest of the response ---------------------------------------- */

    line[0] = (char)a_body[0];
    i = 1;
    eol = FALSE;

    while ((n = TLSRead(tbase, tls, (APTR)a_body, (LONG)sizeof(a_body))) > 0)
    {
        ULONG k;

        total += (ULONG)n;

        for (k = 0; (!eol) && (k < (ULONG)n) &&
                    (i < (ULONG)(sizeof(line) - 1)); k++)
        {
            if (a_body[k] == (UBYTE)'\r' || a_body[k] == (UBYTE)'\n')
            {
                eol = TRUE;
                break;
            }
            line[i++] = (char)a_body[k];
        }
    }

    line[i] = '\0';

    a_log("  %lu bytes of plaintext, first line: %s", total, (LONG)line);

    (VOID)a_check((BOOL)(line[0] == 'H' && line[1] == 'T' &&
                         line[2] == 'T' && line[3] == 'P'),
                  "the response is HTTP", total);
    (VOID)a_check((BOOL)(total > 100), "and there is a body", total);

    TLSClose(tbase, tls);

    /* TLSClose() gives the descriptor back rather than closing it, so this
       must still work. */
    (VOID)a_check((BOOL)(bsd_close_socket(sbase, sock) == 0),
                  "the descriptor was still ours to close",
                  (ULONG)bsd_errno(sbase));

    /* ---- the machine with no clock --------------------------------------- */

    /*
     * The sharp case.  An Amiga with a dead RTC battery reports 1 January
     * 1978, which is before every certificate on the internet was issued, so a
     * library that checks validity dates unconditionally cannot reach a single
     * HTTPS site from such a machine.  src/tlslib/tls_time.c decides to skip
     * the date check when the clock is outside a plausible window, and to say
     * so in TLSInfo().
     *
     * This sets the emulated machine's clock to the AmigaOS epoch, fetches the
     * same page again, and puts the clock back.  Under FS-UAE the clock is
     * normally the host's, so without this the branch would never be
     * exercised and the decision would be a claim rather than a result.
     */
    if (a_clock_open())
    {
        a_clock_set(0, 0);

        sock = a_connect(sbase, address);
        if (a_check((BOOL)(sock >= 0), "connect with the clock at 1978",
                    (ULONG)bsd_errno(sbase)))
        {
            /*
             * TLSA_NoResume, and it is load-bearing.  This is the fourth
             * connection to A_HOST in this run, so without it the library
             * would resume the session the third one established -- and a
             * resumed handshake sends no certificate, verifies no chain and
             * checks no host name, which is exactly what the two assertions
             * below claim still happened.  Forcing a full handshake keeps the
             * test testing what it says it tests.
             */
            why = TLS_OK;
            tls = TLSOpen(tbase, (APTR)sbase, sock,
                          TLSA_HostName, (ULONG)A_HOST,
                          TLSA_NoResume, TRUE,
                          TLSA_Error,    (ULONG)&why);

            if (a_check((BOOL)(tls != NULL),
                        "a machine with no clock still reaches the site",
                        (ULONG)why))
            {
                info.ti_Size = (ULONG)sizeof(info);
                (VOID)TLSInfo(tbase, tls, &info);

                (VOID)a_check((BOOL)(!info.ti_ExpiryChecked),
                              "and TLSInfo() reports the dates were NOT checked",
                              info.ti_UnixTime);
                (VOID)a_check((BOOL)(info.ti_Verified),
                              "while the chain and the host name still were", 0);

                TLSClose(tbase, tls);
            }
            else
            {
                a_log("  %s", (LONG)TLSErrorString(tbase, why));
            }

            (VOID)bsd_close_socket(sbase, sock);
        }

        a_clock_restore();
        a_clock_close();
    }

close_libs:
    CloseLibrary(tbase);
    CloseLibrary(sbase);

done:
    a_log("");
    a_log("%lu checks, %lu failures -- %s", a_checks, a_failures,
          (LONG)((a_failures == 0UL) ? "PASS" : "FAIL"));

    a_flush();

    return (a_failures == 0UL) ? 0 : 20;
}
