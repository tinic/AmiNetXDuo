/*
 * tls.library -- internals.
 *
 * The public contract is include/aminetxduo/tlslib.h; nothing here is visible
 * to a caller.
 *
 * SHAPE
 *
 *   One master base, one semaphore, and everything else hanging off a
 *   TLSConnection.  Unlike bsdsocket.library there is no per-opener state to
 *   keep apart -- no errno, no descriptor table -- so OpenLibrary() hands back
 *   the same base every time and the connection is the only object with
 *   identity.
 *
 *   A TLSConnection owns a whole handshake's worth of memory (about 40 KB) and
 *   is allocated at TLSOpen() rather than reserved up front, because a machine
 *   that never makes a TLS connection should pay for none of it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_TLSLIB_INTERNAL_H
#define AMINETXDUO_TLSLIB_INTERNAL_H

/*
 * tx_api.h FIRST, and this is not a style choice: port/threadx-amiga/inc/
 * tx_port.h typedefs VOID, and so does <exec/types.h>.  Whichever comes second
 * loses, and the error ("two or more data types in declaration specifiers") is
 * a hundred lines away from the include that caused it.  tests/tls/tls_https.c
 * has the same ordering for the same reason.
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
   The protocol allows 16 KB; a 4 MB machine allocating 16 KB of packets for
   an HTTP GET does not want it. */
#define TLS_WRITE_CHUNK             2048

#define TLS_STORE_PATH_MAX          160
#define TLS_DEFAULT_STORE           "DEVS:Internet/certificates"
#define TLS_DEFAULT_SESSIONS        "DEVS:Internet/tlssessions"

/* ------------------------------------------------------- resumption --- */

/*
 * A resumed TLS 1.2 handshake performs NO public-key arithmetic at all, which
 * on this machine is the difference between 6.8 s / 23.3 s and a fraction of a
 * second.  See src/tlslib/tls_resume.c for the mechanism and the evidence
 * behind choosing RFC 5077 tickets over session IDs.
 */

#define TLS_MASTER_SECRET_SIZE      48
#define TLS_RESUME_SID_MAX          32
#define TLS_RESUME_HOST_MAX         64
#define TLS_RESUME_SLOTS            8

/*
 * Largest session ticket we will keep.  Measured on the hosts this was
 * developed against: Cloudflare and nginx both issue 192 bytes, Google 240.
 *
 * The ceiling is not politeness, it is the ClientHello budget below.  A
 * ClientHello from this library runs to about 135 bytes plus the host name, so
 * 256 bytes of ticket plus 32 of session ID leaves roughly seventy bytes of
 * slack against the 500-byte cache even for a sixty-character host name.  A
 * ticket that does not fit is not offered, which costs a full handshake and
 * nothing worse -- but it would cost it SILENTLY, so the cap is set where
 * nothing measured comes close to it.
 */
#define TLS_RESUME_TICKET_MAX       256

/*
 * NX_SECURE_TLS_SESSION carries `UCHAR nx_secure_tls_handshake_cache[500]`,
 * and _nx_secure_tls_send_handshake_record() memcpy()s the whole ClientHello
 * into it with NO bounds check.  Everything this library appends to a
 * ClientHello is therefore checked against this, and dropped rather than
 * written, because the alternative is a silent overwrite of the session
 * struct on a machine with no memory protection.
 */
#define TLS_CLIENTHELLO_CACHE_MAX   500

/* One cached session.  Deliberately fixed-size: it is also the on-disk
   record, and a fixed stride means the file cannot be mis-parsed. */
typedef struct TLSResumeEntry
{
    char    re_Host[TLS_RESUME_HOST_MAX];   /* NUL-terminated, as connected  */
    UWORD   re_Port;
    UWORD   re_CipherSuite;
    UWORD   re_Protocol;
    UBYTE   re_SidLength;
    UBYTE   re_Valid;
    UBYTE   re_Flags;                       /* TLSRE_*                       */
    UBYTE   re_Sid[TLS_RESUME_SID_MAX];
    ULONG   re_Stamp;                       /* UNIX seconds, 0 = no clock    */
    ULONG   re_Lifetime;                    /* server's hint, seconds        */
    ULONG   re_Serial;                      /* LRU order                     */
    UBYTE   re_Master[TLS_MASTER_SECRET_SIZE];
    UWORD   re_TicketLength;
    UBYTE   re_Ticket[TLS_RESUME_TICKET_MAX];
} TLSResumeEntry;

/*
 * In re_Flags.  A session established WITHOUT chain and host-name verification
 * must never be resumed by a connection that asked for verification -- that
 * would let a program which used TLSA_NoVerify to reach a printer poison the
 * cache for every program that comes after it.  The flag is part of the match,
 * so the two populations never mix.
 */
#define TLSRE_VERIFIED      (1U << 0)

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
 * The index lives in the CONNECTION, not in the library base, and is read
 * fresh at every TLSOpen().
 *
 * Caching it in the base was the obvious design and was wrong twice over.  It
 * needs reload detection, because "replace the file" is the whole update story
 * and a resident library would otherwise keep serving the old roots.  And it
 * puts a pointer that one task can free -- a second TLSOpen() with a different
 * TLSA_TrustStore, or after the file changed -- under a pointer another task is
 * reading from inside a handshake, on a machine with no memory protection.
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
} TLSStore;

/* ---------------------------------------------------------------- base --- */

struct TLSLibBase
{
    struct Library          tb_Lib;
    UWORD                   tb_Pad;
    APTR                    tb_SegList;
    struct ExecBase        *tb_SysBase;

