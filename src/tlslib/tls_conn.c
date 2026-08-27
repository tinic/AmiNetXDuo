/*
 * tls.library, the calls a program makes.  The interface and the example
 * program are in include/aminetxduo/tlslib.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_vectors.h"

#include "tls.h"
#include "ami_tls_crypto.h"

#include <exec/execbase.h>
#include <proto/exec.h>

#ifdef TLS13_PROBE_ON
#  include <inline/macros.h>
#  ifndef RawPutChar
#    define RawPutChar(c) \
        LP1NR(0x204, RawPutChar, UBYTE, (c), d0, , EXEC_BASE_NAME)
#  endif
#  ifndef RawIOInit
#    define RawIOInit() \
        LP0NR(0x1F8, RawIOInit, , EXEC_BASE_NAME)
#  endif
static VOID tls13_putc(UBYTE c)
{
    volatile ULONG spin;

    RawPutChar(c);
    for (spin = 0; spin < 3000UL; spin++)
        ;
}

VOID tls13_probe(const char *tag, ULONG v);

VOID tls13_probe(const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    static INT        ready;
    INT               i;

    if (!ready)
    {
        RawIOInit();
        ready = 1;
    }

    while (*tag != '\0')
        tls13_putc((UBYTE)*tag++);
    for (i = 28; i >= 0; i -= 4)
        tls13_putc((UBYTE)hex[(v >> i) & 0xFUL]);
    tls13_putc((UBYTE)'\n');
}
#  define TLS13_PROBE(tag, v)  tls13_probe((tag), (ULONG)(v))
#else
#  define TLS13_PROBE(tag, v)  do { } while (0)
#endif

#define TLS_FD_BITS         32
#define TLS_FD_WORD(fd)     ((ULONG)(fd) / TLS_FD_BITS)
#define TLS_FD_MASK(fd)     (1UL << ((ULONG)(fd) % TLS_FD_BITS))
/* Must match the largest table bsdsocket.library's SBTC_DTABLESIZE accepts. */
#define TLS_FD_MAX          1024

_Static_assert(__builtin_offsetof(struct TLSInfo, ti_Resumed) == TLS_INFO_SIZE_V1,
               "TLS_INFO_SIZE_V1 must be the offset of the first added field");

/* --------------------------------------------------------------- tags --- */

static ULONG tls_tag_data(const struct TagItem *tags, Tag want, ULONG def)
{
    ULONG found = def;

    while (tags != NULL)
    {
        Tag tag = tags->ti_Tag;

        if (tag == TAG_DONE)
            break;

        if (tag == TAG_IGNORE)
        {
            tags++;
            continue;
        }

        if (tag == TAG_MORE)
        {
            tags = (const struct TagItem *)tags->ti_Data;
            continue;
        }

        if (tag == TAG_SKIP)
        {
            tags += 1 + (LONG)tags->ti_Data;
            continue;
        }

        if (tag == want)
            found = tags->ti_Data;

        tags++;
    }

    return found;
}

/* ------------------------------------------------------------ brackets --- */

/*
 * nx_secure keeps one process-wide list of live sessions and guards it with
 * _nx_secure_tls_protection, which it creates on the first session_create()
 * and takes inside every call that touches the list.  tls.library needs the
 * same lock one level up, for the pairs it performs that nx_secure performs
 * singly: session_end() followed by session_delete() has to be one step, or a
 * second task can delete between them.
 *
 * It is the SAME mutex and not a second one, so the two orderings cannot
 * disagree; the shim in tls_netx.c puts it on an Exec signal semaphore, which
 * is recursive for the owning task, so nx_secure's own nesting still works.
 *
 * NEVER held across the handshake or a record read.  nx_secure drops it around
 * every blocking transport call by design, and an outer holder here would undo
 * that and serialise every TLS connection in the machine behind one.
 */
static LONG tls_conn_enter(TLSConnection *conn)
{
    if (conn == NULL)
        return -1;

    return (_tx_mutex_get(&_nx_secure_tls_protection, TX_WAIT_FOREVER) ==
            TX_SUCCESS) ? 0 : -1;
}

static VOID tls_conn_leave(TLSConnection *conn)
{
    if (conn == NULL)
        return;

    (VOID)_tx_mutex_put(&_nx_secure_tls_protection);
}

/* -------------------------------------------------------------- errors --- */

