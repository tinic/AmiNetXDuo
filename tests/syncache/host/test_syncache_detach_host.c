/*
 * AmiNetXDuo issue #50 (F1): a SYN-cache entry outlives its interface.
 *
 * An entry keeps the NX_INTERFACE and, for IPv6, the NXD_IPV6_ADDRESS the SYN
 * arrived on, as raw pointers (_nx_tcp_syncache_fill).  _nx_ip_interface_detach
 * zeroes both and never looks at the cache, so the next SYN-ACK retry, or the
 * accept-queue expiry's RST, sends through what is left.  For IPv6 that is
 * NX_ASSERT(if_ptr != NX_NULL) in _nx_ipv6_packet_send, which on the target
 * sleeps the IP thread forever; for IPv4 the segment goes out of a zeroed or
 * reused interface slot.
 *
 * Linked for real: the SYN cache, the interface detach, the TCP SYN/RST/control
 * senders, the IPv4 route lookup and _nx_ipv6_packet_send.  Stubbed: ThreadX,
 * the packet pool, the link-layer tables detach clears, the IPv4 send (which
 * records the interface it was handed) and _nx_ipv6_header_add (which records
 * that the IPv6 send got past its assert, and stops there).
 *
 * What this asserts is the correct behaviour: once the interface is detached,
 * nothing is sent for a handshake that arrived on it, and no assert fires.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_tcp.h"
#ifdef FEATURE_NX_IPV6
#include "nx_ipv6.h"
#endif

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define K 1u    /* the interface the SYN arrives on and that is detached */

static ULONG host_now;

ULONG _tx_time_get(VOID)                        { return host_now; }
ULONG _nx_amiga_handshake_millis(VOID)          { return 0; }
UINT  _tx_mutex_get(TX_MUTEX *m, ULONG w)       { (void) m; (void) w; return TX_SUCCESS; }
UINT  _tx_mutex_put(TX_MUTEX *m)                { (void) m; return TX_SUCCESS; }

/* NX_ASSERT_FAIL, via host_nx_assert.h.  */
static jmp_buf assert_jump;
static int     assert_armed;
static int     asserts;

void host_nx_assert_fail(const char *file, int line)
{
    printf("     NX_ASSERT failed at %s:%d, %lu s after the SYN"
           " (on the target: the IP thread sleeps forever)\n",
           file, line, (unsigned long) ((host_now - 100000UL) / NX_IP_PERIODIC_RATE));
    asserts++;
    if (!assert_armed)
    {
        abort();
    }
    longjmp(assert_jump, 1);
}

/* What left the stack, and through which interface.  */
static int           sends_v4;
static int           handed_v4;
static int           sends_v6;
static NX_INTERFACE *last_v4_if;
static ULONG         last_v4_source;
static NX_INTERFACE *last_v6_if;

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr, ULONG destination_ip,
                        ULONG type_of_service, ULONG time_to_live, ULONG protocol,
                        ULONG fragment, ULONG next_hop_address)
{
    (void) destination_ip; (void) type_of_service;
    (void) time_to_live; (void) protocol; (void) fragment;

    /* The shipped _nx_ip_packet_send drops a packet with no next hop when
       forwarding is off (nx_ip_packet_send.c, "next_hop_address == 0"), so
       only one with a next hop counts as having left.  */
    handed_v4++;
    if ((next_hop_address == 0) && (ip_ptr -> nx_ip_forward_packet_process == NX_NULL))
    {
        return;
    }
    sends_v4++;
    last_v4_if = packet_ptr -> nx_packet_address.nx_packet_interface_ptr;
    last_v4_source = last_v4_if ? last_v4_if -> nx_interface_ip_address : 0;
}

#ifdef FEATURE_NX_IPV6
UINT _nx_ipv6_header_add(NX_IP *ip_ptr, NX_PACKET **packet_pptr, ULONG protocol,
                         ULONG payload_size, ULONG hop_limit, ULONG traffic_class,
                         ULONG *src_address, ULONG *dest_address, ULONG *fragment)
{
    (void) ip_ptr; (void) protocol; (void) payload_size; (void) hop_limit;
    (void) traffic_class; (void) src_address; (void) dest_address; (void) fragment;

    sends_v6++;
    last_v6_if = (*packet_pptr) -> nx_packet_address.nx_packet_ipv6_address_ptr
                     -> nxd_ipv6_address_attached;
    return NX_NOT_SUCCESSFUL;          /* stop _nx_ipv6_packet_send here */
}
#endif

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol, UINT data_length,
                               ULONG *src, ULONG *dst)
{
    (void) packet_ptr; (void) protocol; (void) data_length; (void) src; (void) dst;
    return 0;
}

