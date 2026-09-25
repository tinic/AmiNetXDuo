/*
 * AmiNetXDuo, SO_REUSEPORT in the NetX Duo core.
 *
 * This is the host half of GitHub issue #38.  The NetX Duo fork lets a UDP
 * socket opt into port sharing by setting nx_udp_socket_share before bind;
 * the dispatch then fans a multicast datagram out to every same-port sharer
 * and delivers a unicast datagram to exactly one.  The BSD layer sets the flag
 * from ASF_REUSEPORT (options.c -> socket.c), and the built-in mDNS responder
 * sets it on its own 5353 socket, so this test drives the core directly.
 *
 * The topology is built by hand out of an NX_IP; ThreadX primitives and the
 * one packet-pool cleanup path are stubbed exactly as test_tcp_source_connect
 * does, and the packet header fields are set to the host-order values the IP
 * receive path leaves behind after its in-place byte swap.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_udp.h"
#include "nx_ip.h"
#include "nx_packet.h"

#include <stdio.h>
#include <string.h>


/* ------------------------------------------------------------- harness ---- */

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("  FAIL %s\n", what);
        return;
    }

    printf("  ok   %s\n", what);
}


/* --------------------------------------------------------------- stubs ---- */

TX_THREAD *_tx_thread_current_ptr;
volatile ULONG _tx_thread_preempt_disable;
ULONG _tx_thread_system_state;

/* Set by the suspended-receiver regression (test 1); makes the resume stub
   below simulate the resumed thread taking the packet and its memory being
   reused.  */
static int h_suspended_receiver_consumes;

VOID _tx_thread_system_suspend(TX_THREAD *thread_ptr)
{
    (void)thread_ptr;
}

VOID _tx_thread_system_resume(TX_THREAD *thread_ptr)
{
NX_PACKET **slot;

    /* Test 1: when the suspended-receiver regression is armed, the resumed
       thread takes the packet the primary delivery just handed it and its
       memory is reused.  Scribble the fields the fan-out reads so a fan-out
       that runs after this resume can no longer see a multicast datagram.  */
    if (h_suspended_receiver_consumes)
    {
        slot =  (NX_PACKET **)thread_ptr -> tx_thread_additional_suspend_info;
        if ((slot) && (*slot))
        {
            (*slot) -> nx_packet_ip_version =  0;
            (*slot) -> nx_packet_ip_header  =  NX_NULL;
        }
    }
}

VOID _tx_thread_system_preempt_check(VOID)
{
}

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

UINT _tx_thread_interrupt_disable(void)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    (void)previous_posture;
}

/* Referenced by nx_packet_allocate.c's error unwind; a full pool is never hit
   here, so the no-op is enough. */
VOID _nx_packet_pool_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}

/* Referenced by nx_udp_socket_bind.c's NX_ANY_PORT and wait_option arms, both
   of which this test avoids (specific port, NX_NO_WAIT). */
UINT _nx_udp_free_port_find(NX_IP *ip_ptr, UINT port, UINT *free_port_ptr)
{
    (void)ip_ptr;
    *free_port_ptr = port;
    return NX_SUCCESS;
}

VOID _nx_udp_bind_cleanup(TX_THREAD *thread_ptr NX_CLEANUP_PARAMETER)
{
    NX_CLEANUP_EXTENSION
    (void)thread_ptr;
}


/* ---------------------------------------------------------- the machine --- */

#define H_PORT          5353U

static NX_IP          h_ip;
static NX_PACKET_POOL h_pool;
static UCHAR          h_pool_memory[16 * 256];

static NX_UDP_SOCKET  h_socket_a;
static NX_UDP_SOCKET  h_socket_b;
static NX_UDP_SOCKET  h_socket_c;   /* non-sharing */

/* A packet buffer laid out as the IP receive path leaves it: IPv4 header at
   the data start, UDP header at the prepend pointer. */
static UCHAR          h_packet_bytes[64];
static NX_PACKET      h_packet;

/* The overflow regression needs a second, distinct packet buffer so the
   oldest entry it drops keeps a destination that differs from the datagram
   that triggers the overflow.  */
static UCHAR          h_fill_bytes[64];
static NX_PACKET      h_fill_packet;

/* The suspended receiver of the first regression, and the slot the primary
   delivery writes the handed-off packet into.  */
static TX_THREAD      h_thread;
static NX_PACKET     *h_received_slot;

/* Set by the synchronous receive callbacks of the second and fourth
   regressions.  */
static int            h_primary_callback_consumed;
static int            h_sibling_callback_closed;

/* Test 5: the sibling's receive callback closes a *different* socket that is
   still pending in the walk.  */
static NX_UDP_SOCKET *h_sibling_to_close;
static int            h_sibling_callback_closed_other;