static LONG tls_error_from_nx(UINT status)
{
    switch (status)
    {
    case NX_SUCCESS:
        return TLS_OK;

    case NX_SECURE_TLS_ISSUER_CERTIFICATE_NOT_FOUND:
    case NX_SECURE_TLS_CERTIFICATE_SIG_CHECK_FAILED:
    case NX_SECURE_TLS_INVALID_SELF_SIGNED_CERT:
    case NX_SECURE_X509_CHAIN_VERIFY_FAILURE:
        return TLS_ERR_UNTRUSTED;

    case NX_SECURE_X509_CERTIFICATE_EXPIRED:
    case NX_SECURE_X509_CERTIFICATE_NOT_YET_VALID:
        return TLS_ERR_EXPIRED;

    case NX_SECURE_X509_CERTIFICATE_DNS_MISMATCH:
        return TLS_ERR_HOSTNAME;

    case NX_SECURE_X509_KEY_USAGE_ERROR:
        return TLS_ERR_KEYUSAGE;

    case NX_NO_PACKET:
    case NX_WAIT_ABORTED:
        return TLS_ERR_TIMEOUT;

    case NX_SECURE_TLS_CLOSE_NOTIFY_RECEIVED:
    case NX_NOT_CONNECTED:
    case NX_DISCONNECT_FAILED:
        return TLS_ERR_CLOSED;

    case NX_NO_MORE_ENTRIES:
    case NX_POOL_ERROR:
        return TLS_ERR_NOMEM;

    default:
        return TLS_ERR_HANDSHAKE;
    }
}

CONST_STRPTR tls_TLSErrorString(register LONG               code    TLSLIB_REG("d0"),
                                register struct TLSLibBase *TLSBase TLSLIB_REG("a6"))
{
    (VOID)TLSBase;

    switch (code)
    {
    case TLS_OK:            return (CONST_STRPTR)"no error";
    case TLS_ERR_NOMEM:     return (CONST_STRPTR)"out of memory";
    case TLS_ERR_BADSOCKET: return (CONST_STRPTR)"not a connected TCP socket";
    case TLS_ERR_NOSTACK:   return (CONST_STRPTR)"bsdsocket.library was built without TLS support";
    case TLS_ERR_TRUSTSTORE:return (CONST_STRPTR)"cannot read DEVS:Internet/certificates";
    case TLS_ERR_HANDSHAKE: return (CONST_STRPTR)"the server did not complete a TLS handshake";
    case TLS_ERR_UNTRUSTED: return (CONST_STRPTR)"the certificate chain reaches no trusted root";
    case TLS_ERR_HOSTNAME:  return (CONST_STRPTR)"the certificate is issued to another host";
    case TLS_ERR_EXPIRED:   return (CONST_STRPTR)"the certificate is expired or not yet valid";
    case TLS_ERR_TIMEOUT:   return (CONST_STRPTR)"the server stopped answering";
    case TLS_ERR_CLOSED:    return (CONST_STRPTR)"the connection is closed";
    case TLS_ERR_IO:        return (CONST_STRPTR)"the network connection failed";
    case TLS_ERR_NOHOSTNAME:return (CONST_STRPTR)"no host name was given to check the certificate against";
    case TLS_ERR_ALERT:     return (CONST_STRPTR)"the server broke off the connection, so the data is incomplete";
    case TLS_ERR_BADHOSTNAME: return (CONST_STRPTR)"the host name is too long for certificate verification";
    case TLS_ERR_KEYUSAGE:  return (CONST_STRPTR)"the certificate is not allowed to be used this way";
    case TLS_ERR_BADPATH:    return (CONST_STRPTR)"the trust store or session file path is too long";
    default:                return (CONST_STRPTR)"internal error";
    }
}

/* ---------------------------------------------------- the name check ----- */

static ULONG tls_certificate_callback(NX_SECURE_TLS_SESSION *session,
                                      NX_SECURE_X509_CERT *certificate)
{
    TLSConnection *conn = tls_conn_for_session(session);
    UINT           status;

    if (conn == NULL)
        return NX_SUCCESS;

    if ((conn->tc_Flags & TLSF_VERIFY) == 0)
        return NX_SUCCESS;

    if (conn->tc_HostNameLength == 0)
        return NX_SECURE_X509_CERTIFICATE_DNS_MISMATCH;

    status = _nx_secure_x509_common_name_dns_check(certificate,
                                                   conn->tc_HostName,
                                                   conn->tc_HostNameLength);
    if (status != NX_SUCCESS)
        return NX_SECURE_X509_CERTIFICATE_DNS_MISMATCH;

    return NX_SUCCESS;
}

/* TLSA_NoVerify.  A function rather than NULL: nx_secure calls this
   pointer unconditionally. */
static UINT tls_verify_none(NX_SECURE_X509_CERTIFICATE_STORE *store,
                            NX_SECURE_X509_CERT *certificate,
                            ULONG current_time)
{
    (VOID)store;
    (VOID)certificate;
    (VOID)current_time;

    return NX_SUCCESS;
}

/* ----------------------------------------------------------- teardown --- */

static VOID tls_conn_free(TLSConnection *conn)
{
    if (conn == NULL)
        return;

    tls_store_detach(conn);
    tls_store_close(&conn->tc_StoreIndex);

    tls_free(conn->tc_Ticket);
    tls_free(conn->tc_RootDer);
    tls_free(conn->tc_RemoteDer);
    tls_free(conn->tc_Remote);

    /* Wipe key material before freeing: AllocVec() hands the memory straight
       back out and there is no MMU here to fault a read of it. */
    if (conn->tc_RecordBuffer != NULL)
        tls_bzero(conn->tc_RecordBuffer, conn->tc_RecordBufferSize);
    tls_free(conn->tc_RecordBuffer);

    if (conn->tc_Metadata != NULL)
        tls_bzero(conn->tc_Metadata, conn->tc_MetadataSize);
    tls_free(conn->tc_Metadata);

    tls_packet_pool_delete(&conn->tc_Pool);
    if (conn->tc_PoolMemory != NULL)
        tls_bzero(conn->tc_PoolMemory,
                  tls_packet_pool_bytes(conn->tc_PoolPackets));
    tls_free(conn->tc_PoolMemory);

    tls_bzero(conn, sizeof(*conn));
    tls_free(conn);
}