/* One packet, reused: every send here is finished with before the next.  */
static NX_PACKET rig_tx_packet;
static UCHAR     rig_tx_buffer[1600];

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    (void) pool_ptr; (void) wait_option;

    memset(&rig_tx_packet, 0, sizeof(rig_tx_packet));
    rig_tx_packet.nx_packet_data_start = rig_tx_buffer;
    rig_tx_packet.nx_packet_data_end = rig_tx_buffer + sizeof(rig_tx_buffer);
    rig_tx_packet.nx_packet_prepend_ptr = rig_tx_buffer + packet_type;
    rig_tx_packet.nx_packet_append_ptr = rig_tx_packet.nx_packet_prepend_ptr;
    *packet_ptr = &rig_tx_packet;
    return NX_SUCCESS;
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)          { (void) packet_ptr; return NX_SUCCESS; }
UINT _nx_packet_transmit_release(NX_PACKET *packet_ptr) { (void) packet_ptr; return NX_SUCCESS; }

/* A SYN fed into the cache from inside the detach: in the unlocked ARP/ND
   gap between its two mutex holds, or from the driver's
   NX_LINK_INTERFACE_DETACH handler.  Models a SYN the IP thread processes
   while the detach is under way.  */
enum { HOOK_NONE, HOOK_GAP, HOOK_DRIVER };
static int  hook_at;
static int  hook_fired;
static void rig_hook(void);

/* The link-layer tables detach clears: nothing here fills them.  */
VOID _nx_arp_interface_entries_delete(NX_IP *ip_ptr, UINT index)
{
    (void) ip_ptr; (void) index;
    if (hook_at == HOOK_GAP)
    {
        rig_hook();
    }
}
UINT _nx_igmp_multicast_interface_leave_internal(NX_IP *ip_ptr, ULONG group, UINT index)
{
    (void) ip_ptr; (void) group; (void) index;
    return NX_SUCCESS;
}
VOID _nx_tcp_socket_connection_reset(NX_TCP_SOCKET *socket_ptr)        { (void) socket_ptr; }
#ifdef FEATURE_NX_IPV6
VOID _nx_nd_cache_interface_entries_delete(NX_IP *ip_ptr, UINT index)
{
    (void) ip_ptr; (void) index;
    if (hook_at == HOOK_GAP)
    {
        rig_hook();
    }
}
UINT _nx_ipv6_multicast_leave(NX_IP *ip_ptr, ULONG *group, NX_INTERFACE *if_ptr)
{
    (void) ip_ptr; (void) group; (void) if_ptr;
    return NX_SUCCESS;
}
VOID _nx_invalidate_destination_entry(NX_IP *ip_ptr, ULONG *next_hop)  { (void) ip_ptr; (void) next_hop; }
#endif

/* Not reached: the accepted-socket hand-over, and _nx_ipv6_packet_send past
   the point _nx_ipv6_header_add stops it.  These only satisfy the link.  */
#define UNREACHED(sig) sig { fprintf(stderr, "unreached: %s\n", #sig); abort(); }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
UNREACHED(VOID _nx_tcp_socket_state_syn_received(NX_TCP_SOCKET *a, NX_TCP_HEADER *b))
#ifdef FEATURE_NX_IPV6
UNREACHED(UINT _nx_packet_copy(NX_PACKET *a, NX_PACKET **b, NX_PACKET_POOL *c, ULONG d))
UNREACHED(VOID _nx_ip_packet_deferred_receive(NX_IP *a, NX_PACKET *b))
UNREACHED(UINT _nx_icmpv6_dest_table_find(NX_IP *a, ULONG *b, NX_IPV6_DESTINATION_ENTRY **c,
                                          ULONG d, ULONG e))
UNREACHED(UINT _nx_icmpv6_dest_table_add(NX_IP *a, ULONG *b, NX_IPV6_DESTINATION_ENTRY **c,
                                         ULONG *d, ULONG e, ULONG f, NXD_IPV6_ADDRESS *g))
UNREACHED(VOID _nx_icmpv6_send_ns(NX_IP *a, ULONG *b, INT c, NXD_IPV6_ADDRESS *d, ULONG e,
                                  ND_CACHE_ENTRY *f))
