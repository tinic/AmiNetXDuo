/*
 * tls.library, internals.
 *
 * The public contract is include/aminetxduo/tlslib.h; nothing here is visible
 * to a caller.
 *
 *   One master base, one semaphore, and everything else hanging off a
 *   TLSConnection.  Unlike bsdsocket.library there is no per-opener state to
 *   keep apart, no errno, no descriptor table, so OpenLibrary() hands back
 *   the same base every time and the connection is the only object with
 *   identity.
 *
 *   A TLSConnection owns a whole handshake's worth of memory (about 40 KB),
 *   allocated at TLSOpen() rather than reserved up front, so a machine that
 *   never makes a TLS connection pays for none of it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLSLIB_INTERNAL_H
#define AMINETXDUO_TLSLIB_INTERNAL_H

/*
 * tx_api.h must come first: port/threadx-amiga/inc/tx_port.h typedefs VOID,
 * and so does <exec/types.h>.  Whichever comes second loses, and the error
 * ("two or more data types in declaration specifiers") points a hundred lines
 * away from the include that caused it.  tests/tls/tls_https.c has the same
 * ordering for the same reason.
 */
#include "tx_api.h"
#include "nx_api.h"
#include "nx_secure_tls.h"
#include "nx_secure_x509.h"

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <dos/dos.h>

#include "aminetxduo/netstack.h"
#include "aminetxduo/nxcontext.h"
#include "aminetxduo/tlslib.h"

/* ------------------------------------------------------------- sizing --- */

/*
 * How many roots one chain may pull out of the store.  One is the answer in
 * every ordinary case: the lazy loader matches the issuer of each received
 * certificate against the store by exact DER name, so a two-deep chain
 * produces one hit.  Two covers a cross-signed chain where an intermediate is
 * also present as a root.
 */
#define TLS_MAX_ROOTS               2

/*
 * Largest root DER we will read out of the store.  The Mozilla set's biggest
 * is 2007 bytes; tools/mkcertstore.py refuses to emit anything larger than
 * this, so the two numbers cannot drift apart silently.
 */
#define TLS_ROOT_DER_MAX            2560

/* Per received certificate.  A public leaf plus SANs runs to ~1.6 KB. */
#define TLS_REMOTE_DER_MAX          2560

#define TLS_DEFAULT_CHAIN           4
#define TLS_MAX_CHAIN               8
#define TLS_DEFAULT_RECORD_BUFFER   10240
#define TLS_MIN_RECORD_BUFFER       4096
#define TLS_DEFAULT_TIMEOUT_MS      120000UL

/* One TLSWrite() turns into records of at most this many plaintext bytes.
   The protocol allows 16 KB; a 1 MB machine allocating 16 KB of packets for
   an HTTP GET does not want it. */
#define TLS_WRITE_CHUNK             2048

#define TLS_STORE_PATH_MAX          160
#define TLS_DEFAULT_STORE           "DEVS:Internet/certificates"
#define TLS_DEFAULT_SESSIONS        "DEVS:Internet/tlssessions"

/* ------------------------------------------------------- resumption --- */

/*
 * A resumed TLS 1.2 handshake performs no public-key arithmetic, which on this
 * machine is the difference between 6.8 s / 23.3 s and a fraction of a second.
 * See src/tlslib/tls_resume.c for the mechanism and the evidence behind
 * choosing RFC 5077 tickets over session IDs.
 */

#define TLS_MASTER_SECRET_SIZE      48
#define TLS_RESUME_SID_MAX          32
#define TLS_RESUME_HOST_MAX         64
#define TLS_RESUME_SLOTS            8

/*
 * Largest session ticket we will keep.  Measured on the hosts this was
 * developed against: Cloudflare and nginx both issue 192 bytes, Google 240.
 *
 * The ceiling comes from the ClientHello budget below.  A ClientHello from
 * this library runs to about 135 bytes plus the host name, so 256 bytes of
 * ticket plus 32 of session ID leaves roughly seventy bytes of slack against
 * the 500-byte cache even for a sixty-character host name.  A ticket that does
 * not fit is not offered, which silently costs a full handshake and nothing
 * worse, so the cap is set where nothing measured comes close to it.
 */
#define TLS_RESUME_TICKET_MAX       256

/*
 * NX_SECURE_TLS_SESSION carries `UCHAR nx_secure_tls_handshake_cache[500]`,
 * and _nx_secure_tls_send_handshake_record() memcpy()s the whole ClientHello
 * into it with no bounds check.  Everything this library appends to a
 * ClientHello is therefore checked against this and dropped rather than
 * written; otherwise it is a silent overwrite of the session struct on a
 * machine with no memory protection.
 */
#define TLS_CLIENTHELLO_CACHE_MAX   500

