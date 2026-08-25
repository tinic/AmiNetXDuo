/* tls.library, TLS 1.2 over a bsdsocket.library descriptor.
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

/* The version is the vector level: adding a vector means bumping this, with no
 * exceptions.  Exec opens on lib_Version >= the version asked for, so a caller
 * compiled against a newer header would jump past an older library's jump
 * table.  Ask for what you use, not for TLS_LIB_VERSION. */
#define TLS_LIB_VERSION     2
#define TLS_LIB_REVISION    0

/* How many user vectors each version has.  TLS_LIB_VECTORS is derived from
 * TLS_LIB_VERSION, and src/tlslib/tls_vectors.c asserts the real table against
 * it at build time, so a new vector cannot ship without the version moving. */
#define TLS_LIB_VECTORS_V1  8
#define TLS_LIB_VECTORS_V2  10

#define TLS_LIB_VECTORS_FOR_(v) TLS_LIB_VECTORS_V##v
#define TLS_LIB_VECTORS_FOR(v)  TLS_LIB_VECTORS_FOR_(v)
#define TLS_LIB_VECTORS         TLS_LIB_VECTORS_FOR(TLS_LIB_VERSION)

/* Opaque.  One per connection, allocated by TLSOpen and freed by TLSClose. */
struct TLSConnection;

/* ---------------------------------------------------------------- tags --- */

/* 'TLS' << 8.  Tag space is unallocated on AmigaOS; a four-byte pattern nobody
   else would pick is the whole of the collision avoidance available. */
#define TLSA_Dummy          (TAG_USER + 0x544C5300UL)

/* STRPTR.  The name you connected to: sent as SNI and checked against the
 * certificate.  Required unless TLSA_NoVerify.  Names longer than 100 bytes are
 * rejected with TLS_ERR_BADHOSTNAME, never truncated. */
#define TLSA_HostName       (TLSA_Dummy + 1)

/* LONG *.  Set to a TLS_ERR_* code when TLSOpen() returns NULL. */
#define TLSA_Error          (TLSA_Dummy + 2)

/* STRPTR.  Trust store to use instead of DEVS:Internet/certificates.  Paths
 * longer than 159 bytes are rejected with TLS_ERR_BADPATH, never truncated. */
#define TLSA_TrustStore     (TLSA_Dummy + 3)

/* BOOL.  Do not verify the chain and do not check the host name: the
 * connection is then encrypted and NOT authenticated. */
#define TLSA_NoVerify       (TLSA_Dummy + 4)

/* ULONG milliseconds, default 120000.  Ceiling on the handshake and on any
 * single TLSRead()/TLSWrite().  Zero means wait forever. */
#define TLSA_Timeout        (TLSA_Dummy + 5)

/* ULONG bytes, default 10240.  The record reassembly buffer; a certificate
 * chain arrives as one flight and has to fit, or TLS_ERR_HANDSHAKE. */
#define TLSA_RecordBuffer   (TLSA_Dummy + 6)

/* ULONG, default 4.  How many certificates the server may send. */
#define TLSA_MaxChain       (TLSA_Dummy + 7)

/* BOOL.  Do not offer a cached session and do not remember this one; every
 * connection then pays the full public-key cost. */
#define TLSA_NoResume       (TLSA_Dummy + 8)

/* STRPTR.  Where the session cache is mirrored, instead of
 * DEVS:Internet/tlssessions.  An empty string keeps it in RAM and writes no
 * master secret to disk.  Paths over 159 bytes: TLS_ERR_BADPATH, never cut. */
#define TLSA_SessionFile    (TLSA_Dummy + 9)

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
#define TLS_ERR_ALERT       14  /* the peer sent a fatal alert; data is short */
#define TLS_ERR_BADHOSTNAME 15  /* host name exceeds the verifier/SNI limit   */
#define TLS_ERR_KEYUSAGE    16  /* leaf keyUsage forbids the negotiated use   */
#define TLS_ERR_BADPATH     17  /* trust/session path exceeds internal limit  */

/* ----------------------------------------------------------------- info --- */

struct TLSInfo
{
    /*
     * Set to sizeof(struct TLSInfo) before the call.  Zero the whole structure
     * first if you opened the library with a version older than the one you
     * compiled against: an older library leaves fields it does not know alone.
     */
    ULONG   ti_Size;

    ULONG   ti_Version;         /* 0x0303 == TLS 1.2, 0x0304 == TLS 1.3       */
    ULONG   ti_CipherSuite;     /* the negotiated suite, IANA number          */
    ULONG   ti_ChainDepth;      /* certificates the server sent               */
    ULONG   ti_HandshakeMillis; /* how long TLSOpen() spent shaking hands     */
    LONG    ti_Error;           /* the last TLS_ERR_* on this connection      */

    BOOL    ti_Verified;        /* the chain reached a root in the trust store */

    /* FALSE when the machine's clock is unset and the certificate's validity
       dates were therefore not checked.  ti_UnixTime is what we believed the
       time was, or 0. */
    BOOL    ti_ExpiryChecked;
    ULONG   ti_UnixTime;

    ULONG   ti_TrustRoots;      /* roots the store holds                      */
    ULONG   ti_RootsLoaded;     /* roots this connection actually parsed      */

    /* Added in library version 2.  A version-1 library leaves these untouched,
       which is why the note on ti_Size says to zero the structure. */

    /* TRUE when this handshake resumed a cached session: no certificate, no
       signature, no key exchange.  ti_HandshakeMillis measures it. */
    BOOL    ti_Resumed;

