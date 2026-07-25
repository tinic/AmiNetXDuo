/*
 * tls.library -- TLS for ordinary Amiga programs.
 *
 * WHAT THIS IS
 *
 *   A small shared library that puts TLS 1.2 over a descriptor you already
 *   have from bsdsocket.library.  You do the socket(), the DNS lookup and the
 *   connect() exactly as you would for plain HTTP; then you hand the
 *   descriptor to TLSOpen() and read and write through the connection it
 *   returns instead of through recv() and send().  Nothing in this header
 *   mentions NetX Duo, and nothing you write has to.
 *
 *   Complete program, minus error handling:
 *
 *       #include <aminetxduo/tlslib.h>
 *
 *       struct Library *SocketBase = OpenLibrary("bsdsocket.library", 4);
 *       struct Library *TLSBase    = OpenLibrary("tls.library", 1);
 *
 *       LONG s = socket(AF_INET, SOCK_STREAM, 0);
 *       connect(s, (struct sockaddr *)&sin, sizeof(sin));
 *
 *       LONG                  why;
 *       struct TLSConnection *tls = TLSOpen(TLSBase, SocketBase, s,
 *                                           TLSA_HostName, (ULONG)"example.com",
 *                                           TLSA_Error,    (ULONG)&why,
 *                                           TAG_END);
 *
 *       TLSWrite(TLSBase, tls, "GET / HTTP/1.0\r\n"
 *                              "Host: example.com\r\n\r\n", 38);
 *
 *       LONG n;
 *       while ((n = TLSRead(TLSBase, tls, buf, sizeof(buf))) > 0)
 *           FWrite(Output(), buf, 1, n);
 *
 *       TLSClose(TLSBase, tls);       -- the descriptor is yours again
 *       CloseLibrary(TLSBase);
 *       CloseLibrary(SocketBase);
 *
 *   The chain is verified against DEVS:Internet/certificates and the host name
 *   is checked against the certificate, both by default.  There is no
 *   "insecure by accident" mode: TLSA_NoVerify has to be asked for, in those
 *   words.
 *
 * WHAT IT COSTS
 *
 *   A handshake to a public HTTPS server is about seven seconds on a 14 MHz
 *   68020 and most of that is one RSA verification per certificate in the
 *   chain.  Budget roughly 40 KB of Fast RAM per open connection.  Neither is
 *   a bug and neither is going to change much; see docs/RESEARCH.md 9.
 *
 * WaitSelect() AND TLS -- READ THIS ONE
 *
 *   A TLS record is not a byte.  The library reads a whole record off the
 *   socket, decrypts it, and hands you plaintext out of it a bit at a time, so
 *   the socket's readability and the connection's readability are two
 *   different questions:
 *
 *     * WaitSelect() can say NOT READABLE while TLSRead() would return data
 *       immediately, because the bytes are already decrypted and sitting in the
 *       library.  A program that waits on the descriptor alone will hang with
 *       its answer already in memory.  This is the dangerous one.
 *
 *     * WaitSelect() can say READABLE while TLSRead() has to block, because
 *       what arrived is the first half of a record and no plaintext can come
 *       out of it until the rest does.
 *
 *   So: do not call WaitSelect() on a descriptor you have given to TLSOpen().
 *   Call TLSWaitSelect(), which takes the same arguments plus the list of TLS
 *   connections involved.  It reports a connection readable if the library is
 *   already holding plaintext for it -- returning at once, without waiting --
 *   and otherwise hands the whole thing to bsdsocket.library's WaitSelect().
 *   TLSPending() is the same test on its own, if you would rather build the
 *   loop yourself.
 *
 *   The second case, a readable socket that yields no plaintext yet, is not
 *   removable without a non-blocking record layer, and it is bounded: the rest
 *   of a record is already on its way.  TLSA_Timeout puts a ceiling on it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLSLIB_H
#define AMINETXDUO_TLSLIB_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <utility/tagitem.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TLS_LIB_NAME        "tls.library"
#define TLS_LIB_VERSION     1
#define TLS_LIB_REVISION    0

/* Opaque.  One per connection, allocated by TLSOpen and freed by TLSClose. */
struct TLSConnection;

/* ---------------------------------------------------------------- tags --- */

/* 'TLS' << 8.  Tag space is a free-for-all on AmigaOS; a four-byte pattern
   nobody else would pick is the whole of the collision avoidance available. */
#define TLSA_Dummy          (TAG_USER + 0x544C5300UL)

/*
 * STRPTR.  The name you connected to.  Sent as SNI (without it a shared-IP
 * host answers with the wrong certificate or refuses outright) and checked
 * against the certificate's subject alternative names.  Required unless
 * TLSA_NoVerify is set -- verifying a chain without checking who it is for
 * proves only that SOMEBODY has a valid certificate.
 */
#define TLSA_HostName       (TLSA_Dummy + 1)