/* One cached session.  Fixed-size because it is also the on-disk record, and
   a fixed stride means the file cannot be mis-parsed. */
typedef struct TLSResumeEntry
{
    char    re_Host[TLS_RESUME_HOST_MAX];   /* NUL-terminated, as connected  */
    UWORD   re_Port;
    UWORD   re_CipherSuite;
    UWORD   re_Protocol;
    UBYTE   re_SidLength;
    UBYTE   re_Valid;
    UBYTE   re_Flags;                       /* TLSRE_*                       */
    UBYTE   re_MaxChain;                    /* certificates this caller allowed */
    ULONG   re_TrustKey;                    /* every trust parameter, folded */
    UBYTE   re_Sid[TLS_RESUME_SID_MAX];
    ULONG   re_Stamp;                       /* UNIX seconds, 0 = no clock    */
    ULONG   re_Lifetime;                    /* server's hint, seconds        */
    ULONG   re_Serial;                      /* LRU order                     */
    UBYTE   re_Master[TLS_MASTER_SECRET_SIZE];
    UWORD   re_TicketLength;
    UBYTE   re_Ticket[TLS_RESUME_TICKET_MAX];
} TLSResumeEntry;

/*
 * In re_Flags, and folded into re_TrustKey along with everything else that
 * decides what a connection is worth.
 *
 * TLSRE_VERIFIED: a session established without chain and host-name
 * verification must never be resumed by a connection that asked for
 * verification, that would let a program which used TLSA_NoVerify to reach a
 * printer poison the cache for every program after it.
 *
 * TLSRE_DATED: whether the certificate validity dates were checked.  On a
 * machine with no clock they are skipped (tls_time.c), and a session
 * established that way must not be resumed once the clock has been set, or
 * setting the clock silently fails to start checking expiry.
 */
#define TLSRE_VERIFIED      (1U << 0)
#define TLSRE_DATED         (1U << 1)

/* Per-connection resumption state, in tc_ResumeFlags. */
#define TLSR_ENABLED        (1UL << 0)  /* the machinery is on for this one  */
#define TLSR_OFFERED        (1UL << 1)  /* a cached session went on the wire */
#define TLSR_RESUMED        (1UL << 2)  /* the server took it                */
#define TLSR_TICKET_NEW     (1UL << 3)  /* a NewSessionTicket arrived        */
#define TLSR_PERSIST        (1UL << 4)  /* mirror the cache to disk          */

/* -------------------------------------------------------- trust store --- */

/*
 * The on-disk index, one entry per root, sorted by key.  See tls_store.c for
 * the file layout and tools/mkcertstore.py for the generator.
 */
typedef struct TLSStoreEntry
{
    ULONG   se_Key;         /* FNV-1a 32 over the subject Name DER  */
    ULONG   se_Offset;      /* absolute file offset of the cert DER */
    ULONG   se_Length;
} TLSStoreEntry;

/*
 * The index lives in the connection, not in the library base, and is read
 * fresh at every TLSOpen().
 *
 * Caching it in the base fails twice over.  It needs reload detection, because
 * replacing the file is the whole update story and a resident library would
 * otherwise keep serving the old roots.  And it puts a pointer that one task
 * can free, a second TLSOpen() with a different TLSA_TrustStore, or after
 * the file changed, under a pointer another task is reading from inside a
 * handshake, on a machine with no memory protection.
 *
 * A per-connection index costs 1,428 bytes for the Mozilla set, against the
 * ~40 KB the connection already needs, and one 1.4 KB read against a handshake
 * that spends seconds on arithmetic.  In exchange the reload question does not
 * exist (every connection reads the current file) and neither does the sharing.
 */
typedef struct TLSStore
{
    char            ts_Path[TLS_STORE_PATH_MAX];
    TLSStoreEntry  *ts_Index;
    ULONG           ts_Count;

    /*
     * A fingerprint of which roots this store holds, FNV-1a over the count
     * and every index record.  Computed once when the index is read, the only
     * moment the whole of it is in memory, so it costs one pass over 1,428
     * bytes that were just read off the disk.
     *
     * A resumed handshake verifies nothing, so the cached session has to be
     * keyed by the trust decision that established it and not merely by the
     * fact that one happened.  See tls_resume.c.
     */
    ULONG           ts_Fingerprint;
} TLSStore;

/* ---------------------------------------------------------------- base --- */

struct TLSLibBase
{
    struct Library          tb_Lib;
    UWORD                   tb_Pad;
    APTR                    tb_SegList;