/* The port-table bucket a 5353 bind lands in. */
static UINT h_index(void)
{
    return (UINT)((H_PORT + (H_PORT >> 8)) & NX_UDP_PORT_TABLE_MASK);
}

/* Put a socket into the state a freshly created NetX UDP socket is in, with
   the opt-in flag off. */
static void h_socket_arm(NX_UDP_SOCKET *socket_ptr)
{
    memset(socket_ptr, 0, sizeof(*socket_ptr));
    socket_ptr -> nx_udp_socket_id   = NX_UDP_ID;
    socket_ptr -> nx_udp_socket_ip_ptr = &h_ip;
    socket_ptr -> nx_udp_socket_port = H_PORT;
    socket_ptr -> nx_udp_socket_queue_maximum = 4;
}

/* Build one IPv4/UDP datagram into an arbitrary packet struct + buffer whose
   destination is `dest_ip` (host order) and whose UDP destination port is
   H_PORT. */
static void h_packet_arm_to(NX_PACKET *packet_ptr, UCHAR *bytes, ULONG dest_ip)
{
    NX_IPV4_HEADER *ipv4;
    NX_UDP_HEADER  *udp;
    ULONG           payload;

    memset(bytes, 0, 64);

    ipv4 = (NX_IPV4_HEADER *)(VOID *)bytes;
    /* Only the destination address is read by the dispatch path; the rest of
       the IPv4 header is not inspected here. */
    ipv4 -> nx_ip_header_destination_ip  = dest_ip;

    /* NX_IPV4_HEADER is 20 bytes; the UDP header follows it. */
    udp = (NX_UDP_HEADER *)(VOID *)(bytes + sizeof(NX_IPV4_HEADER));
    /* The UDP header is still in wire order here: _nx_udp_packet_receive does
       the in-place NX_CHANGE_ULONG_ENDIAN itself.  Store word 0 as the raw
       network bytes -- destination-port bytes at offsets 2 and 3, which on a
       little-endian host is the high half of the ULONG -- so the swap leaves
       5353 in the low half. */
    udp -> nx_udp_header_word_0 =
        (((ULONG)H_PORT & 0x00FFUL) << 24) | (((ULONG)H_PORT & 0xFF00UL) << 8);
    udp -> nx_udp_header_word_1 = 0;

    payload = 0x11223344UL;

    memset(packet_ptr, 0, sizeof(*packet_ptr));
    packet_ptr -> nx_packet_data_start    = bytes;
    packet_ptr -> nx_packet_ip_header     = bytes;
    packet_ptr -> nx_packet_prepend_ptr   = bytes + sizeof(NX_IPV4_HEADER);
    packet_ptr -> nx_packet_append_ptr    = bytes + sizeof(NX_IPV4_HEADER)
                                             + sizeof(NX_UDP_HEADER) + sizeof(payload);
    packet_ptr -> nx_packet_length        = sizeof(NX_UDP_HEADER) + sizeof(payload);
    packet_ptr -> nx_packet_pool_owner    = &h_pool;
    packet_ptr -> nx_packet_ip_version    = NX_IP_VERSION_V4;
}

/* Build one IPv4/UDP datagram into the shared h_packet. */
static void h_packet_arm(ULONG dest_ip)
{
    h_packet_arm_to(&h_packet, h_packet_bytes, dest_ip);
}


/* Test 2: the primary socket's receive callback consumes the queued datagram
   synchronously.  The fixed dispatch has already cloned packet_ptr by the time
   this runs; under the pre-fix order it runs first, and the scribble below
   (standing in for the freed packet's memory being reused) makes the late
   fan-out see a non-multicast datagram.  */
static void h_primary_callback_consume(NX_UDP_SOCKET *socket_ptr)
{
NX_PACKET *p;

    h_primary_callback_consumed = 1;

    p =  socket_ptr -> nx_udp_socket_receive_head;
    if (p)
    {
        socket_ptr -> nx_udp_socket_receive_head =  NX_NULL;
        socket_ptr -> nx_udp_socket_receive_tail =  NX_NULL;
        socket_ptr -> nx_udp_socket_receive_count =  0;

        p -> nx_packet_ip_version =  0;
        p -> nx_packet_ip_header  =  NX_NULL;
    }
}

/* Test 4: the sibling's receive callback closes its own socket.  The fixed
   walk captures the sibling's bound_next before delivering, so it stops
   cleanly; the pre-fix walk follows the closed socket's now-NULL bound_next
   and dereferences NULL.  */
static void h_sibling_callback_close(NX_UDP_SOCKET *socket_ptr)
{
    h_sibling_callback_closed = 1;
    (VOID)_nx_udp_socket_unbind(socket_ptr);
}

/* Test 5: the sibling's receive callback closes a *different* socket that is
   still pending in the walk.  Capture-next alone is not enough here: the
   captured successor is the very node the callback unlinked, so a walk that
   blindly follows it dereferences the removed node's now-NULL bound_next and
   faults.  The fixed walk re-reads the survivor's successor instead.  */