static VOID tls_conn_delete_session(TLSConnection *conn)
{
    if (tls_conn_enter(conn) == 0)
    {
        (VOID)_nx_secure_tls_session_delete(&conn->tc_Session);
        tls_conn_leave(conn);
    }
    else
    {
        (VOID)_nx_secure_tls_session_delete(&conn->tc_Session);
    }
}

/* ---------------------------------------------------------- TLSOpenA --- */

struct TLSConnection *tls_TLSOpenA(
        register APTR                  socket_base TLSLIB_REG("a0"),
        register const struct TagItem *tags        TLSLIB_REG("a1"),
        register LONG                  sock        TLSLIB_REG("d0"),
        register struct TLSLibBase    *TLSBase     TLSLIB_REG("a6"))
{
    TLSConnection           *conn = NULL;
    LONG                    *error_out;
    LONG                     error = TLS_ERR_INTERNAL;
    CONST_STRPTR             hostname;
    CONST_STRPTR             store_path;
    CONST_STRPTR             session_path;
    ULONG                    timeout_ms;
    ULONG                    record_bytes;
    ULONG                    chain;
    ULONG                    metadata_size = 0;
    ULONG                    i;
    UINT                     status;
    ULONG                    start_ticks;

    error_out = (LONG *)tls_tag_data(tags, TLSA_Error, 0);

    if (socket_base == NULL || sock < 0)
    {
        error = TLS_ERR_BADSOCKET;
        goto fail;
    }

    /* ---- the stack ----------------------------------------------------- */

    /*
     * Any bsdsocket.library will do, and this is the whole of what is asked of
     * it: that its jump table reaches the vectors tls_sock.c calls.  Every
     * stack since AmiTCP 4.3 does; a base that does not is not one.
     */
    if (!tls_sock_have_lvo(socket_base, TLS_SOCK_LVO_LAST))
    {
        error = TLS_ERR_NOSTACK;
        goto fail;
    }

    ObtainSemaphore(&TLSBase->tb_Lock);

    if (!TLSBase->tb_CryptoReady)
    {
        /* Idempotent, not re-entrant: hence the semaphore. */
        (VOID)ami_tls_crypto_initialize();
        (VOID)ami_tls_timer_open();
        TLSBase->tb_CryptoReady = TRUE;
    }

    ReleaseSemaphore(&TLSBase->tb_Lock);

    /* The entropy pool is this library's own now (src/common/ami_random.c),
       so it is seeded here rather than borrowed already seeded. */
    (VOID)ami_tls_seed_rng();

    /* ---- the tags ------------------------------------------------------ */

    hostname     = (CONST_STRPTR)tls_tag_data(tags, TLSA_HostName, 0);
    store_path   = (CONST_STRPTR)tls_tag_data(tags, TLSA_TrustStore, 0);
    session_path = (CONST_STRPTR)tls_tag_data(tags, TLSA_SessionFile, 0);
    timeout_ms   = tls_tag_data(tags, TLSA_Timeout, TLS_DEFAULT_TIMEOUT_MS);
    record_bytes = tls_tag_data(tags, TLSA_RecordBuffer, TLS_DEFAULT_RECORD_BUFFER);
    chain        = tls_tag_data(tags, TLSA_MaxChain, TLS_DEFAULT_CHAIN);

    if (record_bytes < TLS_MIN_RECORD_BUFFER)
        record_bytes = TLS_MIN_RECORD_BUFFER;
    if (chain == 0)
        chain = 1;
    if (chain > TLS_MAX_CHAIN)
        chain = TLS_MAX_CHAIN;

    /* ---- the connection ------------------------------------------------ */

    conn = (TLSConnection *)tls_alloc(sizeof(TLSConnection));
    if (conn == NULL)
    {
        error = TLS_ERR_NOMEM;
        goto fail;
    }

    conn->tc_Base        = TLSBase;
    conn->tc_SocketBase  = socket_base;
    conn->tc_Fd          = sock;
    conn->tc_RemoteCount = chain;
    conn->tc_Store       = &conn->tc_StoreIndex;

    if (tls_tag_data(tags, TLSA_NoVerify, 0) == 0)
        conn->tc_Flags |= TLSF_VERIFY;

    conn->tc_Timeout = (timeout_ms == 0)
                       ? NX_WAIT_FOREVER
                       : ((timeout_ms / 1000UL) * NX_IP_PERIODIC_RATE +
                          ((timeout_ms % 1000UL) * NX_IP_PERIODIC_RATE) / 1000UL);
    if (conn->tc_Timeout == 0)
        conn->tc_Timeout = 1;

    if (hostname != NULL)
    {
        ULONG n = tls_strlen((const char *)hostname);

        error = tls_hostname_set(conn, hostname, n);
        if (error != TLS_OK)
            goto fail;
    }

    if ((conn->tc_Flags & TLSF_VERIFY) != 0 && conn->tc_HostNameLength == 0)
    {
        error = TLS_ERR_NOHOSTNAME;
        goto fail;
    }

    if ((conn->tc_Flags & TLSF_VERIFY) != 0)
    {
        error = tls_path_set(conn->tc_StorePath, sizeof(conn->tc_StorePath),
                             (store_path != NULL)
                                 ? store_path
                                 : (CONST_STRPTR)TLS_DEFAULT_STORE);
        if (error != TLS_OK)
            goto fail;
    }

    /* ---- resumption ----------------------------------------------------- */

    /*
     * On by default: a caller that does nothing gets a resumed handshake
     * whenever the far end will give it one, which on this hardware is the
     * difference between seconds and tens of seconds.  TLSA_NoResume turns it
     * off.  TLSA_SessionFile with an empty string keeps the cache in the
     * library and off the disk.
     */
    if (tls_tag_data(tags, TLSA_NoResume, 0) == 0)
    {
        conn->tc_ResumeFlags |= TLSR_ENABLED;

        error = tls_path_set(conn->tc_SessionPath,
                             sizeof(conn->tc_SessionPath),
                             (session_path != NULL)
                                 ? session_path
                                 : (CONST_STRPTR)TLS_DEFAULT_SESSIONS);
        if (error != TLS_OK)
            goto fail;

        if (conn->tc_SessionPath[0] != '\0')
            conn->tc_ResumeFlags |= TLSR_PERSIST;

        conn->tc_Ticket = (UBYTE *)tls_alloc(TLS_RESUME_TICKET_MAX);
        if (conn->tc_Ticket == NULL)
        {
            error = TLS_ERR_NOMEM;
            goto fail;
        }
    }

    /* ---- the transport -------------------------------------------------- */

    {
        UWORD peer_port   = 0;
        UWORD peer_family = TLS_SOCK_AF_INET;

        if (!tls_sock_is_connected_tcp(socket_base, sock, &peer_port,
                                       &peer_family))
        {
            error = TLS_ERR_BADSOCKET;
            goto fail;
        }

        conn->tc_Port = peer_port;

        conn->tc_PoolPackets = tls_packet_pool_count(record_bytes);
        conn->tc_PoolMemory  =
            tls_alloc(tls_packet_pool_bytes(conn->tc_PoolPackets));
        if (conn->tc_PoolMemory == NULL)
        {
            error = TLS_ERR_NOMEM;
            goto fail;
        }

        if (tls_packet_pool_create(&conn->tc_Pool, conn->tc_PoolMemory,
                                   conn->tc_PoolPackets) != NX_SUCCESS)
        {
            error = TLS_ERR_INTERNAL;
            goto fail;
        }

        tls_transport_open(&conn->tc_Transport, socket_base, sock,
                           &conn->tc_Pool, FALSE,
                           (BOOL)(peer_family == TLS_SOCK_AF_INET6));
    }

    /* ---- the trust store ------------------------------------------------ */

    if ((conn->tc_Flags & TLSF_VERIFY) != 0)
    {
        LONG rc = tls_store_open(conn->tc_Store, conn->tc_StorePath);

        if (rc != TLS_OK)
        {
            error = rc;
            goto fail;
        }
    }

    /* ---- buffers -------------------------------------------------------- */

    status = _nx_secure_tls_metadata_size_calculate(&ami_crypto_tls_ciphers_ecc,
                                                     &metadata_size);
    if (status != NX_SUCCESS || metadata_size == 0)
    {
        error = TLS_ERR_INTERNAL;
        goto fail;
    }

    conn->tc_MetadataSize     = metadata_size;
    conn->tc_Metadata         = (UCHAR *)tls_alloc(metadata_size);
    conn->tc_RecordBufferSize = record_bytes;
    conn->tc_RecordBuffer     = (UCHAR *)tls_alloc(record_bytes);
    conn->tc_Remote           = (NX_SECURE_X509_CERT *)
                                tls_alloc(chain * sizeof(NX_SECURE_X509_CERT));
    conn->tc_RemoteDer        = (UCHAR *)tls_alloc(chain * TLS_REMOTE_DER_MAX);
    conn->tc_RootDer          = (UCHAR *)tls_alloc(TLS_MAX_ROOTS * TLS_ROOT_DER_MAX);

    if (conn->tc_Metadata == NULL || conn->tc_RecordBuffer == NULL ||
        conn->tc_Remote == NULL || conn->tc_RemoteDer == NULL ||
        conn->tc_RootDer == NULL)
    {
        error = TLS_ERR_NOMEM;
        goto fail;
    }

    /* ---- the session ---------------------------------------------------- */

    /* session_create() and remote_certificate_allocate() need the ThreadX
       bracket: mutexes, plus the process-wide session list. */
    if (tls_conn_enter(conn) != 0)
    {
        error = TLS_ERR_NOSTACK;
        goto fail;
    }

    status = _nx_secure_tls_session_create(&conn->tc_Session,
                                            &ami_crypto_tls_ciphers_ecc,
                                            (VOID *)conn->tc_Metadata,
                                            conn->tc_MetadataSize);
    if (status != NX_SUCCESS)
    {
        tls_conn_leave(conn);
        error = tls_error_from_nx(status);
        goto fail;
    }

    /* _nx_secure_tls_ecc_initialize() writes both the session's list and the
       process-wide X.509 list, so it gets the complete set; the session's own
       offer is narrowed below. */
    (VOID)_nx_secure_tls_ecc_initialize(&conn->tc_Session,
                                         ami_crypto_ecc_supported_groups,
                                         (USHORT)ami_crypto_ecc_supported_groups_size,
                                         ami_crypto_ecc_curves);

    conn->tc_Session.nx_secure_tls_ecc.nx_secure_tls_ecc_supported_groups =
        ami_crypto_ecc_offered_groups;
    conn->tc_Session.nx_secure_tls_ecc.nx_secure_tls_ecc_supported_groups_count =
        (USHORT)ami_crypto_ecc_offered_groups_size;
    conn->tc_Session.nx_secure_tls_ecc.nx_secure_tls_ecc_curves =
        ami_crypto_ecc_offered_curves;

    /* Before session_start(): it would otherwise reach for the packet pool of
       the socket's NX_IP, and there is no NX_IP here. */
    conn->tc_Session.nx_secure_tls_packet_pool = &conn->tc_Pool;

    (VOID)_nx_secure_tls_session_packet_buffer_set(&conn->tc_Session,
                                                    conn->tc_RecordBuffer,
                                                    conn->tc_RecordBufferSize);

    for (i = 0; i < chain; i++)
    {
        (VOID)_nx_secure_tls_remote_certificate_allocate(
                  &conn->tc_Session, &conn->tc_Remote[i],
                  &conn->tc_RemoteDer[i * TLS_REMOTE_DER_MAX],
                  TLS_REMOTE_DER_MAX);
    }

    if (conn->tc_HostNameLength > 0)
    {
        (VOID)_nx_secure_tls_session_sni_extension_set(&conn->tc_Session,
                                                        &conn->tc_Sni);
    }

    /* tls_time_now() answers 0 when the clock is unset, and 0 is nx_secure's
       own "do not check validity dates". */
    (VOID)_nx_secure_tls_session_time_function_set(&conn->tc_Session,
                                                    tls_time_now);
    conn->tc_ExpiryChecked = tls_time_is_known();
    conn->tc_UnixTime      = tls_time_now();

    tls_registry_add(conn);

    if ((conn->tc_Flags & TLSF_VERIFY) != 0)
    {
        (VOID)_nx_secure_tls_session_certificate_callback_set(
                  &conn->tc_Session, tls_certificate_callback);

        tls_store_attach(conn);
    }
    else
    {
        /* Cannot be NX_NULL: nx_secure_tls_remote_certificate_verify.c calls
           the pointer unconditionally. */
        conn->tc_Session.nx_secure_remote_certificate_verify = tls_verify_none;
    }

    tls_conn_leave(conn);

    /* ---- the handshake -------------------------------------------------- */

    tls_resume_prepare(conn);

    start_ticks = ami_tls_eclock();

    TLS13_PROBE("start.in ", conn->tc_Timeout);

    /*
     * NOT inside tls_conn_enter(): the handshake blocks on the network for as
     * long as TLSA_Timeout allows, and nx_secure drops the protection mutex
     * around every one of those waits itself.
     */
    status = _nx_secure_tls_session_start(&conn->tc_Session,
                                           tls_transport_socket(&conn->tc_Transport),
                                           conn->tc_Timeout);

    TLS13_PROBE("start.out ", (ULONG)status);

    conn->tc_HandshakeMillis =
        ami_tls_eclock_micros(ami_tls_eclock() - start_ticks) / 1000UL;

    if (status != NX_SUCCESS)
    {
        /* A failed handshake that offered a cached session must drop it, or
           the same broken resumption is retried forever. */
        if ((conn->tc_ResumeFlags & TLSR_OFFERED) != 0)
            tls_resume_evict(conn);

        error = tls_error_from_nx(status);
        goto fail_session;
    }

    conn->tc_Flags |= TLSF_HANDSHAKEN;
    conn->tc_Error  = TLS_OK;

    if (conn->tc_Session.nx_secure_tls_session_ciphersuite != NX_NULL)
    {
        conn->tc_CipherSuite =
            (ULONG)conn->tc_Session.nx_secure_tls_session_ciphersuite
                       ->nx_secure_tls_ciphersuite;
    }
    conn->tc_Protocol = (ULONG)conn->tc_Session.nx_secure_tls_protocol_version;

    tls_resume_record(conn);

    if (error_out != NULL)
        *error_out = TLS_OK;

    return conn;

fail_session:
    tls_conn_delete_session(conn);

fail:
    if (conn != NULL)
        tls_conn_free(conn);
    if (error_out != NULL)
        *error_out = error;

    return NULL;
}