    /*
     * Five longwords that let tools/profiler find this library's seglist, so
     * a profile of a program using tls.library names functions inside it
     * rather than showing the whole library as one bar.  Exec publishes
     * struct Library and nothing after it, so the seglist sits at an offset
     * only this source knows; the profiler scans the base's positive half for
     * the magic and checks tb_ProfLibBase against where it found the record.
     * See tools/profiler/prof.h, "HOW A LIBRARY LETS ITS SEGLIST BE FOUND".
     */
    ULONG                   tb_ProfMagic;
    ULONG                   tb_ProfSize;
    ULONG                   tb_ProfLibBase;
    ULONG                   tb_ProfSegList;
    ULONG                   tb_ProfSum;
    struct ExecBase        *tb_SysBase;

    /*
     * Guards the one-time crypto-table build and the session cache below.
     * Everything else this library has belongs to a connection.
     */
    struct SignalSemaphore  tb_Lock;

    BOOL                    tb_CryptoReady;   /* ami_tls_crypto_initialize() */

    /*
     * The resumption cache lives here rather than in a connection: a curl
     * invocation is a process, and the second invocation is a different
     * process, so a cache with a connection's lifetime would resume nothing.
     * A shared library base outlives every program that opens it, so this is
     * the state that makes a second fetch fast.  Allocated on first use, so a
     * machine that never completes a handshake never pays for it.
     */
    TLSResumeEntry         *tb_Sessions;      /* TLS_RESUME_SLOTS entries    */
    ULONG                   tb_SessionSerial;

    /*
     * Which file the resident cache came from, so a connection asking for a
     * different TLSA_SessionFile gets that file's sessions instead of the
     * previous one's.  Without this the tag would be honoured on the write and
     * ignored on the read.  Empty with tb_SessionsLoaded set means RAM only,
     * nothing to reload.
     */
    char                    tb_SessionPath[TLS_STORE_PATH_MAX];
    BOOL                    tb_SessionsLoaded;
};

/* ---------------------------------------------------------- connection --- */

#define TLSF_VERIFY         (1UL << 0)   /* chain + host name are checked    */
#define TLSF_HANDSHAKEN     (1UL << 1)
#define TLSF_EOF            (1UL << 2)
#define TLSF_BROKEN         (1UL << 3)

struct TLSConnection
{
    struct MinNode              tc_Node;
    struct TLSLibBase          *tc_Base;

    APTR                        tc_SocketBase;
    LONG                        tc_Fd;
    NX_TCP_SOCKET              *tc_Socket;

    ULONG                       tc_Flags;
    LONG                        tc_Error;
    ULONG                       tc_Timeout;         /* NetX ticks, 0 = block */

    NX_SECURE_TLS_SESSION       tc_Session;
    NX_SECURE_X509_DNS_NAME     tc_Sni;
    UCHAR                       tc_HostName[NX_SECURE_X509_DNS_NAME_MAX + 1];
    USHORT                      tc_HostNameLength;

    AmiNetCaller                tc_Caller;
    LONG                        tc_Nest;

    UCHAR                      *tc_Metadata;
    ULONG                       tc_MetadataSize;
    UCHAR                      *tc_RecordBuffer;
    ULONG                       tc_RecordBufferSize;

    NX_SECURE_X509_CERT        *tc_Remote;
    UCHAR                      *tc_RemoteDer;
    ULONG                       tc_RemoteCount;

    NX_SECURE_X509_CERT         tc_Root[TLS_MAX_ROOTS];
    UCHAR                      *tc_RootDer;
    ULONG                       tc_RootsLoaded;
    ULONG                       tc_RootKey[TLS_MAX_ROOTS];

    /* Plaintext already decrypted and not yet handed to the caller, why
       TLSPending() and TLSWaitSelect() exist. */
    NX_PACKET                  *tc_Pending;
    ULONG                       tc_PendingOffset;

    TLSStore                    tc_StoreIndex;
    TLSStore                   *tc_Store;      /* == &tc_StoreIndex */
    char                        tc_StorePath[TLS_STORE_PATH_MAX];

    /* ---- resumption, see tls_resume.c ---------------------------------- */

    ULONG                       tc_ResumeFlags;
    UWORD                       tc_Port;

    /* What went into the ClientHello, kept so the ServerHello's echo can be
       compared against it, that echo is the acceptance signal. */
    UBYTE                       tc_OfferSid[TLS_RESUME_SID_MAX];
    UBYTE                       tc_OfferSidLength;

    /* The cached master secret, restored into the session when the server
       accepts.  A copy rather than a pointer into the cache: the cache is
       shared, this connection may run for seconds, and there is no memory
       protection to catch a use-after-free. */
    UBYTE                       tc_Master[TLS_MASTER_SECRET_SIZE];

    UWORD                       tc_TicketLength;
    ULONG                       tc_TicketLifetime;
    UBYTE                      *tc_Ticket;      /* TLS_RESUME_TICKET_MAX     */

    char                        tc_SessionPath[TLS_STORE_PATH_MAX];

