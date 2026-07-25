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

    /* Guards the one-time crypto-table build below, and nothing else: every
       other piece of state this library has belongs to a connection. */
    struct SignalSemaphore  tb_Lock;

    BOOL                    tb_CryptoReady;   /* ami_tls_crypto_initialize() */
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

/* Install the lazy-loading verifier on a session.  Must be called after
   _nx_secure_tls_session_create(); tls_store_detach() before the session is
   deleted. */
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

#endif /* AMINETXDUO_TLSLIB_INTERNAL_H */
