/*
 * GHSA-8w5x-ff58-2fr2 (upstream 5128818d): once nx_tcp_socket_send() accepts
 * a record's packet chain, the chain is the TCP layer's, which may already have
 * released it.  _nx_secure_tls_send_record() must not touch it again; the
 * NX_SECURE_KEY_CLEAR wipe belongs to the one case where the caller still
 * owns the chain, a failed send.
 *
 * nx_secure_tls_send_record.c is compiled into this program against stubs.
 * The TCP send stub takes the chain on success the way the TCP layer does --
 * here by stamping every payload byte with a TCP-owned pattern and taking a
 * snapshot of each packet's header and buffer -- and the test then checks
 * that nothing in the chain changed after the call.  Nothing is released, so
 * the program never touches freed memory, even against a tree without the fix:
 * there the wipe stays inside the stamped buffers and shows as a failure.  A
 * failed send must still wipe every packet's payload and return the TCP
 * status unchanged, and an inactive session wipes nothing.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nx_secure_tls.h"

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (!ok)
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* ------------------------------------------------------------- stubs -- */

TX_MUTEX _nx_secure_tls_protection;

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (void)mutex_ptr; (void)wait_option;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    (void)mutex_ptr;
    return TX_SUCCESS;
}

TX_THREAD *_tx_thread_identify(VOID)
{
    return TX_NULL;
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    return TX_SUCCESS;
}

#define H_IV_SIZE   16u

UINT _nx_secure_tls_session_iv_size_get(NX_SECURE_TLS_SESSION *tls_session, USHORT *iv_size)
{
    (void)tls_session;
    *iv_size = H_IV_SIZE;
    return NX_SUCCESS;
}

/* TLS 1.3 appends the inner content type; in place, at the chain's tail. */
UINT _nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start, ULONG data_size,
                            NX_PACKET_POOL *pool_ptr, ULONG wait_option)
{
    NX_PACKET *last = packet_ptr;

    (void)pool_ptr; (void)wait_option;
    while (last -> nx_packet_next != NX_NULL)
        last = last -> nx_packet_next;
    if ((ULONG)(last -> nx_packet_data_end - last -> nx_packet_append_ptr) < data_size)
        return NX_SIZE_ERROR;
    memcpy(last -> nx_packet_append_ptr, data_start, data_size);
    last -> nx_packet_append_ptr += data_size;
    packet_ptr -> nx_packet_length += data_size;
    return NX_SUCCESS;
}

UINT _nx_secure_tls_record_payload_encrypt(NX_SECURE_TLS_SESSION *tls_session, NX_PACKET *send_packet,
                                           ULONG sequence_num[NX_SECURE_TLS_SEQUENCE_NUMBER_SIZE],
                                           UCHAR record_type)
{
    (void)tls_session; (void)send_packet; (void)sequence_num; (void)record_type;
    return NX_SUCCESS;
}

/* The record MAC: not reached, the test ciphersuite's hash has no operation. */
UINT _nx_secure_tls_record_hash_initialize(NX_SECURE_TLS_SESSION *tls_session,
                                           ULONG sequence_num[NX_SECURE_TLS_SEQUENCE_NUMBER_SIZE],
                                           UCHAR *header, UINT header_length, UINT *length,
                                           UCHAR *record_hash_key)
{
    (void)tls_session; (void)sequence_num; (void)header; (void)header_length; (void)length;
    (void)record_hash_key;
    return NX_NOT_SUCCESSFUL;
}

UINT _nx_secure_tls_record_hash_update(NX_SECURE_TLS_SESSION *tls_session, UCHAR *data, UINT length)
{
    (void)tls_session; (void)data; (void)length;
    return NX_NOT_SUCCESSFUL;
}

UINT _nx_secure_tls_record_hash_calculate(NX_SECURE_TLS_SESSION *tls_session, UCHAR *record_hash,
                                          UINT *hash_length)
{
    (void)tls_session; (void)record_hash; (void)hash_length;
    return NX_NOT_SUCCESSFUL;
}

/* ------------------------------------------------------------ packets -- */

#define H_PACKETS   3
#define H_BUFFER    256
#define H_HEADROOM  64
#define H_PAYLOAD   100
#define H_PLAIN     0x3C        /* the record before the send */
#define H_TCP_OWNED 0xC5        /* what the TCP layer did with it after */

static NX_PACKET packets[H_PACKETS];
static UCHAR     buffers[H_PACKETS][H_BUFFER];

static NX_PACKET snap_packets[H_PACKETS];
static UCHAR     snap_buffers[H_PACKETS][H_BUFFER];

static void chain_build(void)
{
    int i;

    memset(packets, 0, sizeof(packets));
    memset(buffers, 0xEE, sizeof(buffers));
    for (i = 0; i < H_PACKETS; i++)
    {
        packets[i].nx_packet_data_start   = buffers[i];
        packets[i].nx_packet_data_end     = buffers[i] + H_BUFFER;
        packets[i].nx_packet_prepend_ptr  = buffers[i] + H_HEADROOM;
        packets[i].nx_packet_append_ptr   = buffers[i] + H_HEADROOM + H_PAYLOAD;
        packets[i].nx_packet_next         = (i + 1 < H_PACKETS) ? &packets[i + 1] : NX_NULL;
        memset(packets[i].nx_packet_prepend_ptr, H_PLAIN, H_PAYLOAD);
    }
    packets[0].nx_packet_length = H_PACKETS * H_PAYLOAD;
}

