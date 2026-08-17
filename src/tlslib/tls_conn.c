/*
 * tls.library, the calls a program makes.
 *
 * The interface, the reasoning about WaitSelect() and the example program are
 * all in include/aminetxduo/tlslib.h.  Here are the implementation and the
 * decisions that do not belong in a public header.
 *
 *   A connection allocates, listed in one place:
 *
 *     crypto metadata      sized by _nx_secure_tls_metadata_size_calculate()
 *                          rather than guessed.  The vendored table wants
 *                          10,128 bytes for the ECC ciphersuites
 *                          (docs/RESEARCH.md 9) and ours costs one
 *                          AMI_TLS_POWM_SCRATCH_LIMBS area on top, measured,
 *                          so 8 KB more: 18,320
 *     record buffer        TLSA_RecordBuffer, default 10 KB.  A certificate
 *                          flight arrives as one record and must fit
 *     remote certificates  TLSA_MaxChain x (NX_SECURE_X509_CERT + 2.5 KB DER)
 *     roots                TLS_MAX_ROOTS x (NX_SECURE_X509_CERT + 2.5 KB DER)
 *
 *   About 40 KB for the defaults, freed at TLSClose() and not held while no
 *   connection is open, the reason this is a separate library and not part
 *   of bsdsocket.library.
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

/*
 * Temporary, for the TLS 1.3 client bring-up.  tls.library links no ami_log on
 * purpose, so this goes straight out the serial port.  Remove with the
 * investigation.  It is compiled out unless TLS13_PROBE_ON is defined.
 */
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
/* The emulated serial line drops anything written faster than it can clock
   out, which is why an earlier run printed "start." for "start.in ".  This is
   temporary diagnostic code, so a spin between characters is the cheapest
   fix. */
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

    /* Without RawIOInit the serial hardware is never set up and RawPutChar
       drops most of what it is given, which is why an earlier run of this
       probe printed "start." for "start.in " and never reached the value. */
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

/* fd_set, open-coded.  <sys/socket.h> serves, and also brings
   devices/timer.h and the whole netinclude set into a library that needs eight
   lines of it.  The layout is the one src/bsdsocket/select.c implements. */
#define TLS_FD_BITS         32
#define TLS_FD_WORD(fd)     ((ULONG)(fd) / TLS_FD_BITS)
#define TLS_FD_MASK(fd)     (1UL << ((ULONG)(fd) % TLS_FD_BITS))
#define TLS_FD_MAX          256

/* TLS_INFO_SIZE_BASE is the floor below which what was passed is not a
   TLSInfo, and the boundary the fill below stops at for a caller that passed
   less than the whole structure.  If a field is ever inserted above
   ti_Resumed, this stops the build.  Without it the resumption fields go
   silently into an older caller's stack. */
_Static_assert(__builtin_offsetof(struct TLSInfo, ti_Resumed) == TLS_INFO_SIZE_BASE,
               "TLS_INFO_SIZE_BASE must be the offset of the first added field");

/* --------------------------------------------------------------- tags --- */

/*
 * A TagItem walk, rather than utility.library's GetTagData().  Four tags are
 * the only reason to open utility.library here, and TAG_MORE is eleven lines.
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

static LONG tls_conn_enter(TLSConnection *conn)
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

static VOID tls_conn_leave(TLSConnection *conn)
{
    const AmiNetXDuoContext *ctx = tls_netx_ctx();

    if (conn == NULL || ctx == NULL || conn->tc_Nest <= 0)
        return;

    if (--conn->tc_Nest > 0)
        return;

    ctx->nxc_Leave(&conn->tc_Caller);
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
    case TLS_ERR_HANDSHAKE: return (CONST_STRPTR)"the server did not complete a TLS handshake";
    case TLS_ERR_UNTRUSTED: return (CONST_STRPTR)"the certificate chain reaches no trusted root";
    case TLS_ERR_HOSTNAME:  return (CONST_STRPTR)"the certificate is issued to another host";
    case TLS_ERR_EXPIRED:   return (CONST_STRPTR)"the certificate is expired or not yet valid";
    case TLS_ERR_TIMEOUT:   return (CONST_STRPTR)"the server stopped answering";
    case TLS_ERR_CLOSED:    return (CONST_STRPTR)"the connection is closed";
    case TLS_ERR_IO:        return (CONST_STRPTR)"the network connection failed";
    case TLS_ERR_NOHOSTNAME:return (CONST_STRPTR)"no host name was given to check the certificate against";
    case TLS_ERR_ALERT:     return (CONST_STRPTR)"the server broke off the connection, so the data is incomplete";
    case TLS_ERR_REFUSED:   return (CONST_STRPTR)"the certificate was refused by this program";
    default:                return (CONST_STRPTR)"internal error";
    }
}

/* ---------------------------------------------------- the name check ----- */