UNREACHED(INT _nxd_ipv6_search_onlink(NX_IP *a, ULONG *b))
UNREACHED(UINT _nxd_ipv6_router_lookup(NX_IP *a, NX_INTERFACE *b, ULONG *c, VOID **d))
UNREACHED(VOID _nx_ipv6_fragment_process(struct NX_IP_DRIVER_STRUCT *a, UINT b))
UNREACHED(VOID _nx_ip_packet_checksum_compute(NX_PACKET *a))
UNREACHED(ULONG IPv6_Address_Type(ULONG *a))
#endif
#pragma GCC diagnostic pop

static void rig_driver(NX_IP_DRIVER *req)
{
    req -> nx_ip_driver_status = NX_SUCCESS;
    if ((hook_at == HOOK_DRIVER) && (req -> nx_ip_driver_command == NX_LINK_INTERFACE_DETACH))
    {
        rig_hook();
    }
}


static int failures;

static void ok(const char *what, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
    {
        failures++;
    }
}

static NX_IP         rig_ip;
static NX_TCP_LISTEN rig_listen;
static NX_TCP_SOCKET rig_socket;
static NX_PACKET     rig_rx_packet;

static ULONG peer4 = 0xc0a80105UL;                  /* 192.168.1.5  */
static ULONG local4 = 0xc0a80158UL;                 /* 192.168.1.88 */
static ULONG peer4_other = 0x0a000005UL;            /* 10.0.0.5, on interface 0 */
static ULONG local4_other = 0x0a000001UL;           /* 10.0.0.1 */

/* The interface the next rig_syn/rig_ack arrives on: K, or 0 for "other".  */
static UINT rig_if = K;
#ifdef FEATURE_NX_IPV6
static ULONG peer6[4]  = { 0x20010db8UL, 0, 0, 0x5UL };
static ULONG local6[4] = { 0x20010db8UL, 0, 0, 0x88UL };
#endif

static void rig_reset(void)
{
    NX_INTERFACE *ifp;

    memset(&rig_ip, 0, sizeof(rig_ip));
    memset(&rig_listen, 0, sizeof(rig_listen));
    memset(&rig_socket, 0, sizeof(rig_socket));
    memset(&rig_rx_packet, 0, sizeof(rig_rx_packet));

    /* Interface 0: another network, so the route lookup has somewhere else. */
    ifp = &rig_ip.nx_ip_interface[0];
    ifp -> nx_interface_valid = NX_TRUE;
    ifp -> nx_interface_link_up = NX_TRUE;
    ifp -> nx_interface_index = 0;
    ifp -> nx_interface_ip_address = 0x0a000001UL;
    ifp -> nx_interface_ip_network_mask = 0xff000000UL;
    ifp -> nx_interface_ip_network = 0x0a000000UL;
    ifp -> nx_interface_ip_mtu_size = 1500;
    ifp -> nx_interface_link_driver_entry = rig_driver;

    ifp = &rig_ip.nx_ip_interface[K];
    ifp -> nx_interface_valid = NX_TRUE;
    ifp -> nx_interface_link_up = NX_TRUE;
    ifp -> nx_interface_index = (UCHAR) K;
    ifp -> nx_interface_ip_address = local4;
    ifp -> nx_interface_ip_network_mask = 0xffffff00UL;
    ifp -> nx_interface_ip_network = local4 & 0xffffff00UL;
    ifp -> nx_interface_ip_mtu_size = 1500;
    ifp -> nx_interface_link_driver_entry = rig_driver;

#ifdef FEATURE_NX_IPV6
    {
        NXD_IPV6_ADDRESS *a = &rig_ip.nx_ipv6_address[0];

        a -> nxd_ipv6_address_valid = NX_TRUE;
        a -> nxd_ipv6_address_state = NX_IPV6_ADDR_STATE_VALID;
        a -> nxd_ipv6_address_attached = ifp;
        a -> nxd_ipv6_address_prefix_length = 64;
        memcpy(a -> nxd_ipv6_address, local6, sizeof(local6));
        ifp -> nxd_interface_ipv6_address_list_head = a;
        rig_ip.nx_ipv6_hop_limit = 64;
    }
#endif

    rig_listen.nx_tcp_listen_port = 80;
    rig_listen.nx_tcp_listen_queue_maximum = 8;
    rig_listen.nx_tcp_listen_rx_window = 8192;

    rig_socket.nx_tcp_socket_ip_ptr = &rig_ip;
    rig_socket.nx_tcp_socket_state = NX_TCP_LISTEN_STATE;
    rig_socket.nx_tcp_socket_rx_window_default = 8192;

    host_now = 100000;
    rig_if = K;
    hook_at = HOOK_NONE;
    hook_fired = 0;
    sends_v4 = handed_v4 = sends_v6 = asserts = 0;
    last_v4_if = last_v6_if = NX_NULL;

    _nx_tcp_syncache_initialize(&rig_ip);
}

