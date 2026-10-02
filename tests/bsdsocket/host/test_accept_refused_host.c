/*
 * src/bsdsocket/socket.c:bsd_accept() on the host, a connection the
 * listener's bind refuses (#52).
 *
 * NetX's listen table is keyed by port alone, so a listener bound to a
 * specific address can be handed a connection that arrived on another one:
 * the address's interface was removed while the listener stayed on the port.
 * bsd_bind_accepts() refuses it, the peer is reset and the slot goes back on
 * the port.  A blocking accept() then keeps waiting; only a non-blocking one
 * or an expired SO_RCVTIMEO answers EWOULDBLOCK.
 *
 * bsd_wait_sliced() is stubbed with a script: each step either completes a
 * connection on the parked socket (from the bound address or from another)
 * and runs the real bsd_accept_once(), or raises the break, or times out.
 * The NetX calls bsd_accept reaches are stubs that record what was done.
 *
 * N-088: the unaccept that hands a refused slot back can yield (it releases
 * what the peer had queued), and the IP thread's listen callback then runs
 * bsd_listen_refill().  The unaccept stub can run that refill's effect at
 * the yield; the relisten stub answers NX_NOT_CLOSED, as NetX does, for a
 * socket that is not CLOSED.
 *
 * socket.c is #included rather than linked, and -ffunction-sections plus the
 * linker's --gc-sections keep only bsd_accept and its callees.
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

#define H_FDS    2
#define PORT     8080
#define ADDR_A   0x0A000005UL   /* the listener's bound address */
#define ADDR_B   0x0A010005UL   /* another local address, same port */

enum { STEP_REFUSED, STEP_MATCH, STEP_BREAK, STEP_TIMEOUT };

static struct AmiSocketBase h_base;
static AmiSocket            h_listener;
static AmiSocket            h_incoming;
static AmiSocket           *h_table[H_FDS];
static NX_IP                h_ip;

static const int *h_script;
static unsigned   h_script_len;
static unsigned   h_waits;
static ULONG      h_wait_arg[8];
static ULONG      h_now;
static unsigned   h_resets;
static unsigned   h_relistens;
static int        h_refill_at_yield;   /* N-088: refill runs inside unaccept */
static unsigned   h_refilled;
static unsigned   h_refill_skipped;
static unsigned   h_not_closed;

static void h_reset(ULONG flags, ULONG rcvtimeo, const int *script,
                    unsigned len)
{
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_listener, 0, sizeof(h_listener));
    memset(&h_incoming, 0, sizeof(h_incoming));
    memset(&h_table, 0, sizeof(h_table));
    memset(&h_ip, 0, sizeof(h_ip));

    h_base.sb_Table     = h_table;
    h_base.sb_TableSize = H_FDS;
    h_base.sb_StackRefs = 1;
    h_base.sb_StackIp   = &h_ip;
    h_base.sb_BreakMask = 0x1000UL;

    h_ip.nx_ip_interface[0].nx_interface_valid      = 1;
    h_ip.nx_ip_interface[0].nx_interface_ip_address = ADDR_A;
    h_ip.nx_ip_interface[1].nx_interface_valid      = 1;
    h_ip.nx_ip_interface[1].nx_interface_ip_address = ADDR_B;

    h_listener.as_Owner        = &h_base;
    h_listener.as_Flags        = ASF_TCP | ASF_BOUND | ASF_LISTENING | flags;
    h_listener.as_Type         = SOCK_STREAM;
    h_listener.as_ListenPort   = PORT;
    h_listener.as_Backlog      = 1;
    h_listener.as_IncomingCount = 1;
    h_listener.as_Incoming     = &h_incoming;
    h_listener.as_RcvTimeout   = rcvtimeo;
    h_listener.as_LocalAddr.nxd_ip_version        = NX_IP_VERSION_V4;
    h_listener.as_LocalAddr.nxd_ip_address.v4     = ADDR_A;
    h_table[0] = &h_listener;

    h_incoming.as_Flags  = ASF_TCP | ASF_INCOMING | ASF_SERVER;
    h_incoming.as_Parent = &h_listener;
    h_incoming.as_Nx.tcp.nx_tcp_socket_state = NX_TCP_LISTEN_STATE;
    h_incoming.as_Nx.tcp.nx_tcp_socket_reserved_ptr = &h_incoming;

    h_script     = script;
    h_script_len = len;
    h_waits      = 0;
    h_now        = 1000;
    h_resets     = 0;
    h_relistens  = 0;
    h_refill_at_yield = 0;
    h_refilled        = 0;
    h_refill_skipped  = 0;
    h_not_closed      = 0;
}