/* ----------------------------------------------------------- TLSClose --- */

VOID tls_TLSClose(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                  register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"))
{
    (VOID)TLSBase;

    if (conn == NULL)
        return;

    if (tls_conn_enter(conn) == 0)
    {
        if (conn->tc_Pending != NX_NULL)
        {
            (VOID)_nx_packet_release(conn->tc_Pending);
            conn->tc_Pending = NX_NULL;
        }

        (VOID)_nx_secure_tls_session_end(&conn->tc_Session,
                                          5UL * NX_IP_PERIODIC_RATE);

        /* Under the same lock as the end above: session_delete() edits
           NetX Secure's global list, and the two must be one step. */
        (VOID)_nx_secure_tls_session_delete(&conn->tc_Session);

        tls_conn_leave(conn);
    }
    else
    {
        (VOID)_nx_secure_tls_session_delete(&conn->tc_Session);
    }

    /* The descriptor is not closed: it stays the caller's. */
    tls_conn_free(conn);
}

/* ------------------------------------------------------------ TLSRead --- */

LONG tls_TLSRead(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                 register APTR                  buffer  TLSLIB_REG("a1"),
                 register LONG                  length  TLSLIB_REG("d0"),
                 register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"))
{
    NX_PACKET *packet = NX_NULL;
    ULONG      available;
    ULONG      copied = 0;
    UINT       status;

    (VOID)TLSBase;

    if (conn == NULL || buffer == NULL)
        return -1;
    if (length <= 0)
        return 0;
    if ((conn->tc_Flags & TLSF_HANDSHAKEN) == 0)
    {
        conn->tc_Error = TLS_ERR_CLOSED;
        return -1;
    }

    if (conn->tc_Pending == NX_NULL)
    {
        if ((conn->tc_Flags & TLSF_EOF) != 0)
            return 0;

        /* No outer lock: this blocks on the network for TLSA_Timeout, and
           nx_secure drops the protection mutex around that wait itself. */
        status = _nx_secure_tls_session_receive(&conn->tc_Session, &packet,
                                                 conn->tc_Timeout);

        tls_trace("[resume] session_receive -> %ld packet %lx state %ld",
                  (LONG)status, (LONG)packet,
                  (LONG)conn->tc_Session.nx_secure_tls_client_state);

        if (status == NX_SECURE_TLS_ALERT_RECEIVED)
        {
            /* Every alert arrives as this one status; only a warning-level
               close_notify is end of stream.  Anything fatal is a truncation
               the caller must not be allowed to mistake for a whole file. */
            if (conn->tc_Session.nx_secure_tls_received_alert_level ==
                    NX_SECURE_TLS_ALERT_LEVEL_WARNING &&
                conn->tc_Session.nx_secure_tls_received_alert_value ==
                    NX_SECURE_TLS_ALERT_CLOSE_NOTIFY)
            {
                conn->tc_Flags |= TLSF_EOF;
                return 0;
            }

            conn->tc_Flags |= TLSF_EOF;
            conn->tc_Error  = TLS_ERR_ALERT;
            return -1;
        }

        if (status == NX_NOT_CONNECTED)
        {
            /* RFC 5246 7.2.1 calls a bare FIN a truncation error; treated as
               end of stream because many servers close without close_notify. */
            conn->tc_Flags |= TLSF_EOF;
            return 0;
        }

        if (status != NX_SUCCESS || packet == NX_NULL)
        {
            conn->tc_Error = tls_error_from_nx(status);
            return -1;
        }

        conn->tc_Pending       = packet;
        conn->tc_PendingOffset = 0;
    }

    available = conn->tc_Pending->nx_packet_length - conn->tc_PendingOffset;
    if ((ULONG)length < available)
        available = (ULONG)length;

    if (available > 0)
    {
        status = _nx_packet_data_extract_offset(conn->tc_Pending,
                                                conn->tc_PendingOffset,
                                                buffer, available, &copied);
        if (status != NX_SUCCESS)
        {
            conn->tc_Error = TLS_ERR_IO;
            return -1;
        }
    }

    conn->tc_PendingOffset += copied;

    if (conn->tc_PendingOffset >= conn->tc_Pending->nx_packet_length)
    {
        (VOID)_nx_packet_release(conn->tc_Pending);
        conn->tc_Pending       = NX_NULL;
        conn->tc_PendingOffset = 0;
    }

    return (LONG)copied;
}

/* ----------------------------------------------------------- TLSWrite --- */

LONG tls_TLSWrite(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                  register CONST_APTR            buffer  TLSLIB_REG("a1"),
                  register LONG                  length  TLSLIB_REG("d0"),
                  register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"))
{
    NX_PACKET_POOL *pool;
    NX_PACKET      *packet;
    const UBYTE    *src = (const UBYTE *)buffer;
    LONG            sent = 0;
    ULONG           chunk;
    UINT            status = NX_SUCCESS;

    (VOID)TLSBase;

    if (conn == NULL || buffer == NULL)
        return -1;
    if (length <= 0)
        return 0;
    if ((conn->tc_Flags & TLSF_HANDSHAKEN) == 0)
    {
        conn->tc_Error = TLS_ERR_CLOSED;
        return -1;
    }

    pool = &conn->tc_Pool;

    /* No outer lock, for the reason TLSRead() gives: a send blocks on the
       network and nx_secure drops the protection mutex around it. */
    while (sent < length)
    {
        chunk = (ULONG)(length - sent);
        if (chunk > TLS_WRITE_CHUNK)
            chunk = TLS_WRITE_CHUNK;

        packet = NX_NULL;
        status = _nx_secure_tls_packet_allocate(&conn->tc_Session, pool,
                                                 &packet, conn->tc_Timeout);
        if (status != NX_SUCCESS || packet == NX_NULL)
            break;

        status = _nx_packet_data_append(packet, (VOID *)&src[sent], chunk,
                                        pool, conn->tc_Timeout);
        if (status != NX_SUCCESS)
        {
            (VOID)_nx_packet_release(packet);
            break;
        }

        status = _nx_secure_tls_session_send(&conn->tc_Session, packet,
                                              conn->tc_Timeout);
        if (status != NX_SUCCESS)
        {
            (VOID)_nx_packet_release(packet);
            break;
        }

        sent += (LONG)chunk;
    }

    if (sent == 0 && length > 0)
    {
        conn->tc_Error = tls_error_from_nx(status);
        return -1;
    }

    return sent;
}

/* --------------------------------------------------------- TLSPending --- */

LONG tls_TLSPending(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                    register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"))
{
    (VOID)TLSBase;

    if (conn == NULL || conn->tc_Pending == NX_NULL)
        return 0;

    return (LONG)(conn->tc_Pending->nx_packet_length - conn->tc_PendingOffset);
}

/* ------------------------------------------------------------ TLSInfo --- */

LONG tls_TLSInfo(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                 register struct TLSInfo       *info    TLSLIB_REG("a1"),
                 register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"))
{
    (VOID)TLSBase;

    if (conn == NULL || info == NULL)
        return -1;

    /* ti_Size is a version: fields added after TLS_INFO_SIZE_V1 are written
       only when the caller's structure is big enough to hold them. */
    if (info->ti_Size < (ULONG)TLS_INFO_SIZE_V1)
        return -1;

    /* NOT tc_Protocol: that is the legacy record version, which RFC 8446 5.1
       fixes at 0x0303 for the whole of a TLS 1.3 connection.  tc_Protocol
       keeps the raw value because tls_resume.c compares against it. */
#if (NX_SECURE_TLS_TLS_1_3_ENABLED)
    info->ti_Version         = (conn->tc_Session.nx_secure_tls_1_3 != 0)
                                   ? 0x0304UL : conn->tc_Protocol;
#else
    info->ti_Version         = conn->tc_Protocol;
#endif
    info->ti_CipherSuite     = conn->tc_CipherSuite;
    info->ti_ChainDepth      = conn->tc_ChainDepth;
    info->ti_HandshakeMillis = conn->tc_HandshakeMillis;
    info->ti_Error           = conn->tc_Error;
    info->ti_Verified        = (BOOL)(((conn->tc_Flags & TLSF_VERIFY) != 0 &&
                                       (conn->tc_Flags & TLSF_HANDSHAKEN) != 0)
                                      ? TRUE : FALSE);
    info->ti_ExpiryChecked   = conn->tc_ExpiryChecked;
    info->ti_UnixTime        = conn->tc_UnixTime;
    info->ti_TrustRoots      = tls_store_count(conn->tc_Store);
    info->ti_RootsLoaded     = conn->tc_RootsLoaded;

    if (info->ti_Size >= (ULONG)sizeof(struct TLSInfo))
    {
        info->ti_Resumed =
            (BOOL)(((conn->tc_ResumeFlags & TLSR_RESUMED) != 0) ? TRUE : FALSE);
        info->ti_Resumable =
            (BOOL)(((conn->tc_ResumeFlags & TLSR_ENABLED) != 0) ? TRUE : FALSE);
        info->ti_SessionsCached = tls_resume_count(conn->tc_Base);
    }

    return 0;
}

/* -------------------------------------------------------- TLSBuffered --- */

/*
 * Bytes this library holds that are not decrypted yet.  Non-zero means a
 * TLSRead() can make progress without another byte from the socket, which is
 * a different question from TLSPending()'s count of ready plaintext.
 */
LONG tls_TLSBuffered(register struct TLSConnection *conn    TLSLIB_REG("a0"),
                     register struct TLSLibBase    *TLSBase TLSLIB_REG("a6"))
{
    const NX_PACKET *queued;

    (VOID)TLSBase;

    if (conn == NULL)
        return -1;

    queued = conn->tc_Session.nx_secure_record_queue_header;
    if (queued == NX_NULL)
        return 0;

    return (LONG)queued->nx_packet_length;
}

/* ---------------------------------------------------------- TLSRandom --- */

/*
 * Random bytes from this library's own entropy pool, the SHA-256 hash DRBG in
 * src/common/ami_random.c.  Returns the number of bytes written, or -1; never
 * partially fills.
 */
LONG tls_TLSRandom(register APTR               buffer  TLSLIB_REG("a0"),
                   register LONG               length  TLSLIB_REG("d0"),
                   register struct TLSLibBase *TLSBase TLSLIB_REG("a6"))
{
    (VOID)TLSBase;

    if (buffer == NULL || length < 0)
        return -1;

    /* The pool seeds itself on first use, but that collection takes tens of
       milliseconds, so ask for it explicitly rather than inside a handshake. */
    (VOID)ami_tls_seed_rng();

    /* Bytes, not ami_random_rand() draws: that one clears bit 31 to stay
       inside rand()'s range, and the gap lands in every word of a key. */
    ami_random_bytes(buffer, (ULONG)length);

    return length;
}

/* ------------------------------------------------------ TLSWaitSelect --- */

/*
 * WaitSelect() cannot see plaintext this library already holds, so a program
 * that waits on the descriptor alone sleeps with its answer in memory.
 */
LONG tls_TLSWaitSelect(register struct TLSSelect   *sel     TLSLIB_REG("a0"),
                       register struct TLSLibBase *TLSBase TLSLIB_REG("a6"))
{
    ULONG *read_words;
    LONG   ready = 0;
    ULONG  i;

    (VOID)TLSBase;

    if (sel == NULL || sel->ts_Size < (ULONG)sizeof(struct TLSSelect))
        return -1;

    read_words = (ULONG *)sel->ts_Read;

    if (sel->ts_Connections != NULL && read_words != NULL)
    {
        ULONG hits[TLS_FD_MAX / TLS_FD_BITS];
        ULONG words = 0;

        if (sel->ts_NFds > 0)
        {
            words = ((ULONG)sel->ts_NFds + TLS_FD_BITS - 1UL) /
                    TLS_FD_BITS;
            if (words > (ULONG)(TLS_FD_MAX / TLS_FD_BITS))
                words = (ULONG)(TLS_FD_MAX / TLS_FD_BITS);
        }

        for (i = 0; i < words; i++)
            hits[i] = 0;

        for (i = 0; sel->ts_Connections[i] != NULL; i++)
        {
            TLSConnection *conn = sel->ts_Connections[i];
            LONG           fd   = conn->tc_Fd;

            if (fd < 0 || fd >= TLS_FD_MAX || fd >= sel->ts_NFds)
                continue;
            if (conn->tc_Pending == NX_NULL &&
                conn->tc_Session.nx_secure_record_queue_header == NX_NULL)
                continue;
            if ((read_words[TLS_FD_WORD(fd)] & TLS_FD_MASK(fd)) == 0)
                continue;
            if ((hits[TLS_FD_WORD(fd)] & TLS_FD_MASK(fd)) != 0)
                continue;

            hits[TLS_FD_WORD(fd)] |= TLS_FD_MASK(fd);
            ready++;
        }

        if (ready > 0)
        {
            /* Report only the connections that already have plaintext, and do
               not wait.  Descriptors that were also ready are dropped from the
               answer and reported by the next pass. */
            for (i = 0; i < words; i++)
                read_words[i] = hits[i];

            if (sel->ts_Write != NULL)
            {
                ULONG *w = (ULONG *)sel->ts_Write;

                for (i = 0; i < words; i++)
                    w[i] = 0;
            }
            if (sel->ts_Except != NULL)
            {
                ULONG *e = (ULONG *)sel->ts_Except;

                for (i = 0; i < words; i++)
                    e[i] = 0;
            }
            if (sel->ts_SignalMask != NULL)
                *sel->ts_SignalMask = 0;

            return ready;
        }
    }

    if (sel->ts_SocketBase == NULL)
        return -1;

#ifndef TLSLIB_HOST_TEST
    {
        register APTR   a6 __asm("a6") = sel->ts_SocketBase;
        register APTR   a0 __asm("a0") = sel->ts_Read;
        register APTR   a1 __asm("a1") = sel->ts_Write;
        register APTR   a2 __asm("a2") = sel->ts_Except;
        register APTR   a3 __asm("a3") = sel->ts_Timeout;
        register LONG   d0 __asm("d0") = sel->ts_NFds;
        register ULONG *d1 __asm("d1") = sel->ts_SignalMask;
        register LONG   res __asm("d0");
        register LONG _clob_d1 __asm("d1");
        register LONG _clob_a0 __asm("a0");
        register LONG _clob_a1 __asm("a1");

        __asm __volatile ("jsr a6@(-126:W)"     /* -0x07e, WaitSelect */
                          : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0), "=r" (_clob_a1)
                          : "r" (a6), "r" (a0), "r" (a1), "r" (a2),
                            "r" (a3), "r" (d0), "r" (d1)
                          : "cc", "memory");
        return res;
    }
#else
    return -1;
#endif
}