/*
 * LONG *.  Set to a TLS_ERR_* code when TLSOpen() returns NULL.  Without it a
 * failure is indistinguishable from another failure; with it,
 * TLSErrorString() turns it into something to show a user.
 */
#define TLSA_Error          (TLSA_Dummy + 2)

/*
 * STRPTR.  Trust store to use instead of DEVS:Internet/certificates.  For a
 * program with its own pinned set, and for testing.
 */
#define TLSA_TrustStore     (TLSA_Dummy + 3)

/*
 * BOOL.  Do not verify the chain and do not check the host name.  This makes
 * the connection encrypted and NOT authenticated: anyone in the path can be
 * the other end.  It exists because "talk to the printer on my LAN with a
 * self-signed certificate" is a real thing people do; it is not a fallback to
 * reach for when verification fails.
 */
#define TLSA_NoVerify       (TLSA_Dummy + 4)

/*
 * ULONG milliseconds, default 120000.  Ceiling on the handshake and on any
 * single TLSRead()/TLSWrite().  Zero means wait forever.
 */
#define TLSA_Timeout        (TLSA_Dummy + 5)

/*
 * ULONG bytes, default 10240.  The record reassembly buffer.  A TLS record can
 * be 16 KB; a certificate chain arrives as one flight and has to fit.  8 KB
 * holds a two-certificate RSA-2048 chain and 10 KB a public three-deep one.
 * Raise it if a server's chain is refused with TLS_ERR_HANDSHAKE and the log
 * says the buffer was too small; lower it if 10 KB is more than you have.
 */
#define TLSA_RecordBuffer   (TLSA_Dummy + 6)

/*
 * ULONG, default 4.  How many certificates the server may send.  Each costs an
 * NX_SECURE_X509_CERT plus a 2 KB DER buffer, and each costs one public-key
 * verification -- seconds, on this hardware.
 */
#define TLSA_MaxChain       (TLSA_Dummy + 7)

/* --------------------------------------------------------------- errors --- */

#define TLS_OK               0
#define TLS_ERR_NOMEM        1  /* out of memory                              */
#define TLS_ERR_BADSOCKET    2  /* not a connected TCP descriptor             */
#define TLS_ERR_NOSTACK      3  /* bsdsocket.library has no TLS support built */
#define TLS_ERR_TRUSTSTORE   4  /* the trust store is missing or unreadable   */
#define TLS_ERR_HANDSHAKE    5  /* the server would not agree on a handshake  */
#define TLS_ERR_UNTRUSTED    6  /* the chain reached no root we trust         */
#define TLS_ERR_HOSTNAME     7  /* the certificate is for somebody else       */
#define TLS_ERR_EXPIRED      8  /* the certificate is expired or not yet valid*/
#define TLS_ERR_TIMEOUT      9  /* the other end went quiet                   */
#define TLS_ERR_CLOSED      10  /* the connection is finished                 */
#define TLS_ERR_IO          11  /* the socket failed underneath us            */
#define TLS_ERR_NOHOSTNAME  12  /* verification asked for, no TLSA_HostName   */
#define TLS_ERR_INTERNAL    13  /* a bug on our side                          */

/* ----------------------------------------------------------------- info --- */

struct TLSInfo
{
    ULONG   ti_Size;            /* set to sizeof(struct TLSInfo) before the call */

    ULONG   ti_Version;         /* 0x0303 == TLS 1.2                          */
    ULONG   ti_CipherSuite;     /* the negotiated suite, IANA number          */
    ULONG   ti_ChainDepth;      /* certificates the server sent               */
    ULONG   ti_HandshakeMillis; /* how long TLSOpen() spent shaking hands     */
    LONG    ti_Error;           /* the last TLS_ERR_* on this connection      */

    BOOL    ti_Verified;        /* the chain reached a root in the trust store */

    /*
     * FALSE when the machine's clock is unset and the certificate's validity
     * dates were therefore NOT checked -- see the note in the library's
     * documentation.  ti_UnixTime is what we believed the time was, or 0.
     */
    BOOL    ti_ExpiryChecked;
    ULONG   ti_UnixTime;

    ULONG   ti_TrustRoots;      /* roots the store holds                      */
    ULONG   ti_RootsLoaded;     /* roots this connection actually parsed      */
};

/* ------------------------------------------------------ WaitSelect() ------ */

struct TLSSelect
{
    ULONG   ts_Size;            /* sizeof(struct TLSSelect)                   */

    APTR    ts_SocketBase;      /* your bsdsocket.library base                */
    LONG    ts_NFds;            /* highest descriptor + 1, as for WaitSelect  */
    APTR    ts_Read;            /* fd_set *, may be NULL                      */
    APTR    ts_Write;
    APTR    ts_Except;
    APTR    ts_Timeout;         /* struct timeval *, NULL blocks forever      */
    ULONG  *ts_SignalMask;      /* in/out, exactly as WaitSelect uses it      */

