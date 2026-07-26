/*
 * AmiNetXDuo -- TLS session resumption, measured against a real public server.
 *
 * WHAT THIS IS FOR
 *
 *   A full TLS handshake on a 14 MHz 68020 is 6.8 s for an RSA certificate
 *   chain and 23.3 s for an ECDSA one, nearly all of it public-key
 *   arithmetic.  A RESUMED handshake does none of that arithmetic.  This
 *   program measures both against the same host in the same run, so the
 *   difference is a measurement rather than a composition of two runs on two
 *   days.
 *
 *   It also proves the things that make resumption safe to have on by default:
 *   that a resumed connection actually carries data, that a ticket the server
 *   refuses produces a full handshake instead of a failure, and that
 *   TLSA_NoResume means what it says.
 *
 *   Linked against NOTHING of ours, exactly like tests/tls/tls_api.c: it opens
 *   two shared libraries by name and uses their published vectors.  If this
 *   ever needs a target_link_libraries line, the API has failed.
 *
 *   Cross-PROCESS resumption is not provable from inside one process and this
 *   program does not claim it.  tests/tls/run-resume.sh does that part, by
 *   running the shipped `fetch` command twice and then rebooting the machine
 *   and running it again.
 *
 * STAGING
 *
 *   LIBS:bsdsocket.library, LIBS:tls.library, DEVS:Internet/certificates,
 *   DEVS:NetInterfaces/eth0 and DEVS:a2065.device -- see run-resume.sh.
 *
 * NOT A BASELINE.  It depends on the internet, on FS-UAE's SLIRP NAT, on a
 * third party's server, and on that server's willingness to issue and honour a
 * session ticket.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>

#include "aminetxduo/tlslib.h"


/* ------------------------------------------------------------- logging --- */

#ifndef RawPutChar
#  define RawPutChar(c) \
      LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#endif

#define R_LOG_SIZE  8192

static char  r_log_buffer[R_LOG_SIZE];
static ULONG r_log_used;

static VOID r_put(UBYTE c)
{
    RawPutChar(c);

    if (r_log_used < (ULONG)(R_LOG_SIZE - 1))
        r_log_buffer[r_log_used++] = (char)c;
}

static VOID r_put_char(register UBYTE c      __asm("d0"),
                       register APTR  unused __asm("a3"))
{
    (VOID)unused;
    if (c != '\0')
        r_put(c);
}

static VOID r_log(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    RawDoFmt((STRPTR)fmt, args, (void (*)()) r_put_char, NULL);
    va_end(args);

    r_put('\n');
}

static VOID r_flush(VOID)
{
    BPTR out = Output();

    if (out != (BPTR)0)
        (VOID)Write(out, (APTR)r_log_buffer, (LONG)r_log_used);
}

static ULONG r_checks;
static ULONG r_failures;

static BOOL r_check(BOOL ok, const char *what, ULONG detail)
{
    r_checks++;
    if (!ok)
    {
        r_failures++;
        r_log("  FAIL %s (0x%lx)", (LONG)what, detail);
        return FALSE;
    }

    r_log("  ok   %s", (LONG)what);
    return TRUE;
}


/* ------------------------------------------- bsdsocket.library, by hand --- */

/* Offsets from the Roadshow NDK pragmas; see the note in tests/tls/tls_api.c
   about why this is spelled out rather than pulled from <proto/>. */

struct r_sockaddr_in
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;
    UBYTE   sin_zero[8];
};

struct r_hostent
{
    char   *h_name;
    char  **h_aliases;
    LONG    h_addrtype;
    LONG    h_length;
    char  **h_addr_list;
};

#define R_AF_INET       2
#define R_SOCK_STREAM   1

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

static struct r_hostent *bsd_gethostbyname(struct Library *base, const char *name)
{
    register struct Library   *a6 __asm("a6") = base;
    register const char       *a0 __asm("a0") = name;
    register struct r_hostent *res __asm("d0");

