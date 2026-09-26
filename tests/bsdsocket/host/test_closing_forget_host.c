/*
 * socket.c's closing list across a stack teardown (#53).
 *
 * bsd_closing_head outlives the netstack.  A socket still parked on it when
 * the netstack goes -- the last close found the kernel down and could not
 * drain -- keeps a TCP socket on that stack's NX_IP.  ami_ns_destroy() now
 * tears the NX socket down and frees the NX_IP and the pool, and the stack can
 * come up again; the next stack's sweep then reached the socket and called
 * NetX on the freed NX_IP (with NX_DISABLE_ERROR_CHECKING, a mutex get on
 * freed memory).  That last close now empties the list (library.c
 * bsd_child_close_gate()); test_expunge t_tableless_last_closer checks that
 * it does, this that an emptied list is never reached again.
 *
 * NOT TESTED HERE: the cork segment such a socket holds.  cork.c is not
 * compiled in; h_stack_down() stands in for bsd_cork_stop() with a plain
 * drop, so this says nothing about the stop.  That the stop leaves no
 * segment past the pool, bracketed or not, and across a refused stop, is
 * test_cork's (t_stop_during_pass, t_refused_pass_segment).  A parked socket
 * never holds as_RxPending: bsd_tcp_close_start() aborts instead of parking
 * one that does.
 *
 * socket.c is #included rather than linked, and -ffunction-sections plus the
 * linker's --gc-sections keep only the closing list and its callees.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "nx_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

static AmiSocket  h_sock;
static NX_IP      h_ip;
static NX_PACKET  h_cork;           /* a cork segment from the old pool  */
static BOOL       h_old_stack_gone; /* the pool and NX_IP are freed      */
static unsigned   h_releases;       /* nx_packet_release() calls          */
static unsigned   h_releases_freed; /* ... into the freed pool           */
static unsigned   h_nx_calls;       /* NetX socket calls                  */
static unsigned   h_nx_calls_freed; /* ... on the freed NX_IP            */
static unsigned   h_disposes;

/* ---- the stubs the closing list reaches --------------------------------- */

static VOID h_nx(NX_TCP_SOCKET *s)
{
    h_nx_calls++;
    if (h_old_stack_gone && s->nx_tcp_socket_ip_ptr == &h_ip)
        h_nx_calls_freed++;
}

UINT _nxe_packet_release(NX_PACKET **p)
{
    h_releases++;
    if (h_old_stack_gone && *p == &h_cork)
        h_releases_freed++;
    *p = NX_NULL;
    return NX_SUCCESS;
}

/* cork.c's: the segment goes back to the pool. */
VOID bsd_cork_drop(AmiSocket *s)
{
    NX_PACKET *pkt = s->as_CorkPkt;

    s->as_CorkPkt = NX_NULL;
    if (pkt != NX_NULL)
        (VOID)nx_packet_release(pkt);
}