static void rig_packet(ULONG version)
{
    rig_rx_packet.nx_packet_ip_version = (UCHAR) version;
#ifdef FEATURE_NX_IPV6
    if (version == NX_IP_VERSION_V6)
    {
        rig_rx_packet.nx_packet_address.nx_packet_ipv6_address_ptr = &rig_ip.nx_ipv6_address[0];
        return;
    }
#endif
    rig_rx_packet.nx_packet_address.nx_packet_interface_ptr = &rig_ip.nx_ip_interface[rig_if];
}

#define RIG_PEER4  ((rig_if == K) ? &peer4 : &peer4_other)
#define RIG_LOCAL4 ((rig_if == K) ? &local4 : &local4_other)

static void rig_syn(ULONG version, ULONG irs)
{
    NX_TCP_HEADER h;

    memset(&h, 0, sizeof(h));
    h.nx_tcp_sequence_number = irs;
    h.nx_tcp_header_word_3 = NX_TCP_SYN_BIT | 8192UL;
    rig_packet(version);

#ifdef FEATURE_NX_IPV6
    if (version == NX_IP_VERSION_V6)
    {
        _nx_tcp_syncache_syn_received(&rig_ip, &rig_listen, &rig_rx_packet, &h, peer6, local6,
                                      40000, &rig_ip.nx_ip_interface[K], 1440, 2,
                                      NX_TRUE, NX_TRUE, 777);
        return;
    }
#endif
    _nx_tcp_syncache_syn_received(&rig_ip, &rig_listen, &rig_rx_packet, &h, RIG_PEER4, RIG_LOCAL4,
                                  40000, &rig_ip.nx_ip_interface[rig_if], 1460, 2,
                                  NX_TRUE, NX_TRUE, 777);
}

static void rig_ack(ULONG version, ULONG irs, ULONG iss)
{
    NX_TCP_HEADER h;

    memset(&h, 0, sizeof(h));
    h.nx_tcp_sequence_number = irs + 1;
    h.nx_tcp_acknowledgment_number = iss + 1;
    h.nx_tcp_header_word_3 = NX_TCP_ACK_BIT | 8192UL;
    rig_packet(version);

#ifdef FEATURE_NX_IPV6
    if (version == NX_IP_VERSION_V6)
    {
        (void) _nx_tcp_syncache_ack_received(&rig_ip, &rig_listen, &rig_rx_packet, &h, peer6,
                                             local6, 40000, &rig_ip.nx_ip_interface[K],
                                             NX_TRUE, 888);
        return;
    }
#endif
    (void) _nx_tcp_syncache_ack_received(&rig_ip, &rig_listen, &rig_rx_packet, &h, RIG_PEER4,
                                         RIG_LOCAL4, 40000, &rig_ip.nx_ip_interface[rig_if],
                                         NX_TRUE, 888);
}

/* The cache's bookkeeping after a flush: the two counters say what is live,
   and the age, accept, hash and free lists all agree with them.  */
static void rig_cache_check(ULONG live_age, ULONG live_accept)
{
    NX_TCP_SYNCACHE       *c = &rig_ip.nx_ip_tcp_syncache;
    NX_TCP_SYNCACHE_ENTRY *e;
    ULONG                  n_age = 0, n_accept = 0, n_hash = 0, n_free = 0;
    UINT                   b;

    for (e = c -> nx_tcp_syncache_age_head; e; e = e -> nx_tcp_syncache_age_next)
    {
        n_age++;
    }
    for (e = c -> nx_tcp_syncache_accept_head; e; e = e -> nx_tcp_syncache_age_next)
    {
        n_accept++;
    }
    for (b = 0; b < NX_TCP_SYNCACHE_BUCKETS; b++)
    {
        for (e = c -> nx_tcp_syncache_hash[b]; e; e = e -> nx_tcp_syncache_hash_next)
        {
            n_hash++;
        }
    }
    for (e = c -> nx_tcp_syncache_free; e; e = e -> nx_tcp_syncache_hash_next)
    {
        n_free++;
    }

    printf("     cache: count %lu accept %lu; lists age %lu accept %lu hash %lu free %lu\n",
           (unsigned long) c -> nx_tcp_syncache_count,
           (unsigned long) c -> nx_tcp_syncache_accept_count,
           (unsigned long) n_age, (unsigned long) n_accept,
           (unsigned long) n_hash, (unsigned long) n_free);
    ok("the entry count is what survives", c -> nx_tcp_syncache_count == live_age && n_age == live_age);
    ok("the accept count is what survives",
       c -> nx_tcp_syncache_accept_count == live_accept && n_accept == live_accept);
    ok("the hash chains hold exactly the live entries", n_hash == live_age + live_accept);
    ok("every other entry is back on the free list",
       n_free == NX_TCP_SYNCACHE_SIZE - live_age - live_accept);
}

