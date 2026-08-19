/*
 * tls.library, TLS for ordinary Amiga programs.
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
 *       TLSClose(TLSBase, tls);, the descriptor is yours again
 *       CloseLibrary(TLSBase);
 *       CloseLibrary(SocketBase);
 *
 *   The chain is verified against DEVS:Internet/certificates and the host name
 *   is checked against the certificate, both by default.  There is no
 *   "insecure by accident" mode: TLSA_NoVerify has to be asked for, in those
 *   words.
 *
 *   A full handshake to a public HTTPS server is about seven seconds on a
 *   14 MHz 68020 for an RSA certificate chain and about twenty-three for an
 *   ECDSA one, nearly all of it public-key arithmetic, one signature
 *   verification per certificate, plus a key exchange.  Budget roughly 40 KB of
 *   Fast RAM per open connection.  See docs/RESEARCH.md 9.
 *
 *   A resumed handshake does none of that arithmetic, and this library resumes
 *   automatically: there is no API for it, and the second connection to a host
 *   you have already visited takes a fraction of a second instead of seven or
 *   twenty-three.  The cache lives in the library and in
 *   DEVS:Internet/tlssessions, so it survives your program exiting, running
 *   the same command twice is the case it exists for.  TLSInfo()'s ti_Resumed
 *   says which kind of handshake you got.
 *
 *   What that costs: a cached session holds the master secret for that session
 *   in the library's memory and on disk, in the clear.  On a machine with no
 *   memory protection the memory copy changes little, every task can already
 *   read every other task's memory, but the file means an attacker who takes
 *   the disk can decrypt captured traffic from the resumed sessions, which the
 *   full handshake would not have allowed.  TLSA_NoResume turns it off
 *   entirely; TLSA_SessionFile with an empty string keeps the cache in RAM and
 *   off the disk.
 *
 *   A TLS record is not a byte.  The library reads a whole record off the
 *   socket, decrypts it, and hands you plaintext out of it a bit at a time, so
 *   the socket's readability and the connection's readability are two
 *   different questions:
 *
 *     * WaitSelect() can say not readable while TLSRead() would return data
 *       immediately, because the bytes are already decrypted and sitting in the
 *       library.  A program that waits on the descriptor alone hangs with its
 *       answer already in memory.
 *
 *     * WaitSelect() can say readable while TLSRead() has to block, because
 *       what arrived is the first half of a record and no plaintext can come
 *       out of it until the rest does.
 *
 *   Do not call WaitSelect() on a descriptor you have given to TLSOpen().  Call
 *   TLSWaitSelect(), which takes the same arguments plus the list of TLS
 *   connections involved.  It reports a connection readable if the library is
 *   already holding plaintext for it, returning at once without waiting, and
 *   otherwise passes the whole thing to bsdsocket.library's WaitSelect().
 *   TLSPending() is the same test on its own, if you would rather build the
 *   loop yourself.
 *
 *   The second case, a readable socket that yields no plaintext yet, is not
 *   removable without a non-blocking record layer, and it is bounded: the rest
 *   of a record is already on its way.  TLSA_Timeout puts a ceiling on it.
 *
 *   DEVS:Internet/certificates, a file of certificate authorities the machine
 *   trusts.  Without it TLSOpen() fails with TLS_ERR_TRUSTSTORE rather than
 *   connecting to something it cannot vouch for.  It is read fresh on every
 *   connection, so replacing the file is the whole of the update story: no
 *   reboot, no flushing the library.  tools/mkcertstore.py turns any PEM
 *   bundle into one.
 *
 *   Certificates carry validity dates and a great many Amigas have no working
 *   clock, in which case AmigaOS reports 1978, earlier than every certificate
 *   on the internet.  Rather than refuse every connection on such a machine,
 *   this library skips the validity dates when the clock is obviously unset and
 *   checks them when it is not.  TLSInfo()'s ti_ExpiryChecked says which
 *   happened, so a program that wants to tell the user can.
 *
 *   The chain signature and the host name are checked either way, so an
 *   impostor is still refused; what is given up is any assurance that a
 *   long-since-revoked certificate has stopped working.
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

/*
 * The version is the vector level.
 *
 *   Adding a vector means bumping TLS_LIB_VERSION, with no exceptions.
 *
 *   Exec opens a library when lib_Version >= the version asked for and looks at
 *   nothing else.  A library that grew a vector without growing its version
 *   still answers OpenLibrary(TLS_LIB_NAME, 1), and the caller, compiled
 *   against the newer header, jumps to an offset that library never had.
 *   MakeLibrary() stopped at the (APTR)-1 terminator, so the jump table is not
 *   that long: the jump goes into whatever is in front of the base, on a
 *   machine with no memory protection.  Version 2 shipped one call away from
 *   that.
 *
 *   Done right, the standard mechanism does the work.  A program that needs
 *   TLSBuffered() asks for 2 and is refused by an older library, which is a
 *   legible failure at OpenLibrary() rather than a wild jump later.  A program
 *   that only needs the original eight keeps asking for 1 and keeps working
 *   against every version since.
 *
 *   Ask for what you use, not for TLS_LIB_VERSION.  Passing the constant means
 *   recompiling makes your program demand a library it does not need.
 *
 *   TLS_LIB_REVISION is zero and stays zero.  bsdsocket.library and
 *   usergroup.library report revision 0 for the same reason: nothing in this
 *   project reads a revision, and a number nobody reads goes stale.
 *
 *   1   TLSOpenA TLSClose TLSRead TLSWrite TLSPending TLSInfo
 *       TLSErrorString TLSWaitSelect
 *   2   + TLSRandom, TLSBuffered.  Also the version at which TLSInfo() began
 *       filling ti_Resumed, ti_Resumable and ti_SessionsCached, see the note
 *       on ti_Size, which is what makes asking version 1 for those fields safe.
 */
