/*
 * tls.library, TLSClose() keeps every NetX Secure operation inside one lock.
 *
 * TLS sessions share process-wide created-object state.  session_end() was
 * bracketed but session_delete() ran after the bracket, so two Exec Tasks
 * closing different connections could mutate that shared state concurrently.
 * This host test records the call order at the vector boundary.  EDL is safe;
 * the old ELD order fails.
 *
 * The bracket used to be the ThreadX baton, borrowed from our own
 * bsdsocket.library over a private LVO.  tls.library runs on any stack now and
 * borrows nothing, so the bracket is _nx_secure_tls_protection -- nx_secure's
 * OWN mutex, taken one level up, so that the two orderings cannot disagree.
 * The invariant under test did not change and neither did this test's shape:
 * only which pair of calls is instrumented.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"
#include "tls_vectors.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int  checks;
static int  failures;
static int  h_depth;
static int  h_enter_calls;
static int  h_leave_calls;
static int  h_end_calls;
static int  h_delete_calls;
static int  h_delete_active;
static int  h_delete_max;
static int  h_concurrent;
static char h_order[16];
static int  h_order_len;
static pthread_mutex_t h_baton = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_state = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_start = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  h_start_cond = PTHREAD_COND_INITIALIZER;
static int             h_start_ready;
static int             h_start_go;
static NX_TCP_SOCKET   h_socket;

#define CHECK(expr)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(expr))                                                       \
        {                                                                  \
            failures++;                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #expr);             \
        }                                                                  \
    } while (0)

static VOID h_mark(char c)
{
    if (h_order_len + 1 < (int)sizeof(h_order))
    {
        h_order[h_order_len++] = c;
        h_order[h_order_len] = '\0';
    }
}

/*
 * The instrumented pair.  tls_conn.c reaches nx_secure's protection mutex
 * through these, and nothing else in this link defines them, so every
 * acquisition the shipping code makes is counted here.
 */
TX_MUTEX _nx_secure_tls_protection;

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)wait_option;

    CHECK(mutex_ptr == &_nx_secure_tls_protection);

    (VOID)pthread_mutex_lock(&h_baton);
    CHECK(h_depth == 0);
    h_depth++;
    h_enter_calls++;

    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    CHECK(mutex_ptr == &_nx_secure_tls_protection);

    CHECK(h_depth == 1);
    h_mark('L');
    h_depth--;
    h_leave_calls++;
    (VOID)pthread_mutex_unlock(&h_baton);

    return TX_SUCCESS;
}

/*
 * The transport.  Its own behaviour is test_tls_transport.c's subject; here it
 * only has to hand back something session_start() can be given -- and to
 * record `server`, which is the whole of what puts nx_secure on its server
 * branch (_nx_secure_tls_session_start() reads nx_tcp_socket_client_type).
 */
static int h_transport_server = -1;

VOID tls_transport_open(TLSTransport *transport, APTR socket_base, LONG fd,
                        NX_PACKET_POOL *pool, BOOL server, BOOL ipv6)
{
    (VOID)socket_base; (VOID)fd; (VOID)pool; (VOID)ipv6;
    memset(transport, 0, sizeof(*transport));
    h_transport_server = (server != FALSE);
}

NX_TCP_SOCKET *tls_transport_socket(TLSTransport *transport)
{
    (VOID)transport;
    return &h_socket;
}

BOOL tls_sock_have_lvo(APTR base, ULONG lvo)
{
    (VOID)lvo;
    return (BOOL)((base != NULL) ? TRUE : FALSE);
}

BOOL tls_sock_is_connected_tcp(APTR base, LONG fd, UWORD *port, UWORD *family)
{
    (VOID)fd;

    if (port != NULL)
        *port = 443;
    if (family != NULL)
        *family = TLS_SOCK_AF_INET;

    return (BOOL)((base != NULL) ? TRUE : FALSE);
}

VOID ObtainSemaphore(struct SignalSemaphore *semaphore)
{
    (VOID)semaphore;
}

VOID ReleaseSemaphore(struct SignalSemaphore *semaphore)
{
    (VOID)semaphore;
}

APTR tls_alloc(ULONG size)
{
    return calloc(1, (size_t)size);
}

ULONG tls_strlen(const char *text)
{
    return (ULONG)strlen(text);
}