static void h_sibling_callback_close_other(NX_UDP_SOCKET *socket_ptr)
{
    (void)socket_ptr;
    h_sibling_callback_closed_other = 1;
    (VOID)_nx_udp_socket_unbind(h_sibling_to_close);
}


int main(void)
{
    ULONG dest;
    UINT  status;

    memset(&h_ip, 0, sizeof(h_ip));
    h_ip.nx_ip_id = NX_IP_ID;
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;

    if (_nx_packet_pool_create(&h_pool, "host", 256, h_pool_memory,
                               sizeof(h_pool_memory)) != NX_SUCCESS)
    {
        printf("UdpShare: no packet pool\n");
        return 1;
    }

    h_ip.nx_ip_default_packet_pool = &h_pool;

    /* ---- the opt-in bind gate ------------------------------------------- */

    /* Every participant must opt in before bind, so A opts in first. */
    h_socket_arm(&h_socket_a);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    status = _nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "first bind of 5353 succeeds");

    /* A second sharer may co-bind the port. */
    h_socket_arm(&h_socket_b);
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    status = _nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "a sharing socket co-binds 5353");

    /* A non-sharer may not. */
    h_socket_arm(&h_socket_c);
    status = _nx_udp_socket_bind(&h_socket_c, H_PORT, NX_NO_WAIT);
    h_check(status == NX_PORT_UNAVAILABLE,
            "a non-sharing socket is refused on a shared port");

    /* And the reverse: a port held without sharing will not take a sharer,
       which is the "everyone opts in" rule from the other side. */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    status = _nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "a lone non-sharing socket binds");

    h_socket_arm(&h_socket_b);
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    status = _nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT);
    h_check(status == NX_PORT_UNAVAILABLE,
            "a sharer is refused on a port held without sharing");

    /* ---- multicast fans out to every sharer ----------------------------- */

    /* Rebuild a clean two-socket shared list for the dispatch checks. */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_arm(&h_socket_b);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_socket_share = NX_TRUE;

    status = _nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "rearm: socket A binds");
    status = _nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "rearm: socket B co-binds");

    dest = 0xE0000001UL;   /* 224.0.0.1, host order */
    h_packet_arm(dest);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_socket_a.nx_udp_socket_receive_count == 1,
            "multicast: primary socket queued one");
    h_check(h_socket_b.nx_udp_socket_receive_count == 1,
            "multicast: co-bound socket received the clone");

    /* ---- unicast is delivered to exactly one ---------------------------- */

    dest = 0x0A000001UL;   /* 10.0.0.1, host order */
    h_packet_arm(dest);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_socket_a.nx_udp_socket_receive_count == 2,
            "unicast: the primary socket got the second datagram");
    h_check(h_socket_b.nx_udp_socket_receive_count == 1,
            "unicast: the co-bound socket did not get it");

    /* ---- a non-sharing sole owner is not fanned out --------------------- */

    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_a.nx_udp_socket_share = NX_FALSE;

    status = _nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT);
    h_check(status == NX_SUCCESS, "rearm: a lone non-sharing socket binds");

    dest = 0xE0000001UL;
    h_packet_arm(dest);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_socket_a.nx_udp_socket_receive_count == 1,
            "multicast to a lone socket is delivered once, no fan-out");

    /* ---- regression 1: a suspended receiver owns the packet ------------- */

    /* The primary has a waiting receiver, so the primary delivery resumes it
       (handing over packet_ptr) instead of queueing.  The resumed receiver
       takes the packet; the fan-out must have cloned it to B first.  */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_arm(&h_socket_b);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    h_check(_nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 1: A binds");
    h_check(_nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 1: B co-binds");

    memset(&h_thread, 0, sizeof(h_thread));
    h_thread.tx_thread_suspended_next = &h_thread;
    h_thread.tx_thread_suspended_previous = &h_thread;
    h_thread.tx_thread_additional_suspend_info = &h_received_slot;
    h_received_slot = NX_NULL;
    h_socket_a.nx_udp_socket_receive_suspension_list = &h_thread;
    h_socket_a.nx_udp_socket_receive_suspended_count = 1;

    h_suspended_receiver_consumes = 1;
    h_packet_arm(0xE0000001UL);
    _nx_udp_packet_receive(&h_ip, &h_packet);
    h_suspended_receiver_consumes = 0;

    h_check(h_received_slot == &h_packet,
            "regression 1: the primary handed the original to the waiter");
    h_check(h_socket_a.nx_udp_socket_receive_suspended_count == 0,
            "regression 1: the waiter came off the suspension list");
    h_check(h_socket_b.nx_udp_socket_receive_count == 1,
            "regression 1: the sibling still got a clone");

    /* ---- regression 2: the receive callback consumes synchronously ------- */

    /* A's receive callback dequeues and "frees" the datagram the moment it is
       queued.  The fan-out must have cloned packet_ptr before that ran.  */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_arm(&h_socket_b);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    h_socket_a.nx_udp_receive_callback = h_primary_callback_consume;
    h_primary_callback_consumed = 0;
    h_check(_nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 2: A binds");
    h_check(_nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 2: B co-binds");

    h_packet_arm(0xE0000001UL);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_primary_callback_consumed == 1,
            "regression 2: the callback consumed the queued datagram");
    h_check(h_socket_a.nx_udp_socket_receive_count == 0,
            "regression 2: the primary queue was drained");
    h_check(h_socket_b.nx_udp_socket_receive_count == 1,
            "regression 2: the sibling still got a clone");

    /* ---- regression 3: queue overflow reassigns packet_ptr ------------- */

    /* Fill A's queue to its maximum with a unicast datagram, then deliver a
       multicast one.  The overflow drops the oldest (the unicast) and, in the
       pre-fix code, reassigns packet_ptr to it, so the late fan-out reads a
       unicast and skips the sibling.  */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_arm(&h_socket_b);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    h_socket_a.nx_udp_socket_queue_maximum = 1;
    h_check(_nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 3: A binds");
    h_check(_nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 3: B co-binds");

    h_packet_arm_to(&h_fill_packet, h_fill_bytes, 0x0A000001UL);
    _nx_udp_packet_receive(&h_ip, &h_fill_packet);

    h_packet_arm(0xE0000001UL);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_socket_a.nx_udp_socket_receive_count == 1,
            "regression 3: the primary holds the newest datagram");
    h_check(h_socket_b.nx_udp_socket_receive_count == 1,
            "regression 3: the sibling still got a clone of the multicast");

    /* ---- regression 4: the sibling callback closes its socket ------------ */

    /* B's receive callback unbinds B.  The fixed walk captures B's bound_next
       before delivering, so it stops cleanly; the pre-fix walk follows the
       closed socket's now-NULL bound_next and faults.  */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_arm(&h_socket_b);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_receive_callback = h_sibling_callback_close;
    h_sibling_callback_closed = 0;
    h_check(_nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 4: A binds");
    h_check(_nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 4: B co-binds");

    h_packet_arm(0xE0000001UL);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_sibling_callback_closed == 1,
            "regression 4: the sibling callback ran");
    h_check(h_socket_b.nx_udp_socket_bound_next == NX_NULL,
            "regression 4: the sibling unbound itself");
    h_check(h_socket_a.nx_udp_socket_receive_count == 1,
            "regression 4: the primary still holds its datagram");

    /* ---- regression 5: the sibling callback closes a different sibling ----- */

    /* Three sharers, so the walk visits B then C.  B's receive callback
       unbinds C.  The fixed walk re-reads B's re-linked successor (A) and
       stops cleanly; capture-next alone would follow C's now-NULL bound_next
       and fault.  */
    h_ip.nx_ip_udp_port_table[h_index()] = NX_NULL;
    h_socket_arm(&h_socket_a);
    h_socket_arm(&h_socket_b);
    h_socket_arm(&h_socket_c);
    h_socket_a.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_socket_share = NX_TRUE;
    h_socket_c.nx_udp_socket_share = NX_TRUE;
    h_socket_b.nx_udp_receive_callback = h_sibling_callback_close_other;
    h_sibling_to_close = &h_socket_c;
    h_sibling_callback_closed_other = 0;
    h_check(_nx_udp_socket_bind(&h_socket_a, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 5: A binds");
    h_check(_nx_udp_socket_bind(&h_socket_b, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 5: B co-binds");
    h_check(_nx_udp_socket_bind(&h_socket_c, H_PORT, NX_NO_WAIT) == NX_SUCCESS,
            "regression 5: C co-binds");

    h_packet_arm(0xE0000001UL);
    _nx_udp_packet_receive(&h_ip, &h_packet);

    h_check(h_sibling_callback_closed_other == 1,
            "regression 5: the sibling callback ran and closed C");
    h_check(h_socket_c.nx_udp_socket_bound_next == NX_NULL,
            "regression 5: C was unbound");
    h_check(h_socket_a.nx_udp_socket_receive_count == 1,
            "regression 5: the primary still holds its datagram");
    h_check(h_socket_b.nx_udp_socket_receive_count == 1,
            "regression 5: B got its clone before closing C");
    h_check(h_socket_c.nx_udp_socket_receive_count == 0,
            "regression 5: C (closed) got no clone");

    if (h_failures == 0)
    {
        printf("UdpShare: all %lu checks passed\n", h_checks);
        return 0;
    }

    printf("UdpShare: %lu of %lu checks failed\n", h_failures, h_checks);
    return 1;
}
