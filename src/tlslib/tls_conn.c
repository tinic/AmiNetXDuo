/*
 * tls.library -- the eight calls a program actually makes.
 *
 * The contract, the reasoning about WaitSelect() and the example program are
 * all in include/aminetxduo/tlslib.h.  What is here is the implementation and
 * the decisions that do not belong in a public header.
 *
 * MEMORY
 *
 *   A connection allocates, in one place so it can be read off:
 *
 *     crypto metadata      sized by _nx_secure_tls_metadata_size_calculate()
 *                          rather than guessed -- measured at 16,272 bytes for
 *                          our ECC ciphersuite table (docs/RESEARCH.md 9)
 *     record buffer        TLSA_RecordBuffer, default 10 KB.  A certificate
 *                          flight arrives as one record and has to fit
 *     remote certificates  TLSA_MaxChain x (NX_SECURE_X509_CERT + 2.5 KB DER)
 *     roots                TLS_MAX_ROOTS x (NX_SECURE_X509_CERT + 2.5 KB DER)
 *
 *   About 40 KB for the defaults.  It is freed at TLSClose() and none of it is
 *   held while no connection is open, which is the reason this is a separate
 *   library and not a corner of bsdsocket.library.
 *
 * THE THREADX BRACKET
 *
 *   NetX Duo rejects a caller that is not a ThreadX thread, so every entry
 *   point that reaches the stack brackets itself with the enter/leave the
 *   context supplies.  An application never sees this.  Brackets nest, so a
 *   program that is already inside one (because it linked against the netstack
 *   directly) is not disturbed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_vectors.h"

#include "tls.h"
#include "ami_tls_crypto.h"

#include <exec/execbase.h>
#include <proto/exec.h>

/* fd_set, open-coded.  <sys/socket.h> would do, and would also drag
   devices/timer.h and the whole netinclude set into a library that needs eight
   lines of it. The layout is the one src/bsdsocket/select.c implements. */
#define TLS_FD_BITS         32
#define TLS_FD_WORD(fd)     ((ULONG)(fd) / TLS_FD_BITS)
#define TLS_FD_MASK(fd)     (1UL << ((ULONG)(fd) % TLS_FD_BITS))
#define TLS_FD_MAX          256

/* --------------------------------------------------------------- tags --- */

/*
 * A TagItem walk, rather than utility.library's GetTagData().  Opening
 * utility.library for four tags would be the only reason this library needed
 * it, and TAG_MORE is eleven lines.
 */
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

LONG tls_conn_enter(TLSConnection *conn)
{
    const AmiNetXDuoContext *ctx = tls_netx_ctx();

    if (conn == NULL || ctx == NULL)
        return -1;

    if (conn->tc_Nest > 0)
    {
        conn->tc_Nest++;
        return 0;
    }

    if (ctx->nxc_Enter(&conn->tc_Caller) != AMI_NET_OK)
        return -1;

    conn->tc_Nest = 1;

    return 0;
}

VOID tls_conn_leave(TLSConnection *conn)
{
    const AmiNetXDuoContext *ctx = tls_netx_ctx();

    if (conn == NULL || ctx == NULL || conn->tc_Nest <= 0)
        return;

    if (--conn->tc_Nest > 0)
        return;

    ctx->nxc_Leave(&conn->tc_Caller);
}

/* -------------------------------------------------------------- errors --- */

LONG tls_error_from_nx(UINT status)
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

CONST_STRPTR tls_TLSErrorString(register LONG               code    __asm("d0"),
                                register struct TLSLibBase *TLSBase __asm("a6"))
{
    (VOID)TLSBase;

    switch (code)
    {
    case TLS_OK:            return (CONST_STRPTR)"no error";
    case TLS_ERR_NOMEM:     return (CONST_STRPTR)"out of memory";
    case TLS_ERR_BADSOCKET: return (CONST_STRPTR)"not a connected TCP socket";
    case TLS_ERR_NOSTACK:   return (CONST_STRPTR)"bsdsocket.library was built without TLS support";
    case TLS_ERR_TRUSTSTORE:return (CONST_STRPTR)"cannot read DEVS:Internet/certificates";
    case TLS_ERR_HANDSHAKE: return (CONST_STRPTR)"the server would not complete a TLS handshake";
    case TLS_ERR_UNTRUSTED: return (CONST_STRPTR)"the certificate chain reaches no trusted root";
    case TLS_ERR_HOSTNAME:  return (CONST_STRPTR)"the certificate is issued to another host";
    case TLS_ERR_EXPIRED:   return (CONST_STRPTR)"the certificate is expired or not yet valid";
    case TLS_ERR_TIMEOUT:   return (CONST_STRPTR)"the server stopped responding";
    case TLS_ERR_CLOSED:    return (CONST_STRPTR)"the connection is closed";
    case TLS_ERR_IO:        return (CONST_STRPTR)"the network connection failed";
    case TLS_ERR_NOHOSTNAME:return (CONST_STRPTR)"no host name was given to check the certificate against";
    default:                return (CONST_STRPTR)"internal error";
    }
}

