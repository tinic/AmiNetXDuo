/* Real socket.c TX policy: scarce memory, competing writers and opt-out.
 * No network or packet allocation is simulated. SPDX-License-Identifier: MIT */
#include "bsdsocket_vectors.h"
#include "aminetxduo/sana2.h"
#include <stdio.h>
#include <string.h>

static ULONG h_now = 1000, h_bps = 1000000000UL;
static unsigned h_gets, h_mutex_puts, h_failures;
static UINT h_mutex_status = TX_SUCCESS;
ULONG ami_millis(VOID) { return h_now; }
ULONG ami_sana2_get_bps(const AmiSana2If *p) { (void)p; return h_bps; }
ULONG ami_sana2_get_tcp_grow_rtt(const AmiSana2If *p) { (void)p; return 5; }
UINT _txe_mutex_get(TX_MUTEX *p, ULONG wait)
{ (void)p; h_gets++; if (wait != TX_NO_WAIT) h_failures++; return h_mutex_status; }
UINT _txe_mutex_put(TX_MUTEX *p) { (void)p; h_mutex_puts++; return TX_SUCCESS; }
UINT _tx_mutex_get(TX_MUTEX *p, ULONG wait) { return _txe_mutex_get(p, wait); }
UINT _tx_mutex_put(TX_MUTEX *p) { return _txe_mutex_put(p); }

#include "socket.c"

