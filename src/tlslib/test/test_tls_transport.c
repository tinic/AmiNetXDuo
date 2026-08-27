/*
 * tls.library over an ordinary socket, src/tlslib/tls_netx.c.
 *
 * nx_secure reaches the network through exactly two calls, nx_tcp_socket_send
 * and nx_tcp_socket_receive.  They used to be forwarded into our own
 * bsdsocket.library's NetX Duo through a private LVO, which is why tls.library
 * answered TLS_ERR_NOSTACK on Roadshow, AmiTCP and Miami.  They are send() and
 * recv() now, and this test drives the real functions over a real socketpair:
 * tls_sock.c's host half is the same call sequence against POSIX.
 *
 * What is being defended:
 *   * a chained record goes out in order and in one piece;
 *   * a successful send releases the packet and a failed one does not, which
 *     is NetX Duo's ownership rule and a leak or a double free either way;
 *   * a timeout is NX_NO_PACKET and a hangup is NX_NOT_CONNECTED, because
 *     tls_conn.c turns the first into TLS_ERR_TIMEOUT and the second into end
 *     of stream, and swapping them silently truncates a download;
 *   * a broken transport stays broken, rather than resuming a TLS stream whose
 *     sequence numbers no longer line up.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int checks;
static int failures;

#define CHECK(expr)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(expr))                                                       \
        {                                                                  \
            failures++;                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #expr);               \
        }                                                                  \
    } while (0)

/* ------------------------------------------------- what tls_netx.c wants --- */

VOID tls_bzero(APTR ptr, ULONG size)          { memset(ptr, 0, (size_t)size); }
VOID tls_memcpy(APTR d, const void *s, ULONG n) { memcpy(d, s, (size_t)n); }
VOID tls_delay_ticks(ULONG ticks)             { (VOID)ticks; }

static ULONG arrivals;

VOID ami_random_arrival(VOID)
{
    arrivals++;
}

UINT _nx_secure_tls_local_certificate_add(NX_SECURE_TLS_SESSION *session,
                                          NX_SECURE_X509_CERT *certificate)
{
    (VOID)session;
    (VOID)certificate;
    return NX_SUCCESS;
}

/* Exec.  Forbid()/Permit() are counted rather than ignored: a path out of
   tls_netx.c that leaves the machine in Forbid() is a hang, not a slowdown. */
static int forbid_depth;
static int forbid_max_depth;

VOID Forbid(VOID)
{
    forbid_depth++;
    if (forbid_depth > forbid_max_depth)
        forbid_max_depth = forbid_depth;
}

VOID Permit(VOID)                                { forbid_depth--; }
VOID InitSemaphore(struct SignalSemaphore *s)    { memset(s, 0, sizeof(*s)); }
VOID ObtainSemaphore(struct SignalSemaphore *s)  { (VOID)s; }
VOID ReleaseSemaphore(struct SignalSemaphore *s) { (VOID)s; }
LONG AttemptSemaphore(struct SignalSemaphore *s) { (VOID)s; return 1; }

/* ------------------------------------------------------------- fixture --- */

typedef struct Rig
{
    int             fds[2];         /* [0] is ours, [1] is the peer  */
    TLSTransport    transport;
    NX_PACKET_POOL  pool;
    ULONG           packets;
    void           *memory;
} Rig;

static int rig_open(Rig *rig)
{
    int room = 262144;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, rig->fds) != 0)
        return 0;

    /* The chained-send test writes more than one default socketpair buffer
       and only reads afterwards, so the send must not have to wait for a
       reader that is not running yet. */
    (void)setsockopt(rig->fds[0], SOL_SOCKET, SO_SNDBUF, &room, sizeof(room));
    (void)setsockopt(rig->fds[1], SOL_SOCKET, SO_RCVBUF, &room, sizeof(room));

    rig->packets = tls_packet_pool_count(TLS_DEFAULT_RECORD_BUFFER);
    rig->memory  = malloc((size_t)tls_packet_pool_bytes(rig->packets));
    if (rig->memory == NULL)
        return 0;

    if (tls_packet_pool_create(&rig->pool, rig->memory, rig->packets) !=
        NX_SUCCESS)
        return 0;

    /* `base` is unused by the host half of tls_sock.c; the descriptor is the
       whole of what the transport carries. */
    tls_transport_open(&rig->transport, (APTR)rig, rig->fds[0], &rig->pool,
                       FALSE, FALSE);

    return 1;
}

static void rig_close(Rig *rig)
{
    if (rig->fds[0] >= 0)
        close(rig->fds[0]);
    if (rig->fds[1] >= 0)
        close(rig->fds[1]);

    tls_packet_pool_delete(&rig->pool);
    free(rig->memory);
}

/* One NetX Duo tick, so a "wait" in these tests is milliseconds and not a
   pause anybody notices. */
#define ONE_TICK    1UL

/* ---------------------------------------------------------------- tests --- */