/* ---------------------------------------------------- the name check ----- */

/*
 * nx_secure verifies the CHAIN; it does not check who the certificate is FOR
 * unless the application asks.  Without this, any host with any valid
 * certificate can answer for any other -- which is not a subtle failure, it is
 * the whole of what TLS is supposed to prevent, so it is on by default and
 * TLSA_NoVerify is the only way past it.
 */
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

/*
 * TLSA_NoVerify.  Not a NULL pointer: nx_secure calls this unconditionally.
 */
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

    tls_free(conn->tc_RootDer);
    tls_free(conn->tc_RemoteDer);
    tls_free(conn->tc_Remote);
    tls_free(conn->tc_RecordBuffer);
    tls_free(conn->tc_Metadata);
    tls_free(conn);
}

/* ---------------------------------------------------------- TLSOpenA --- */

struct TLSConnection *tls_TLSOpenA(
        register APTR                  socket_base __asm("a0"),
        register const struct TagItem *tags        __asm("a1"),
        register LONG                  sock        __asm("d0"),
        register struct TLSLibBase    *TLSBase     __asm("a6"))
{
    const AmiNetXDuoContext *ctx;
    TLSConnection           *conn = NULL;
    LONG                    *error_out;
    LONG                     error = TLS_ERR_INTERNAL;
    CONST_STRPTR             hostname;
    CONST_STRPTR             store_path;
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

    ObtainSemaphore(&TLSBase->tb_Lock);

    if (tls_netx_bind(socket_base) != 0)
    {
        ReleaseSemaphore(&TLSBase->tb_Lock);
        error = TLS_ERR_NOSTACK;
        goto fail;
    }

    if (!TLSBase->tb_CryptoReady)
    {
        /*
         * Builds the private secp256r1 curve that routes P-256 through
         * src/crypto68k (3.8x).  Idempotent, not re-entrant -- hence the
         * semaphore -- and it costs a few milliseconds once per boot.
         */
        (VOID)ami_tls_crypto_initialize();
        (VOID)ami_tls_timer_open();
        TLSBase->tb_CryptoReady = TRUE;
    }

    ReleaseSemaphore(&TLSBase->tb_Lock);

    ctx = tls_netx_ctx();

    /* ---- the tags ------------------------------------------------------ */

    hostname     = (CONST_STRPTR)tls_tag_data(tags, TLSA_HostName, 0);
    store_path   = (CONST_STRPTR)tls_tag_data(tags, TLSA_TrustStore, 0);
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

        if (n > (ULONG)NX_SECURE_X509_DNS_NAME_MAX)
            n = (ULONG)NX_SECURE_X509_DNS_NAME_MAX;

        for (i = 0; i < n; i++)
        {
            conn->tc_HostName[i]            = (UCHAR)hostname[i];
            conn->tc_Sni.nx_secure_x509_dns_name[i] = (UCHAR)hostname[i];
        }
        conn->tc_HostName[n]                     = 0;
        conn->tc_HostNameLength                  = (USHORT)n;
        conn->tc_Sni.nx_secure_x509_dns_name_length = (USHORT)n;
        conn->tc_Sni.nx_secure_x509_dns_name_next   = NX_NULL;
    }

    if ((conn->tc_Flags & TLSF_VERIFY) != 0 && conn->tc_HostNameLength == 0)
    {
        error = TLS_ERR_NOHOSTNAME;
        goto fail;
    }

    tls_strncpy(conn->tc_StorePath,
                (store_path != NULL) ? (const char *)store_path
                                     : TLS_DEFAULT_STORE,
                sizeof(conn->tc_StorePath));

    /* ---- the transport -------------------------------------------------- */

    conn->tc_Socket = ctx->nxc_TcpSocket(socket_base, sock);
    if (conn->tc_Socket == NX_NULL)
    {
        error = TLS_ERR_BADSOCKET;
        goto fail;
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

    status = _nx_secure_tls_session_create(&conn->tc_Session,
                                            &ami_crypto_tls_ciphers_ecc,
                                            (VOID *)conn->tc_Metadata,
                                            conn->tc_MetadataSize);
    if (status != NX_SUCCESS)
    {
        error = tls_error_from_nx(status);
        goto fail;
    }

    (VOID)_nx_secure_tls_ecc_initialize(&conn->tc_Session,
                                         ami_crypto_ecc_supported_groups,
                                         (USHORT)ami_crypto_ecc_supported_groups_size,
                                         ami_crypto_ecc_curves);

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

    /*
     * The clock.  tls_time_now() answers 0 when the machine's clock is
     * obviously unset, and 0 is nx_secure's own "do not check validity dates" --
     * see src/tlslib/tls_time.c for the argument and for what it gives up.
     */
    (VOID)_nx_secure_tls_session_time_function_set(&conn->tc_Session,
                                                    tls_time_now);
    conn->tc_ExpiryChecked = tls_time_is_known();
    conn->tc_UnixTime      = tls_time_now();

    if ((conn->tc_Flags & TLSF_VERIFY) != 0)
    {
        (VOID)_nx_secure_tls_session_certificate_callback_set(
                  &conn->tc_Session, tls_certificate_callback);

        /* Registers the connection AND swaps in the lazy root loader. */
        tls_store_attach(conn);
    }
    else
    {
        /*
         * No trust store, and nothing to verify against: nx_secure would fail
         * the chain outright, so the check has to be REPLACED rather than left
         * to fail.  It cannot be set to NX_NULL -- nx_secure_tls_remote_
         * certificate_verify.c calls the pointer unconditionally.  This is what
         * TLSA_NoVerify buys, and it is why it is spelled out in the header.
         */
        conn->tc_Session.nx_secure_remote_certificate_verify = tls_verify_none;
    }

    /* ---- the handshake -------------------------------------------------- */

    if (tls_conn_enter(conn) != 0)
    {
        error = TLS_ERR_NOSTACK;
        goto fail_session;
    }

    start_ticks = ami_tls_eclock();

    status = _nx_secure_tls_session_start(&conn->tc_Session, conn->tc_Socket,
                                           conn->tc_Timeout);

    conn->tc_HandshakeMillis =
        ami_tls_eclock_micros(ami_tls_eclock() - start_ticks) / 1000UL;

    tls_conn_leave(conn);

    if (status != NX_SUCCESS)
    {
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

    if (error_out != NULL)
        *error_out = TLS_OK;

    return conn;

fail_session:
    (VOID)_nx_secure_tls_session_delete(&conn->tc_Session);

fail:
    if (conn != NULL)
        tls_conn_free(conn);
    if (error_out != NULL)
        *error_out = error;

    return NULL;
}

/* ----------------------------------------------------------- TLSClose --- */

VOID tls_TLSClose(register struct TLSConnection *conn    __asm("a0"),
                  register struct TLSLibBase    *TLSBase __asm("a6"))
{
    const AmiNetXDuoContext *ctx = tls_netx_ctx();

    (VOID)TLSBase;

    if (conn == NULL)
        return;

    if (tls_conn_enter(conn) == 0)
    {
        if (conn->tc_Pending != NX_NULL && ctx != NULL)
        {
            (VOID)ctx->nxc_packet_release(conn->tc_Pending);
            conn->tc_Pending = NX_NULL;
        }

        /* A close_notify, so the peer knows this was not a truncation. Best
           effort: a server that has already gone is not an error here. */
        (VOID)_nx_secure_tls_session_end(&conn->tc_Session,
                                          5UL * NX_IP_PERIODIC_RATE);

        tls_conn_leave(conn);
    }

    /*
     * The delete is OUTSIDE the bracket test on purpose.  A session that was
     * created is on nx_secure's process-wide created list, and the memory it
     * occupies is about to be freed -- so leaving it linked would put a
     * dangling pointer in a global on a machine with no memory protection,
     * which is a much worse outcome than a delete that fails.  The bracket can
     * only fail when the ThreadX kernel is already down, in which case there is
     * nothing left for the delete to disturb either.
     */
    (VOID)_nx_secure_tls_session_delete(&conn->tc_Session);

    /*
     * The descriptor is NOT closed.  It was the caller's before TLSOpen() and
     * it is the caller's again -- which is what makes "speak plain HTTP to a
     * proxy, then upgrade" possible, and what keeps the ownership rule simple.
     */
    tls_conn_free(conn);
}

/* ------------------------------------------------------------ TLSRead --- */

LONG tls_TLSRead(register struct TLSConnection *conn    __asm("a0"),
                 register APTR                  buffer  __asm("a1"),
                 register LONG                  length  __asm("d0"),
                 register struct TLSLibBase    *TLSBase __asm("a6"))
{
    const AmiNetXDuoContext *ctx = tls_netx_ctx();
    NX_PACKET               *packet = NX_NULL;
    ULONG                    available;
    ULONG                    copied = 0;
    UINT                     status;

    (VOID)TLSBase;

    if (conn == NULL || buffer == NULL || ctx == NULL)
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

        if (tls_conn_enter(conn) != 0)
        {
            conn->tc_Error = TLS_ERR_NOSTACK;
            return -1;
        }

        status = _nx_secure_tls_session_receive(&conn->tc_Session, &packet,
                                                 conn->tc_Timeout);
        tls_conn_leave(conn);

        if (status == NX_SECURE_TLS_CLOSE_NOTIFY_RECEIVED ||
            status == NX_NOT_CONNECTED ||
            status == NX_SECURE_TLS_ALERT_RECEIVED)
        {
            /* An orderly close_notify, or the peer simply going away.  Both
               are end-of-stream to a reader; the distinction only matters to
               somebody auditing for truncation attacks, which this stack does
               not defend against anyway. */
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
        status = ctx->nxc_packet_data_extract_offset(conn->tc_Pending,
                                                      conn->tc_PendingOffset,
                                                      buffer, available,
                                                      &copied);
        if (status != NX_SUCCESS)
        {
            conn->tc_Error = TLS_ERR_IO;
            return -1;
        }
    }

    conn->tc_PendingOffset += copied;

    if (conn->tc_PendingOffset >= conn->tc_Pending->nx_packet_length)
    {
        (VOID)ctx->nxc_packet_release(conn->tc_Pending);
        conn->tc_Pending       = NX_NULL;
        conn->tc_PendingOffset = 0;
    }

    return (LONG)copied;
}

/* ----------------------------------------------------------- TLSWrite --- */

LONG tls_TLSWrite(register struct TLSConnection *conn    __asm("a0"),
                  register CONST_APTR            buffer  __asm("a1"),
                  register LONG                  length  __asm("d0"),
                  register struct TLSLibBase    *TLSBase __asm("a6"))
{
    const AmiNetXDuoContext *ctx = tls_netx_ctx();
    NX_PACKET_POOL          *pool;
    NX_PACKET               *packet;
    const UBYTE             *src = (const UBYTE *)buffer;
    LONG                     sent = 0;
    ULONG                    chunk;
    UINT                     status = NX_SUCCESS;

    (VOID)TLSBase;

    if (conn == NULL || buffer == NULL || ctx == NULL)
        return -1;
    if (length <= 0)
        return 0;
    if ((conn->tc_Flags & TLSF_HANDSHAKEN) == 0)
    {
        conn->tc_Error = TLS_ERR_CLOSED;
        return -1;
    }

    pool = ctx->nxc_Pool();
    if (pool == NX_NULL)
    {
        conn->tc_Error = TLS_ERR_IO;
        return -1;
    }

    if (tls_conn_enter(conn) != 0)
    {
        conn->tc_Error = TLS_ERR_NOSTACK;
        return -1;
    }

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

        status = ctx->nxc_packet_data_append(packet, (VOID *)&src[sent], chunk,
                                              pool, conn->tc_Timeout);
        if (status != NX_SUCCESS)
        {
            (VOID)ctx->nxc_packet_release(packet);
            break;
        }

        status = _nx_secure_tls_session_send(&conn->tc_Session, packet,
                                              conn->tc_Timeout);
        if (status != NX_SUCCESS)
        {
            (VOID)ctx->nxc_packet_release(packet);
            break;
        }

        sent += (LONG)chunk;
    }

    tls_conn_leave(conn);

    if (sent == 0 && length > 0)
    {
        conn->tc_Error = tls_error_from_nx(status);
        return -1;
    }

    return sent;
}