    /* FALSE when resumption was switched off for this connection, either by
       TLSA_NoResume or because there was no TLSA_HostName to key a cache on. */
    BOOL    ti_Resumable;

    /* Sessions the library currently holds, across all hosts and all
       programs.  Diagnostic. */
    ULONG   ti_SessionsCached;
};

/* The size of the structure before ti_Resumed existed.  Forty and not
 * forty-four: BOOL on classic AmigaOS is a SHORT.  The library asserts this
 * number against the real offset at build time. */
#define TLS_INFO_SIZE_V1    40

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

    /* NULL-terminated array of the TLS connections whose descriptors appear in
       ts_Read.  Any of them already holding plaintext makes this return at once
       with that descriptor set.  May be NULL. */
    struct TLSConnection **ts_Connections;
};

/* --------------------------------------------------------------- vectors --- */

/* Since version 1. */
#define TLS_LVO_TLSOpenA        (-30)
#define TLS_LVO_TLSClose        (-36)
#define TLS_LVO_TLSRead         (-42)
#define TLS_LVO_TLSWrite        (-48)
#define TLS_LVO_TLSPending      (-54)
#define TLS_LVO_TLSInfo         (-60)
#define TLS_LVO_TLSErrorString  (-66)
#define TLS_LVO_TLSWaitSelect   (-72)

/* Since version 2.  A program calling either of these must open the library
   with 2, or it will jump past the end of an older library's jump table. */
#define TLS_LVO_TLSRandom       (-78)
#define TLS_LVO_TLSBuffered     (-84)

/* The last vector, so a caller that wants to check rather than trust can
   compare against lib_NegSize. */
#define TLS_LVO_LAST            TLS_LVO_TLSBuffered

/* Everything below is m68k assembly against the library's jump table, which a
 * host compiler cannot parse.  Define TLSLIB_NO_INLINE_STUBS to get the tags,
 * the error codes and the LVOs without it. */
#ifndef TLSLIB_NO_INLINE_STUBS

/* Inline stubs, each taking the library base explicitly so a program can hold
 * two.  a0 and a1 must be "+r" and not plain inputs: d0, d1, a0 and a1 are
 * scratch in the AmigaOS ABI, so as inputs the compiler reuses whatever the
 * previous call left in them. */

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
                      : "=r" (res), "+r" (a0), "+r" (a1)
                      : "r" (a6), "r" (d0)
                      : "d1", "cc", "memory");
    return res;
}

/* The varargs spelling; the TAG_END terminator is appended for you.  At least
 * one tag is required -- call TLSOpenA(base, sockbase, sock, NULL) for the
 * defaults and nothing else. */
#define TLSOpen(base, sockbase, sock, ...)                                  \
    ({ const ULONG _tlstags[] = { __VA_ARGS__, TAG_END, 0 };                \
       TLSOpenA((base), (sockbase), (sock),                                 \
                (const struct TagItem *)_tlstags); })

static __inline VOID TLSClose(struct Library *base, struct TLSConnection *conn)
{
    register struct Library       *a6 __asm("a6") = base;
    register struct TLSConnection *a0 __asm("a0") = conn;

    __asm __volatile ("jsr a6@(-36:W)"
                      : "+r" (a0)
                      : "r" (a6)
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
                      : "=r" (res), "+r" (a0), "+r" (a1)
                      : "r" (a6), "r" (d0)
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
                      : "=r" (res), "+r" (a0), "+r" (a1)
                      : "r" (a6), "r" (d0)
                      : "d1", "cc", "memory");
    return res;
}

static __inline LONG TLSPending(struct Library *base, struct TLSConnection *conn)
{
    register struct Library       *a6 __asm("a6") = base;
    register struct TLSConnection *a0 __asm("a0") = conn;
    register LONG                  res __asm("d0");

    __asm __volatile ("jsr a6@(-54:W)"
                      : "=r" (res), "+r" (a0)
                      : "r" (a6)
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
                      : "=r" (res), "+r" (a0), "+r" (a1)
                      : "r" (a6)
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
                      : "=r" (res), "+r" (a0)
                      : "r" (a6)
                      : "d1", "a1", "cc", "memory");
    return res;
}

/* Bytes this library is holding that have not been decrypted yet, or -1.
 * Non-zero means TLSRead() can make progress without another byte arriving; it
 * does NOT mean TLSRead() will not block, since that may be half a record. */
static __inline LONG TLSBuffered(struct Library *base,
                                 struct TLSConnection *conn)
{
    register struct Library       *a6 __asm("a6") = base;
    register struct TLSConnection *a0 __asm("a0") = conn;
    register LONG                  res __asm("d0");

    __asm __volatile ("jsr a6@(-84:W)"
                      : "=r" (res), "+r" (a0)
                      : "r" (a6)
                      : "d1", "a1", "cc", "memory");
    return res;
}

/* Fill a buffer with random bytes.  Returns the count, or -1.  The pool lives
 * in bsdsocket.library, so this requires a connection to have been opened at
 * least once in this program; before that it answers -1 rather than zeroes. */
static __inline LONG TLSRandom(struct Library *base, APTR buffer, LONG length)
{
    register struct Library *a6 __asm("a6") = base;
    register APTR            a0 __asm("a0") = buffer;
    register LONG            d0 __asm("d0") = length;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-78:W)"
                      : "=r" (res), "+r" (a0)
                      : "r" (a6), "r" (d0)
                      : "d1", "a1", "cc", "memory");
    return res;
}

#endif /* TLSLIB_NO_INLINE_STUBS */

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_TLSLIB_H */