static void test_receive(void)
{
    Rig        rig;
    NX_PACKET *packet = NX_NULL;
    UCHAR      out[64];
    ULONG      copied = 0;
    ULONG      before;

    printf("tls_transport: recv fills a packet and credits arrival timing\n");

    if (!rig_open(&rig))
    {
        printf("  SKIP: no socketpair\n");
        return;
    }

    before = arrivals;

    CHECK(write(rig.fds[1], "hello", 5) == 5);
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 NX_WAIT_FOREVER) == NX_SUCCESS);
    CHECK(packet != NX_NULL);
    CHECK(packet->nx_packet_length == 5);
    CHECK(_nx_packet_data_extract_offset(packet, 0, out, sizeof(out),
                                         &copied) == NX_SUCCESS);
    CHECK(copied == 5);
    CHECK(memcmp(out, "hello", 5) == 0);

    /* The pool's arrival source is the only entropy this library can collect
       on a stack that is not ours; a record off the wire has to feed it. */
    CHECK(arrivals == before + 1);

    CHECK(_nx_packet_release(packet) == NX_SUCCESS);
    CHECK(rig.pool.nx_packet_pool_available == rig.packets);

    rig_close(&rig);
}

static void test_receive_timeout(void)
{
    Rig        rig;
    NX_PACKET *packet = (NX_PACKET *)0x1;

    printf("tls_transport: a quiet peer is NX_NO_PACKET, and costs no block\n");

    if (!rig_open(&rig))
    {
        printf("  SKIP: no socketpair\n");
        return;
    }

    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 ONE_TICK) == NX_NO_PACKET);

    /* Not a packet allocated and then thrown away: a timeout on a connection
       whose pool is nearly empty must not be the thing that empties it. */
    CHECK(rig.pool.nx_packet_pool_available == rig.packets);

    rig_close(&rig);
}

static void test_receive_hangup(void)
{
    Rig        rig;
    NX_PACKET *packet = NX_NULL;

    printf("tls_transport: a FIN is NX_NOT_CONNECTED and it is remembered\n");

    if (!rig_open(&rig))
    {
        printf("  SKIP: no socketpair\n");
        return;
    }

    close(rig.fds[1]);
    rig.fds[1] = -1;

    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 NX_WAIT_FOREVER) == NX_NOT_CONNECTED);
    CHECK(rig.pool.nx_packet_pool_available == rig.packets);

    /* And every call after it, without going near the descriptor again. */
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 NX_WAIT_FOREVER) == NX_NOT_CONNECTED);
    CHECK(_nx_tcp_socket_send(tls_transport_socket(&rig.transport),
                              (NX_PACKET *)&rig, NX_WAIT_FOREVER) ==
          NX_NOT_CONNECTED);

    rig_close(&rig);
}

static void test_send_chain(void)
{
    Rig        rig;
    NX_PACKET *packet = NX_NULL;
    UCHAR     *source;
    UCHAR     *echo;
    ULONG      total;
    ULONG      i;
    ssize_t    got;
    size_t     read_total = 0;

    printf("tls_transport: a chained record goes out whole and in order\n");

    if (!rig_open(&rig))
    {
        printf("  SKIP: no socketpair\n");
        return;
    }

    /* Two blocks and a bit: the send loop has to walk nx_packet_next and
       respect each link's own prepend/append window, not the head's. */
    total  = (TLS_PACKET_PAYLOAD * 2UL) + 37UL;
    source = (UCHAR *)malloc((size_t)total);
    echo   = (UCHAR *)malloc((size_t)total);
    CHECK(source != NULL && echo != NULL);
    for (i = 0; i < total; i++)
        source[i] = (UCHAR)((i * 31UL) & 0xFFUL);

    CHECK(_nx_packet_allocate(&rig.pool, &packet, NX_IPv4_TCP_PACKET, 0) ==
          NX_SUCCESS);
    CHECK(_nx_packet_data_append(packet, source, total, &rig.pool, 0) ==
          NX_SUCCESS);
    CHECK(packet->nx_packet_length == total);

    CHECK(_nx_tcp_socket_send(tls_transport_socket(&rig.transport), packet,
                              NX_WAIT_FOREVER) == NX_SUCCESS);

    /* NetX Duo's rule: a send that succeeded owns the packet.  A leak here is
       a connection that dies after a few kilobytes of POST body. */
    CHECK(rig.pool.nx_packet_pool_available == rig.packets);

    while (read_total < (size_t)total)
    {
        got = read(rig.fds[1], &echo[read_total], (size_t)total - read_total);
        if (got <= 0)
            break;
        read_total += (size_t)got;
    }

    CHECK(read_total == (size_t)total);
    CHECK(memcmp(echo, source, (size_t)total) == 0);

    free(source);
    free(echo);
    rig_close(&rig);
}