    /*
     * Guards the one-time crypto-table build and the session cache below.
     * Everything else this library has belongs to a connection.
     */
    struct SignalSemaphore  tb_Lock;

    BOOL                    tb_CryptoReady;   /* ami_tls_crypto_initialize() */

    /*
     * The resumption cache, and the reason it lives HERE rather than in a
     * connection: a curl invocation is a process, and the second invocation is
     * a different process.  A cache with a connection's lifetime would resume
     * nothing that anybody actually does.  A shared library base outlives every
     * program that opens it, so this is the state that makes "run fetch twice"
     * fast.  Allocated on first use, so a machine that never completes a
     * handshake never pays for it.
     */
    TLSResumeEntry         *tb_Sessions;      /* TLS_RESUME_SLOTS entries    */
    ULONG                   tb_SessionSerial;

    /*
     * Which file the resident cache came from, so that a connection asking for
     * a DIFFERENT TLSA_SessionFile gets that file's sessions instead of the
     * previous one's.  Without this the tag would be honoured on the write and
     * ignored on the read, which is worse than not having it.  Empty and
     * tb_SessionsLoaded set means "RAM only, nothing to reload".
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

    /* Plaintext already decrypted and not yet handed to the caller.  This is
       the whole reason TLSPending() and TLSWaitSelect() exist. */
    NX_PACKET                  *tc_Pending;
    ULONG                       tc_PendingOffset;

    TLSStore                    tc_StoreIndex;
    TLSStore                   *tc_Store;      /* == &tc_StoreIndex */
    char                        tc_StorePath[TLS_STORE_PATH_MAX];

    /* ---- resumption, see tls_resume.c ---------------------------------- */

    ULONG                       tc_ResumeFlags;
    UWORD                       tc_Port;

    /* What we put in the ClientHello, kept so the ServerHello's echo can be
       compared against it -- that echo is the whole acceptance signal. */
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

/* ------------------------------------------------------------ tls_netx.c -- */

/*
 * The stack, borrowed from bsdsocket.library.  tls_netx_bind() is called once
 * per library open with the caller's SocketBase; it is idempotent, and after
 * the first success tls_netx_ctx() is non-NULL for the life of the library.
 */
LONG                     tls_netx_bind(APTR socket_base);
const AmiNetXDuoContext *tls_netx_ctx(VOID);

/* ---------------------------------------------------------- tls_store.c -- */

LONG  tls_store_open(TLSStore *store, const char *path);
VOID  tls_store_close(TLSStore *store);
ULONG tls_store_count(const TLSStore *store);

/*
 * Read the root whose subject Name hashes to `key`.  Returns the DER length,
 * or 0 if there is no such root.  This is the only path that touches the disk
 * during a handshake, and it happens at most twice.
 */
ULONG tls_store_fetch(TLSStore *store, ULONG key, UCHAR *buffer, ULONG size);

/* FNV-1a over a certificate's issuer Name DER, walked out of the raw
   certificate.  0 means "could not find it", which no real key ever is
   because tools/mkcertstore.py rejects that hash. */
ULONG tls_cert_issuer_key(const NX_SECURE_X509_CERT *cert);

/*
 * Put the connection in the session->connection registry.  Every connection
 * needs this, verified or not: it is how the vendored-override layer in
 * tls_resume.c finds its way back from an NX_SECURE_TLS_SESSION.
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

/* ----------------------------------------------------------- tls_time.c -- */

/*
 * The value handed to nx_secure as "now", in UNIX seconds -- or 0, which is
 * nx_secure's own encoding for "do not check validity dates".  See tls_time.c
 * for the argument.
 */
ULONG tls_time_now(VOID);
BOOL  tls_time_is_known(VOID);

/* -------------------------------------------------------- tls_runtime.c -- */

BOOL  tls_runtime_open(VOID);
VOID  tls_runtime_close(VOID);

APTR  tls_alloc(ULONG size);
VOID  tls_free(APTR ptr);
VOID  tls_bzero(APTR ptr, ULONG size);
ULONG tls_strlen(const char *s);
VOID  tls_strncpy(char *dst, const char *src, ULONG size);

extern struct ExecBase   *SysBase;
extern struct DosLibrary *DOSBase;

/* ----------------------------------------------------------- tls_conn.c -- */

LONG  tls_conn_enter(TLSConnection *conn);
VOID  tls_conn_leave(TLSConnection *conn);
LONG  tls_error_from_nx(UINT status);

/*
 * The serial-port commentary, compiled in only by -DTLS_RESUME_TRACE.  It
 * lives in tls_resume.c because that is where most of it is used; nothing in a
 * shipping build calls it, and nothing in a shipping build contains it.
 */
#ifdef TLS_RESUME_TRACE
VOID tls_trace(const char *fmt, ...);
#else
#  define tls_trace(...)    ((VOID)0)
#endif

/* --------------------------------------------------------- tls_resume.c -- */

/* Before the handshake: find a cached session for this host and arm the
   connection to offer it.  Silent no-op when there is nothing to offer. */
VOID  tls_resume_prepare(TLSConnection *conn);

/* After a successful handshake: record what was learned -- a new master
   secret and session ID, a new ticket, or a refreshed entry after a
   resumption.  Writes the disk mirror when the cache changed. */
VOID  tls_resume_record(TLSConnection *conn);

/* Drop the cached session for this host.  Called when a resumption went
   wrong, so the next attempt is a clean full handshake. */
VOID  tls_resume_evict(TLSConnection *conn);

/* How many sessions the cache holds, for TLSInfo(). */
ULONG tls_resume_count(struct TLSLibBase *base);

#endif /* AMINETXDUO_TLSLIB_INTERNAL_H */
