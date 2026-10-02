/*
 * Audit N-032: the verdict _nx_tcp_transmit_cleanup gives a send whose wait
 * expired, per TCP state.
 *
 * The send path accepts ESTABLISHED and CLOSE_WAIT
 * (nx_tcp_socket_send_internal.c), so an expiry in either is a timeout --
 * NX_WINDOW_OVERFLOW, or NX_TX_QUEUE_DEPTH with the queue full -- which
 * src/bsdsocket/select.c's sliced wait retries.  Every other state is
 * NX_NOT_CONNECTED, which our send path turns into EPIPE and a cleared
 * ASF_CONNECTED (src/bsdsocket/transfer.c).  In every case the thread is
 * unlinked and resumed, and the packet it was sending is not released: it is
 * still the caller's.
 *
 * Linked for real: _nx_tcp_transmit_cleanup.  Stubbed: ThreadX's resume and
 * the packet release.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "nx_api.h"
#include "nx_tcp.h"

#include <stdio.h>
#include <string.h>

TX_THREAD           *_tx_thread_current_ptr;
volatile ULONG       _tx_thread_system_state;
volatile UINT        _tx_thread_preempt_disable;
TX_THREAD            _tx_timer_thread;

static int resumed;
static int released;

VOID _tx_thread_system_resume(TX_THREAD *thread_ptr)
{
    (void) thread_ptr;
    _tx_thread_preempt_disable--;
    resumed++;
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void) packet_ptr;
    released++;
    return NX_SUCCESS;
}

UINT _tx_thread_interrupt_disable(VOID)              { return 0; }
VOID _tx_thread_interrupt_restore(UINT p)            { (void) p; }
UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG f, UINT o)  { (void) g; (void) f; (void) o; return TX_SUCCESS; }
UINT _txe_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG f, UINT o) { (void) g; (void) f; (void) o; return TX_SUCCESS; }
VOID _nx_tcp_cleanup_deferred(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    (void) thread_ptr;
    NX_CLEANUP_EXTENSION
}

static const struct
{
    UINT        state;
    const char *name;
    UINT        want;           /* queue not full */
} cases[] = {
    { NX_TCP_ESTABLISHED, "ESTABLISHED", NX_WINDOW_OVERFLOW },
    { NX_TCP_CLOSE_WAIT,  "CLOSE_WAIT",  NX_WINDOW_OVERFLOW },
    { NX_TCP_CLOSED,      "CLOSED",      NX_NOT_CONNECTED   },
    { NX_TCP_LISTEN_STATE,"LISTEN",      NX_NOT_CONNECTED   },
    { NX_TCP_SYN_SENT,    "SYN_SENT",    NX_NOT_CONNECTED   },
    { NX_TCP_FIN_WAIT_1,  "FIN_WAIT_1",  NX_NOT_CONNECTED   },
    { NX_TCP_FIN_WAIT_2,  "FIN_WAIT_2",  NX_NOT_CONNECTED   },
    { NX_TCP_CLOSING,     "CLOSING",     NX_NOT_CONNECTED   },
    { NX_TCP_LAST_ACK,    "LAST_ACK",    NX_NOT_CONNECTED   },
    { NX_TCP_TIMED_WAIT,  "TIMED_WAIT",  NX_NOT_CONNECTED   },
};

static NX_TCP_SOCKET sock;
static TX_THREAD     sender, other;
static NX_PACKET     pkt;

/* One expiry: `sender` suspended on the socket's transmit list.  */
static UINT expire(UINT state, int queue_full)
{
    memset(&sock, 0, sizeof(sock));
    memset(&sender, 0, sizeof(sender));
    memset(&other, 0, sizeof(other));
    _tx_thread_current_ptr = &other;
    _tx_thread_system_state = 0;
    resumed = 0;
    released = 0;

    sock.nx_tcp_socket_id    = NX_TCP_ID;
    sock.nx_tcp_socket_state = state;
    sock.nx_tcp_socket_transmit_queue_maximum = 8;
    sock.nx_tcp_socket_transmit_sent_count    = queue_full ? 8 : 3;
    sock.nx_tcp_socket_transmit_suspension_list = &sender;
    sock.nx_tcp_socket_transmit_suspended_count = 1;

    sender.tx_thread_state                    = TX_TCP_IP;
    sender.tx_thread_suspend_cleanup          = _nx_tcp_transmit_cleanup;
    sender.tx_thread_suspend_control_block    = &sock;
    sender.tx_thread_suspended_next           = &sender;
    sender.tx_thread_suspended_previous       = &sender;
    sender.tx_thread_additional_suspend_info  = &pkt;

    _nx_tcp_transmit_cleanup(&sender NX_CLEANUP_ARGUMENT);
    return sender.tx_thread_suspend_status;
}

static const char *status_name(UINT s)
{
    switch (s)
    {
    case NX_WINDOW_OVERFLOW: return "NX_WINDOW_OVERFLOW";
    case NX_TX_QUEUE_DEPTH:  return "NX_TX_QUEUE_DEPTH";
    case NX_NOT_CONNECTED:   return "NX_NOT_CONNECTED";
    default:                 return "other";
    }
}

int main(void)
{
    UINT i;
    int  bad = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int full;

        for (full = 0; full < 2; full++)
        {
            UINT want = cases[i].want;
            UINT got;
            int  ok;

            if (full && want == NX_WINDOW_OVERFLOW)
                want = NX_TX_QUEUE_DEPTH;
            got = expire(cases[i].state, full);
            ok  = got == want && resumed == 1 && released == 0
                  && sock.nx_tcp_socket_transmit_suspension_list == NX_NULL
                  && sock.nx_tcp_socket_transmit_suspended_count == 0
                  && sender.tx_thread_suspend_cleanup == TX_NULL
                  && sender.tx_thread_additional_suspend_info == &pkt;
            printf("%s %-11s queue %s: %s (want %s), resumed %d, packet released %d\n",
                   ok ? "ok  " : "FAIL", cases[i].name, full ? "full    " : "not full",
                   status_name(got), status_name(want), resumed, released);
            bad |= !ok;
        }
    }
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