/* ---- the scripted wait --------------------------------------------------- */

UINT bsd_wait_sliced(struct AmiSocketBase *base, ULONG wait,
                     BsdSlicedCall call, VOID *arg, BOOL *aborted)
{
    NX_TCP_SOCKET *tcp = &h_incoming.as_Nx.tcp;
    int            step;

    (VOID)base;
    *aborted = FALSE;

    if (h_waits < sizeof(h_wait_arg) / sizeof(h_wait_arg[0]))
        h_wait_arg[h_waits] = wait;

    /* Past the end of the script is a busy loop: stop it. */
    if (h_waits >= h_script_len)
    {
        h_waits++;
        *aborted = TRUE;
        return NX_SUCCESS;
    }

    step = h_script[h_waits++];
    h_now += 50;

    switch (step)
    {
    case STEP_REFUSED:
    case STEP_MATCH:
        tcp->nx_tcp_socket_state = NX_TCP_ESTABLISHED;
        tcp->nx_tcp_socket_connect_interface =
            &h_ip.nx_ip_interface[(step == STEP_MATCH) ? 0 : 1];
        tcp->nx_tcp_socket_connect_ip.nxd_ip_version    = NX_IP_VERSION_V4;
        tcp->nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0x0A010063UL;
        tcp->nx_tcp_socket_connect_port = 40000;
        return call(arg, NX_NO_WAIT);
    case STEP_BREAK:
        *aborted = TRUE;
        return NX_SUCCESS;
    default:
        return NX_NO_PACKET;
    }
}

ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks, LONG flags)
{
    if ((sock->as_Flags & ASF_NONBLOCK) != 0 || (flags & MSG_DONTWAIT) != 0)
        return NX_NO_WAIT;
    return (timeout_ticks != 0) ? timeout_ticks : NX_WAIT_FOREVER;
}

ULONG _tx_time_get(VOID) { return h_now; }

/* ---- the NetX calls bsd_accept reaches ----------------------------------- */

UINT _nxe_tcp_socket_disconnect(NX_TCP_SOCKET *socket_ptr, ULONG wait_option)
{
    /* NX_NO_WAIT on an established socket is NetX's reset. */
    if (wait_option == NX_NO_WAIT &&
        socket_ptr->nx_tcp_socket_state == NX_TCP_ESTABLISHED)
        h_resets++;
    socket_ptr->nx_tcp_socket_state = NX_TCP_CLOSED;
    return NX_SUCCESS;
}

/* bsd_listen_refill() (select.c) as the IP thread runs it from the listen
   callback when a handshake completes: held off by ASF_RELISTENING, else the
   first CLOSED socket on the list is relistened and the SYN cache hands it
   the finished connection. */
static VOID h_refill(AmiSocket *listener)
{
    AmiSocket *p;

    if ((listener->as_Flags & ASF_RELISTENING) != 0)
    {
        h_refill_skipped++;
        return;
    }

    for (p = listener->as_Incoming; p != NULL; p = p->as_IncomingNext)
    {
        if (p->as_Nx.tcp.nx_tcp_socket_state != NX_TCP_CLOSED)
            continue;
        p->as_Nx.tcp.nx_tcp_socket_state = NX_TCP_ESTABLISHED;
        h_refilled++;
        break;
    }
}

UINT _nxe_tcp_server_socket_unaccept(NX_TCP_SOCKET *socket_ptr)
{
    socket_ptr->nx_tcp_socket_state = NX_TCP_CLOSED;

    /* The receive-queue flush releases a packet to a pool waiter: the baton
       goes to the IP thread, which runs the listen callback (N-088). */
    if (h_refill_at_yield)
    {
        h_refill_at_yield = 0;
        h_refill(&h_listener);
    }
    return NX_SUCCESS;
}

UINT _nxe_tcp_server_socket_relisten(NX_IP *ip_ptr, UINT port,
                                     NX_TCP_SOCKET *socket_ptr)
{
    (VOID)ip_ptr; (VOID)port;
    h_relistens++;
    if (socket_ptr->nx_tcp_socket_state != NX_TCP_CLOSED)
    {
        /* What bsd_listen_return() does with this is destroy the socket,
           which resets the connection the refill just gave it. */
        h_not_closed++;
        printf("  FAIL N-088: the hand-back's relisten met a socket the refill "
               "already relistened; a live connection would be reset\n");
        printf("accept_refused: %lu checks, %lu failures\n", h_checks,
               h_failures + 1);
        exit(1);
    }
    socket_ptr->nx_tcp_socket_state = NX_TCP_LISTEN_STATE;
    return NX_SUCCESS;
}