UINT _nxe_tcp_socket_receive_notify(NX_TCP_SOCKET *s,
                                    VOID (*cb)(NX_TCP_SOCKET *))
{
    (VOID)cb;
    h_nx(s);
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_disconnect(NX_TCP_SOCKET *s, ULONG wait_option)
{
    (VOID)wait_option;
    h_nx(s);
    s->nx_tcp_socket_state = NX_TCP_CLOSED;
    return NX_SUCCESS;
}

UINT _nxe_tcp_server_socket_unaccept(NX_TCP_SOCKET *s) { h_nx(s); return NX_SUCCESS; }
UINT _nxe_tcp_client_socket_unbind(NX_TCP_SOCKET *s)   { h_nx(s); return NX_SUCCESS; }
UINT _nxe_tcp_socket_delete(NX_TCP_SOCKET *s)          { h_nx(s); return NX_SUCCESS; }

/* The group memberships go on the socket's NX_IP. */
VOID bsd_mcast_close(AmiSocket *s) { h_nx(&s->as_Nx.tcp); }

ULONG _tx_time_get(VOID) { return 0UL; }

VOID ami_log(int level, const char *fmt, ...) { (VOID)level; (VOID)fmt; }
VOID ami_mem_socket_delta(LONG d) { (VOID)d; }
VOID ami_free(APTR p) { (VOID)p; h_disposes++; }   /* h_sock is static */

/* Linked in by the close paths a parked socket never takes again. */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define H_TRAP(decl) decl { printf("  TRAP %s\n", __func__); abort(); }
H_TRAP(UINT _nxe_udp_socket_delete(NX_UDP_SOCKET *s))
H_TRAP(UINT _nxe_udp_socket_unbind(NX_UDP_SOCKET *s))
H_TRAP(VOID _nx_tcp_packet_send_fin(NX_TCP_SOCKET *s, ULONG seq))
H_TRAP(VOID _nx_tcp_packet_send_rst(NX_TCP_SOCKET *s, NX_TCP_HEADER *h))
H_TRAP(VOID bsd_raw_close(AmiSocket *s))
H_TRAP(BOOL bsd_cork_close_linger(AmiSocket *s, ULONG linger, ULONG *left))
H_TRAP(BOOL bsd_cork_close_graceful(AmiSocket *s))

#include "socket.c"

/* ---- the test ------------------------------------------------------------ */

/* A connected TCP socket, closed gracefully with a cork segment held. */
static VOID h_park(VOID)
{
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_ip, 0, sizeof(h_ip));
    h_sock.as_Flags = ASF_TCP | ASF_CONNECTED | ASF_NXBOUND;
    h_sock.as_Nx.tcp.nx_tcp_socket_ip_ptr = &h_ip;
    h_sock.as_Nx.tcp.nx_tcp_socket_state  = NX_TCP_FIN_WAIT_1;
    h_sock.as_CorkPkt = &h_cork;
    bsd_closing_park(&h_sock);

    h_old_stack_gone = FALSE;
    h_releases = h_releases_freed = h_nx_calls = h_nx_calls_freed = 0;
    h_disposes = 0;
}

/* The last close could not drain and, when `forget`, empties the list
   (library.c); the segment is dropped by hand, a stand-in for the stop and
   not a test of it; ami_ns_destroy() tears the NX socket down, and the NX_IP
   and the pool are freed. */
static VOID h_stack_down(BOOL forget)
{
    bsd_cork_drop(&h_sock);
    if (forget)
        bsd_closing_head = NULL;
    h_sock.as_Nx.tcp.nx_tcp_socket_state = NX_TCP_CLOSED;
    h_sock.as_Nx.tcp.nx_tcp_socket_id    = 0;
    h_old_stack_gone = TRUE;
}

/* The next stack: a close sweeps, the last opener drains. */
static VOID h_next_stack(VOID)
{
    bsd_closing_sweep();
    bsd_closing_drain();
    printf("closing_forget releases=%u into_freed=%u nx_calls_freed=%u "
           "disposes=%u\n", h_releases, h_releases_freed, h_nx_calls_freed,
           h_disposes);
}

static VOID t_forgotten_across_restart(VOID)
{
    printf("a parked socket across a stack teardown and restart\n");

    /* As before: the list kept.  The harness sees the sweep reach it. */
    h_park();
    h_stack_down(FALSE);
    h_next_stack();
    CHECK(h_nx_calls_freed != 0,
          "kept, the next stack's sweep calls NetX on the freed NX_IP");

    /* Emptied by that last close, as library.c does now. */
    h_park();
    h_stack_down(TRUE);
    h_next_stack();
    CHECK(h_releases == 1 && h_releases_freed == 0,
          "the segment went back before the pool, and nothing after");
    CHECK(h_nx_calls_freed == 0, "no NetX call on the freed NX_IP");
    CHECK(h_disposes == 0, "and the socket is left, not freed");
}

/* The same socket swept while its stack is up: the segment goes back and the
   socket is freed. */
static VOID t_swept_while_up(VOID)
{
    printf("a parked socket swept while its stack is up\n");

    h_park();
    h_sock.as_Nx.tcp.nx_tcp_socket_state = NX_TCP_CLOSED;
    bsd_closing_sweep();

    CHECK(h_releases == 1, "the cork segment released");
    CHECK(h_disposes == 1, "and the socket freed");
    CHECK(bsd_closing_head == NULL, "the list is empty");
}

int main(void)
{
    t_swept_while_up();
    t_forgotten_across_restart();
    printf("closing_forget checks=%lu failures=%lu\n", h_checks, h_failures);
    return h_failures ? 1 : 0;
}