static NX_IP h_ip;
static NX_PACKET_POOL h_pool;
static AmiSocket h_sockets[2];
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); h_failures++; } } while (0)
static void h_reset(void)
{
    unsigned i;
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_pool, 0, sizeof(h_pool));
    memset(h_sockets, 0, sizeof(h_sockets));
    h_ip.nx_ip_default_packet_pool = &h_pool;
    h_pool.nx_packet_pool_total = h_pool.nx_packet_pool_available = 4096;
    h_ip.nx_ip_tcp_created_sockets_count = 1;
    h_ip.nx_ip_tcp_created_sockets_ptr = &h_sockets[0].as_Nx.tcp;
    h_ip.nx_ip_interface[0].nx_interface_additional_link_info = &h_ip;
    for (i = 0; i < 2; i++)
    {
        NX_TCP_SOCKET *t = &h_sockets[i].as_Nx.tcp;
        h_sockets[i].as_SettleRtt = 30;
        t->nx_tcp_socket_name = bsd_tcp_name;
        t->nx_tcp_socket_reserved_ptr = &h_sockets[i];
        t->nx_tcp_socket_ip_ptr = &h_ip;
        t->nx_tcp_socket_state = NX_TCP_ESTABLISHED;
        t->nx_tcp_socket_connect_interface = &h_ip.nx_ip_interface[0];
        t->nx_tcp_socket_connect_mss = 1448;
        t->nx_tcp_socket_rx_window_current = 144800;
        t->nx_tcp_socket_transmit_queue_maximum = 1024;
        t->nx_tcp_socket_created_next = &h_sockets[0].as_Nx.tcp;
    }
    h_gets = h_mutex_puts = 0;
    h_mutex_status = TX_SUCCESS;
    h_now = 1000;
    h_bps = 1000000000UL;
}
int main(void)
{
    NX_TCP_SOCKET *t = &h_sockets[0].as_Nx.tcp;
    h_reset();
    bsd_tcp_tx_autotune(t, 100);
    CHECK(h_gets == 0 && t->nx_tcp_socket_transmit_queue_maximum == 1024);
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(h_sockets[0].as_AutoTxDepth == 3484 && h_gets == 1 && h_mutex_puts == 1);
    CHECK(t->nx_tcp_socket_transmit_queue_maximum_default == 3484);
    /* A second pressured writer reclaims unused credit, no packet wait. */
    h_ip.nx_ip_tcp_created_sockets_count = 2;
    t->nx_tcp_socket_created_next = &h_sockets[1].as_Nx.tcp;
    t->nx_tcp_socket_tx_outstanding_bytes = 144800;
    h_pool.nx_packet_pool_available -= 100;
    bsd_tcp_tx_autotune(&h_sockets[1].as_Nx.tcp, 4000000);
    CHECK(h_sockets[0].as_AutoTxDepth == 1692);
    CHECK(h_sockets[1].as_AutoTxDepth == 1692);
    CHECK(h_gets == 2 && h_mutex_puts == 2);

    h_reset(); h_pool.nx_packet_pool_total = 94;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(h_gets == 0 && h_sockets[0].as_AutoTxDepth == 0);
    h_reset(); h_sockets[0].as_SndBuf = 32768;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(h_gets == 0 && t->nx_tcp_socket_transmit_queue_maximum == 1024);
    h_reset(); h_sockets[0].as_SettleRtt = 1;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(h_gets == 0);
    h_reset(); h_pool.nx_packet_pool_available = 512;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(h_sockets[0].as_AutoTxDepth == 0 && t->nx_tcp_socket_transmit_queue_maximum == 1024);
    h_reset(); h_mutex_status = TX_NOT_AVAILABLE;
    bsd_tcp_tx_autotune(t, 4000000);
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(h_gets == 1 && h_mutex_puts == 0 && h_sockets[0].as_AutoTxDepth == 0);
    h_now += 30; h_mutex_status = TX_SUCCESS;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(h_gets == 2 && h_mutex_puts == 1 && h_sockets[0].as_AutoTxDepth == 3484);
    h_reset(); t->nx_tcp_socket_name = "native";
    t->nx_tcp_socket_reserved_ptr = (void *)1;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(h_gets == 0);
    /* Receive autotuning: double within a round trip, out of the budget
       the other receivers leave, never past the scale or the ceiling. */
    h_reset(); h_pool.nx_packet_pool_payload_size = 1568;
    t->nx_tcp_socket_rx_window_maximum = 3211264;
    t->nx_tcp_socket_rx_window_default = 802816;
    t->nx_tcp_socket_rx_window_current = 700000;
    t->nx_tcp_rcv_win_scale_value = 6;
    t->nx_tcp_socket_rx_sequence = 5000000;
    h_sockets[0].as_RxAutoMillis = 1000; h_now = 1020;
    bsd_tcp_rx_autotune(t);
    CHECK(t->nx_tcp_socket_rx_window_default == 1605632);
    CHECK(t->nx_tcp_socket_rx_window_current == 1502816);
    CHECK(h_sockets[0].as_SettleWant == 1605632);
    CHECK(h_sockets[0].as_RxAutoMark == 1204224 && h_sockets[0].as_RxAutoSeq == 5000000);
    CHECK(h_sockets[0].as_RxAutoMillis == 1020);
    h_now = 1051;
    bsd_tcp_rx_autotune(t);
    CHECK(t->nx_tcp_socket_rx_window_default == 1605632 && h_sockets[0].as_RxAutoMillis == 1051);
    h_ip.nx_ip_tcp_created_sockets_count = 2;
    t->nx_tcp_socket_created_next = &h_sockets[1].as_Nx.tcp;
    h_sockets[1].as_Nx.tcp.nx_tcp_socket_rx_window_default = 2000000;
    h_now = 1060;
    bsd_tcp_rx_autotune(t);
    CHECK(t->nx_tcp_socket_rx_window_default == 1605632);
    h_sockets[1].as_Nx.tcp.nx_tcp_socket_state = NX_TCP_CLOSE_WAIT;
    h_now = 1070;
    bsd_tcp_rx_autotune(t);
    CHECK(t->nx_tcp_socket_rx_window_default == 3211264 && h_sockets[0].as_RxAutoMark == 0);
    h_reset(); h_pool.nx_packet_pool_payload_size = 1568;
    t->nx_tcp_socket_rx_window_maximum = 3211264;
    t->nx_tcp_socket_rx_window_default = 65535;
    t->nx_tcp_rcv_win_scale_value = 0;
    h_sockets[0].as_RxAutoMillis = 1000;
    bsd_tcp_rx_autotune(t);
    CHECK(t->nx_tcp_socket_rx_window_default == 65535);
    printf("TCP autotune: %u h_failures\n", h_failures);
    return h_failures != 0;
}
