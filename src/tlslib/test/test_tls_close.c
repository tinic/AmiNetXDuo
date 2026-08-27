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
 * The transport.  Its own behaviour is test_tls_transport.c's subject; here
 * it only has to hand back something session_start() can be given.
 */
VOID tls_transport_open(TLSTransport *transport, APTR socket_base, LONG fd,
                        NX_PACKET_POOL *pool, BOOL server, BOOL ipv6)
{
    (VOID)socket_base; (VOID)fd; (VOID)pool; (VOID)server; (VOID)ipv6;
    memset(transport, 0, sizeof(*transport));
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

UINT _nx_secure_tls_session_create(NX_SECURE_TLS_SESSION *session,
                                   const NX_SECURE_TLS_CRYPTO *crypto,
                                   VOID *metadata, ULONG metadata_size)
{
    (VOID)session;
    (VOID)crypto;
    (VOID)metadata;
    (VOID)metadata_size;
    CHECK(h_depth == 1);
    return NX_INVALID_PARAMETERS;
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

    base->tb_CryptoReady = TRUE;
    conn = tls_TLSOpenA((APTR)base, tags, 3, base);

    CHECK(conn == NULL);
    CHECK(h_enter_calls == enters + 1);
    CHECK(h_leave_calls == leaves + 1);
    CHECK(h_depth == 0);
}

int main(void)
{
    struct TLSLibBase *base;
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

    free(base);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