/* ---------------------------------------------------------- TCP stub -- */

static UINT       h_tcp_status;
static int        h_tcp_calls;
static NX_PACKET *h_tcp_packet;

UINT _nx_tcp_socket_send(NX_TCP_SOCKET *socket_ptr, NX_PACKET *packet_ptr, ULONG wait_option)
{
    NX_PACKET *p;

    (void)socket_ptr; (void)wait_option;
    h_tcp_calls++;
    h_tcp_packet = packet_ptr;

    if (h_tcp_status != NX_SUCCESS)
        return h_tcp_status;            /* the caller keeps the chain */

    /* Accepted: from here the chain is TCP's to send, release and reuse. */
    for (p = packet_ptr; p != NX_NULL; p = p -> nx_packet_next)
        memset(p -> nx_packet_prepend_ptr, H_TCP_OWNED,
               (size_t)(p -> nx_packet_append_ptr - p -> nx_packet_prepend_ptr));
    memcpy(snap_packets, packets, sizeof(packets));
    memcpy(snap_buffers, buffers, sizeof(buffers));
    return NX_SUCCESS;
}

/* -------------------------------------------------------------- cases -- */

static NX_SECURE_TLS_SESSION          session;
static NX_CRYPTO_METHOD               no_mac;          /* nx_crypto_operation NULL */
static NX_SECURE_TLS_CIPHERSUITE_INFO suite;

static void session_init(int active, int tls_1_3)
{
    memset(&session, 0, sizeof(session));
    memset(&suite, 0, sizeof(suite));
    memset(&no_mac, 0, sizeof(no_mac));
    suite.nx_secure_tls_hash                   = &no_mac;
    session.nx_secure_tls_session_ciphersuite  = &suite;
    session.nx_secure_tls_protocol_version     = NX_SECURE_TLS_VERSION_TLS_1_2;
    session.nx_secure_tls_local_session_active = (UINT)active;
    session.nx_secure_tls_1_3                  = (UCHAR)tls_1_3;
}

static int payload_is(const NX_PACKET *p, UCHAR v)
{
    const UCHAR *b;

    for (b = p -> nx_packet_prepend_ptr; b < p -> nx_packet_append_ptr; b++)
        if (*b != v)
            return 0;
    return 1;
}

static void run(const char *name, int active, int tls_1_3, UINT tcp_status)
{
    UINT status;
    int  i;
    int  ok;
    char what[160];

    session_init(active, tls_1_3);
    chain_build();
    h_tcp_status = tcp_status;
    h_tcp_calls  = 0;
    h_tcp_packet = NX_NULL;

    status = _nx_secure_tls_send_record(&session, &packets[0], NX_SECURE_TLS_APPLICATION_DATA, NX_NO_WAIT);

    snprintf(what, sizeof(what), "%s: status 0x%x, the TCP send's 0x%x", name, status, tcp_status);
    expect(status == tcp_status, what);
    snprintf(what, sizeof(what), "%s: one TCP send, of the record's chain", name);
    expect(h_tcp_calls == 1 && h_tcp_packet == &packets[0], what);

    if (tcp_status == NX_SUCCESS)
    {
        snprintf(what, sizeof(what), "%s: the chain TCP took is untouched after the send", name);
        expect(memcmp(snap_packets, packets, sizeof(packets)) == 0 &&
               memcmp(snap_buffers, buffers, sizeof(buffers)) == 0, what);
        return;
    }

    /* A wipe covers the whole record, header and IV included; with no
       session keys only the payload the caller wrote is compared. */
    ok = 1;
    for (i = 0; i < H_PACKETS; i++)
    {
        if (active)
        {
            ok &= payload_is(&packets[i], 0x00);
        }
        else
        {
            const UCHAR *b;

            for (b = buffers[i] + H_HEADROOM; b < buffers[i] + H_HEADROOM + H_PAYLOAD; b++)
                ok &= (*b == H_PLAIN);
        }
    }
    snprintf(what, sizeof(what), "%s: every packet %s", name,
             active ? "wiped (the caller still owns the chain)" : "left alone (no session keys)");
    expect(ok, what);
}

int main(void)
{
    run("TLS 1.3 accepted",                 1, 1, NX_SUCCESS);
    run("TLS 1.2 accepted",                 1, 0, NX_SUCCESS);
    run("inactive session accepted",        0, 0, NX_SUCCESS);
    run("TLS 1.3 not connected",            1, 1, NX_NOT_CONNECTED);
    run("TLS 1.2 window overflow",          1, 0, NX_WINDOW_OVERFLOW);
    run("TLS 1.2 transmit queue full",      1, 0, NX_TX_QUEUE_DEPTH);
    run("inactive session not connected",   0, 0, NX_NOT_CONNECTED);

    printf("send_record_ownership: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