/* --------------------------------------------------------- TLSPending --- */

LONG tls_TLSPending(register struct TLSConnection *conn    __asm("a0"),
                    register struct TLSLibBase    *TLSBase __asm("a6"))
{
    (VOID)TLSBase;

    if (conn == NULL || conn->tc_Pending == NX_NULL)
        return 0;

    return (LONG)(conn->tc_Pending->nx_packet_length - conn->tc_PendingOffset);
}

/* ------------------------------------------------------------ TLSInfo --- */

LONG tls_TLSInfo(register struct TLSConnection *conn    __asm("a0"),
                 register struct TLSInfo       *info    __asm("a1"),
                 register struct TLSLibBase    *TLSBase __asm("a6"))
{
    (VOID)TLSBase;

    if (conn == NULL || info == NULL)
        return -1;
    if (info->ti_Size < (ULONG)sizeof(struct TLSInfo))
        return -1;

    info->ti_Version         = conn->tc_Protocol;
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

    return 0;
}

/* ------------------------------------------------------ TLSWaitSelect --- */

/*
 * WaitSelect() cannot see plaintext this library is already holding, and a
 * program that waits on the descriptor alone therefore sleeps with its answer
 * in memory.  This is the fix, and it is a wrapper rather than a change to
 * bsdsocket.library because bsdsocket.library must not know what TLS is.
 */