#define TLS_LIB_VERSION     2
#define TLS_LIB_REVISION    0

/*
 * How many user vectors each version has, and the only place that is written
 * down.  TLS_LIB_VECTORS is derived from TLS_LIB_VERSION rather than maintained
 * beside it, and src/tlslib/tls_vectors.c asserts the real table against it at
 * build time.
 *
 * That turns the rule above into a mechanism: add a vector to the table and the
 * build fails, and the only way to make it pass is to declare a
 * TLS_LIB_VECTORS_V<n> and point TLS_LIB_VERSION at it.  A new vector cannot
 * reach a shipped library without the version moving.
 */
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

/*
 * STRPTR.  The name you connected to.  Sent as SNI (without it a shared-IP
 * host answers with the wrong certificate or refuses outright) and checked
 * against the certificate's subject alternative names.  Required unless
 * TLSA_NoVerify is set: verifying a chain without checking who it is for
 * proves only that somebody has a valid certificate.  Names longer than 100
 * bytes are rejected with TLS_ERR_BADHOSTNAME; they are never truncated into
 * a different identity.
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
 * the connection encrypted and not authenticated: anyone in the path can be the
 * other end.  It exists for cases like a LAN printer with a self-signed
 * certificate; it is not a fallback for when verification fails.
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
 * verification, seconds, on this hardware.
 */
#define TLSA_MaxChain       (TLSA_Dummy + 7)

/*
 * BOOL.  Do not offer a cached session and do not remember this one.  Every
 * connection then pays the full public-key cost, seven seconds for an RSA
 * chain on a 68020, twenty-three for an ECDSA one, so this is for a program
 * that wants forward secrecy on every connection more than it wants speed.
 * Resumption is on by default because on this hardware the trade usually goes
 * the other way.
 */
#define TLSA_NoResume       (TLSA_Dummy + 8)

