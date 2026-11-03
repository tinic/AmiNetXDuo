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
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
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

/* timer.device's E-clock, here in microseconds of real time. */
static ULONG h_now_micros(VOID)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONG)((unsigned long long)ts.tv_sec * 1000000ULL +
                   (unsigned long long)ts.tv_nsec / 1000ULL);
}

static BOOL h_timer_open = TRUE;
BOOL  ami_tls_timer_is_open(VOID)          { return h_timer_open; }
ULONG ami_tls_eclock(VOID)                 { return h_now_micros(); }
ULONG ami_tls_eclock_micros(ULONG ticks)   { return ticks; }

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

/* The fail-closed self-test forces this so it can check that a fixture the
   harness cannot build is a failure, not a silent skip. */
static int rig_fail;

static int rig_open(Rig *rig)
{
    int room = 262144;

    if (rig_fail || socketpair(AF_UNIX, SOCK_STREAM, 0, rig->fds) != 0)
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

/* A test whose fixture cannot be built has verified nothing, so it must fail
   rather than skip: a skip records no check and lets the binary exit 0 with
   nothing actually run. */
static int rig_ready(Rig *rig)
{
    if (rig_open(rig))
        return 1;

    /* The self-test forces `rig_fail` to walk this path; a forced refusal is
       silent so the probe does not read as a real failure in the log. */
    if (!rig_fail)
        printf("  FAIL: rig_open could not set up a socketpair\n");
    checks++;
    failures++;
    return 0;
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

    if (!rig_ready(&rig))
        return;

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

    if (!rig_ready(&rig))
        return;

    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 ONE_TICK) == NX_NO_PACKET);

    /* Not a packet allocated and then thrown away: a timeout on a connection
       whose pool is nearly empty must not be the thing that empties it. */
    CHECK(rig.pool.nx_packet_pool_available == rig.packets);

    rig_close(&rig);
}

/* Milliseconds as NetX ticks. */
static ULONG h_ticks(ULONG ms)
{
    ULONG t = (ms * (ULONG)NX_IP_PERIODIC_RATE) / 1000UL;

    return (t == 0) ? 1 : t;
}

/*
 * F-280: one call's waits share one budget.  Three receives on a quiet peer,
 * each asking for 60 ms, under a 100 ms budget: the first waits 60, the
 * second the 40 left, and the third returns at once.  Before, every wait got
 * its full wait_option and the three took 180 ms.
 */
static void test_receive_budget_quiet(void)
{
    Rig        rig;
    NX_PACKET *packet = NX_NULL;
    ULONG      start;
    ULONG      third;
    ULONG      total;

    printf("tls_transport: a call's waits share one TLSA_Timeout budget\n");

    if (!rig_ready(&rig))
        return;

    tls_transport_budget(&rig.transport, h_ticks(100));
    start = h_now_micros();
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 h_ticks(60)) == NX_NO_PACKET);
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 h_ticks(60)) == NX_NO_PACKET);
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 h_ticks(60)) == NX_NO_PACKET);
    total = h_now_micros() - start;
    /* Wall-clock upper bounds also count unrelated CI runner scheduling.
       The transport's remaining budget is the deterministic invariant. */
    CHECK(rig.transport.tt_BudgetMicros == 0);
    CHECK(total >= 90000UL);                     /* but the budget was used */

    /* Off again: a wait gets its own wait_option. */
    tls_transport_budget_end(&rig.transport);
    start = h_now_micros();
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 h_ticks(30)) == NX_NO_PACKET);
    CHECK(h_now_micros() - start >= 20000UL);

    /* No timer.device: the documented fallback, each wait on its own.  Three
       60 ms waits under a 100 ms budget take their full 180 ms, and the
       third is not cut short. */
    h_timer_open = FALSE;
    tls_transport_budget(&rig.transport, h_ticks(100));
    start = h_now_micros();
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 h_ticks(60)) == NX_NO_PACKET);
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 h_ticks(60)) == NX_NO_PACKET);
    third = h_now_micros();
    CHECK(_nx_tcp_socket_receive(tls_transport_socket(&rig.transport), &packet,
                                 h_ticks(60)) == NX_NO_PACKET);
    CHECK(h_now_micros() - third >= 45000UL);
    CHECK(h_now_micros() - start >= 150000UL);
    tls_transport_budget_end(&rig.transport);
    h_timer_open = TRUE;

    /* Past 2^32 us the budget cannot be held or timed: per-wait, as before. */
    tls_transport_budget(&rig.transport, 0xFFFFFFF0UL);
    CHECK(rig.transport.tt_BudgetOn == NX_FALSE);
    tls_transport_budget(&rig.transport, h_ticks(4294967UL));
    CHECK(rig.transport.tt_BudgetOn == NX_TRUE);
    tls_transport_budget_end(&rig.transport);

    /* NX_WAIT_FOREVER is no budget, as TLSA_Timeout 0 is unlimited. */
    tls_transport_budget(&rig.transport, NX_WAIT_FOREVER);
    CHECK(rig.transport.tt_BudgetOn == NX_FALSE);
    tls_transport_budget_end(&rig.transport);

    rig_close(&rig);
}

