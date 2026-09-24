/*
 * AmiNetXDuo, the MSS read without the IP mutex: _nx_tcp_socket_mss_compute()
 * gives what nx_tcp_socket_mss_get() gives, in every state, and only the
 * locked one takes the mutex.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_tcp.h"

#include <stdio.h>
#include <string.h>

/* Built twice: as the library ships (timestamps off) and with
   NX_ENABLE_TCP_TIMESTAMP, the branch the two readings could disagree on. */
#ifdef NX_ENABLE_TCP_TIMESTAMP
#define T_TS    1
#else
#define T_TS    0
#endif

static unsigned long t_checks;
static unsigned long t_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        t_checks++;                                                           \
        if (!(cond))                                                          \
        {                                                                     \
            t_failures++;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static unsigned long t_gets;
static unsigned long t_puts;
static TX_MUTEX     *t_held;

/* Both spellings: tx_api.h maps tx_mutex_get() to one or the other on
   TX_DISABLE_ERROR_CHECKING. */
UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)wait_option;
    t_gets++;
    t_held = mutex_ptr;
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    t_puts++;
    if (t_held == mutex_ptr)
        t_held = TX_NULL;
    return TX_SUCCESS;
}

UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    return _tx_mutex_get(mutex_ptr, wait_option);
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{
    return _tx_mutex_put(mutex_ptr);
}

static NX_IP         t_ip;
static NX_TCP_SOCKET t_sock;

static VOID t_setup(UINT state, ULONG mss_set, ULONG connect_mss, UINT ts)
{
    memset(&t_sock, 0, sizeof(t_sock));
    t_sock.nx_tcp_socket_ip_ptr            = &t_ip;
    t_sock.nx_tcp_socket_id                = NX_TCP_ID;
    t_sock.nx_tcp_socket_state             = state;
    t_sock.nx_tcp_socket_mss               = mss_set;
    t_sock.nx_tcp_socket_connect_mss       = connect_mss;
#ifdef NX_ENABLE_TCP_TIMESTAMP
    t_sock.nx_tcp_socket_timestamp_enabled = (UCHAR)(ts ? NX_TRUE : NX_FALSE);
#else
    (VOID)ts;
#endif
}

/* One reading each way against the expected answer, and the mutex traffic of
   each: one get and one put for the locked call, none for the peek. */
static VOID t_both(const char *what, ULONG want)
{
    ULONG         locked = 0xDEADBEEFUL;
    ULONG         peek;
    unsigned long g0 = t_gets, p0 = t_puts;

    CHECK(_nx_tcp_socket_mss_get(&t_sock, &locked) == NX_SUCCESS,
          "%s: the locked call succeeds", what);
    CHECK(t_gets == g0 + 1 && t_puts == p0 + 1 && t_held == TX_NULL,
          "%s: the locked call takes and gives back the mutex once", what);

    g0 = t_gets;
    p0 = t_puts;
    peek = _nx_tcp_socket_mss_compute(&t_sock);
    CHECK(t_gets == g0 && t_puts == p0, "%s: the peek takes no mutex", what);

    CHECK(peek == locked, "%s: peek %lu == locked %lu", what,
          (unsigned long)peek, (unsigned long)locked);
    CHECK(locked == want, "%s: %lu, expected %lu", what,
          (unsigned long)locked, (unsigned long)want);
}

static VOID t_named(void)
{
    printf("tcp mss: the named cases\n");

    t_setup(NX_TCP_CLOSED, 0, 0, 0);
    t_both("CLOSED, no MSS set", NX_TCP_MSS_SIZE);

    t_setup(NX_TCP_CLOSED, 1000, 0, 0);
    t_both("CLOSED, MSS set", 1000);

    t_setup(NX_TCP_SYN_SENT, 0, 1460, 1);
    t_both("SYN_SENT, no MSS set", NX_TCP_MSS_SIZE);

    t_setup(NX_TCP_SYN_SENT, 900, 1460, 1);
    t_both("SYN_SENT, MSS set", 900);

    t_setup(NX_TCP_ESTABLISHED, 0, 1460, 0);
    t_both("ESTABLISHED, no timestamps", 1460);

    t_setup(NX_TCP_ESTABLISHED, 0, 1460, 1);
    t_both("ESTABLISHED, timestamps", T_TS ? 1448 : 1460);

    t_setup(NX_TCP_ESTABLISHED, 900, 1460, 1);
    t_both("ESTABLISHED, timestamps, MSS set (ignored)", T_TS ? 1448 : 1460);

    t_setup(NX_TCP_ESTABLISHED, 0, 12, 1);
    t_both("ESTABLISHED, timestamps, connect_mss 12", 12);

    t_setup(NX_TCP_ESTABLISHED, 0, 13, 1);
    t_both("ESTABLISHED, timestamps, connect_mss 13", T_TS ? 1 : 13);

    t_setup(NX_TCP_ESTABLISHED, 0, 0, 1);
    t_both("ESTABLISHED, timestamps, connect_mss 0", 0);

    t_setup(NX_TCP_CLOSE_WAIT, 0, 1460, 1);
    t_both("CLOSE_WAIT, timestamps", T_TS ? 1448 : 1460);

    t_setup(NX_TCP_CLOSE_WAIT, 0, 1460, 0);
    t_both("CLOSE_WAIT, no timestamps", 1460);

    /* What a RST leaves: the state back to CLOSED, the negotiated fields as
       they were.  Both readings go back to the pre-connect answer. */
    t_setup(NX_TCP_ESTABLISHED, 0, 1460, 1);
    t_sock.nx_tcp_socket_state = NX_TCP_CLOSED;
    t_both("after RST, CLOSED, no MSS set", NX_TCP_MSS_SIZE);

    t_setup(NX_TCP_ESTABLISHED, 700, 1460, 1);
    t_sock.nx_tcp_socket_state = NX_TCP_CLOSED;
    t_both("after RST, CLOSED, MSS set", 700);
}

/* Every state, both timestamp settings, the edges of the subtraction. */
static VOID t_sweep(void)
{
    static const ULONG connect[] = { 0, 1, 11, 12, 13, 536, 1448, 1460, 65535 };
    static const ULONG set[]     = { 0, 1, 536, 1000 };
    UINT     state;
    unsigned c, m, ts;
    char     what[96];

    printf("tcp mss: every state x timestamps x connect_mss x MSS set\n");

    for (state = 0; state <= NX_TCP_LAST_ACK; state++)
        for (ts = 0; ts < 2; ts++)
            for (c = 0; c < sizeof(connect) / sizeof(connect[0]); c++)
                for (m = 0; m < sizeof(set) / sizeof(set[0]); m++)
                {
                    ULONG want;

                    if (state < NX_TCP_ESTABLISHED)
                        want = set[m] ? set[m] : NX_TCP_MSS_SIZE;
                    else if (T_TS && ts && connect[c] > 12)
                        want = connect[c] - 12;
                    else
                        want = connect[c];

                    snprintf(what, sizeof(what),
                             "state %u ts %u connect_mss %lu mss %lu",
                             state, ts, (unsigned long)connect[c],
                             (unsigned long)set[m]);
                    t_setup(state, set[m], connect[c], ts);
                    t_both(what, want);
                }
}

int main(void)
{
    printf("tcp mss, timestamps %s\n\n", T_TS ? "compiled in" : "off");

    t_named();
    t_sweep();

    printf("\n%lu checks, %lu failures\n", t_checks, t_failures);

    return (t_failures == 0) ? 0 : 1;
}