/* One tick of the IP thread's periodic pass, with the assert unwinding here. */
static void rig_tick(void)
{
    host_now += NX_IP_PERIODIC_RATE;
    assert_armed = 1;
    if (setjmp(assert_jump) == 0)
    {
        _nx_tcp_syncache_periodic(&rig_ip);
    }
    assert_armed = 0;
}

/* Every tick up to the accept queue's expiry, which covers the whole retry
   ladder, the half-open timeout and the RST for a queued handshake.  */
static void rig_run_out(void)
{
    ULONG i;
    ULONG ticks = (NX_TCP_SYNCACHE_ACCEPT_TIMEOUT / NX_IP_PERIODIC_RATE) + 2;

    for (i = 0; i < ticks && asserts == 0; i++)
    {
        rig_tick();
    }
}

static void rig_detach(void)
{
    ok("the interface detaches", _nx_ip_interface_detach(&rig_ip, K) == NX_SUCCESS);
#ifdef FEATURE_NX_IPV6
    ok("which clears the IPv6 address's interface",
       rig_ip.nx_ipv6_address[0].nxd_ipv6_address_attached == NX_NULL);
#endif
    sends_v4 = handed_v4 = sends_v6 = 0;
    last_v4_if = last_v6_if = NX_NULL;
}

#ifdef FEATURE_NX_IPV6
/* SYN on interface K, SYN-ACK lost, K detached: the retry.  */
static void case_v6_retry(void)
{
    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    rig_syn(NX_IP_VERSION_V6, 0x1000);
    ok("the SYN is cached", rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_count == 1);
    ok("and answered through interface K",
       sends_v6 == 1 && last_v6_if == &rig_ip.nx_ip_interface[K]);

    rig_detach();
    rig_cache_check(0, 0);
    rig_run_out();

    ok("no NX_ASSERT once the interface is gone", asserts == 0);
    ok("and no SYN-ACK is sent for a handshake on a detached interface",
       sends_v6 == 0);
}

/* SYN and ACK on K with no socket parked: queued.  K detached: the queue's
   expiry sends a RST through the same path.  */
static void case_v6_accept(void)
{
    ULONG iss;

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = NX_NULL;
    rig_syn(NX_IP_VERSION_V6, 0x2000);
    iss = rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_age_head -> nx_tcp_syncache_iss;
    rig_ack(NX_IP_VERSION_V6, 0x2000, iss);
    ok("the handshake is queued for accept",
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_accept_count == 1);

    rig_detach();
    rig_cache_check(0, 0);
    rig_run_out();

    ok("no NX_ASSERT once the interface is gone", asserts == 0);
    ok("and no RST is sent for a queued handshake on a detached interface",
       sends_v6 == 0);
}

/* A lone nxd_ipv6_address_delete with a live IPv6 entry on it, and an IPv4
   entry on the same interface: the IPv6 one goes, sending nothing; the IPv4
   one stays and is still retried through K.  */
static void case_v6_delete(void)
{
    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    rig_syn(NX_IP_VERSION_V6, 0x5000);
    rig_syn(NX_IP_VERSION_V4, 0x5100);
    ok("both SYNs are cached", rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_count == 2);

    ok("the IPv6 address is deleted", _nxd_ipv6_address_delete(&rig_ip, 0) == NX_SUCCESS);
    ok("which zeroes it", rig_ip.nx_ipv6_address[0].nxd_ipv6_address_attached == NX_NULL);
    rig_cache_check(1, 0);
    ok("and the survivor is the IPv4 entry on K",
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_age_head -> nx_tcp_syncache_peer_ip.nxd_ip_version
           == NX_IP_VERSION_V4);
    sends_v4 = handed_v4 = sends_v6 = 0;
    last_v4_if = last_v6_if = NX_NULL;

    rig_run_out();

    ok("no NX_ASSERT once the address is gone", asserts == 0);
    ok("no SYN-ACK is sent for the deleted address", sends_v6 == 0);
    ok("the IPv4 SYN-ACK is still retried through K",
       sends_v4 > 0 && last_v4_if == &rig_ip.nx_ip_interface[K]);
}
#endif

