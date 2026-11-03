/*
 * The rest of test_af_inet6_23's link closure: functions reached only by
 * TCP, raw sockets, waits and name lookups, none of which the cases run.
 * Here, apart from the headers that declare them, so one shape serves all;
 * a call is a defect in the test and stops it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>

#define H_NEVER(name) void name(void); void name(void) { abort(); }

H_NEVER(name)
H_NEVER(_nxde_tcp_client_socket_connect)
H_NEVER(_nxde_tcp_client_socket_source_connect)
H_NEVER(_nxe_tcp_client_socket_bind)
H_NEVER(_nxe_tcp_client_socket_port_get)
H_NEVER(_nxe_tcp_client_socket_unbind)
H_NEVER(_nxe_tcp_free_port_find)
H_NEVER(_nxe_tcp_server_socket_unaccept)
H_NEVER(_nxe_tcp_socket_create)
H_NEVER(_nxe_tcp_socket_delete)
H_NEVER(_nxe_tcp_socket_disconnect)
H_NEVER(_nxe_tcp_socket_receive_notify)
H_NEVER(_nx_tcp_packet_send_fin)
H_NEVER(_nx_tcp_packet_send_rst)
H_NEVER(_nx_tcp_syncache_hash)
H_NEVER(_txe_mutex_get)
H_NEVER(_txe_mutex_put)
H_NEVER(_tx_thread_identify)
H_NEVER(_tx_thread_sleep)
H_NEVER(ami_bsd_tcp_budget)
H_NEVER(ami_bsd_tcp_rx_auto_ceiling)
H_NEVER(ami_bsd_tcp_tx_queue_for)
H_NEVER(ami_bsd_tcp_window_for)
H_NEVER(ami_bsd_tcp_window_max_for)
H_NEVER(ami_random_bytes)
H_NEVER(ami_random_ulong)
H_NEVER(bsd_raw_open)
H_NEVER(bsd_raw_close)
H_NEVER(bsd_wait_event)
H_NEVER(bsd_tcp_disconnect_callback)
H_NEVER(bsd_tcp_urgent_notify)
H_NEVER(netstack_resolve_reverse_until)
H_NEVER(ami_netdb_serv_by_port)
H_NEVER(bsd_if_name_by_index)
H_NEVER(_nxe_packet_release)