const NX_SECURE_TLS_CRYPTO ami_crypto_tls_ciphers_ecc = { 0 };
const USHORT ami_crypto_ecc_supported_groups[] = { 0 };
const NX_CRYPTO_METHOD *ami_crypto_ecc_curves[] = { NULL };
const UINT ami_crypto_ecc_supported_groups_size = 1;
const USHORT ami_crypto_ecc_offered_groups[] = { 0 };
const NX_CRYPTO_METHOD *ami_crypto_ecc_offered_curves[] = { NULL };
const UINT ami_crypto_ecc_offered_groups_size = 1;

/* ------------------------------------------------ the server identity --- */

static int    h_cert_init_calls;
static int    h_cert_add_calls;
static int    h_key_reset_calls;
static UINT   h_cert_key_type;
static USHORT h_cert_der_length;
static USHORT h_cert_key_length;

UINT _nx_secure_x509_certificate_initialize(NX_SECURE_X509_CERT *certificate,
                                            UCHAR *data, USHORT length,
                                            UCHAR *raw, USHORT raw_size,
                                            const UCHAR *key, USHORT key_length,
                                            UINT key_type)
{
    (VOID)certificate; (VOID)data; (VOID)key;

    /* The DER is parsed in place, so a raw buffer here would mean the
       certificate outliving the bytes it points at. */
    CHECK(raw == NULL);
    CHECK(raw_size == 0);

    h_cert_init_calls++;
    h_cert_der_length = length;
    h_cert_key_length = key_length;
    h_cert_key_type   = key_type;

    return NX_SUCCESS;
}

UINT ami_tls_local_certificate_add(NX_SECURE_TLS_SESSION *session,
                                   NX_SECURE_X509_CERT *certificate)
{
    (VOID)session; (VOID)certificate;
    h_cert_add_calls++;
    return NX_SUCCESS;
}

VOID ami_tls_rsa_key_reset(VOID)
{
    h_key_reset_calls++;
}

/* dos.library, for the two DER files tls_server.c reads. */
BPTR Open(STRPTR name, LONG mode)
{
    (VOID)mode;
    return (BPTR)fopen((const char *)name, "rb");
}

VOID Close(BPTR fh)
{
    if (fh != (BPTR)0)
        fclose((FILE *)fh);
}

LONG Read(BPTR fh, APTR buffer, LONG length)
{
    return (LONG)fread(buffer, 1, (size_t)length, (FILE *)fh);
}

UINT ami_tls_crypto_initialize(VOID) { return NX_SUCCESS; }
BOOL ami_tls_timer_open(VOID) { return TRUE; }
ULONG ami_tls_eclock(VOID) { return 0; }
ULONG ami_tls_eclock_micros(ULONG ticks) { return ticks; }
ULONG ami_tls_seed_rng(VOID) { return 256; }
VOID  ami_random_bytes(APTR buffer, ULONG length) { memset(buffer, 0, length); }

UINT _nx_secure_tls_metadata_size_calculate(
        const NX_SECURE_TLS_CRYPTO *crypto, ULONG *size)
{
    (VOID)crypto;
    *size = 64;
    return NX_SUCCESS;
}

/* Off for the ordering tests, which want TLSOpen() to stop right here; on for
   the server tests, which are about what happens after it. */
static int h_create_ok;

/*
 * WHAT CREATES _nx_secure_tls_protection.  Nothing called it for a while, and
 * the consequence was every TLSOpen() in the tree answering TLS_ERR_NOSTACK,
 * because tls_conn_enter() then took a mutex the shim in tls_netx.c had no
 * slot for.  Counted, and checked against session_create below: it has to
 * happen before the first session exists, because it also resets the
 * process-wide session list.
 */
static int h_initialize_calls;
static int h_initialize_before_first_create;

VOID _nx_secure_tls_initialize(VOID)
{
    h_initialize_calls++;
}

/* Which protocol version a server session was pinned to, and how often. */
static USHORT h_version_override;
static int    h_version_override_calls;