/* Detach must not over-flush: a half-open and a queued handshake on
   interface 0 survive K's detach, and the half-open one is still retried
   through interface 0.  */
static void case_keep_other(void)
{
    ULONG iss;

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = NX_NULL;

    rig_if = 0;
    rig_syn(NX_IP_VERSION_V4, 0x6000);
    iss = rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_age_head -> nx_tcp_syncache_iss;
    rig_ack(NX_IP_VERSION_V4, 0x6000, iss);         /* queued on 0 */
    ok("a handshake on interface 0 is queued for accept",
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_accept_count == 1);
    peer4_other++;                                  /* another peer on 0 */
    rig_syn(NX_IP_VERSION_V4, 0x6200);
    peer4_other--;

    rig_if = K;
    rig_syn(NX_IP_VERSION_V4, 0x6300);              /* half-open on K */
    ok("two half-open, one queued",
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_count == 2 &&
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_accept_count == 1);

    rig_detach();
    rig_cache_check(1, 1);
    ok("the survivors are both on interface 0",
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_age_head -> nx_tcp_syncache_interface
           == &rig_ip.nx_ip_interface[0] &&
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_accept_head -> nx_tcp_syncache_interface
           == &rig_ip.nx_ip_interface[0]);

    rig_tick();
    ok("the half-open one on 0 is still retried, through 0",
       sends_v4 == 1 && last_v4_if == &rig_ip.nx_ip_interface[0] && asserts == 0);
}

/* A deferred SYN, with the assert unwinding here rather than aborting.  */
static void rig_syn_armed(ULONG version, ULONG irs)
{
    assert_armed = 1;
    if (setjmp(assert_jump) == 0)
    {
        rig_syn(version, irs);
    }
    assert_armed = 0;
}

/* No live entry points at interface K or its IPv6 address.  */
static int rig_k_referenced(void)
{
    NX_TCP_SYNCACHE       *c = &rig_ip.nx_ip_tcp_syncache;
    NX_TCP_SYNCACHE_ENTRY *e;
    int                    n = 0;

    for (e = c -> nx_tcp_syncache_age_head; e; e = e -> nx_tcp_syncache_age_next)
    {
        n += (e -> nx_tcp_syncache_interface == &rig_ip.nx_ip_interface[K]);
    }
    for (e = c -> nx_tcp_syncache_accept_head; e; e = e -> nx_tcp_syncache_age_next)
    {
        n += (e -> nx_tcp_syncache_interface == &rig_ip.nx_ip_interface[K]);
    }
    return n;
}

/* The hook: one SYN of each family on K, from inside the detach.  */
static void rig_hook(void)
{
    hook_at = HOOK_NONE;                /* ARP and ND both call in: once */
    hook_fired++;
    rig_if = K;
    rig_syn(NX_IP_VERSION_V4, 0x7000);
#ifdef FEATURE_NX_IPV6
    rig_syn(NX_IP_VERSION_V6, 0x7100);
#endif
}

/* (a) gap, (b) driver: a SYN taken while K is being detached is gone once
   the detach returns, and nothing is sent for it afterwards.  */
static void case_during(int where)
{
    int sent_during;

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    hook_at = where;

    ok("the interface detaches", _nx_ip_interface_detach(&rig_ip, K) == NX_SUCCESS);
    sent_during = sends_v4 + sends_v6;
    printf("     SYNs injected %s: %d; SYN-ACKs sent during the detach: %d\n",
           (where == HOOK_GAP) ? "in the ARP/ND gap" : "from the driver's detach",
           hook_fired, sent_during);
    ok("the hook fired", hook_fired == 1);
    ok("no entry references the detached interface", rig_k_referenced() == 0);
    rig_cache_check(0, 0);

    sends_v4 = handed_v4 = sends_v6 = 0;
    rig_run_out();
    ok("no NX_ASSERT", asserts == 0);
    ok("nothing is sent after the detach", sends_v4 == 0 && handed_v4 == 0 && sends_v6 == 0);
}

/* (c) a deferred SYN presented with K's old interface pointer after the
   detach has returned: the slot zeroed, then refilled by another card under
   another address.  */