    __asm __volatile ("jsr a6@(-210:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

/* --------------------------------------------------------------- the run -- */

#define R_PORT          443

/*
 * Two hosts, because the two chains cost wildly different amounts and the
 * saving is the point:
 *
 *   tls-v1-2.badssl.com  RSA leaf, TLS_ECDHE_RSA_*, 6.8 s cold
 *   ecc256.badssl.com    ECDSA leaf, TLS_ECDHE_ECDSA_*, 23.3 s cold
 *
 * Both are nginx behind badssl.com rather than a CDN, which is what makes them
 * usable cold at 14 MHz at all; the Cloudflare case is run-resume.sh's job.
 */
#define R_HOST_RSA      "tls-v1-2.badssl.com"
#define R_HOST_ECDSA    "ecc256.badssl.com"

#define R_SESSIONS      "DEVS:Internet/tlssessions"

/* A valid trust store holding one unrelated self-signed root -- staged by
   tests/tls/run-resume.sh, which builds it with tools/mkcertstore.py. */
#define R_OTHERSTORE    "DEVS:Internet/otherstore"
#define R_TAMPERED      "DH0:tampered.sessions"

/* The session-cache record stride, from src/tlslib/tls_resume.c: 164 bytes of
   fixed fields plus TLS_RESUME_TICKET_MAX.  Spelled out rather than included
   because this program is deliberately linked against nothing of ours. */
#define R_RECORD        424

static UBYTE r_body[2048];

static ULONG r_address(struct Library *sbase, const char *host)
{
    struct r_hostent *he;
    ULONG             address = 0;
    ULONG             i;

    he = bsd_gethostbyname(sbase, host);
    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
        return 0;

    for (i = 0; i < 4; i++)
        address = (address << 8) | (ULONG)(UBYTE)he->h_addr_list[0][i];

    return address;
}

static LONG r_connect(struct Library *sbase, ULONG address)
{
    struct r_sockaddr_in sin;
    LONG                 sock;
    ULONG                i;

    for (i = 0; i < sizeof(sin.sin_zero); i++)
        sin.sin_zero[i] = 0;

    sin.sin_len    = (UBYTE)sizeof(sin);
    sin.sin_family = (UBYTE)R_AF_INET;
    sin.sin_port   = (UWORD)R_PORT;
    sin.sin_addr   = address;

    sock = bsd_socket(sbase, R_AF_INET, R_SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    if (bsd_connect(sbase, sock, (APTR)&sin, (LONG)sizeof(sin)) != 0)
    {
        (VOID)bsd_close_socket(sbase, sock);
        return -1;
    }

    return sock;
}

/*
 * One GET, over whatever connection is handed in.  Returns the number of
 * plaintext bytes and fills `first` with the status line, because a resumed
 * session that hands back garbage would otherwise pass every timing check.
 */
static ULONG r_fetch(struct Library *tbase, struct TLSConnection *tls,
                     const char *host, char *first, ULONG first_size)
{
    /* Static, not on the stack.  A Shell command gets 4 KB, tls.library runs
       NetX Duo and nx_secure on the caller's stack, and this program is three
       frames deeper than tests/tls/tls_api.c ever is. */
    static char request[160];

    ULONG len = 0;
    ULONG total = 0;
    ULONG i;
    LONG  n;
    BOOL  eol = FALSE;

    static const char part1[] = "GET / HTTP/1.0\r\nHost: ";
    static const char part2[] = "\r\nConnection: close\r\n\r\n";

    for (i = 0; part1[i] != '\0'; i++)
        request[len++] = part1[i];
    for (i = 0; host[i] != '\0' && len < (ULONG)(sizeof(request) - 32); i++)
        request[len++] = host[i];
    for (i = 0; part2[i] != '\0'; i++)
        request[len++] = part2[i];

    first[0] = '\0';

    n = TLSWrite(tbase, tls, (CONST_APTR)request, (LONG)len);
    if (n != (LONG)len)
    {
        r_log("  TLSWrite(%ld) returned %ld", (LONG)len, n);
        return 0;
    }

    i = 0;
    while ((n = TLSRead(tbase, tls, (APTR)r_body, (LONG)sizeof(r_body))) > 0)
    {
        ULONG k;

        total += (ULONG)n;

        for (k = 0; (!eol) && (k < (ULONG)n) && (i < first_size - 1); k++)
        {
            if (r_body[k] == (UBYTE)'\r' || r_body[k] == (UBYTE)'\n')
            {
                eol = TRUE;
                break;
            }
            first[i++] = (char)r_body[k];
        }
    }

    first[i] = '\0';

    if (total == 0)
        r_log("  TLSRead(%lx, %lx) returned %ld", (LONG)tls, (LONG)r_body, n);

    return total;
}

/*
 * A handshake, with everything worth knowing about it reported.  `sessions` is
 * the TLSA_SessionFile to use, or NULL for the default; `no_resume` sets
 * TLSA_NoResume.  Returns the handshake time in milliseconds, or 0 on failure,
 * and sets *resumed.
 */
static ULONG r_handshake(struct Library *sbase, struct Library *tbase,
                         const char *host, ULONG address,
                         const char *sessions, BOOL no_resume,
                         BOOL *resumed, ULONG *body_bytes, char *first,
                         ULONG first_size)
{
    struct TLSConnection *tls;
    LONG                  sock;
    LONG                  why = TLS_OK;
    ULONG                 slot = 0;
    ULONG                 millis = 0;

    /*
     * Static, not automatic, and this is not tidiness.  A Shell command gets
     * 4,096 bytes of stack on Kickstart 3.1, and tls.library brackets its
     * caller into ThreadX with THAT stack -- NetX Duo, nx_secure and the
     * bignum code all run on it (docs/RESEARCH.md).  This program calls into
     * the library three frames deeper than tests/tls/tls_api.c does, and every
     * local here is a byte the record layer does not get.
     */
    static struct TLSInfo info;
    static struct TagItem tags[6];

    *resumed    = FALSE;
    *body_bytes = 0;
    first[0]    = '\0';

    sock = r_connect(sbase, address);
    if (sock < 0)
        return 0;

    tags[slot].ti_Tag = TLSA_HostName; tags[slot].ti_Data = (ULONG)host; slot++;
    tags[slot].ti_Tag = TLSA_Error;    tags[slot].ti_Data = (ULONG)&why; slot++;

    if (sessions != NULL)
    {
        tags[slot].ti_Tag  = TLSA_SessionFile;
        tags[slot].ti_Data = (ULONG)sessions;
        slot++;
    }
    if (no_resume)
    {
        tags[slot].ti_Tag  = TLSA_NoResume;
        tags[slot].ti_Data = TRUE;
        slot++;
    }

    tags[slot].ti_Tag = TAG_END; tags[slot].ti_Data = 0;

    tls = TLSOpenA(tbase, (APTR)sbase, sock, tags);

    if (tls == NULL)
    {
        r_log("  TLSOpen(%s) failed: %s", (LONG)host,
              (LONG)TLSErrorString(tbase, why));
        (VOID)bsd_close_socket(sbase, sock);
        return 0;
    }

    info.ti_Size = (ULONG)sizeof(info);
    if (TLSInfo(tbase, tls, &info) == 0)
    {
        millis   = info.ti_HandshakeMillis;
        *resumed = info.ti_Resumed;

        r_log("  %s: %s handshake, %lu ms, suite 0x%lx, %ld certificate(s), "
              "%ld session(s) cached",
              (LONG)host,
              (LONG)(info.ti_Resumed ? "RESUMED" : "full"),
              millis, info.ti_CipherSuite, info.ti_ChainDepth,
              info.ti_SessionsCached);
    }

    *body_bytes = r_fetch(tbase, tls, host, first, first_size);

    if (*body_bytes == 0)
    {
        info.ti_Size = (ULONG)sizeof(info);
        if (TLSInfo(tbase, tls, &info) == 0)
            r_log("  last error on the connection: %s",
                  (LONG)TLSErrorString(tbase, info.ti_Error));
    }

    TLSClose(tbase, tls);
    (VOID)bsd_close_socket(sbase, sock);

    /* A handshake that took no measurable time is still a handshake; return 1
       so that 0 keeps meaning "did not happen". */
    return (millis == 0) ? 1 : millis;
}

/*
 * Make a copy of the session cache with the ticket and the session ID
 * corrupted, so the server can neither decrypt the one nor recognise the
 * other.  The layout is the one documented in src/tlslib/tls_resume.c:
 * a 16-byte header, then fixed-size records with the session ID at offset 72
 * and the ticket at 164.
 *
 * Corrupting BOTH matters.  A ticket the server rejects would still leave the
 * session ID on the wire, and a server with a working ID cache could resume
 * from that instead -- which would be correct behaviour and a failed test.
 */
static BOOL r_tamper(const char *in, const char *out)
{
    UBYTE *buffer;
    BPTR   fh;
    LONG   size;
    LONG   i;
    LONG   records;

    buffer = (UBYTE *)AllocVec(65536UL, MEMF_PUBLIC | MEMF_CLEAR);
    if (buffer == NULL)
        return FALSE;

    fh = Open((STRPTR)in, MODE_OLDFILE);
    if (fh == (BPTR)0)
    {
        FreeVec(buffer);
        return FALSE;
    }

    size = Read(fh, buffer, 65536L);
    Close(fh);

    if (size < (LONG)(16 + R_RECORD))
    {
        FreeVec(buffer);
        return FALSE;
    }

    records = (size - 16) / R_RECORD;

    for (i = 0; i < records; i++)
    {
        UBYTE *rec = &buffer[16 + (i * R_RECORD)];

        rec[72]  = (UBYTE)(rec[72] ^ 0xFF);     /* session ID */
        rec[168] = (UBYTE)(rec[168] ^ 0xFF);    /* ticket     */
        rec[169] = (UBYTE)(rec[169] ^ 0xFF);
    }

    fh = Open((STRPTR)out, MODE_NEWFILE);
    if (fh == (BPTR)0)
    {
        FreeVec(buffer);
        return FALSE;
    }

    (VOID)Write(fh, buffer, size);
    Close(fh);
    FreeVec(buffer);

    return TRUE;
}

static BOOL r_is_http(const char *line)
{
    return (BOOL)((line[0] == 'H' && line[1] == 'T' && line[2] == 'T' &&
                   line[3] == 'P') ? TRUE : FALSE);
}

static VOID r_host_round(struct Library *sbase, struct Library *tbase,
                         const char *host)
{
    ULONG address;
    ULONG cold;
    ULONG warm;
    ULONG cold_bytes = 0;
    ULONG warm_bytes = 0;
    BOOL  resumed = FALSE;

    static char first[96];

    r_log("");
    r_log("--- %s", (LONG)host);

    address = r_address(sbase, host);
    if (!r_check((BOOL)(address != 0), "gethostbyname()", 0))
        return;

    /* ---- cold ---------------------------------------------------------- */

    cold = r_handshake(sbase, tbase, host, address, NULL, FALSE,
                       &resumed, &cold_bytes, first, sizeof(first));

    if (!r_check((BOOL)(cold != 0), "cold handshake completed", 0))
        return;

    (VOID)r_check((BOOL)(!resumed),
                  "the first handshake to a host is NOT resumed", 0);
    (VOID)r_check(r_is_http(first), "and it carries HTTP", cold_bytes);

    /* ---- warm ---------------------------------------------------------- */

    warm = r_handshake(sbase, tbase, host, address, NULL, FALSE,
                       &resumed, &warm_bytes, first, sizeof(first));

    if (!r_check((BOOL)(warm != 0), "second handshake completed", 0))
        return;

    (VOID)r_check((BOOL)(resumed),
                  "the SECOND handshake to the same host IS resumed", 0);
    (VOID)r_check(r_is_http(first),
                  "a resumed session transfers data correctly", warm_bytes);
    (VOID)r_check((BOOL)(warm_bytes > 100),
                  "and the whole body arrives", warm_bytes);

    /*
     * The deliverable.  Two seconds is a floor chosen to be unarguable rather
     * than tight: the cold figures are 6.8 s and 23.3 s and the resumed ones
     * are a fraction of a second, so anything close to this bound means
     * something has gone wrong rather than that the margin was thin.
     */
    (VOID)r_check((BOOL)(warm * 2UL < cold),
                  "the resumed handshake is at least twice as fast", warm);

    r_log("  DELTA %s: cold %lu ms -> resumed %lu ms", (LONG)host, cold, warm);
}

/*
 * THE STACK, AND WHY THIS PROGRAM NEEDS ITS OWN
 *
 *   A command started by the Kickstart 3.1 Shell gets 4,096 bytes of stack,
 *   and tls.library brackets its caller into ThreadX with THAT stack -- NetX
 *   Duo, nx_secure and the bignum code all run on it (docs/RESEARCH.md, the
 *   `fetch` traveller).  This program calls into the library from three frames
 *   down rather than from main(), and the symptom of running out was not a
 *   crash: the handshake completed and then TLSRead() returned -1 having never
 *   reached the record layer, because the library's own file-scope pointer to
 *   the borrowed NetX Duo context had been overwritten from below.  A machine
 *   with no memory protection does not fault, it just does something else.
 *
 *   Same answer as src/tools/fetch.c: 64 KB of our own, through StackSwap()
 *   (exec V36, so it is on the 3.1 floor).  The trampoline has no locals, no
 *   arguments and is noinline, because between the two swaps the stack pointer
 *   belongs to the other stack.
 */
#define R_STACK_SIZE    (64UL * 1024UL)

static struct StackSwapStruct r_sss;
static int                    r_result;

static int r_run(VOID);

static __attribute__((noinline)) VOID r_trampoline(VOID)
{
    StackSwap(&r_sss);
    r_result = r_run();
    StackSwap(&r_sss);
}

int main(VOID)
{
    APTR stack = AllocMem(R_STACK_SIZE, MEMF_ANY);

    if (stack != NULL)
    {
        r_sss.stk_Lower   = stack;
        r_sss.stk_Upper   = (ULONG)stack + R_STACK_SIZE;
        r_sss.stk_Pointer = (APTR)((ULONG)stack + R_STACK_SIZE);

        r_trampoline();

        FreeMem(stack, R_STACK_SIZE);
    }
    else
    {
        r_result = r_run();
    }

    return r_result;
}

static int r_run(VOID)
{
    struct Library *sbase;
    struct Library *tbase;
    ULONG           address;
    ULONG           millis;
    ULONG           bytes = 0;
    BOOL            resumed = FALSE;

    static char first[96];

    r_log("AmiNetXDuo -- TLS session resumption");

    sbase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4UL);
    if (!r_check((BOOL)(sbase != NULL), "OpenLibrary(bsdsocket.library)", 0))
        goto done;

    tbase = OpenLibrary((CONST_STRPTR)TLS_LIB_NAME, (ULONG)TLS_LIB_VERSION);
    if (!r_check((BOOL)(tbase != NULL), "OpenLibrary(tls.library)", 0))
    {
        CloseLibrary(sbase);
        goto done;
    }

    /* ---- the measurement, one host per chain type ----------------------- */

    r_host_round(sbase, tbase, R_HOST_RSA);
    r_host_round(sbase, tbase, R_HOST_ECDSA);

    /* ---- a ticket the server will not accept ---------------------------- */

    /*
     * The failure mode that matters most in practice.  Tickets expire, servers
     * rotate their ticket keys without telling anybody, and a machine with no
     * clock cannot age its own cache.  A client that treats a refused ticket as
     * an error would be worse than one with no resumption at all, because it
     * would break AFTER working.
     */
    r_log("");
    r_log("--- a ticket the server refuses");

    address = r_address(sbase, R_HOST_RSA);

    if (r_check(r_tamper(R_SESSIONS, R_TAMPERED),
                "the session cache was copied and corrupted", 0))
    {
        millis = r_handshake(sbase, tbase, R_HOST_RSA, address,
                             R_TAMPERED, FALSE, &resumed, &bytes,
                             first, sizeof(first));

        (VOID)r_check((BOOL)(millis != 0),
                      "a refused ticket still completes a handshake", 0);
        (VOID)r_check((BOOL)(!resumed),
                      "and it is a FULL handshake, not a failure", 0);
        (VOID)r_check(r_is_http(first),
                      "and the page still arrives", bytes);

        /* The refused entry must have been replaced, not left to fail
           forever: the very next connection has to resume again. */
        millis = r_handshake(sbase, tbase, R_HOST_RSA, address,
                             R_TAMPERED, FALSE, &resumed, &bytes,
                             first, sizeof(first));

        (VOID)r_check((BOOL)(millis != 0 && resumed),
                      "and the session it replaced it with resumes", 0);

        (VOID)DeleteFile((STRPTR)R_TAMPERED);
    }

    /* ---- a DIFFERENT trust store must not inherit the verification ------ */

    /*
     * THE ONE THAT MATTERS MOST, and the regression test for a real defect.
     *
     * A resumed handshake verifies nothing -- no certificate, no signature, no
     * host name.  So a session cached after verification against one trust
     * store must NOT be handed to a caller presenting a different store: the
     * caller would get a connection the library says is verified, against
     * roots it never offered and that signed nothing in the chain.
     *
     * The first version of the cache keyed on a BOOLEAN saying verification
     * had happened rather than on what it happened against, and did exactly
     * that.  DH0:otherstore is a valid, well-formed trust store holding one
     * unrelated self-signed root; the correct answer is a REFUSAL, the same
     * one a cold handshake gives, and anything else means resumption is
     * laundering a trust decision.
     */
    r_log("");
    r_log("--- a session must not be resumed under a different trust store");

    {
        struct TLSConnection *tls;
        LONG                  sock;
        LONG                  why = TLS_OK;

        /* Make sure there IS a session cached for this host first, or the
           test would pass by having nothing to wrongly resume. */
        millis = r_handshake(sbase, tbase, R_HOST_RSA, address, NULL, FALSE,
                             &resumed, &bytes, first, sizeof(first));
        (VOID)r_check((BOOL)(millis != 0 && resumed),
                      "a verified session is sitting in the cache", 0);

        sock = r_connect(sbase, address);
        if (r_check((BOOL)(sock >= 0), "connect with the wrong store", 0))
        {
            static struct TagItem wrong[4];

            wrong[0].ti_Tag  = TLSA_HostName;   wrong[0].ti_Data = (ULONG)R_HOST_RSA;
            wrong[1].ti_Tag  = TLSA_TrustStore; wrong[1].ti_Data = (ULONG)R_OTHERSTORE;
            wrong[2].ti_Tag  = TLSA_Error;      wrong[2].ti_Data = (ULONG)&why;
            wrong[3].ti_Tag  = TAG_END;         wrong[3].ti_Data = 0;

            tls = TLSOpenA(tbase, (APTR)sbase, sock, wrong);

            (VOID)r_check((BOOL)(tls == NULL),
                          "a store that signed nothing in the chain is REFUSED",
                          (ULONG)why);
            (VOID)r_check((BOOL)(why == TLS_ERR_UNTRUSTED ||
                                 why == TLS_ERR_HANDSHAKE),
                          "and refused for an UNTRUSTED reason", (ULONG)why);

            if (tls != NULL)
            {
                struct TLSInfo bad;

                bad.ti_Size = (ULONG)sizeof(bad);
                if (TLSInfo(tbase, tls, &bad) == 0)
                    r_log("  LEAK: resumed=%ld verified=%ld",
                          (LONG)bad.ti_Resumed, (LONG)bad.ti_Verified);
                TLSClose(tbase, tls);
            }

            (VOID)bsd_close_socket(sbase, sock);
        }

        /* And the correct store still resumes -- a "fix" that simply stops
           resuming is not a fix. */
        millis = r_handshake(sbase, tbase, R_HOST_RSA, address, NULL, FALSE,
                             &resumed, &bytes, first, sizeof(first));
        (VOID)r_check((BOOL)(millis != 0 && resumed),
                      "and the CORRECT store still resumes", 0);
        (VOID)r_check(r_is_http(first), "and still transfers data", bytes);
    }

    /* ---- TLSA_NoResume -------------------------------------------------- */

    r_log("");
    r_log("--- TLSA_NoResume");

    millis = r_handshake(sbase, tbase, R_HOST_RSA, address, NULL, TRUE,
                         &resumed, &bytes, first, sizeof(first));

    (VOID)r_check((BOOL)(millis != 0), "TLSA_NoResume still connects", 0);
    (VOID)r_check((BOOL)(!resumed),
                  "and refuses to resume even with a session in the cache", 0);
    (VOID)r_check(r_is_http(first), "and fetches the page", bytes);

    CloseLibrary(tbase);
    CloseLibrary(sbase);

done:
    r_log("");
    r_log("%lu checks, %lu failures -- %s", r_checks, r_failures,
          (LONG)((r_failures == 0UL) ? "PASS" : "FAIL"));

    r_flush();

    return (r_failures == 0UL) ? 0 : 20;
}