/*
 * STRPTR.  Where the session cache is mirrored, instead of
 * DEVS:Internet/tlssessions.  An empty string means nowhere: the cache stays in
 * the library, so a second connection from the same or another program still
 * resumes, but nothing survives a reboot and no master secret is written to
 * disk.
 */
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

/* ----------------------------------------------------------------- info --- */

struct TLSInfo
{
    /*
     * Set to sizeof(struct TLSInfo) before the call.
     *
     * Zero the whole structure first if you opened the library with a version
     * older than the one you compiled against.  ti_Size lets an old caller talk
     * to a new library; in the other direction, a new caller asks an old
     * library for fields it has never heard of, and it fills what it knows and
     * leaves the rest of your stack alone.  Zeroed, an older library's silence
     * reads as FALSE and 0, which is correct.  Uninitialised, it reads as
     * whatever was on the stack.
     */
    ULONG   ti_Size;

    ULONG   ti_Version;         /* 0x0303 == TLS 1.2                          */
    ULONG   ti_CipherSuite;     /* the negotiated suite, IANA number          */
    ULONG   ti_ChainDepth;      /* certificates the server sent               */
    ULONG   ti_HandshakeMillis; /* how long TLSOpen() spent shaking hands     */
    LONG    ti_Error;           /* the last TLS_ERR_* on this connection      */

    BOOL    ti_Verified;        /* the chain reached a root in the trust store */

    /*
     * FALSE when the machine's clock is unset and the certificate's validity
     * dates were therefore not checked, see the note in the library's
     * documentation.  ti_UnixTime is what we believed the time was, or 0.
     */
    BOOL    ti_ExpiryChecked;
    ULONG   ti_UnixTime;

    ULONG   ti_TrustRoots;      /* roots the store holds                      */
    ULONG   ti_RootsLoaded;     /* roots this connection actually parsed      */

    /*
     * Added in library version 2.  Set ti_Size to sizeof(struct TLSInfo) and
     * you get them; a program compiled against the older header passes the
     * older size, gets everything above, and is not broken by this.
     *
     * Reading these from a library you opened with version 1 is allowed and is
     * why the note on ti_Size says to zero the structure: a version-1 library
     * leaves them untouched, so zero means "this library cannot resume" rather
     * than whatever the stack happened to hold.
     */

    /*
     * TRUE when this handshake resumed a cached session: no certificate was
     * sent, no signature was verified, no key exchange happened, and the whole
     * thing took a fraction of a second.  ti_HandshakeMillis measures it.
     */
    BOOL    ti_Resumed;

    /* FALSE when resumption was switched off for this connection, either by
       TLSA_NoResume or because there was no TLSA_HostName to key a cache on. */
    BOOL    ti_Resumable;

    /* Sessions the library currently holds, across all hosts and all
       programs.  Diagnostic. */
    ULONG   ti_SessionsCached;
};

/*
 * The size of the structure before ti_Resumed existed.  A caller passing this
 * gets the original fields and nothing else, which is the whole compatibility
 * mechanism and is why ti_Size is a required input.
 *
 * Forty and not forty-four: BOOL on classic AmigaOS is a SHORT, so ti_Verified
 * and ti_ExpiryChecked are two bytes each, not four.  The library asserts this
 * number against the real offset at build time rather than trusting the
 * arithmetic in this comment.
 */
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

    /*
     * NULL-terminated array of the TLS connections whose descriptors appear in
     * ts_Read.  Any of them already holding plaintext makes this return at
     * once with that descriptor set.  May be NULL, in which case this is
     * WaitSelect() with extra steps.
     */
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
   compare against lib_NegSize.  Opening with the right version is the primary
   defence; this is a second one. */
#define TLS_LVO_LAST            TLS_LVO_TLSBuffered