static void case_late(void)
{
    NX_INTERFACE *ifp = &rig_ip.nx_ip_interface[K];
    int           reuse;
    ULONG         cookie;

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = NX_NULL;  /* a completed one queues */

    /* A SYN answered before the detach: its ISS is a valid cookie, so an ACK
       with no entry behind it can still rebuild the connection.  */
    rig_syn(NX_IP_VERSION_V4, 0x8800);
    cookie = rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_age_head -> nx_tcp_syncache_iss;
    rig_detach();

    rig_ack(NX_IP_VERSION_V4, 0x8800, cookie);
    printf("     cookie ACK after the detach: cookies valid %lu, queued %lu\n",
           (unsigned long) rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_cookies_valid,
           (unsigned long) rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_accept_count);
    ok("a cookie ACK on the detached interface builds no connection",
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_accept_count == 0 &&
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_cookies_valid == 0);

    for (reuse = 0; reuse < 2; reuse++)
    {
        if (reuse)
        {
            ifp -> nx_interface_valid = NX_TRUE;
            ifp -> nx_interface_link_up = NX_TRUE;
            ifp -> nx_interface_ip_address = 0xc0a80163UL;      /* 192.168.1.99/24 */
            ifp -> nx_interface_ip_network_mask = 0xffffff00UL;
            ifp -> nx_interface_ip_network = 0xc0a80100UL;
            ifp -> nx_interface_ip_mtu_size = 1500;
            ifp -> nx_interface_link_driver_entry = rig_driver;
#ifdef FEATURE_NX_IPV6
            {
                /* The address slot refilled too, under another address.  */
                NXD_IPV6_ADDRESS *a = &rig_ip.nx_ipv6_address[0];
                ULONG             other6[4] = { 0x20010db8UL, 0, 0, 0x99UL };

                a -> nxd_ipv6_address_valid = NX_TRUE;
                a -> nxd_ipv6_address_state = NX_IPV6_ADDR_STATE_VALID;
                a -> nxd_ipv6_address_attached = ifp;
                a -> nxd_ipv6_address_prefix_length = 64;
                memcpy(a -> nxd_ipv6_address, other6, sizeof(other6));
                ifp -> nxd_interface_ipv6_address_list_head = a;
            }
#endif
        }

        rig_if = K;
        rig_syn_armed(NX_IP_VERSION_V4, 0x8000 + (ULONG) reuse);
#ifdef FEATURE_NX_IPV6
        rig_syn_armed(NX_IP_VERSION_V6, 0x8100 + (ULONG) reuse);
#endif
        printf("     %s: count %lu, SYN-ACKs v4 %d (handed %d) v6 %d\n",
               reuse ? "slot reused as 192.168.1.99" : "slot zeroed",
               (unsigned long) rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_count,
               sends_v4, handed_v4, sends_v6);
        ok(reuse ? "reused slot: no entry for a SYN sent to the old address"
                 : "zeroed slot: no entry for a deferred SYN",
           rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_count == 0);
        ok("and nothing is sent", sends_v4 == 0 && handed_v4 == 0 && sends_v6 == 0);
        ok("and no NX_ASSERT", asserts == 0);
    }

    rig_run_out();
    ok("no NX_ASSERT", asserts == 0);
    ok("nothing is sent later", sends_v4 == 0 && handed_v4 == 0 && sends_v6 == 0);
    rig_cache_check(0, 0);
}

#ifdef FEATURE_NX_IPV6
/* (d) a deferred IPv6 SYN presented with a deleted address's pointer, then
   again after the slot is refilled under another address.  */
static void case_v6_late_delete(void)
{
    NXD_IPV6_ADDRESS *a = &rig_ip.nx_ipv6_address[0];
    ULONG             other6[4] = { 0x20010db8UL, 0, 0, 0x99UL };

    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    ok("the IPv6 address is deleted", _nxd_ipv6_address_delete(&rig_ip, 0) == NX_SUCCESS);

    rig_syn_armed(NX_IP_VERSION_V6, 0x9000);
    ok("deleted address: no entry", rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_count == 0);
    ok("and no NX_ASSERT", asserts == 0);
    ok("and no SYN-ACK", sends_v6 == 0);

    a -> nxd_ipv6_address_valid = NX_TRUE;
    a -> nxd_ipv6_address_state = NX_IPV6_ADDR_STATE_VALID;
    a -> nxd_ipv6_address_attached = &rig_ip.nx_ip_interface[K];
    a -> nxd_ipv6_address_prefix_length = 64;
    memcpy(a -> nxd_ipv6_address, other6, sizeof(other6));
    rig_ip.nx_ip_interface[K].nxd_interface_ipv6_address_list_head = a;

    rig_syn_armed(NX_IP_VERSION_V6, 0x9001);
    ok("slot refilled as ::99: no entry for a SYN sent to ::88",
       rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_count == 0);
    ok("and no SYN-ACK", sends_v6 == 0);

    rig_run_out();
    ok("no NX_ASSERT", asserts == 0);
    rig_cache_check(0, 0);
}
#endif