/*
 * nx_secure checks the chain.  It does not check who the certificate is for
 * unless the application asks.  Without this, any host with any valid
 * certificate can answer for any other, so the check is on by default and
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

    /*
     * A TLSA_VerifyHook has already been shown this certificate, including
     * whether the name matched, and said to go on.  nx_secure runs this
     * callback after the verification function, so this is the second half of
     * a decision the caller has already made; making it again here would let
     * the library overrule the program it exists to serve.
     */
    if ((conn->tc_Flags & TLSF_HOOKED) != 0)
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
 * TLSA_NoVerify.  A function rather than NULL: nx_secure calls this
 * unconditionally.
 */
static UINT tls_verify_none(NX_SECURE_X509_CERTIFICATE_STORE *store,
                            NX_SECURE_X509_CERT *certificate,
                            ULONG current_time)
{
    TLSConnection *conn = tls_conn_for_store(store);

    (VOID)certificate;
    (VOID)current_time;

    /*
     * Nothing is checked, so no hook is consulted; the public header says
     * TLSA_NoVerify turns it off along with everything else.  The certificates
     * are still formatted, because "who am I talking to" is a question
     * TLSInfo() answers on an unverified connection too, and on those it is
     * the only answer there is.
     */
    tls_cert_capture(conn, store);

    return NX_SUCCESS;
}

/* ----------------------------------------------------------- teardown --- */