    /*
     * NULL-terminated array of the TLS connections whose descriptors appear in
     * ts_Read.  Any of them already holding plaintext makes this return at
     * once with that descriptor set.  May be NULL, in which case this is
     * WaitSelect() with extra steps.
     */
    struct TLSConnection **ts_Connections;
};

/* --------------------------------------------------------------- vectors --- */

#define TLS_LVO_TLSOpenA        (-30)
#define TLS_LVO_TLSClose        (-36)
#define TLS_LVO_TLSRead         (-42)
#define TLS_LVO_TLSWrite        (-48)
#define TLS_LVO_TLSPending      (-54)
#define TLS_LVO_TLSInfo         (-60)
#define TLS_LVO_TLSErrorString  (-66)
#define TLS_LVO_TLSWaitSelect   (-72)

/*
 * Inline stubs.  Hand-written rather than generated from an .fd because there
 * is no .fd to generate them from yet, and because a caller should be able to
 * use this library with nothing but this header.  Every one takes the library
 * base explicitly -- no global TLSBase -- so that a program can hold two, and
 * so that nothing here fights a <proto/> header.
 */

static __inline struct TLSConnection *
TLSOpenA(struct Library *base, APTR socket_base, LONG sock,
         const struct TagItem *tags)
{
    register struct Library      *a6  __asm("a6") = base;
    register APTR                 a0  __asm("a0") = socket_base;
    register const struct TagItem *a1 __asm("a1") = tags;
    register LONG                 d0  __asm("d0") = sock;
    register struct TLSConnection *res __asm("d0");

    __asm __volatile ("jsr a6@(-30:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (d0)
                      : "d1", "cc", "memory");
    return res;
}

#define TLSOpen(base, sockbase, sock, ...) \
    ({ const struct TagItem _tlstags[] = { __VA_ARGS__ }; \
       TLSOpenA((base), (sockbase), (sock), _tlstags); })

static __inline VOID TLSClose(struct Library *base, struct TLSConnection *conn)
{
    register struct Library       *a6 __asm("a6") = base;
    register struct TLSConnection *a0 __asm("a0") = conn;

    __asm __volatile ("jsr a6@(-36:W)"
                      :
                      : "r" (a6), "r" (a0)
                      : "d0", "d1", "a1", "cc", "memory");
}

static __inline LONG TLSRead(struct Library *base, struct TLSConnection *conn,
                             APTR buffer, LONG length)
{
    register struct Library       *a6 __asm("a6") = base;
    register struct TLSConnection *a0 __asm("a0") = conn;
    register APTR                  a1 __asm("a1") = buffer;
    register LONG                  d0 __asm("d0") = length;
    register LONG                  res __asm("d0");

    __asm __volatile ("jsr a6@(-42:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (d0)
                      : "d1", "cc", "memory");
    return res;
}

static __inline LONG TLSWrite(struct Library *base, struct TLSConnection *conn,
                              CONST_APTR buffer, LONG length)
{
    register struct Library       *a6 __asm("a6") = base;
    register struct TLSConnection *a0 __asm("a0") = conn;
    register CONST_APTR            a1 __asm("a1") = buffer;
    register LONG                  d0 __asm("d0") = length;
    register LONG                  res __asm("d0");

    __asm __volatile ("jsr a6@(-48:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0), "r" (a1), "r" (d0)
                      : "d1", "cc", "memory");
    return res;
}

static __inline LONG TLSPending(struct Library *base, struct TLSConnection *conn)
{
    register struct Library       *a6 __asm("a6") = base;
    register struct TLSConnection *a0 __asm("a0") = conn;
    register LONG                  res __asm("d0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

static __inline LONG TLSInfo(struct Library *base, struct TLSConnection *conn,
                             struct TLSInfo *info)
{
    register struct Library       *a6 __asm("a6") = base;
    register struct TLSConnection *a0 __asm("a0") = conn;
    register struct TLSInfo       *a1 __asm("a1") = info;
    register LONG                  res __asm("d0");

    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0), "r" (a1)
                      : "d1", "cc", "memory");
    return res;
}

static __inline CONST_STRPTR TLSErrorString(struct Library *base, LONG code)
{
    register struct Library *a6 __asm("a6") = base;
    register LONG            d0 __asm("d0") = code;
    register CONST_STRPTR    res __asm("d0");

    __asm __volatile ("jsr a6@(-66:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (d0)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static __inline LONG TLSWaitSelect(struct Library *base, struct TLSSelect *sel)
{
    register struct Library   *a6 __asm("a6") = base;
    register struct TLSSelect *a0 __asm("a0") = sel;
    register LONG              res __asm("d0");

    __asm __volatile ("jsr a6@(-72:W)"
                      : "=r" (res)
                      : "r" (a6), "r" (a0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TLSLIB_H */
