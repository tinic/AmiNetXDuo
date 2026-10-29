/* Real socket.c TX policy: scarce memory, competing writers and opt-out.
 * No network or packet allocation is simulated. SPDX-License-Identifier: MIT */
#include "bsdsocket_vectors.h"
#include "aminetxduo/sana2.h"
#include <stdio.h>
#include <string.h>

static ULONG now = 1000, bps = 1000000000UL;
static unsigned gets, mutex_puts, failures;
static UINT mutex_status = TX_SUCCESS;
ULONG ami_millis(VOID) { return now; }
ULONG ami_sana2_get_bps(const AmiSana2If *p) { (void)p; return bps; }
ULONG ami_sana2_get_tcp_grow_rtt(const AmiSana2If *p) { (void)p; return 5; }
UINT _txe_mutex_get(TX_MUTEX *p, ULONG wait)
{ (void)p; gets++; if (wait != TX_NO_WAIT) failures++; return mutex_status; }
UINT _txe_mutex_put(TX_MUTEX *p) { (void)p; mutex_puts++; return TX_SUCCESS; }
UINT _tx_mutex_get(TX_MUTEX *p, ULONG wait) { return _txe_mutex_get(p, wait); }
UINT _tx_mutex_put(TX_MUTEX *p) { return _txe_mutex_put(p); }

#include "socket.c"

static NX_IP ip;
static NX_PACKET_POOL pool;
static AmiSocket sockets[2];
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static void reset(void)
{
    unsigned i;
    memset(&ip, 0, sizeof(ip));
    memset(&pool, 0, sizeof(pool));
    memset(sockets, 0, sizeof(sockets));
    ip.nx_ip_default_packet_pool = &pool;
    pool.nx_packet_pool_total = pool.nx_packet_pool_available = 4096;
    ip.nx_ip_tcp_created_sockets_count = 1;
    ip.nx_ip_tcp_created_sockets_ptr = &sockets[0].as_Nx.tcp;
    ip.nx_ip_interface[0].nx_interface_additional_link_info = &ip;
    for (i = 0; i < 2; i++)
    {
        NX_TCP_SOCKET *t = &sockets[i].as_Nx.tcp;
        sockets[i].as_SettleRtt = 30;
        t->nx_tcp_socket_name = bsd_tcp_name;
        t->nx_tcp_socket_reserved_ptr = &sockets[i];
        t->nx_tcp_socket_ip_ptr = &ip;
        t->nx_tcp_socket_state = NX_TCP_ESTABLISHED;
        t->nx_tcp_socket_connect_interface = &ip.nx_ip_interface[0];
        t->nx_tcp_socket_connect_mss = 1448;
        t->nx_tcp_socket_rx_window_current = 144800;
        t->nx_tcp_socket_transmit_queue_maximum = 1024;
        t->nx_tcp_socket_created_next = &sockets[0].as_Nx.tcp;
    }
    gets = mutex_puts = 0;
    mutex_status = TX_SUCCESS;
    now = 1000;
    bps = 1000000000UL;
}
int main(void)
{
    NX_TCP_SOCKET *t = &sockets[0].as_Nx.tcp;
    reset();
    bsd_tcp_tx_autotune(t, 100);
    CHECK(gets == 0 && t->nx_tcp_socket_transmit_queue_maximum == 1024);
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(sockets[0].as_AutoTxDepth == 3484 && gets == 1 && mutex_puts == 1);
    CHECK(t->nx_tcp_socket_transmit_queue_maximum_default == 3484);
    /* A second pressured writer reclaims unused credit, no packet wait. */
    ip.nx_ip_tcp_created_sockets_count = 2;
    t->nx_tcp_socket_created_next = &sockets[1].as_Nx.tcp;
    t->nx_tcp_socket_tx_outstanding_bytes = 144800;
    pool.nx_packet_pool_available -= 100;
    bsd_tcp_tx_autotune(&sockets[1].as_Nx.tcp, 4000000);
    CHECK(sockets[0].as_AutoTxDepth == 1692);
    CHECK(sockets[1].as_AutoTxDepth == 1692);
    CHECK(gets == 2 && mutex_puts == 2);

    reset(); pool.nx_packet_pool_total = 94;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(gets == 0 && sockets[0].as_AutoTxDepth == 0);
    reset(); sockets[0].as_SndBuf = 32768;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(gets == 0 && t->nx_tcp_socket_transmit_queue_maximum == 1024);
    reset(); sockets[0].as_SettleRtt = 1;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(gets == 0);
    reset(); pool.nx_packet_pool_available = 512;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(sockets[0].as_AutoTxDepth == 0 && t->nx_tcp_socket_transmit_queue_maximum == 1024);
    reset(); mutex_status = TX_NOT_AVAILABLE;
    bsd_tcp_tx_autotune(t, 4000000);
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(gets == 1 && mutex_puts == 0 && sockets[0].as_AutoTxDepth == 0);
    now += 30; mutex_status = TX_SUCCESS;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(gets == 2 && mutex_puts == 1 && sockets[0].as_AutoTxDepth == 3484);
    reset(); t->nx_tcp_socket_name = "native";
    t->nx_tcp_socket_reserved_ptr = (void *)1;
    bsd_tcp_tx_autotune(t, 4000000);
    CHECK(gets == 0);
    printf("TCP autotune: %u failures\n", failures);
    return failures != 0;
}