/*
 * Everything below is m68k assembly against the library's jump table, so a
 * host compiler cannot parse it: "a6" is not a register name it knows.  Define
 * TLSLIB_NO_INLINE_STUBS to get the tags, the error codes and the LVOs without
 * it.  src/tlslib/test/test_tls_resume.c is the only thing that does, and it
 * does so because tls_internal.h includes this header and a host test of
 * tls_resume.c needs the rest of it.  Nothing that runs on the Amiga defines
 * it, so the shipping library and every program that links against it see this
 * header exactly as they did.
 */
#ifndef TLSLIB_NO_INLINE_STUBS

/*
 * Inline stubs.  Hand-written rather than generated from an .fd because there
 * is no .fd to generate them from yet, and because a caller should be able to
 * use this library with nothing but this header.  Every one takes the library
 * base explicitly, no global TLSBase, so that a program can hold two, and
 * so that nothing here fights a <proto/> header.
 *
 * a0 and a1 are read-write operands rather than plain inputs.
 *
 *   d0, d1, a0 and a1 are scratch registers in the AmigaOS ABI: a library
 *   function may leave anything in them.  An inline stub that lists a0 and a1
 *   only as inputs tells the compiler the opposite, so two calls in a row get
 *   the arguments loaded once and the second call is made with whatever the
 *   first one left behind.
 *
 *   That happened here: TLSWrite() followed by TLSRead() compiled to one
 *   `moveal a5,a0` before the write and none before the read, so TLSRead() ran
 *   on a garbage connection pointer and answered -1 with no error set.  Marking
 *   them "+r" is accurate, and the generated code reloads them.
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
                      : "=r" (res), "+r" (a0), "+r" (a1)
                      : "r" (a6), "r" (d0)
                      : "d1", "cc", "memory");
    return res;
}

/*
 * The varargs spelling.  Tags are written flat, as pairs, and the terminator
 * is appended for you:
 *
 *     tls = TLSOpen(TLSBase, SocketBase, s,
 *                   TLSA_HostName, (ULONG)"example.com",
 *                   TLSA_Error,    (ULONG)&why);
 *
 * At least one tag is required; call TLSOpenA(base, sockbase, sock, NULL) for
 * the defaults and nothing else.  A statement expression rather than a true
 * varargs stub, because a shared library's ABI is register-based and this
 * header has no .fd to generate a stack-to-register shim from.
 */
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

/*
 * Bytes this library is holding that have not been decrypted yet, or -1.
 *
 * TLSPending() answers whether plaintext is ready; this answers the other half,
 * and a non-blocking caller needs both.  One TCP segment routinely carries more
 * than one TLS record, so after a TLSRead() the rest sit undecrypted inside the
 * library: the socket is not readable, TLSPending() is 0, and a whole record is
 * already in memory.  A loop that waits on the descriptor in that state waits
 * for data it already has.
 *
 * Non-zero means TLSRead() can make progress without another byte arriving.  It
 * does not mean TLSRead() will not block, what is buffered may be half a
 * record, which is what TLSA_Timeout puts a ceiling on.
 */
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

/*
 * Fill a buffer with random bytes.  Returns the count, or -1.
 *
 * This is the machine's one entropy pool, the same SHA-256 hash DRBG the TLS
 * session keys come from, seeded by bsdsocket.library, rather than a second
 * generator of unknown quality.  It is here because a ported client asks its
 * TLS layer for randomness (curl routes every Curl_rand() through it) and the
 * alternative is the client seeding an LCG off the clock.
 *
 * An Amiga has no hardware RNG, and the seed this pool is credited with is
 * around twenty bits on an unattended machine.  That is enough for a nonce, a
 * boundary string or a request id, which is what a client wants it for;
 * docs/RESEARCH.md 9 sets out the limits in full.
 *
 * Requires a connection to have been opened at least once in this program:
 * the pool lives in bsdsocket.library and this library reaches it through the
 * link TLSOpen() establishes.  Before that it answers -1 rather than zeroes.
 */
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