LONG tls_TLSWaitSelect(register struct TLSSelect   *sel     __asm("a0"),
                       register struct TLSLibBase *TLSBase  __asm("a6"))
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

        for (i = 0; i < (ULONG)(TLS_FD_MAX / TLS_FD_BITS); i++)
            hits[i] = 0;

        for (i = 0; sel->ts_Connections[i] != NULL; i++)
        {
            TLSConnection *conn = sel->ts_Connections[i];
            LONG           fd   = conn->tc_Fd;

            if (fd < 0 || fd >= TLS_FD_MAX)
                continue;
            if (conn->tc_Pending == NX_NULL)
                continue;
            if ((read_words[TLS_FD_WORD(fd)] & TLS_FD_MASK(fd)) == 0)
                continue;

            hits[TLS_FD_WORD(fd)] |= TLS_FD_MASK(fd);
            ready++;
        }

        if (ready > 0)
        {
            /*
             * Report ONLY the connections that already have plaintext, and do
             * not wait.  Descriptors that were also ready are dropped from the
             * answer, which is a spurious-wakeup shape every select() caller
             * already has to tolerate: the next pass reports them.  The
             * opposite mistake -- claiming a TLS socket is not readable -- is
             * a hang, and is the one this call exists to remove.
             */
            for (i = 0; i < (ULONG)(TLS_FD_MAX / TLS_FD_BITS); i++)
                read_words[i] = hits[i];

            if (sel->ts_Write != NULL)
            {
                ULONG *w = (ULONG *)sel->ts_Write;

                for (i = 0; i < (ULONG)(TLS_FD_MAX / TLS_FD_BITS); i++)
                    w[i] = 0;
            }
            if (sel->ts_Except != NULL)
            {
                ULONG *e = (ULONG *)sel->ts_Except;

                for (i = 0; i < (ULONG)(TLS_FD_MAX / TLS_FD_BITS); i++)
                    e[i] = 0;
            }
            if (sel->ts_SignalMask != NULL)
                *sel->ts_SignalMask = 0;

            return ready;
        }
    }

    if (sel->ts_SocketBase == NULL)
        return -1;

    {
        register APTR   a6 __asm("a6") = sel->ts_SocketBase;
        register APTR   a0 __asm("a0") = sel->ts_Read;
        register APTR   a1 __asm("a1") = sel->ts_Write;
        register APTR   a2 __asm("a2") = sel->ts_Except;
        register APTR   a3 __asm("a3") = sel->ts_Timeout;
        register LONG   d0 __asm("d0") = sel->ts_NFds;
        register ULONG *d1 __asm("d1") = sel->ts_SignalMask;
        register LONG   res __asm("d0");

        __asm __volatile ("jsr a6@(-126:W)"     /* -0x07e, WaitSelect */
                          : "=r" (res)
                          : "r" (a6), "r" (a0), "r" (a1), "r" (a2),
                            "r" (a3), "r" (d0), "r" (d1)
                          : "cc", "memory");
        return res;
    }
}