UINT _nxe_tcp_server_socket_accept(NX_TCP_SOCKET *socket_ptr,
                                   ULONG wait_option)
{
    (VOID)socket_ptr; (VOID)wait_option;
    return NX_IN_PROGRESS;
}

UINT _nxde_tcp_socket_peer_info_get(NX_TCP_SOCKET *socket_ptr,
                                    NXD_ADDRESS *peer_ip_address,
                                    ULONG *peer_port)
{
    *peer_ip_address = socket_ptr->nx_tcp_socket_connect_ip;
    *peer_port       = socket_ptr->nx_tcp_socket_connect_port;
    return NX_SUCCESS;
}

/* ---- the rest of the closure --------------------------------------------- */

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

LONG bsd_nx_enter(struct AmiSocketBase *base) { (VOID)base; return 0; }

VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; }
VOID Forbid(VOID) { }
VOID Permit(VOID) { }

LONG bsd_errno_from_nx(UINT status) { (VOID)status; return AMI_EIO; }

/* The refill after a successful accept asks for a fresh slot; none is had,
   which bsd_listen_rearm() already takes in its stride. */
APTR ami_alloc(ULONG size) { (VOID)size; return NULL; }

/* socket.c's stored-zone checks (#51); no slot is ever reused here. */
ULONG netstack_interface_epoch(UWORD index) { (VOID)index; return 0; }

/* Linked in by the slot rebuild, the destroy path and bsd_accept_once's
   suspending arm, none of which a case reaches: the parked socket is never
   short, never destroyed, and the script completes it before the arm.  Each
   traps, so a case that strays onto one of them cannot pass. */
/* A trap never reads its arguments. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define H_TRAP(decl) decl { printf("  TRAP %s\n", __func__); abort(); }
H_TRAP(UINT _nxe_tcp_socket_create(NX_IP *ip, NX_TCP_SOCKET *s, CHAR *n,
       ULONG tos, ULONG frag, UINT ttl, ULONG win,
       VOID (*urg)(NX_TCP_SOCKET *), VOID (*disc)(NX_TCP_SOCKET *), UINT size))
H_TRAP(UINT _nxe_tcp_socket_delete(NX_TCP_SOCKET *s))
H_TRAP(UINT _nxe_tcp_client_socket_unbind(NX_TCP_SOCKET *s))
H_TRAP(UINT _nxe_tcp_socket_receive_notify(NX_TCP_SOCKET *s,
       VOID (*cb)(NX_TCP_SOCKET *)))
H_TRAP(UINT _nxe_tcp_socket_state_wait(NX_TCP_SOCKET *s, UINT st, ULONG w))
H_TRAP(UINT _nxe_udp_socket_delete(NX_UDP_SOCKET *s))
H_TRAP(UINT _nxe_udp_socket_unbind(NX_UDP_SOCKET *s))
H_TRAP(UINT _nxe_packet_release(NX_PACKET **p))
H_TRAP(VOID _nx_tcp_packet_send_fin(NX_TCP_SOCKET *s, ULONG seq))
H_TRAP(VOID _nx_tcp_packet_send_rst(NX_TCP_SOCKET *s, NX_TCP_HEADER *h))
H_TRAP(UINT _txe_mutex_get(TX_MUTEX *m, ULONG w))
H_TRAP(UINT _txe_mutex_put(TX_MUTEX *m))
H_TRAP(VOID ami_free(APTR p))
H_TRAP(VOID ami_mem_socket_delta(LONG d))
H_TRAP(ULONG ami_random_ulong(VOID))
H_TRAP(ULONG ami_bsd_tcp_budget(ULONG pool, ULONG payload))
H_TRAP(ULONG ami_bsd_tcp_window_for(ULONG pool, ULONG payload, ULONG users))
H_TRAP(ULONG ami_bsd_tcp_window_max_for(ULONG pool, ULONG payload, ULONG users))
H_TRAP(UINT _nxe_tcp_server_socket_unlisten(NX_IP *ip, UINT port))
H_TRAP(UINT _nxe_tcp_server_socket_listen(NX_IP *ip, UINT port,
       NX_TCP_SOCKET *s, UINT queue, VOID (*cb)(NX_TCP_SOCKET *, UINT)))
H_TRAP(VOID bsd_listen_callback(NX_TCP_SOCKET *s, UINT port))
H_TRAP(VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size))
H_TRAP(VOID bsd_bzero(APTR p, ULONG size))
H_TRAP(VOID bsd_cmsg_reset(AmiSocket *s))
H_TRAP(VOID bsd_events_attach(AmiSocket *s))
H_TRAP(VOID bsd_mcast_close(AmiSocket *s))
H_TRAP(VOID bsd_raw_close(AmiSocket *s))
H_TRAP(VOID bsd_tcp_disconnect_callback(NX_TCP_SOCKET *s))
H_TRAP(VOID bsd_tcp_urgent_notify(NX_TCP_SOCKET *s))
/* IPv6 helpers from other units: no case binds a scoped address or hands
   back a v4-mapped peer. */