static VOID tls_conn_free(TLSConnection *conn)
{
    if (conn == NULL)
        return;

    tls_store_detach(conn);
    tls_store_close(&conn->tc_StoreIndex);
    tls_cert_release(conn);

    tls_free(conn->tc_Ticket);
    tls_free(conn->tc_RootDer);
    tls_free(conn->tc_RemoteDer);
    tls_free(conn->tc_Remote);

    /*
     * Wipe before freeing.  The record buffer holds the last plaintext, the
     * metadata block holds the expanded cipher and MAC keys, and the
     * connection itself carries nx_secure_tls_key_material, the master
     * secret and both directions' keys and IVs.  AllocVec() hands memory
     * straight back out, and there is no MMU here to fault a read of it.  The
     * next task to ask for 40 KB gets the last connection's keys as its
     * uninitialised contents.
     */
    if (conn->tc_RecordBuffer != NULL)
        tls_bzero(conn->tc_RecordBuffer, conn->tc_RecordBufferSize);
    tls_free(conn->tc_RecordBuffer);

    if (conn->tc_Metadata != NULL)
        tls_bzero(conn->tc_Metadata, conn->tc_MetadataSize);
    tls_free(conn->tc_Metadata);

    tls_bzero(conn, sizeof(*conn));
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
         * src/crypto68k (3.8x).  Idempotent, not re-entrant, hence the
         * semaphore, and a few milliseconds once per boot.
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

    /*
     * TLSA_VerifyHook.  Read only when there is something to report on: with
     * TLSA_NoVerify nothing is checked, so the hook has nothing to be asked
     * about and the header says setting both means the hook loses.  A hook with
     * no entry point is no hook, rather than a wild jump inside a handshake.
     */
    if ((conn->tc_Flags & TLSF_VERIFY) != 0)
    {
        struct Hook *hook = (struct Hook *)tls_tag_data(tags, TLSA_VerifyHook, 0);

        if (hook != NULL && hook->h_Entry != NULL)
            conn->tc_Hook = hook;
    }

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

        tls_strncpy(conn->tc_SessionPath,
                    (session_path != NULL) ? (const char *)session_path
                                           : TLS_DEFAULT_SESSIONS,
                    sizeof(conn->tc_SessionPath));

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

    conn->tc_Socket = ctx->nxc_TcpSocket(socket_base, sock);
    if (conn->tc_Socket == NX_NULL)
    {
        error = TLS_ERR_BADSOCKET;
        goto fail;
    }

    /* The cache is keyed by host and port, so the same name on 443 and on
       8443 are two sessions.  The port is the socket's, not the caller's.
       TLSOpen() is never told one. */
    conn->tc_Port = (UWORD)conn->tc_Socket->nx_tcp_socket_connect_port;

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

    /*
     * Two lists out of one call.  _nx_secure_tls_ecc_initialize() writes both
     * the session's TLS list and the process-wide X.509 list, so it is called
     * with the complete set for the sake of certificate chains, and the
     * session's own list is then narrowed to what the ClientHello offers.  An
     * offer of P-384 and P-521 costs a key generation each before the
     * ClientHello is sent and buys nothing.  P-256 is what these servers pick.
     */
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
     * obviously unset, and 0 is nx_secure's own "do not check validity dates".
     * See src/tlslib/tls_time.c for what that gives up.
     */
    (VOID)_nx_secure_tls_session_time_function_set(&conn->tc_Session,
                                                    tls_time_now);
    conn->tc_ExpiryChecked = tls_time_is_known();
    conn->tc_UnixTime      = tls_time_now();

    /*
     * Every connection goes in the registry, checked or not.  The
     * session-resumption overrides in tls_resume.c get from an
     * NX_SECURE_TLS_SESSION back to this structure through it, and resumption
     * does not care whether the chain is being checked.
     */
    tls_registry_add(conn);

    if ((conn->tc_Flags & TLSF_VERIFY) != 0)
    {
        (VOID)_nx_secure_tls_session_certificate_callback_set(
                  &conn->tc_Session, tls_certificate_callback);

        /* Swaps in the lazy root loader, on top of the registration above. */
        tls_store_attach(conn);
    }
    else
    {
        /*
         * No trust store, and nothing to check against.  nx_secure fails
         * the chain outright, so the check is replaced rather than left to
         * fail.  It cannot be set to NX_NULL, nx_secure_tls_remote_
         * certificate_verify.c calls the pointer unconditionally.  This is
         * what TLSA_NoVerify does, and why the header spells it out.
         */
        conn->tc_Session.nx_secure_remote_certificate_verify = tls_verify_none;
    }

    /* ---- the handshake -------------------------------------------------- */

    /*
     * Look for a cached session before the bracket.  This reads a file, and a
     * disk access inside the ThreadX bracket holds the baton away from the IP
     * thread.  The trust store's fetch must read inside the bracket, because
     * the issuer is not known until the server has spoken.  This one is known
     * from the host name.
     */
    tls_resume_prepare(conn);

    if (tls_conn_enter(conn) != 0)
    {
        error = TLS_ERR_NOSTACK;
        goto fail_session;
    }

    start_ticks = ami_tls_eclock();

    TLS13_PROBE("start.in ", conn->tc_Timeout);

    status = _nx_secure_tls_session_start(&conn->tc_Session, conn->tc_Socket,
                                           conn->tc_Timeout);

    TLS13_PROBE("start.out ", (ULONG)status);

    conn->tc_HandshakeMillis =
        ami_tls_eclock_micros(ami_tls_eclock() - start_ticks) / 1000UL;

    tls_conn_leave(conn);

    if (status != NX_SUCCESS)
    {
        /*
         * A handshake that offered a cached session and then failed must drop
         * that session, or the machine retries the same broken resumption
         * forever.  A wrong answer here costs one full handshake.  An omission
         * costs a host that never works again until the entry ages out.
         */
        if ((conn->tc_ResumeFlags & TLSR_OFFERED) != 0 &&
            (conn->tc_Flags & TLSF_REFUSED) == 0)
        {
            tls_resume_evict(conn);
        }

        /*
         * A refusal is not a failure.  Every status that stops a handshake
         * looks alike from here, so the flag rather than the status carries
         * it, and TLS_ERR_REFUSED is what lets a caller tell "the user said
         * no" from "it did not work" and not offer to retry.
         */
        error = ((conn->tc_Flags & TLSF_REFUSED) != 0)
                ? TLS_ERR_REFUSED : tls_error_from_nx(status);
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

    /*
     * Also outside the bracket: this is the other half of the disk mirror.
     *
     * Not when a TLSA_VerifyHook let a chain through that did not check out.
     * A cached session records that the check happened and against what, and a
     * later connection that resumes it performs no check at all: caching this
     * one would let one program's "yes, I know, go on" silently answer for
     * every program after it, including ones with no hook.  The cost is one
     * full handshake next time, on the connections where that is the right
     * price.
     */
    if (conn->tc_VerifyReason == TLS_OK)
        tls_resume_record(conn);

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
     * The delete sits outside the bracket test.  A session that was created is
     * on nx_secure's process-wide created list, and the memory it occupies is
     * about to be freed, so a session left linked puts a dangling pointer in
     * a global on a machine with no memory protection, worse than a delete
     * that fails.  The bracket can only fail when the ThreadX kernel is
     * already down, where there is nothing left for the delete to disturb.
     */
    (VOID)_nx_secure_tls_session_delete(&conn->tc_Session);

    /*
     * The descriptor is not closed.  It was the caller's before TLSOpen() and
     * is the caller's again, which is what allows plain HTTP to a proxy and
     * then an upgrade.
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

        tls_trace("[resume] session_receive -> %ld packet %lx state %ld",
                  (LONG)status, (LONG)packet,
                  (LONG)conn->tc_Session.nx_secure_tls_client_state);

        if (status == NX_SECURE_TLS_ALERT_RECEIVED)
        {
            /*
             * Every alert arrives as this one status.  The alert itself is
             * left on the session.  A close_notify is a warning and means the
             * peer finished, so it is end of stream.  Anything fatal means the
             * peer tore the connection down mid-transfer, a decrypt_error, a
             * bad_record_mac, an internal_error.  A report of that as end of
             * stream hands the caller a truncated file it cannot tell from a
             * complete one.
             *
             * NX_SECURE_TLS_CLOSE_NOTIFY_RECEIVED is not tested for: only the
             * DTLS half of nx_secure ever returns it, and DTLS is not built.
             */
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
            /*
             * A bare FIN with no close_notify.  RFC 5246 7.2.1 calls that a
             * truncation and says to treat it as an error, and this does not.
             * Many servers still close the socket the moment they finish
             * writing, and a failure for those breaks downloads that work
             * today.
             */
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

    /*
     * ti_Size is a version.  A caller compiled against the header before
     * resumption existed passes the smaller size and gets every field it knows
     * about.  The resumption fields are written only when the caller's
     * structure is big enough to hold them.  Anything smaller than the
     * original structure is not a TLSInfo.
     */
    if (info->ti_Size < (ULONG)TLS_INFO_SIZE_BASE)
        return -1;

    info->ti_Version         = conn->tc_Protocol;
    info->ti_CipherSuite     = conn->tc_CipherSuite;
    info->ti_ChainDepth      = conn->tc_ChainDepth;
    info->ti_HandshakeMillis = conn->tc_HandshakeMillis;
    info->ti_Error           = conn->tc_Error;

    /*
     * NULL on a resumed handshake, because a resumed handshake sends no
     * certificate; ti_Resumed is what tells the two apart.  The library's, and
     * alive until TLSClose().
     */
    info->ti_PeerChain       = conn->tc_Certs;
    info->ti_Peer            = (conn->tc_CertCount > 0) ? conn->tc_Certs : NULL;

    /*
     * The chain reached a root in the trust store, which is not the same as
     * the handshake having completed once a TLSA_VerifyHook can carry one
     * past a check that failed.  tc_VerifyReason is what the check concluded
     * before the caller was asked.
     */
    info->ti_Verified        = (BOOL)(((conn->tc_Flags & TLSF_VERIFY) != 0 &&
                                       (conn->tc_Flags & TLSF_HANDSHAKEN) != 0 &&
                                       conn->tc_VerifyReason == TLS_OK)
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
 * Bytes this library is holding that have not been decrypted yet.
 *
 * TLSPending() answers whether plaintext is ready, which is not the whole
 * question.  nx_secure reads whatever the socket gives it, and one TCP segment
 * routinely carries more than one TLS record, so after a TLSRead() the
 * remaining records sit undecrypted in nx_secure's queue.  The socket is then
 * not readable, TLSPending() is 0, and a complete record is sitting in memory.
 * A caller that treats an unreadable socket as "nothing to do" waits for data
 * it already has.
 *
 * Non-zero here means a TLSRead() can make progress without another byte from
 * the socket.  Kept out of TLSPending() because the two answers mean different
 * things: plaintext is ready to copy, and buffered bytes can still be half a
 * record.  A caller that polls without blocking must test both, and read when
 * either is set.  The read can still block if what is buffered is an
 * incomplete record, which is the same bound TLSA_Timeout already covers.
 */
LONG tls_TLSBuffered(register struct TLSConnection *conn    __asm("a0"),
                     register struct TLSLibBase    *TLSBase __asm("a6"))
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
 * The machine's entropy pool, which this library already borrows from
 * bsdsocket.library (see tls_netx.c) and which nothing outside can reach.
 *
 * A ported client asks its TLS backend for random bytes, curl routes every
 * Curl_rand() through the backend when it is built with TLS, and otherwise
 * falls back to a time-seeded LCG in the client.  The pool behind this is a
 * SHA-256 hash DRBG.  docs/RESEARCH.md 9 records how little entropy the seed
 * carries on an unattended Amiga, and this call inherits that.  It is an
 * improvement on an LCG, and it is the same generator the TLS session keys
 * come from, so a caller cannot get two different qualities of randomness by
 * accident.
 *
 * Returns the number of bytes written, or -1.  Never partially fills, because
 * the generator cannot fail.
 */
LONG tls_TLSRandom(register APTR               buffer  __asm("a0"),
                   register LONG               length  __asm("d0"),
                   register struct TLSLibBase *TLSBase __asm("a6"))
{
    const AmiNetXDuoContext *ctx;

    (VOID)TLSBase;

    if (buffer == NULL || length < 0)
        return -1;

    /*
     * The pool lives in bsdsocket.library and is reached through the private
     * context, so a caller that has not opened a connection yet has nothing to
     * draw from.  Report that rather than handing back zeroes.
     */
    ctx = tls_netx_ctx();
    if (ctx == NULL)
        return -1;

    /*
     * Bytes, not rand() draws.  nxc_random_rand() owes rand()'s caller a value
     * in 0..0x7FFFFFFF and clears bit 31 to provide it.  All four bytes of one
     * draw packed together therefore left bit 7 clear in every fourth byte, 31
     * bits per 32, at a fixed position the caller cannot see and does not look
     * for.  nxc_random_bytes() is the same generator without the obligation,
     * and one call covers the whole buffer.
     */
    ctx->nxc_random_bytes(buffer, (ULONG)length);

    return length;
}

/* ------------------------------------------------------ TLSWaitSelect --- */

/*
 * WaitSelect() cannot see plaintext this library is already holding, so a
 * program that waits on the descriptor alone sleeps with its answer in memory.
 * A wrapper rather than a change to bsdsocket.library, because
 * bsdsocket.library must not know what TLS is.
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
             * Report only the connections that already have plaintext, and do
             * not wait.  Descriptors that were also ready are dropped from the
             * answer.  Every select() caller already tolerates that, and the
             * next pass reports them.  The opposite mistake, a claim that a
             * TLS socket is not readable, is a hang, and is what this call
             * exists to remove.
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
}