    ULONG                       tc_HandshakeMillis;
    ULONG                       tc_UnixTime;
    BOOL                        tc_ExpiryChecked;
    ULONG                       tc_ChainDepth;
    ULONG                       tc_CipherSuite;
    ULONG                       tc_Protocol;
};

typedef struct TLSConnection TLSConnection;

/* ------------------------------------------------------------ tls_netx.c, */

/*
 * The stack, borrowed from bsdsocket.library.  tls_netx_bind() is called once
 * per library open with the caller's SocketBase; it is idempotent, and after
 * the first success tls_netx_ctx() is non-NULL for the life of the library.
 */
LONG                     tls_netx_bind(APTR socket_base);
const AmiNetXDuoContext *tls_netx_ctx(VOID);

/* ---------------------------------------------------------- tls_store.c, */

LONG  tls_store_open(TLSStore *store, const char *path);
VOID  tls_store_close(TLSStore *store);
ULONG tls_store_count(const TLSStore *store);

/*
 * Read the root whose subject Name hashes to `key`.  Returns the DER length,
 * or 0 if there is no such root.  This is the only path that touches the disk
 * during a handshake, and it happens at most twice.
 */

/* FNV-1a over a certificate's issuer Name DER, walked out of the raw
   certificate.  0 means not found, which no real key ever is because
   tools/mkcertstore.py rejects that hash. */

/*
 * Put the connection in the session->connection registry.  Every connection
 * needs this, verified or not: it is how the vendored-override layer in
 * tls_resume.c gets back from an NX_SECURE_TLS_SESSION to the connection.
 */
VOID  tls_registry_add(TLSConnection *conn);

/* Install the lazy-loading verifier on a session.  Must be called after
   _nx_secure_tls_session_create() and after tls_registry_add();
   tls_store_detach() before the session is deleted. */
VOID  tls_store_attach(TLSConnection *conn);
VOID  tls_store_detach(TLSConnection *conn);

/* The connection behind a session, for the callbacks nx_secure hands only a
   session pointer to. */
TLSConnection *tls_conn_for_session(const NX_SECURE_TLS_SESSION *session);

/* ----------------------------------------------------------- tls_time.c, */

/*
 * The value handed to nx_secure as "now", in UNIX seconds, or 0, which is
 * nx_secure's own encoding for "do not check validity dates".  See tls_time.c.
 */
ULONG tls_time_now(VOID);
BOOL  tls_time_is_known(VOID);

/*
 * DateStamp() with nothing applied to it: wall time on a machine with a clock,
 * seconds since boot on one without.  Elapsed time either way, which is what
 * ageing a cached session needs.  TLS_CLOCK_FLOOR tells the two apart.
 */
ULONG tls_time_monotonic(VOID);

/* TLS_CLOCK_FLOOR, TLS_RESUME_MAX_AGE and tls_expired() live here.  The header
   includes nothing, so it costs this one nothing to pull in. */
#include "tls_expiry.h"

/* -------------------------------------------------------- tls_runtime.c, */

BOOL  tls_runtime_open(VOID);
VOID  tls_runtime_close(VOID);

APTR  tls_alloc(ULONG size);
VOID  tls_free(APTR ptr);
VOID  tls_bzero(APTR ptr, ULONG size);
ULONG tls_strlen(const char *s);
VOID  tls_strncpy(char *dst, const char *src, ULONG size);

extern struct ExecBase   *SysBase;
extern struct DosLibrary *DOSBase;

/* ----------------------------------------------------------- tls_conn.c, */


/*
 * Serial-port tracing, compiled in only by -DTLS_RESUME_TRACE.  It lives in
 * tls_resume.c, where most of it is used; a shipping build neither calls nor
 * contains it.
 */
#ifdef TLS_RESUME_TRACE
VOID tls_trace(const char *fmt, ...);
#else
#  define tls_trace(...)    ((VOID)0)
#endif

/* --------------------------------------------------------- tls_resume.c, */

/* Before the handshake: find a cached session for this host and arm the
   connection to offer it.  Silent no-op when there is nothing to offer. */
VOID  tls_resume_prepare(TLSConnection *conn);

/* After a successful handshake: record what was learned, a new master
   secret and session ID, a new ticket, or a refreshed entry after a
   resumption.  Writes the disk mirror when the cache changed. */
VOID  tls_resume_record(TLSConnection *conn);

/* Drop the cached session for this host.  Called when a resumption went
   wrong, so the next attempt is a clean full handshake. */
VOID  tls_resume_evict(TLSConnection *conn);

/* How many sessions the cache holds, for TLSInfo(). */
ULONG tls_resume_count(struct TLSLibBase *base);

#endif /* AMINETXDUO_TLSLIB_INTERNAL_H */