H_TRAP(UINT anx6_scope(const ULONG *addr))
H_TRAP(VOID bsd_addr_to_v4mapped(NXD_ADDRESS *addr, ULONG v4))
H_TRAP(VOID bsd_words_to_in6(const ULONG words[4], UBYTE bytes[16]))
#pragma GCC diagnostic pop

/* ---- the test ------------------------------------------------------------ */

static LONG h_accept(void)
{
    h_base.sb_Errno = 0;
    return bsd_accept(0, NULL, NULL, &h_base);
}

int main(void)
{
    /* A shared listener can belong to another opener while this base accepts.
       If allocating the accepted fd fails, the returned slot belongs to the
       listener again, not to the unsuccessful accepting base. */
    {
        static const int s[] = { STEP_MATCH };
        struct AmiSocketBase owner;

        h_reset(0, 0, s, 1);
        memset(&owner, 0, sizeof(owner));
        owner.sb_StackRefs = 1;
        owner.sb_StackIp = &h_ip;
        h_listener.as_Owner = &owner;
        h_table[1] = BSD_FD_RESERVED;
        CHECK(h_accept() == -1 && h_base.sb_Errno == AMI_EMFILE,
              "a full accepting table returns the descriptor error");
        CHECK(h_incoming.as_Parent == &h_listener &&
              h_incoming.as_Owner == &owner &&
              (h_incoming.as_Flags & ASF_INCOMING) != 0,
              "failed accept restores its listener's current owner");
        CHECK(h_listener.as_Incoming == &h_incoming && h_relistens == 1,
              "the failed accept remains parked and rearmed");
    }

    /* Blocking: a refused peer is reset, and accept waits on for the next. */
    {
        static const int s[] = { STEP_REFUSED, STEP_MATCH };
        LONG fd;

        h_reset(0, 0, s, 2);
        fd = h_accept();
        CHECK(h_base.sb_Errno != AMI_EWOULDBLOCK,
              "blocking accept does not answer EWOULDBLOCK on a refusal");
        CHECK(fd == 1, "blocking accept returns the next, matching peer");
        CHECK(h_waits == 2, "blocking accept waited again after the refusal");
        CHECK(h_resets == 1, "the refused peer was reset");
        CHECK(h_relistens == 1, "the refused slot went back on the port");
        CHECK(h_wait_arg[1] == NX_WAIT_FOREVER,
              "the second wait is still forever");
    }

    /* Blocking: the break still interrupts the wait that follows a refusal. */
    {
        static const int s[] = { STEP_REFUSED, STEP_REFUSED, STEP_BREAK };

        h_reset(0, 0, s, 3);
        CHECK(h_accept() == -1, "blocking accept, then break, fails");
        CHECK(h_base.sb_Errno == AMI_EINTR,
              "break after refusals is EINTR, not EWOULDBLOCK");
        CHECK(h_waits == 3, "one wait per refusal, then the break");
        CHECK(h_resets == 2, "each refused peer was reset");
    }

    /* Non-blocking: a refusal is still EWOULDBLOCK, with no second wait. */
    {
        static const int s[] = { STEP_REFUSED, STEP_MATCH };

        h_reset(ASF_NONBLOCK, 0, s, 2);
        CHECK(h_accept() == -1, "non-blocking accept of a refusal fails");
        CHECK(h_base.sb_Errno == AMI_EWOULDBLOCK,
              "non-blocking refusal is EWOULDBLOCK");
        CHECK(h_waits == 1, "non-blocking accept waits once");
        CHECK(h_resets == 1, "the refused peer was reset");
    }

    /* SO_RCVTIMEO: the wait after a refusal gets what is left, not a fresh
       timeout, and running out is EWOULDBLOCK as a plain timeout is. */
    {
        static const int s[] = { STEP_REFUSED, STEP_TIMEOUT };

        h_reset(0, 120, s, 2);
        CHECK(h_accept() == -1, "timed accept runs out");
        CHECK(h_base.sb_Errno == AMI_EWOULDBLOCK,
              "an expired SO_RCVTIMEO is EWOULDBLOCK");
        CHECK(h_waits == 2, "timed accept waited again after the refusal");
        CHECK(h_wait_arg[0] == 120 && h_wait_arg[1] == 70,
              "the second wait is the remainder of SO_RCVTIMEO");
    }

    /* SO_RCVTIMEO spent by the refusal itself: one last look, no wait. */
    {
        static const int s[] = { STEP_REFUSED, STEP_TIMEOUT };

        h_reset(0, 40, s, 2);
        CHECK(h_accept() == -1, "spent timed accept fails");
        CHECK(h_base.sb_Errno == AMI_EWOULDBLOCK,
              "a spent SO_RCVTIMEO is EWOULDBLOCK");
        CHECK(h_waits == 2 && h_wait_arg[1] == NX_NO_WAIT,
              "a spent SO_RCVTIMEO polls once without waiting");
    }

    /* The other refusal: a V6ONLY wildcard listener handed an IPv4 peer. */
    {
        static const int s[] = { STEP_REFUSED, STEP_BREAK };

        h_reset(ASF_V6ONLY, 0, s, 2);
        memset(&h_listener.as_LocalAddr, 0, sizeof(h_listener.as_LocalAddr));
        h_listener.as_LocalAddr.nxd_ip_version = NX_IP_VERSION_V6;
        CHECK(h_accept() == -1, "blocking V6ONLY accept, then break, fails");
        CHECK(h_base.sb_Errno == AMI_EINTR,
              "V6ONLY refusal is not EWOULDBLOCK on a blocking accept");
        CHECK(h_waits == 2, "V6ONLY accept waited again after the refusal");
        CHECK(h_resets == 1, "the refused IPv4 peer was reset");
    }

    /* N-088, the refused-peer hand-back: the refill at the unaccept's yield
       is held off, the slot is relistened by the hand-back, and the only
       reset is the refused peer's. */
    {
        static const int s[] = { STEP_REFUSED, STEP_BREAK };

        h_reset(0, 0, s, 2);
        h_refill_at_yield = 1;
        CHECK(h_accept() == -1 && h_base.sb_Errno == AMI_EINTR,
              "N-088: refusal then break, as without the refill");
        CHECK(h_refill_skipped == 1 && h_refilled == 0 && h_not_closed == 0,
              "N-088: the refill does not take a slot being handed back");
        CHECK(h_resets == 1 && h_relistens == 1 &&
              h_incoming.as_Nx.tcp.nx_tcp_socket_state == NX_TCP_LISTEN_STATE &&
              h_listener.as_Incoming == &h_incoming,
              "N-088: the hand-back relistens it, and only the refused peer is reset");
        CHECK((h_listener.as_Flags & ASF_RELISTENING) == 0,
              "N-088: the refill is let back in afterwards");
    }

    /* N-088, the out-of-descriptors hand-back. */
    {
        static const int s[] = { STEP_MATCH };

        h_reset(0, 0, s, 1);
        h_table[1] = BSD_FD_RESERVED;
        h_refill_at_yield = 1;
        CHECK(h_accept() == -1 && h_base.sb_Errno == AMI_EMFILE,
              "N-088: a full table is EMFILE");
        CHECK(h_refill_skipped == 1 && h_refilled == 0 && h_not_closed == 0 &&
              h_incoming.as_Nx.tcp.nx_tcp_socket_state == NX_TCP_LISTEN_STATE &&
              h_incoming.as_Parent == &h_listener &&
              h_listener.as_Incoming == &h_incoming,
              "N-088: the slot goes back to the listener, relistened by the hand-back");
        CHECK((h_listener.as_Flags & ASF_RELISTENING) == 0,
              "N-088: and the refill is let back in");
    }

    printf("accept_refused: %lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}

#include "socket.c"