/*
 * A slow peer: one byte every 40 ms.  Every single wait succeeds, which is
 * why a per-wait timeout never fired; under a 100 ms budget the call-level
 * ceiling does, well before the peer's ten.
 *
 * Each wait is allowed a second, so a timeout before the peer is done can
 * only be the budget's.  The count is bounded, not pinned: the budget is
 * charged the time inside waits, and on a loaded host the time between them
 * lets more bytes arrive.  The first byte is there before the budget starts,
 * so the fork and a late first write cannot take it all.
 */
static void test_receive_budget_slow_peer(void)
{
    Rig        rig;
    NX_PACKET *packet;
    pid_t      peer;
    int        got = 0;
    int        i;
    UINT       status = NX_SUCCESS;

    printf("tls_transport: a slow peer runs out one call's budget\n");

    if (!rig_ready(&rig))
        return;

    peer = fork();
    if (peer == 0)
    {
        for (i = 0; i < 10; i++)
        {
            if (i > 0)
                usleep(40000);
            if (write(rig.fds[1], "x", 1) != 1)
                break;
        }
        _exit(0);
    }
    CHECK(peer > 0);

    {
        struct pollfd first = { rig.fds[0], POLLIN, 0 };

        CHECK(poll(&first, 1, 5000) == 1);
    }

    tls_transport_budget(&rig.transport, h_ticks(100));
    for (i = 0; i < 10; i++)
    {
        packet = NX_NULL;
        status = _nx_tcp_socket_receive(tls_transport_socket(&rig.transport),
                                        &packet, h_ticks(1000));
        if (status != NX_SUCCESS)
            break;
        got++;
        (VOID)_nx_packet_release(packet);
    }
    tls_transport_budget_end(&rig.transport);

    CHECK(status == NX_NO_PACKET);
    CHECK(got >= 1 && got < 10);
    CHECK(rig.transport.tt_Broken == NX_FALSE);   /* a timeout, not a hangup */

    if (peer > 0)
        (void)waitpid(peer, NULL, 0);
    rig_close(&rig);
}

static void test_receive_hangup(void)
{
    Rig        rig;
    NX_PACKET *packet = NX_NULL;

    printf("tls_transport: a FIN is NX_NOT_CONNECTED and it is remembered\n");

    if (!rig_ready(&rig))
        return;

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

    if (!rig_ready(&rig))
        return;

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

    if (!rig_ready(&rig))
        return;

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

/*
 * The peer family of an application's socket. One created with AF_INET6 ==
 * 23 (bebbo ndk-include) answers getpeername() with 23 at bytes 0 and 1.
 */
static void test_peer_family(void)
{
    static const UBYTE v4[4]      = { 16, TLS_SOCK_AF_INET, 0, 1 };
    static const UBYTE v4_old[4]  = { TLS_SOCK_AF_INET, 0, 0, 1 };
    static const UBYTE v6[4]      = { 28, TLS_SOCK_AF_INET6, 0, 1 };
    static const UBYTE v6_23[4]   = { TLS_SOCK_AF_INET6_23,
                                      TLS_SOCK_AF_INET6_23, 0, 1 };

    printf("tls_transport: a peer address's family in every spelling\n");

    CHECK(tls_sock_family(v4) == TLS_SOCK_AF_INET);
    CHECK(tls_sock_family(v4_old) == TLS_SOCK_AF_INET);
    CHECK(tls_sock_family(v6) == TLS_SOCK_AF_INET6);
    CHECK(tls_sock_family(v6_23) == TLS_SOCK_AF_INET6);
}

static void test_socket_type(void)
{
    Rig rig;

    printf("tls_transport: client and server present the client_type "
           "nx_secure branches on\n");

    if (!rig_ready(&rig))
        return;

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

/* The bug this guards: a rig_open failure used to skip the test and return,
   which recorded no check and no failure, so a whole suite of skips exited 0.
   Force the fixture open to refuse and assert the harness records it as a
   failure, so the ctest `tls_transport` cannot pass with nothing run. */
static void test_rig_open_fail_closed(void)
{
    Rig rig;
    int checks_delta;
    int failures_delta;
    int before_checks;
    int before_failures;

    printf("tls_transport: a rig the fixture cannot build is a failure, not a skip\n");

    before_checks   = checks;
    before_failures = failures;

    rig_fail = 1;
    (void)rig_ready(&rig);
    rig_fail = 0;

    checks_delta   = checks   - before_checks;
    failures_delta = failures - before_failures;

    /* Undo the probe's own record before asserting, so this self-test does not
       fail a suite whose fixtures all opened; the deltas below are what CHECK
       looks at, and a regression that records nothing makes them 0 and fails. */
    checks   = before_checks;
    failures = before_failures;

    CHECK(checks_delta   == 1);
    CHECK(failures_delta == 1);
}

int main(void)
{
    test_receive();
    test_receive_timeout();
    test_receive_budget_quiet();
    test_receive_budget_slow_peer();
    test_receive_hangup();
    test_send_chain();
    test_send_failure_keeps_packet();
    test_socket_type();
    test_peer_family();
    test_forbid_balance();
    test_rig_open_fail_closed();

    printf("%d checks, %d failure(s)\n", checks, failures);

    return (failures == 0) ? 0 : 1;
}