UINT _nx_secure_tls_session_protocol_version_override(
        NX_SECURE_TLS_SESSION *session, USHORT version)
{
    (VOID)session;
    h_version_override = version;
    h_version_override_calls++;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_session_create(NX_SECURE_TLS_SESSION *session,
                                   const NX_SECURE_TLS_CRYPTO *crypto,
                                   VOID *metadata, ULONG metadata_size)
{
    (VOID)session;
    (VOID)crypto;
    (VOID)metadata;
    (VOID)metadata_size;
    CHECK(h_depth == 1);
    if (h_initialize_calls > 0)
        h_initialize_before_first_create = 1;
    return h_create_ok ? NX_SUCCESS : NX_INVALID_PARAMETERS;
}

UINT _nx_secure_tls_ecc_initialize(NX_SECURE_TLS_SESSION *session,
                                   const USHORT *groups, USHORT group_count,
                                   const NX_CRYPTO_METHOD **curves)
{
    (VOID)session; (VOID)groups; (VOID)group_count; (VOID)curves;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_session_packet_buffer_set(NX_SECURE_TLS_SESSION *session,
                                               UCHAR *buffer, ULONG size)
{
    (VOID)session; (VOID)buffer; (VOID)size;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_remote_certificate_allocate(NX_SECURE_TLS_SESSION *session,
                                                NX_SECURE_X509_CERT *certificate,
                                                UCHAR *buffer, UINT size)
{
    (VOID)session; (VOID)certificate; (VOID)buffer; (VOID)size;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_session_sni_extension_set(NX_SECURE_TLS_SESSION *session,
                                               NX_SECURE_X509_DNS_NAME *name)
{
    (VOID)session; (VOID)name;
    return NX_SUCCESS;
}

/* The ALPN pair is test_tls_alpn.c's subject; tls_alpn.c is linked here only
   because tls_conn.c calls its encoder on the TLSOpen() path. */
UINT _nx_secure_tls_alpn_protocol_set(NX_SECURE_TLS_SESSION *session,
                                      const UCHAR *list, USHORT length)
{
    (VOID)session; (VOID)list; (VOID)length;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_alpn_protocol_get(NX_SECURE_TLS_SESSION *session,
                                      const UCHAR **protocol, UCHAR *length)
{
    (VOID)session; (VOID)protocol; (VOID)length;
    return NX_SECURE_TLS_EXTENSION_NOT_FOUND;
}

UINT _nx_secure_tls_session_time_function_set(NX_SECURE_TLS_SESSION *session,
                                               ULONG (*time_func)(void))
{
    (VOID)session; (VOID)time_func;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_session_certificate_callback_set(
        NX_SECURE_TLS_SESSION *session,
        ULONG (*callback)(NX_SECURE_TLS_SESSION *, NX_SECURE_X509_CERT *))
{
    (VOID)session; (VOID)callback;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_session_start(NX_SECURE_TLS_SESSION *session,
                                  NX_TCP_SOCKET *socket, UINT wait_option)
{
    (VOID)session; (VOID)socket; (VOID)wait_option;
    return NX_SUCCESS;
}

UINT _nx_secure_x509_common_name_dns_check(NX_SECURE_X509_CERT *certificate,
                                           const UCHAR *dns_name,
                                           UINT dns_name_length)
{
    (VOID)certificate; (VOID)dns_name; (VOID)dns_name_length;
    return NX_SUCCESS;
}

TLSConnection *tls_conn_for_session(const NX_SECURE_TLS_SESSION *session)
{
    (VOID)session;
    return NULL;
}

LONG tls_store_open(TLSStore *store, const char *path)
{
    (VOID)store; (VOID)path;
    return TLS_OK;
}

VOID tls_store_attach(TLSConnection *conn) { (VOID)conn; }
VOID tls_registry_add(TLSConnection *conn) { (VOID)conn; }
VOID tls_resume_prepare(TLSConnection *conn) { (VOID)conn; }
VOID tls_resume_evict(TLSConnection *conn) { (VOID)conn; }
VOID tls_resume_record(TLSConnection *conn) { (VOID)conn; }
BOOL tls_time_is_known(VOID) { return TRUE; }
ULONG tls_time_now(VOID) { return 1; }

UINT _nx_secure_tls_session_end(NX_SECURE_TLS_SESSION *session,
                                ULONG wait_option)
{
    (VOID)session;
    (VOID)wait_option;

    CHECK(h_depth == 1);
    h_mark('E');
    h_end_calls++;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_session_delete(NX_SECURE_TLS_SESSION *session)
{
    struct timespec pause = { 0, 50L * 1000L * 1000L };

    (VOID)session;

    CHECK(h_depth == 1);
    h_mark('D');
    h_delete_calls++;

    (VOID)pthread_mutex_lock(&h_state);
    h_delete_active++;
    if (h_delete_active > h_delete_max)
        h_delete_max = h_delete_active;
    (VOID)pthread_mutex_unlock(&h_state);

    if (h_concurrent)
        (VOID)nanosleep(&pause, NULL);

    (VOID)pthread_mutex_lock(&h_state);
    h_delete_active--;
    (VOID)pthread_mutex_unlock(&h_state);

    return NX_SUCCESS;
}

VOID tls_store_detach(TLSConnection *conn)
{
    (VOID)conn;
}

VOID tls_store_close(TLSStore *store)
{
    (VOID)store;
}

VOID tls_bzero(APTR ptr, ULONG size)
{
    memset(ptr, 0, (size_t)size);
}

VOID tls_memcpy(APTR dst, const void *src, ULONG size)
{
    memcpy(dst, src, (size_t)size);
}

VOID tls_free(APTR ptr)
{
    free(ptr);
}

static void *h_close_worker(void *arg)
{
    TLSConnection *conn = (TLSConnection *)arg;

    (VOID)pthread_mutex_lock(&h_start);
    h_start_ready++;
    (VOID)pthread_cond_broadcast(&h_start_cond);
    while (!h_start_go)
        (VOID)pthread_cond_wait(&h_start_cond, &h_start);
    (VOID)pthread_mutex_unlock(&h_start);

    tls_TLSClose(conn, conn->tc_Base);
    return NULL;
}

static VOID h_test_concurrent_close(struct TLSLibBase *base)
{
    TLSConnection *a;
    TLSConnection *b;
    pthread_t      ta;
    pthread_t      tb;
    int            create_a;
    int            create_b;
    int            workers;

    printf("tls_close: two Tasks cannot delete sessions concurrently\n");

    a = (TLSConnection *)calloc(1, sizeof(*a));
    b = (TLSConnection *)calloc(1, sizeof(*b));
    CHECK(a != NULL);
    CHECK(b != NULL);
    if (a == NULL || b == NULL)
    {
        free(a);
        free(b);
        return;
    }

    a->tc_Base = base;
    b->tc_Base = base;

    h_concurrent = 1;
    h_delete_max = 0;
    h_start_ready = 0;
    h_start_go = 0;

    create_a = pthread_create(&ta, NULL, h_close_worker, a);
    create_b = pthread_create(&tb, NULL, h_close_worker, b);
    CHECK(create_a == 0);
    CHECK(create_b == 0);
    workers = (create_a == 0) + (create_b == 0);

    (VOID)pthread_mutex_lock(&h_start);
    while (h_start_ready < workers)
        (VOID)pthread_cond_wait(&h_start_cond, &h_start);
    h_start_go = 1;
    (VOID)pthread_cond_broadcast(&h_start_cond);
    (VOID)pthread_mutex_unlock(&h_start);

    if (create_a == 0)
        CHECK(pthread_join(ta, NULL) == 0);
    else
        free(a);
    if (create_b == 0)
        CHECK(pthread_join(tb, NULL) == 0);
    else
        free(b);
    CHECK(h_delete_max == 1);
    CHECK(h_delete_active == 0);
    CHECK(h_depth == 0);

    h_concurrent = 0;
}

static VOID h_test_hostname_limit(VOID)
{
    TLSConnection conn;
    char          accepted[NX_SECURE_X509_DNS_NAME_MAX + 1];
    char          rejected[NX_SECURE_X509_DNS_NAME_MAX + 2];
    ULONG         i;

    printf("tls_open: an over-limit host name is rejected, not truncated\n");

    memset(&conn, 0, sizeof(conn));
    for (i = 0; i < (ULONG)NX_SECURE_X509_DNS_NAME_MAX; i++)
        accepted[i] = (char)('a' + (i % 26UL));
    accepted[NX_SECURE_X509_DNS_NAME_MAX] = '\0';

    CHECK(tls_hostname_set(&conn, (CONST_STRPTR)accepted,
                           (ULONG)NX_SECURE_X509_DNS_NAME_MAX) == TLS_OK);
    CHECK(conn.tc_HostNameLength == NX_SECURE_X509_DNS_NAME_MAX);
    CHECK(conn.tc_Sni.nx_secure_x509_dns_name_length ==
          NX_SECURE_X509_DNS_NAME_MAX);
    CHECK(memcmp(conn.tc_HostName, accepted,
                 NX_SECURE_X509_DNS_NAME_MAX) == 0);

    memset(&conn, 0, sizeof(conn));
    for (i = 0; i <= (ULONG)NX_SECURE_X509_DNS_NAME_MAX; i++)
        rejected[i] = (char)('a' + (i % 26UL));
    rejected[NX_SECURE_X509_DNS_NAME_MAX + 1] = '\0';

    CHECK(tls_hostname_set(&conn, (CONST_STRPTR)rejected,
                           (ULONG)NX_SECURE_X509_DNS_NAME_MAX + 1UL) ==
          TLS_ERR_BADHOSTNAME);
    CHECK(conn.tc_HostNameLength == 0);
    CHECK(conn.tc_Sni.nx_secure_x509_dns_name_length == 0);
}

static VOID h_test_path_limit(VOID)
{
    char  accepted[TLS_STORE_PATH_MAX];
    char  rejected[TLS_STORE_PATH_MAX + 1];
    char  copied[TLS_STORE_PATH_MAX];
    ULONG i;

    printf("tls_open: over-limit file paths are rejected, not truncated\n");

    for (i = 0; i + 1 < sizeof(accepted); i++)
        accepted[i] = (char)('a' + (i % 26UL));
    accepted[sizeof(accepted) - 1] = '\0';

    memset(copied, 0x5A, sizeof(copied));
    CHECK(tls_path_set(copied, sizeof(copied),
                       (CONST_STRPTR)accepted) == TLS_OK);
    CHECK(strcmp(copied, accepted) == 0);

    for (i = 0; i < TLS_STORE_PATH_MAX; i++)
        rejected[i] = (char)('a' + (i % 26UL));
    rejected[TLS_STORE_PATH_MAX] = '\0';

    memset(copied, 0x5A, sizeof(copied));
    CHECK(tls_path_set(copied, sizeof(copied),
                       (CONST_STRPTR)rejected) == TLS_ERR_BADPATH);
    CHECK((UBYTE)copied[0] == 0x5A);
}

static VOID h_test_waitselect_unique_descriptors(VOID)
{
    TLSConnection  conn;
    TLSConnection *connections[3];
    NX_PACKET      pending;
    struct TLSSelect sel;
    ULONG          read_words[8];
    LONG           ready;

    printf("tls_waitselect: duplicate connections count one descriptor\n");

    memset(&conn, 0, sizeof(conn));
    memset(&pending, 0, sizeof(pending));
    memset(&sel, 0, sizeof(sel));
    memset(read_words, 0, sizeof(read_words));

    conn.tc_Fd = 37;
    conn.tc_Pending = &pending;
    connections[0] = &conn;
    connections[1] = &conn;
    connections[2] = NULL;
    read_words[1] = 1UL << 5;

    sel.ts_Size = (ULONG)sizeof(sel);
    sel.ts_NFds = 38;
    sel.ts_Read = read_words;
    sel.ts_Connections = connections;

    ready = tls_TLSWaitSelect(&sel, NULL);
    CHECK(ready == 1);
    CHECK(read_words[0] == 0UL);
    CHECK(read_words[1] == (1UL << 5));

    /* select() considers [0, nfds); a set bit at exactly nfds is outside the
       request and the TLS buffered fast path must obey the same boundary. */
    sel.ts_NFds = 37;
    ready = tls_TLSWaitSelect(&sel, NULL);
    CHECK(ready == -1);
    CHECK(read_words[1] == (1UL << 5));
}

static VOID h_test_waitselect_encrypted_queue(VOID)
{
    TLSConnection   conn;
    TLSConnection  *connections[2];
    NX_PACKET       queued;
    struct TLSSelect sel;
    ULONG           read_words[8];
    LONG            ready;

    printf("tls_waitselect: queued encrypted records are readable\n");

    memset(&conn, 0, sizeof(conn));
    memset(&queued, 0, sizeof(queued));
    memset(&sel, 0, sizeof(sel));
    memset(read_words, 0, sizeof(read_words));

    conn.tc_Fd = 19;
    conn.tc_Session.nx_secure_record_queue_header = &queued;
    connections[0] = &conn;
    connections[1] = NULL;
    read_words[0] = 1UL << 19;

    sel.ts_Size = (ULONG)sizeof(sel);
    sel.ts_NFds = 20;
    sel.ts_Read = read_words;
    sel.ts_Connections = connections;

    ready = tls_TLSWaitSelect(&sel, NULL);
    CHECK(ready == 1);
    CHECK(read_words[0] == (1UL << 19));
}

static VOID h_test_waitselect_wide_descriptor(VOID)
{
    TLSConnection   conn;
    TLSConnection  *connections[2];
    NX_PACKET       pending;
    struct TLSSelect sel;
    ULONG           read_words[32];
    ULONG           write_words[32];
    ULONG           except_words[32];
    LONG            ready;
    ULONG           i;

    printf("tls_waitselect: buffered descriptors follow SBTC_DTABLESIZE\n");

    memset(&conn, 0, sizeof(conn));
    memset(&pending, 0, sizeof(pending));
    memset(&sel, 0, sizeof(sel));
    memset(read_words, 0, sizeof(read_words));
    for (i = 0; i < 32; i++)
    {
        write_words[i] = 0xaaaaaaaaUL;
        except_words[i] = 0x55555555UL;
    }

    conn.tc_Fd = 300;
    conn.tc_Pending = &pending;
    connections[0] = &conn;
    connections[1] = NULL;
    read_words[9] = 1UL << 12;
    read_words[10] = 0xdeadbeefUL;

    sel.ts_Size = (ULONG)sizeof(sel);
    sel.ts_NFds = 301;
    sel.ts_Read = read_words;
    sel.ts_Write = write_words;
    sel.ts_Except = except_words;
    sel.ts_Connections = connections;

    ready = tls_TLSWaitSelect(&sel, NULL);
    CHECK(ready == 1);
    CHECK(read_words[9] == (1UL << 12));
    CHECK(read_words[10] == 0xdeadbeefUL);
    CHECK(write_words[9] == 0UL);
    CHECK(except_words[9] == 0UL);
    CHECK(write_words[10] == 0xaaaaaaaaUL);
    CHECK(except_words[10] == 0x55555555UL);
}

/* ------------------------------------------------------ server mode --- */

/*
 * TLSA_Server, the tag that makes nx_secure's server handshake reachable.
 *
 * The identity loader is driven directly rather than through TLSOpenA.  A tag
 * carries its data in a ULONG, which is a pointer on the target and half of
 * one on this host, so a path cannot travel through a tag list here; the tag
 * plumbing that CAN be checked without one is below, and
 * test_tls_transport.c's server check is the other half -- that a server
 * transport presents the client_type nx_secure branches on.
 */

static void h_write_file(const char *path, UBYTE lead, size_t length)
{
    FILE  *f = fopen(path, "wb");
    size_t i;

    CHECK(f != NULL);
    if (f == NULL)
        return;

    fputc(lead, f);
    for (i = 1; i < length; i++)
        fputc((int)(i & 0xFF), f);
    fclose(f);
}

static VOID h_test_server_identity(struct TLSLibBase *base)
{
    const char    *cert = "test_tls_close_cert.der";
    const char    *key  = "test_tls_close_key.der";
    const char    *pem  = "test_tls_close_cert.pem";
    const char    *big  = "test_tls_close_big.der";
    TLSConnection *conn;
    TLSConnection *second;

    printf("tls_server: the server identity loads, or is refused whole\n");

    h_write_file(cert, 0x30, 512);
    h_write_file(key, 0x30, 300);

    conn = (TLSConnection *)calloc(1, sizeof(*conn));
    CHECK(conn != NULL);
    if (conn == NULL)
        return;
    conn->tc_Base = base;

    h_cert_init_calls = 0;
    h_cert_add_calls  = 0;
    h_key_reset_calls = 0;
    base->tb_ServerKeys = 0;

    CHECK(tls_server_identity(conn, (CONST_STRPTR)cert, (CONST_STRPTR)key,
                              TLS_KEY_RSA) == TLS_OK);
    CHECK(h_cert_init_calls == 1);
    CHECK(h_cert_add_calls == 1);
    CHECK(h_cert_der_length == 512);
    CHECK(h_cert_key_length == 300);
    CHECK(h_cert_key_type == NX_SECURE_X509_KEY_TYPE_RSA_PKCS1_DER);
    CHECK(conn->tc_LocalDerLength == 512);
    CHECK(conn->tc_LocalKeyLength == 300);
    CHECK(base->tb_ServerKeys == 1);

    /*
     * The prime table in ami_tls_crypto.c points into the key buffer, and it
     * is process-wide.  A second server must not have it cleared out from
     * under it when the first one closes, and the last one out must clear it
     * or it points at freed memory.
     */
    second = (TLSConnection *)calloc(1, sizeof(*second));
    CHECK(second != NULL);
    if (second != NULL)
    {
        second->tc_Base = base;
        CHECK(tls_server_identity(second, (CONST_STRPTR)cert, (CONST_STRPTR)key,
                                  TLS_KEY_RSA) == TLS_OK);
        CHECK(base->tb_ServerKeys == 2);

        tls_server_forget(conn);
        CHECK(base->tb_ServerKeys == 1);
        CHECK(h_key_reset_calls == 0);

        tls_server_forget(second);
        CHECK(base->tb_ServerKeys == 0);
        CHECK(h_key_reset_calls == 1);

        /* Idempotent: TLSClose() calls it and so does a failed TLSOpen(). */
        tls_server_forget(second);
        CHECK(h_key_reset_calls == 1);
        CHECK(second->tc_LocalDer == NULL);
        CHECK(second->tc_LocalKey == NULL);

        free(second);
    }

    /*
     * AN RSA SERVER IS PINNED TO TLS 1.2 AND AN EC ONE IS NOT.  1.3 signs
     * CertificateVerify with RSA-PSS and nx_crypto has no PSS sign, so an
     * unpinned RSA server reaches CertificateVerify and fails there; the
     * emulator arm saw exactly that before this existed.  ECDSA
     * CertificateVerify is complete, so an EC key is left at whatever the
     * handshake negotiates.
     */
    CHECK(h_version_override_calls == 2);        /* the two RSA loads above */
    CHECK(h_version_override == NX_SECURE_TLS_VERSION_TLS_1_2);

    /* TLS_KEY_EC must reach nx_secure as the SEC1 type: handing an EC key to
       the PKCS#1 parser is a certificate that loads and a signature that is
       never valid. */
    h_version_override_calls = 0;
    CHECK(tls_server_identity(conn, (CONST_STRPTR)cert, (CONST_STRPTR)key,
                              TLS_KEY_EC) == TLS_OK);
    CHECK(h_cert_key_type == NX_SECURE_X509_KEY_TYPE_EC_DER);
    CHECK(h_version_override_calls == 0);
    tls_server_forget(conn);

    /* An unknown key type is refused rather than guessed at. */
    CHECK(tls_server_identity(conn, (CONST_STRPTR)cert, (CONST_STRPTR)key,
                              99UL) == TLS_ERR_BADCERT);

    /* No identity at all, and a half one. */
    CHECK(tls_server_identity(conn, NULL, (CONST_STRPTR)key, TLS_KEY_RSA) ==
          TLS_ERR_NOCERT);
    CHECK(tls_server_identity(conn, (CONST_STRPTR)cert, (CONST_STRPTR)"",
                              TLS_KEY_RSA) == TLS_ERR_NOCERT);

    /* A file that is not there. */
    CHECK(tls_server_identity(conn, (CONST_STRPTR)cert,
                              (CONST_STRPTR)"no-such-key.der",
                              TLS_KEY_RSA) == TLS_ERR_BADCERT);
    tls_server_forget(conn);

    /* PEM, which is what somebody will hand it first.  Refused on the leading
       byte rather than three layers down in an X.509 parse. */
    h_write_file(pem, (UBYTE)'-', 64);
    CHECK(tls_server_identity(conn, (CONST_STRPTR)pem, (CONST_STRPTR)key,
                              TLS_KEY_RSA) == TLS_ERR_BADCERT);
    tls_server_forget(conn);

    /* One byte past the ceiling is refused, never read short: half a DER
       structure parses as a different one. */
    h_write_file(big, 0x30, TLS_SERVER_DER_MAX + 1);
    CHECK(tls_server_identity(conn, (CONST_STRPTR)big, (CONST_STRPTR)key,
                              TLS_KEY_RSA) == TLS_ERR_BADCERT);
    tls_server_forget(conn);

    /* Exactly at the ceiling still loads, so the refusal above is the length
       and not the loop. */
    h_write_file(big, 0x30, TLS_SERVER_DER_MAX);
    CHECK(tls_server_identity(conn, (CONST_STRPTR)big, (CONST_STRPTR)key,
                              TLS_KEY_RSA) == TLS_OK);
    CHECK(h_cert_der_length == TLS_SERVER_DER_MAX);
    tls_server_forget(conn);

    free(conn);

    remove(cert);
    remove(key);
    remove(pem);
    remove(big);
}

static VOID h_test_server_needs_identity(struct TLSLibBase *base)
{
    struct TagItem tags[] = {
        { TLSA_Server, 1 },
        { TAG_DONE,    0 }
    };

    printf("tls_open: TLSA_Server without a certificate is refused\n");

    base->tb_CryptoReady = TRUE;
    h_transport_server = -1;

    /* Before any session exists, and before the transport: a server with no
       identity has nothing to be a server with, and finding that out at the
       handshake would mean a ClientHello answered with an alert. */
    CHECK(tls_TLSOpenA((APTR)base, tags, 5, base) == NULL);
    CHECK(h_transport_server == -1);
}

static VOID h_test_open_create_serialized(struct TLSLibBase *base)
{
    struct TagItem tags[] = {
        { TLSA_NoVerify, 1 },
        { TLSA_NoResume, 1 },
        { TAG_DONE, 0 }
    };
    TLSConnection *conn;
    int            enters = h_enter_calls;
    int            leaves = h_leave_calls;

    printf("tls_open: session creation stays inside the shared-state lock\n");

    /*
     * THE FIRST TLSOpen() IN A PROCESS HAS TO CREATE THE PROTECTION MUTEX.
     * tb_CryptoReady false is that first call.  Without
     * _nx_secure_tls_initialize() here, tls_conn_enter() takes a mutex the
     * shim never registered, gets TX_MUTEX_ERROR, and the connection is
     * refused with TLS_ERR_NOSTACK -- which is what shipped, about our own
     * bsdsocket.library, for every connection in the tree.
     */
    base->tb_CryptoReady = FALSE;
    h_initialize_calls              = 0;
    h_initialize_before_first_create = 0;

    conn = tls_TLSOpenA((APTR)base, tags, 3, base);
    CHECK(conn == NULL);
    CHECK(h_initialize_calls == 1);
    CHECK(h_initialize_before_first_create == 1);
    CHECK(base->tb_CryptoReady);

    enters = h_enter_calls;
    leaves = h_leave_calls;

    base->tb_CryptoReady = TRUE;
    conn = tls_TLSOpenA((APTR)base, tags, 3, base);

    CHECK(conn == NULL);
    CHECK(h_enter_calls == enters + 1);
    CHECK(h_leave_calls == leaves + 1);
    CHECK(h_depth == 0);

    /* And a connection with no TLSA_Server is a client to the transport, so
       nx_secure takes its client branch.  test_tls_transport.c checks the
       other end of that: which nx_tcp_socket_client_type each one produces. */
    CHECK(h_transport_server == 0);
}

int main(void)
{
    struct TLSLibBase *base;

    /* Unbuffered: a fault in a stubbed vector loses a buffered line, and the
       line is what says which check was running. */
    setvbuf(stdout, NULL, _IONBF, 0);

    TLSConnection     *conn;

    printf("tls_close: session deletion stays inside the shared-state lock\n");

    base = (struct TLSLibBase *)calloc(1, sizeof(*base));
    conn = (TLSConnection *)calloc(1, sizeof(*conn));
    if (base == NULL || conn == NULL)
    {
        free(base);
        free(conn);
        return 1;
    }

    conn->tc_Base = base;
    tls_TLSClose(conn, base);

    CHECK(strcmp(h_order, "EDL") == 0);
    CHECK(h_depth == 0);
    CHECK(h_enter_calls == 1);
    CHECK(h_leave_calls == 1);
    CHECK(h_end_calls == 1);
    CHECK(h_delete_calls == 1);

    h_test_concurrent_close(base);
    h_test_hostname_limit();
    h_test_path_limit();
    h_test_waitselect_unique_descriptors();
    h_test_waitselect_encrypted_queue();
    h_test_waitselect_wide_descriptor();
    h_test_open_create_serialized(base);
    h_test_server_needs_identity(base);
    h_test_server_identity(base);

    free(base);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
