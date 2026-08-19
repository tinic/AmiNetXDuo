/*
 * tls.library, TLSClose() keeps every NetX Secure operation inside the
 * ThreadX bracket.
 *
 * TLS sessions share process-wide created-object state and protection
 * mutexes.  session_end() was bracketed but session_delete() ran after leave,
 * so two Exec Tasks closing different connections could mutate that shared
 * state concurrently as plain, unadopted Tasks.  This host test records the
 * call order at the vector boundary.  EDL is safe; the old ELD order fails.
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

static LONG h_enter(AmiNetCaller *caller)
{
    (VOID)caller;

    (VOID)pthread_mutex_lock(&h_baton);
    CHECK(h_depth == 0);
    h_depth++;
    h_enter_calls++;
    return AMI_NET_OK;
}

static VOID h_leave(AmiNetCaller *caller)
{
    (VOID)caller;

    CHECK(h_depth == 1);
    h_mark('L');
    h_depth--;
    h_leave_calls++;
    (VOID)pthread_mutex_unlock(&h_baton);
}

static const AmiNetXDuoContext h_context =
{
    .nxc_Enter = h_enter,
    .nxc_Leave = h_leave
};

const AmiNetXDuoContext *tls_netx_ctx(VOID)
{
    return &h_context;
}

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

int main(void)
{
    struct TLSLibBase *base;
    TLSConnection     *conn;

    printf("tls_close: session deletion stays inside ThreadX serialization\n");

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

    free(base);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