/* SYN on K, SYN-ACK lost, K detached.  Detached and left empty, the retry's
   route lookup fails and the shipped IPv4 send drops it for want of a next
   hop, so that arm passes today.  With `reuse' the slot is then taken by
   another card on the same LAN under another address, as
   _nx_ip_interface_attach would fill it (its fields set here directly;
   attach itself pulls the driver in): the stale pointer now routes, and the
   SYN-ACK leaves from an address the peer never sent its SYN to.  */
static void case_v4_retry(int reuse)
{
    rig_reset();
    rig_listen.nx_tcp_listen_socket_ptr = &rig_socket;
    rig_syn(NX_IP_VERSION_V4, 0x3000);
    ok("the SYN is cached", rig_ip.nx_ip_tcp_syncache.nx_tcp_syncache_count == 1);
    ok("and answered through interface K",
       sends_v4 == 1 && last_v4_if == &rig_ip.nx_ip_interface[K]);

    rig_detach();
    rig_cache_check(0, 0);

    if (reuse)
    {
        NX_INTERFACE *ifp = &rig_ip.nx_ip_interface[K];

        ifp -> nx_interface_valid = NX_TRUE;
        ifp -> nx_interface_link_up = NX_TRUE;
        ifp -> nx_interface_ip_address = 0xc0a80163UL;      /* 192.168.1.99/24 */
        ifp -> nx_interface_ip_network_mask = 0xffffff00UL;
        ifp -> nx_interface_ip_network = 0xc0a80100UL;
        ifp -> nx_interface_ip_mtu_size = 1500;
        ifp -> nx_interface_link_driver_entry = rig_driver;
    }

    rig_run_out();

    printf("     after the detach: %d SYN-ACK(s) handed to IPv4, %d with a next hop\n",
           handed_v4, sends_v4);
    if (sends_v4)
    {
        printf("     the last through slot %u (%s), source 0x%08lx; the SYN was to 0x%08lx\n", (unsigned) (last_v4_if - rig_ip.nx_ip_interface),
               (last_v4_if && last_v4_if -> nx_interface_valid) ? "valid" : "zeroed",
               (unsigned long) last_v4_source, (unsigned long) local4);
    }
    ok("no NX_ASSERT", asserts == 0);
    ok(reuse ? "and no SYN-ACK leaves through the slot's new network"
             : "and no SYN-ACK leaves through the detached slot",
       sends_v4 == 0);
}


int main(int argc, char **argv)
{
    const char *which = (argc > 1) ? argv[1] : "";

    if (strcmp(which, "v4") == 0)
    {
        case_v4_retry(0);
    }
    else if (strcmp(which, "v4reuse") == 0)
    {
        case_v4_retry(1);
    }
#ifdef FEATURE_NX_IPV6
    else if (strcmp(which, "v6") == 0)
    {
        case_v6_retry();
    }
    else if (strcmp(which, "v6accept") == 0)
    {
        case_v6_accept();
    }
    else if (strcmp(which, "v6delete") == 0)
    {
        case_v6_delete();
    }
    else if (strcmp(which, "v6late") == 0)
    {
        case_v6_late_delete();
    }
#endif
    else if (strcmp(which, "keep") == 0)
    {
        case_keep_other();
    }
    else if (strcmp(which, "gap") == 0)
    {
        case_during(HOOK_GAP);
    }
    else if (strcmp(which, "driver") == 0)
    {
        case_during(HOOK_DRIVER);
    }
    else if (strcmp(which, "late") == 0)
    {
        case_late();
    }
    else
    {
        printf("usage: test_syncache_detach v4|v4reuse|v6|v6accept|v6delete|v6late|keep|gap|driver|late\n");
        return 2;
    }

    if (failures != 0)
    {
        printf("syncache_detach %s: %d failures\n", which, failures);
        return 1;
    }
    printf("syncache_detach %s: all ok\n", which);
    return 0;
}