static void test_send_failure_keeps_packet(void)
{
    Rig        rig;
    NX_PACKET *packet = NX_NULL;
    ULONG      available;

    printf("tls_transport: a failed send leaves the packet to its caller\n");

    if (!rig_open(&rig))
    {
        printf("  SKIP: no socketpair\n");
        return;
    }

    CHECK(_nx_packet_allocate(&rig.pool, &packet, NX_IPv4_TCP_PACKET, 0) ==
          NX_SUCCESS);
    CHECK(_nx_packet_data_append(packet, "xyz", 3, &rig.pool, 0) == NX_SUCCESS);
    available = rig.pool.nx_packet_pool_available;

    /* Both ends gone: the wait cannot succeed and nothing is written. */
    close(rig.fds[0]);
    close(rig.fds[1]);
    rig.fds[0] = -1;
    rig.fds[1] = -1;

    CHECK(_nx_tcp_socket_send(tls_transport_socket(&rig.transport), packet,
                              ONE_TICK) != NX_SUCCESS);

    /* nx_secure releases it itself on a failure; a release here as well is a
       double free on a machine with no MMU. */
    CHECK(rig.pool.nx_packet_pool_available == available);
    CHECK(_nx_packet_release(packet) == NX_SUCCESS);

    rig_close(&rig);
}

static void test_socket_type(void)
{
    Rig rig;

    printf("tls_transport: client and server present the client_type "
           "nx_secure branches on\n");

    if (!rig_open(&rig))
    {
        printf("  SKIP: no socketpair\n");
        return;
    }

    /*
     * _nx_secure_tls_session_start() reads nx_tcp_socket_client_type and
     * NOTHING else to decide whether this session runs the client handshake
     * or the server one, so this field is the whole of what TLSA_Server does.
     * Getting it backwards is a server that sends a ClientHello.
     */
    tls_transport_open(&rig.transport, (APTR)&rig, rig.fds[0], &rig.pool,
                       FALSE, FALSE);
    CHECK(rig.transport.tt_Socket.nx_tcp_socket_client_type == NX_TRUE);
    CHECK(rig.transport.tt_Socket.nx_tcp_socket_connect_ip.nxd_ip_version ==
          NX_IP_VERSION_V4);

    tls_transport_open(&rig.transport, (APTR)&rig, rig.fds[0], &rig.pool,
                       TRUE, TRUE);
    CHECK(rig.transport.tt_Socket.nx_tcp_socket_client_type == NX_FALSE);
    CHECK(rig.transport.tt_Socket.nx_tcp_socket_connect_ip.nxd_ip_version ==
          NX_IP_VERSION_V6);

    /* And no NX_IP behind it, which every read of it in nx_secure is guarded
       on: there is no NetX Duo instance in this library to point at. */
    CHECK(rig.transport.tt_Socket.nx_tcp_socket_ip_ptr == NX_NULL);

    /* The address nx_secure is handed is the transport, so the two entry
       points can cast it back. */
    CHECK((VOID *)tls_transport_socket(&rig.transport) == (VOID *)&rig.transport);

    rig_close(&rig);
}

static void test_forbid_balance(void)
{
    TX_MUTEX mutex;

    printf("tls_transport: the mutex shim leaves no Forbid() outstanding\n");

    memset(&mutex, 0, sizeof(mutex));

    CHECK(_tx_mutex_create(&mutex, (CHAR *)"test", TX_NO_INHERIT) == TX_SUCCESS);
    CHECK(forbid_depth == 0);

    /* Recursive, the way nx_secure nests it around the session list. */
    CHECK(_tx_mutex_get(&mutex, TX_WAIT_FOREVER) == TX_SUCCESS);
    CHECK(_tx_mutex_get(&mutex, TX_WAIT_FOREVER) == TX_SUCCESS);
    CHECK(_tx_mutex_put(&mutex) == TX_SUCCESS);
    CHECK(_tx_mutex_put(&mutex) == TX_SUCCESS);

    /* An unknown mutex is refused, not silently treated as taken. */
    {
        TX_MUTEX stranger;

        memset(&stranger, 0, sizeof(stranger));
        CHECK(_tx_mutex_get(&stranger, TX_WAIT_FOREVER) == TX_MUTEX_ERROR);
        CHECK(_tx_mutex_put(&stranger) == TX_MUTEX_ERROR);
    }

    CHECK(_tx_mutex_delete(&mutex) == TX_SUCCESS);
    CHECK(_tx_mutex_get(&mutex, TX_WAIT_FOREVER) == TX_MUTEX_ERROR);

    CHECK(forbid_depth == 0);
    CHECK(forbid_max_depth == 1);

    /* No task here is a ThreadX thread, and nx_secure_tls_send_record.c
       compares the answer against an NX_IP thread that does not exist. */
    CHECK(_tx_thread_identify() == TX_NULL);
}

int main(void)
{
    test_receive();
    test_receive_timeout();
    test_receive_hangup();
    test_send_chain();
    test_send_failure_keeps_packet();
    test_socket_type();
    test_forbid_balance();

    printf("%d checks, %d failure(s)\n", checks, failures);

    return (failures == 0) ? 0 : 1;
}
