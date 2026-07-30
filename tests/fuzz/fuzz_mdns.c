/*
 * AmiNetXDuo -- host fuzz driver for the mDNS packet parser.
 *
 * mDNS is the more exposed of the two parsers: 224.0.0.251:5353 takes
 * unauthenticated multicast from any host on the segment, and no query has to
 * have been sent for a packet to arrive.  The parsing is in
 * addons/mdns/nxd_mdns.c -- src/netstack/netstack_mdns.c starts the module,
 * picks the host label and maps ".local" onto it, and parses nothing.
 *
 * The entry point is _nx_mdns_thread_entry(), the module's own thread, and it
 * is reached without touching vendored source: nx_mdns_create() registers it
 * with tx_thread_create(), the stub below keeps the function pointer, and the
 * driver calls it.  Its loop then runs for real -- event flags, the receive
 * loop, the packet-chain check, the interface lookup, _nx_mdns_packet_process()
 * and the release -- with _nx_udp_socket_receive() handing over the fuzz bytes
 * as a packet.  The loop is a `while(1)`, so the escape is a longjmp from the
 * event-flag wait at the top of it, where nothing is in flight.
 *
 * Both halves of the module are compiled, as they are on target: the responder
 * that answers a query for this machine's name, and the client that resolves
 * somebody else's.  So a hostile query reaches the response builder and a
 * hostile response reaches the peer cache.
 *
 * Type widths are the host's, not the target's, and that is forced:
 * nx_mdns_create() passes the instance pointer to tx_thread_create() as a
 * ULONG and the thread casts it back, which needs a ULONG at least as wide as
 * a pointer.  fuzz_dns uses tests/perf/host/shim for the m68k's 32-bit ULONG;
 * this one cannot.  UINT, USHORT and UCHAR are the same width either way, and
 * that is where the name and record arithmetic lives.
 *
 * Usage:
 *   fuzz_mdns -s                 every seed case, named
 *   fuzz_mdns -c NAME            one seed case by name
 *   fuzz_mdns < datagram         one datagram from stdin
 *   fuzz_mdns -r SEED COUNT      seeds plus mutations, no corpus needed
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_udp.h"
#include "nx_packet.h"
#include "nxd_mdns.h"

#include "fuzz_wire.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FZ_LABEL        "amiga"
#define FZ_LOCAL        "amiga.local"
#define FZ_OUR_IP       IP_ADDRESS(169, 254, 3, 4)
#define FZ_NETMASK      IP_ADDRESS(255, 255, 0, 0)

static NX_IP            fz_ip;
static NX_MDNS          fz_mdns;

/* The module's pool, for the responses it builds.  They go nowhere, but they
   are allocated and appended to for real. */
#define FZ_POOL_PAYLOAD 1568
#define FZ_POOL_PACKETS 16
static ULONG            fz_pool_area[((FZ_POOL_PAYLOAD + sizeof(NX_PACKET) +
                                      32) * FZ_POOL_PACKETS) / sizeof(ULONG)];
static NX_PACKET_POOL   fz_pool;

/* And a pool for what arrives, sized past 512 on purpose. */
#define FZ_WIRE_PAYLOAD (FZW_MAX + 64)
#define FZ_WIRE_PACKETS 8
static ULONG            fz_wire_area[((FZ_WIRE_PAYLOAD + sizeof(NX_PACKET) +
                                      32) * FZ_WIRE_PACKETS) / sizeof(ULONG)];
static NX_PACKET_POOL   fz_wire_pool;

/*
 * The caches, at the sizes src/netstack/netstack_internal.h ships, and on the
 * heap rather than in a static so ASan puts a redzone either side: the cache
 * allocator is the one part of this module that does its own address
 * arithmetic, and a static array's neighbour is valid memory that a stride
 * error would silently land in.
 */
/* Scaled up from the 1024/2048 src/netstack ships, because an NX_MDNS_RR is
   built from pointers and this is a host build where those are 64 bits: the
   target's byte counts hold roughly half as many records here, and
   _nx_mdns_cache_add_string() spins rather than failing when it runs out
   during nx_mdns_enable(). The sizes are a host-side capacity question, not
   part of what is under test. */
#define FZ_LOCAL_CACHE_BYTES    4096
#define FZ_PEER_CACHE_BYTES     8192
static ULONG           *fz_local_cache;
static ULONG           *fz_peer_cache;
static ULONG            fz_stack[1024];

static FzwBuf           fz_case;
static int              fz_delivered;
static unsigned long    fz_cases;
static const char      *fz_case_name = "stdin";

/* What the module registered, and how to get back out of it. */
static VOID           (*fz_thread_entry)(ULONG);
static ULONG            fz_thread_input;
static jmp_buf          fz_escape;
static int              fz_escape_armed;
static ULONG            fz_events_pending;
static int              fz_event_calls;

static ULONG            fz_sends;


/* ----------------------------------------------------------- ThreadX ----- */

static TX_THREAD        fz_caller_thread;

TX_THREAD              *_tx_thread_current_ptr = &fz_caller_thread;
TX_THREAD               _tx_timer_thread;
UINT                    _tx_thread_preempt_disable;
volatile ULONG          _tx_thread_system_state;

UINT _tx_thread_interrupt_disable(VOID) { return 0; }

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    NX_PARAMETER_NOT_USED(previous_posture);
}

UINT _tx_thread_sleep(ULONG timer_ticks)
{
    NX_PARAMETER_NOT_USED(timer_ticks);
    return TX_SUCCESS;
}

TX_THREAD *_tx_thread_identify(VOID) { return _tx_thread_current_ptr; }

VOID _tx_thread_system_suspend(TX_THREAD *t) { NX_PARAMETER_NOT_USED(t); }
VOID _tx_thread_system_resume(TX_THREAD *t)  { NX_PARAMETER_NOT_USED(t); }
VOID _tx_thread_system_preempt_check(VOID)   { }

UINT _tx_thread_preemption_change(TX_THREAD *t, UINT new_threshold,
                                  UINT *old_threshold)
{
    NX_PARAMETER_NOT_USED(t);
    NX_PARAMETER_NOT_USED(new_threshold);

    if (old_threshold != NX_NULL)
        *old_threshold = 0;

    return TX_SUCCESS;
}

static ULONG fz_ticks;

ULONG _tx_time_get(VOID)
{
    return fz_ticks++;
}

UINT _txe_mutex_create(TX_MUTEX *m, CHAR *name, UINT inherit, UINT size)
{
    NX_PARAMETER_NOT_USED(m);
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(inherit);
    NX_PARAMETER_NOT_USED(size);
    return TX_SUCCESS;
}

UINT _txe_mutex_delete(TX_MUTEX *m)
{
    NX_PARAMETER_NOT_USED(m);
    return TX_SUCCESS;
}

UINT _txe_mutex_get(TX_MUTEX *m, ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(m);
    NX_PARAMETER_NOT_USED(wait_option);
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *m)
{
    NX_PARAMETER_NOT_USED(m);
    return TX_SUCCESS;
}

/* The module's own thread, kept rather than started. */
UINT _txe_thread_create(TX_THREAD *thread_ptr, CHAR *name,
                        VOID (*entry_function)(ULONG), ULONG entry_input,
                        VOID *stack_start, ULONG stack_size, UINT priority,
                        UINT preempt_threshold, ULONG time_slice,
                        UINT auto_start, UINT size)
{
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(stack_start);
    NX_PARAMETER_NOT_USED(stack_size);
    NX_PARAMETER_NOT_USED(priority);
    NX_PARAMETER_NOT_USED(preempt_threshold);
    NX_PARAMETER_NOT_USED(time_slice);
    NX_PARAMETER_NOT_USED(auto_start);
    NX_PARAMETER_NOT_USED(size);

    memset(thread_ptr, 0, sizeof(*thread_ptr));

    fz_thread_entry = entry_function;
    fz_thread_input = entry_input;

    return TX_SUCCESS;
}

UINT _txe_thread_delete(TX_THREAD *t)
{
    NX_PARAMETER_NOT_USED(t);
    return TX_SUCCESS;
}

UINT _txe_thread_terminate(TX_THREAD *t)
{
    NX_PARAMETER_NOT_USED(t);
    return TX_SUCCESS;
}

UINT _txe_timer_create(TX_TIMER *timer_ptr, CHAR *name,
                       VOID (*expiration_function)(ULONG), ULONG input,
                       ULONG initial_ticks, ULONG reschedule_ticks,
                       UINT auto_activate, UINT size)
{
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(expiration_function);
    NX_PARAMETER_NOT_USED(input);
    NX_PARAMETER_NOT_USED(initial_ticks);
    NX_PARAMETER_NOT_USED(reschedule_ticks);
    NX_PARAMETER_NOT_USED(auto_activate);
    NX_PARAMETER_NOT_USED(size);

    memset(timer_ptr, 0, sizeof(*timer_ptr));

    return TX_SUCCESS;
}

UINT _txe_timer_delete(TX_TIMER *t)
{
    NX_PARAMETER_NOT_USED(t);
    return TX_SUCCESS;
}

UINT _txe_timer_deactivate(TX_TIMER *t)
{
    NX_PARAMETER_NOT_USED(t);
    return TX_SUCCESS;
}

UINT _txe_timer_activate(TX_TIMER *t)
{
    NX_PARAMETER_NOT_USED(t);
    return TX_SUCCESS;
}

UINT _txe_timer_change(TX_TIMER *t, ULONG initial_ticks, ULONG reschedule_ticks)
{
    NX_PARAMETER_NOT_USED(t);
    NX_PARAMETER_NOT_USED(initial_ticks);
    NX_PARAMETER_NOT_USED(reschedule_ticks);
    return TX_SUCCESS;
}

/*
 * Reported inactive with nothing remaining.  _nx_mdns_timer_set() then makes
 * nx_mdns_timer_min_count equal to the record's own timer count, so one
 * NX_MDNS_TIMER_EVENT expires whatever was just scheduled -- which is how the
 * driver walks probing and announcing forward without a real timer.
 */
UINT _txe_timer_info_get(TX_TIMER *t, CHAR **name, UINT *active,
                         ULONG *remaining_ticks, ULONG *reschedule_ticks,
                         TX_TIMER **next_timer)
{
    NX_PARAMETER_NOT_USED(t);

    if (name != TX_NULL)
        *name = (CHAR *)"mdns";
    if (active != TX_NULL)
        *active = NX_FALSE;
    if (remaining_ticks != TX_NULL)
        *remaining_ticks = 0;
    if (reschedule_ticks != TX_NULL)
        *reschedule_ticks = 0;
    if (next_timer != TX_NULL)
        *next_timer = TX_NULL;

    return TX_SUCCESS;
}

UINT _txe_event_flags_create(TX_EVENT_FLAGS_GROUP *g, CHAR *name, UINT size)
{
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(size);

    memset(g, 0, sizeof(*g));

    return TX_SUCCESS;
}

UINT _txe_event_flags_delete(TX_EVENT_FLAGS_GROUP *g)
{
    NX_PARAMETER_NOT_USED(g);
    return TX_SUCCESS;
}

UINT _txe_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT option)
{
    NX_PARAMETER_NOT_USED(g);
    NX_PARAMETER_NOT_USED(option);

    fz_events_pending |= flags;

    return TX_SUCCESS;
}

/*
 * The top of the module's loop, and the way out of it.  A few iterations, not
 * one: processing a query only *schedules* the answer, so the send the module
 * decided on happens on a later pass.  The timer event is folded in from the
 * second pass onwards because the real one fires on its own.  The escape is
 * here, at the top of the loop, where nothing is allocated.
 */
#define FZ_PUMP_ITERATIONS  4

UINT _txe_event_flags_get(TX_EVENT_FLAGS_GROUP *g, ULONG requested,
                          UINT get_option, ULONG *actual, ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(g);
    NX_PARAMETER_NOT_USED(get_option);
    NX_PARAMETER_NOT_USED(wait_option);

    if (fz_event_calls >= FZ_PUMP_ITERATIONS && fz_escape_armed)
        longjmp(fz_escape, 1);

    if (fz_event_calls > 0)
        fz_events_pending |= NX_MDNS_TIMER_EVENT;

    fz_event_calls++;

    *actual           = fz_events_pending & requested;
    fz_events_pending = 0;

    return TX_SUCCESS;
}


/* --------------------------------------------------------------- UDP ----- */

UINT _nx_udp_socket_create(NX_IP *ip_ptr, NX_UDP_SOCKET *socket_ptr,
                           CHAR *name, ULONG type_of_service, ULONG fragment,
                           UINT time_to_live, ULONG queue_maximum)
{
    NX_PARAMETER_NOT_USED(name);
    NX_PARAMETER_NOT_USED(type_of_service);
    NX_PARAMETER_NOT_USED(fragment);
    NX_PARAMETER_NOT_USED(time_to_live);
    NX_PARAMETER_NOT_USED(queue_maximum);

    memset(socket_ptr, 0, sizeof(*socket_ptr));
    socket_ptr -> nx_udp_socket_ip_ptr = ip_ptr;
    socket_ptr -> nx_udp_socket_id     = NX_UDP_ID;

    return NX_SUCCESS;
}

UINT _nx_udp_socket_delete(NX_UDP_SOCKET *socket_ptr)
{
    socket_ptr -> nx_udp_socket_id = 0;
    return NX_SUCCESS;
}

UINT _nx_udp_socket_bind(NX_UDP_SOCKET *socket_ptr, UINT port,
                         ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(wait_option);

    socket_ptr -> nx_udp_socket_port       = port;
    socket_ptr -> nx_udp_socket_bound_next = socket_ptr;

    return NX_SUCCESS;
}

UINT _nx_udp_socket_unbind(NX_UDP_SOCKET *socket_ptr)
{
    socket_ptr -> nx_udp_socket_bound_next = NX_NULL;
    return NX_SUCCESS;
}

UINT _nx_udp_socket_receive_notify(NX_UDP_SOCKET *socket_ptr,
                                   VOID (*notify)(NX_UDP_SOCKET *))
{
    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(notify);
    return NX_SUCCESS;
}

/* Whatever the responder decided to say, counted and dropped. */
UINT _nx_udp_socket_source_send(NX_UDP_SOCKET *socket_ptr,
                                NX_PACKET *packet_ptr, ULONG ip_address,
                                UINT port, UINT address_index)
{
    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(ip_address);
    NX_PARAMETER_NOT_USED(port);
    NX_PARAMETER_NOT_USED(address_index);

    fz_sends++;
    _nx_packet_release(packet_ptr);

    return NX_SUCCESS;
}

UINT _nx_udp_socket_send(NX_UDP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                         ULONG ip_address, UINT port)
{
    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(ip_address);
    NX_PARAMETER_NOT_USED(port);

    fz_sends++;
    _nx_packet_release(packet_ptr);

    return NX_SUCCESS;
}

UINT _nxd_udp_socket_send(NX_UDP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                          NXD_ADDRESS *ip_address, UINT port)
{
    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(ip_address);
    NX_PARAMETER_NOT_USED(port);

    fz_sends++;
    _nx_packet_release(packet_ptr);

    return NX_SUCCESS;
}

UINT _nxd_udp_socket_source_send(NX_UDP_SOCKET *socket_ptr,
                                 NX_PACKET *packet_ptr,
                                 NXD_ADDRESS *ip_address, UINT port,
                                 UINT address_index)
{
    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(ip_address);
    NX_PARAMETER_NOT_USED(port);
    NX_PARAMETER_NOT_USED(address_index);

    fz_sends++;
    _nx_packet_release(packet_ptr);

    return NX_SUCCESS;
}

/*
 * The wire.  One datagram per wake-up, then an empty queue.
 *
 * NX_MDNS_ENABLE_ADDRESS_CHECK is on by default in nxd_mdns.h, so the module
 * reads the IP header behind the datagram and the UDP header in front of it
 * before it will look at a byte of the message: RFC 6762 §6 requires the
 * source port to be 5353 and §11 requires the sender to be on-link.  Both
 * headers are supplied here, in the host byte order the IP and UDP receive
 * paths leave them in.
 *
 * The IP header is a separate object rather than bytes in front of
 * nx_packet_prepend_ptr, because NX_IPV4_HEADER's fields are ULONG -- eight
 * bytes on this host, four in the space NetX Duo leaves in a packet.
 */
static NX_IPV4_HEADER   fz_wire_ip_header;
static ULONG            fz_wire_src = IP_ADDRESS(169, 254, 9, 9);
static ULONG            fz_wire_dst = IP_ADDRESS(224, 0, 0, 251);
static UINT             fz_wire_sport = NX_MDNS_UDP_PORT;

UINT _nx_udp_socket_receive(NX_UDP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                            ULONG wait_option)
{
    NX_PACKET *p;

    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(wait_option);

    if (fz_delivered)
        return NX_NO_PACKET;

    if (_nx_packet_allocate(&fz_wire_pool, &p, NX_UDP_PACKET, NX_NO_WAIT) !=
        NX_SUCCESS)
        return NX_NO_PACKET;

    if (fz_case.len > 0)
        memcpy(p -> nx_packet_prepend_ptr, fz_case.b, fz_case.len);

    memset(&fz_wire_ip_header, 0, sizeof(fz_wire_ip_header));
    fz_wire_ip_header.nx_ip_header_source_ip      = fz_wire_src;
    fz_wire_ip_header.nx_ip_header_destination_ip = fz_wire_dst;

    /* Source port in the top half of the first UDP word, destination in the
       bottom, as _nx_udp_packet_receive() leaves it. */
    *(UINT *)(p -> nx_packet_prepend_ptr - 8) =
        (UINT)((fz_wire_sport << 16) | NX_MDNS_UDP_PORT);

    p -> nx_packet_append_ptr   = p -> nx_packet_prepend_ptr + fz_case.len;
    p -> nx_packet_length       = (ULONG)fz_case.len;
    p -> nx_packet_ip_version   = NX_IP_VERSION_V4;
    p -> nx_packet_ip_interface = &fz_ip.nx_ip_interface[0];
    p -> nx_packet_ip_header    = (UCHAR *)&fz_wire_ip_header;
    p -> nx_packet_next         = NX_NULL;
    p -> nx_packet_last         = NX_NULL;

    fz_delivered = 1;
    *packet_ptr  = p;

    return NX_SUCCESS;
}


/* -------------------------------------------------------------- IP/IGMP -- */

UINT _nx_ipv4_multicast_interface_join(NX_IP *ip_ptr, ULONG group_address,
                                       UINT interface_index)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(group_address);
    NX_PARAMETER_NOT_USED(interface_index);
    return NX_SUCCESS;
}

UINT _nx_ipv4_multicast_interface_leave(NX_IP *ip_ptr, ULONG group_address,
                                        UINT interface_index)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(group_address);
    NX_PARAMETER_NOT_USED(interface_index);
    return NX_SUCCESS;
}

VOID _nx_ip_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    _nx_packet_release(packet_ptr);
}


/* ------------------------------------------------------- mDNS-only seeds -- */

/*
 * The shapes that only mean something on 5353: a query rather than a response,
 * the unicast-response and cache-flush bits, known-answer suppression, a probe,
 * a goodbye, and the service records a browser would send.
 */

static void fzm_query_a(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, 0, 1, 0, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
}

static void fzm_query_any(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, 0, 1, 0, 0, 0);
    fzw_question(w, qname, FZW_TYPE_ANY, FZW_CLASS_IN);
}

/* The QU bit: answer this one by unicast (RFC 6762 §5.4). */
static void fzm_query_unicast(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, 0, 1, 0, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_QU);
}

static void fzm_query_many(FzwBuf *w, const char *qname)
{
    int i;

    fzw_hdr(w, 0, 0, 6, 0, 0, 0);
    for (i = 0; i < 6; i++)
        fzw_question(w, qname, (unsigned)(i + 1), FZW_CLASS_IN);
}

/* A question whose name is a compression pointer, which is not legal in a
   question but arrives anyway. */
static void fzm_query_compressed(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, 0, 2, 0, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
}

/* The A records below carry 169.254.1.2, and the high first octet is load
 * bearing: a byte >= 128 is what makes NX_MDNS_GET_ULONG_DATA's shift overflow
 * reachable (netxduo 6baec373). Reverting that patch fails `fuzz_mdns -s` on
 * this seed. A 10.x address here would still parse and would test less. */

/* Somebody else answering to this machine's name: the conflict path. */
static void fzm_conflict(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 0, 1, 0, 0);
    fzw_name(w, qname);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_FLUSH);
    fzw_u32(w, 120);
    fzw_u16(w, 4);
    fzw_u32(w, 0xA9FE0102UL);
}

/* The same, with a TTL of zero: the goodbye of RFC 6762 §10.1. */
static void fzm_goodbye(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 0, 1, 0, 0);
    fzw_name(w, qname);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_FLUSH);
    fzw_u32(w, 0);
    fzw_u16(w, 4);
    fzw_u32(w, 0xA9FE0102UL);
}

/* A query carrying its own answer, which suppresses ours. */
static void fzm_known_answer(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, 0, 1, 1, 0, 0);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 120);
    fzw_u16(w, 4);
    fzw_u32(w, 0xA9FE0304UL);
}

/* A probe: a query with the proposed record in the authority section. */
static void fzm_probe(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, 0, 1, 0, 1, 0);
    fzw_question(w, qname, FZW_TYPE_ANY, FZW_CLASS_IN);
    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 120);
    fzw_u16(w, 4);
    fzw_u32(w, 0xA9FE0506UL);
}

/* A service browse and the records that answer one. */
static void fzm_service_ptr(FzwBuf *w, const char *qname)
{
    size_t at;

    NX_PARAMETER_NOT_USED(qname);
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 0, 1, 0, 0);
    fzw_name(w, "_http._tcp.local");
    fzw_u16(w, FZW_TYPE_PTR);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 4500);
    at = w->len;
    fzw_u16(w, 0);
    fzw_name(w, "amiga._http._tcp.local");
    fzw_patch16(w, at, (unsigned)(w->len - at - 2));
}

static void fzm_service_srv_txt(FzwBuf *w, const char *qname)
{
    size_t at;

    NX_PARAMETER_NOT_USED(qname);
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 0, 2, 0, 1);

    fzw_name(w, "amiga._http._tcp.local");
    fzw_u16(w, FZW_TYPE_SRV);
    fzw_u16(w, FZW_CLASS_FLUSH);
    fzw_u32(w, 120);
    at = w->len;
    fzw_u16(w, 0);
    fzw_u16(w, 0); fzw_u16(w, 0); fzw_u16(w, 80);
    fzw_name(w, "amiga.local");
    fzw_patch16(w, at, (unsigned)(w->len - at - 2));

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_TXT);
    fzw_u16(w, FZW_CLASS_FLUSH);
    fzw_u32(w, 120);
    fzw_u16(w, 10);
    fzw_u8(w, 9);
    fzw_raw(w, "path=/amp", 9);

    fzw_name(w, "amiga.local");
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_FLUSH);
    fzw_u32(w, 120);
    fzw_u16(w, 4);
    fzw_u32(w, 0xA9FE0304UL);
}

/* An NSEC record, which is how a responder says a name has no other types. */
static void fzm_nsec(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR | FZW_AA, 0, 1, 0, 0);
    fzw_name(w, qname);
    fzw_u16(w, FZW_TYPE_NSEC);
    fzw_u16(w, FZW_CLASS_FLUSH);
    fzw_u32(w, 120);
    fzw_u16(w, 5);
    fzw_ptr(w, 12);
    fzw_u8(w, 0);
    fzw_u8(w, 1);
    fzw_u8(w, 0x40);
}

/* A label at exactly the 63-byte limit, and the name that is one byte over. */
static void fzm_label_63(FzwBuf *w, const char *qname)
{
    int i;

    NX_PARAMETER_NOT_USED(qname);
    fzw_hdr(w, 0, 0, 1, 0, 0, 0);
    fzw_u8(w, 63);
    for (i = 0; i < 63; i++)
        fzw_u8(w, 'a' + (unsigned)(i % 26));
    fzw_u8(w, 5);
    fzw_raw(w, "local", 5);
    fzw_u8(w, 0);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
}

/* A multi-label name under .local, which is not a host name. */
static void fzm_deep_local(FzwBuf *w, const char *qname)
{
    NX_PARAMETER_NOT_USED(qname);
    fzw_hdr(w, 0, 0, 1, 0, 0, 0);
    fzw_question(w, "a.b.c.d.e.local", FZW_TYPE_A, FZW_CLASS_IN);
}

/* A name that is not in .local at all, which the module must ignore. */
static void fzm_not_local(FzwBuf *w, const char *qname)
{
    NX_PARAMETER_NOT_USED(qname);
    fzw_hdr(w, 0, 0, 1, 0, 0, 0);
    fzw_question(w, "amiga.example.com", FZW_TYPE_A, FZW_CLASS_IN);
}

/* ".local" with an empty first label. */
static void fzm_bare_local(FzwBuf *w, const char *qname)
{
    NX_PARAMETER_NOT_USED(qname);
    fzw_hdr(w, 0, 0, 1, 0, 0, 0);
    fzw_u8(w, 0);
    fzw_u8(w, 5);
    fzw_raw(w, "local", 5);
    fzw_u8(w, 0);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
}

/* A query and a response in one datagram, which RFC 6762 §6 says to ignore
   the question half of. */
static void fzm_query_and_answer(FzwBuf *w, const char *qname)
{
    fzw_hdr(w, 0, FZW_QR, 2, 2, 1, 1);
    fzw_question(w, qname, FZW_TYPE_A, FZW_CLASS_IN);
    fzw_question(w, qname, FZW_TYPE_ANY, FZW_CLASS_QU);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_FLUSH);
    fzw_u32(w, 120);
    fzw_u16(w, 4);
    fzw_u32(w, 0xA9FE0708UL);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_AAAA);
    fzw_u16(w, FZW_CLASS_FLUSH);
    fzw_u32(w, 120);
    fzw_u16(w, 16);
    fzw_u32(w, 0xFE800000UL);
    fzw_u32(w, 0);
    fzw_u32(w, 0);
    fzw_u32(w, 1);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_A);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 0);
    fzw_u16(w, 4);
    fzw_u32(w, 0);

    fzw_ptr(w, 12);
    fzw_u16(w, FZW_TYPE_TXT);
    fzw_u16(w, FZW_CLASS_IN);
    fzw_u32(w, 0);
    fzw_u16(w, 1);
    fzw_u8(w, 0);
}

/* Sixty questions, so the response builder runs the record set repeatedly. */
static void fzm_question_flood(FzwBuf *w, const char *qname)
{
    int i;

    fzw_hdr(w, 0, 0, 60, 0, 0, 0);
    for (i = 0; i < 60; i++)
    {
        fzw_ptr(w, 12);
        fzw_u16(w, FZW_TYPE_ANY);
        fzw_u16(w, FZW_CLASS_IN);
    }
    NX_PARAMETER_NOT_USED(qname);
}

static const FzwSeed fzm_seeds[] =
{
    { "mdns_query_a",           fzm_query_a           },
    { "mdns_query_any",         fzm_query_any          },
    { "mdns_query_unicast",     fzm_query_unicast      },
    { "mdns_query_many",        fzm_query_many         },
    { "mdns_query_compressed",  fzm_query_compressed   },
    { "mdns_conflict",          fzm_conflict           },
    { "mdns_goodbye",           fzm_goodbye            },
    { "mdns_known_answer",      fzm_known_answer       },
    { "mdns_probe",             fzm_probe              },
    { "mdns_service_ptr",       fzm_service_ptr        },
    { "mdns_service_srv_txt",   fzm_service_srv_txt    },
    { "mdns_nsec",              fzm_nsec               },
    { "mdns_label_63",          fzm_label_63           },
    { "mdns_deep_local",        fzm_deep_local         },
    { "mdns_not_local",         fzm_not_local          },
    { "mdns_bare_local",        fzm_bare_local         },
    { "mdns_query_and_answer",  fzm_query_and_answer   },
    { "mdns_question_flood",    fzm_question_flood     }
};

#define FZM_SEED_COUNT  (int)(sizeof(fzm_seeds) / sizeof(fzm_seeds[0]))
#define FZ_TOTAL_SEEDS  (FZW_SEED_COUNT + FZM_SEED_COUNT)

static const FzwSeed *fz_seed(int i)
{
    return (i < FZW_SEED_COUNT) ? &fzw_seeds[i]
                                : &fzm_seeds[i - FZW_SEED_COUNT];
}


/* --------------------------------------------------------- the drives ---- */

static void fz_fail(const char *what)
{
    printf("fuzz_mdns: %s (case '%s', %lu cases in)\n", what, fz_case_name,
           fz_cases);
    fflush(stdout);
    abort();
}

static void fz_pool_check(void)
{
    if (fz_wire_pool.nx_packet_pool_available !=
        fz_wire_pool.nx_packet_pool_total)
        fz_fail("a received datagram was not released");

    if (fz_pool.nx_packet_pool_available != fz_pool.nx_packet_pool_total)
        fz_fail("a response packet was not released");
}

/*
 * Run one iteration of the module's own thread with `events` pending.  The
 * longjmp comes from the wait at the top of the second iteration, so the first
 * one has finished and released everything it took.
 */
static void fz_pump(ULONG events)
{
    fz_events_pending |= events;
    fz_event_calls     = 0;

    if (setjmp(fz_escape) == 0)
    {
        fz_escape_armed = 1;
        fz_thread_entry(fz_thread_input);
    }

    fz_escape_armed = 0;
}

static void fz_start(void)
{
    UINT status;

    memset(fz_local_cache, 0, FZ_LOCAL_CACHE_BYTES);
    memset(fz_peer_cache, 0, FZ_PEER_CACHE_BYTES);

    status = nx_mdns_create(&fz_mdns, &fz_ip, &fz_pool, 4,
                            fz_stack, (ULONG)sizeof(fz_stack),
                            (UCHAR *)FZ_LABEL,
                            fz_local_cache, (UINT)FZ_LOCAL_CACHE_BYTES,
                            fz_peer_cache, (UINT)FZ_PEER_CACHE_BYTES,
                            NX_NULL);
    if (status != NX_SUCCESS)
        fz_fail("nx_mdns_create failed");

    if (fz_thread_entry == NX_NULL)
        fz_fail("the module did not register a thread to drive");

    if (nx_mdns_enable(&fz_mdns, 0) != NX_SUCCESS)
        fz_fail("nx_mdns_enable failed");

    /*
     * Probing, three rounds of it (RFC 6762 §9), so the record set reaches the
     * state where a query gets answered rather than queued.
     */
    fz_delivered = 1;
    fz_pump(NX_MDNS_PROBING_SEND_EVENT | NX_MDNS_ANNOUNCING_SEND_EVENT |
            NX_MDNS_TIMER_EVENT);
    fz_pump(NX_MDNS_PROBING_SEND_EVENT | NX_MDNS_TIMER_EVENT);
    fz_pump(NX_MDNS_ANNOUNCING_SEND_EVENT | NX_MDNS_TIMER_EVENT);
}

static void fz_stop(void)
{
    (VOID)nx_mdns_disable(&fz_mdns, 0);
    (VOID)nx_mdns_delete(&fz_mdns);
    fz_thread_entry = NX_NULL;
}

/*
 * One datagram.  The module is created and deleted around it so a case cannot
 * be shielded by state the previous one left in the caches -- and so the
 * caches are exercised from empty every time, which is the state a machine is
 * in when it has just booted onto a hostile segment.
 */
static void fz_run_once(const unsigned char *data, size_t len)
{
    if (len > FZW_MAX)
        len = FZW_MAX;

    memcpy(fz_case.b, data, len);
    fz_case.len = len;

    fz_start();

    fz_delivered = 0;
    fz_pump(NX_MDNS_PKT_RX_EVENT);

    /* And again, so the second copy meets the cache the first one filled --
       which is where duplicate suppression and conflict handling live. */
    fz_delivered = 0;
    fz_pump(NX_MDNS_PKT_RX_EVENT | NX_MDNS_TIMER_EVENT);

    fz_pool_check();
    fz_stop();

    fz_cases++;
}

static void fz_run_seed(int which)
{
    FzwBuf w;

    fzw_reset(&w);
    fz_seed(which) -> build(&w, FZ_LOCAL);

    fz_case_name = fz_seed(which) -> name;

    fz_run_once(w.b, w.len);
}

/*
 * The harness has to be shown to reach the responder, not merely to survive
 * it.  Two things are checked, and the second is the one that matters: a
 * response claiming this machine's own name has to make the module give the
 * name up and start probing for another.  Nothing but parsing the datagram and
 * acting on its contents produces that.
 */
static void fz_selftest(void)
{
    FzwBuf w;
    ULONG  before;
    char   claimed[NX_MDNS_HOST_NAME_MAX + 1];

    fz_case_name = "selftest";

    fzw_reset(&w);
    fzm_conflict(&w, FZ_LOCAL);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len = w.len;

    fz_start();

    /* The A record and the NSEC that goes with it. */
    if (fz_mdns.nx_mdns_local_rr_count != 2)
        fz_fail("the responder did not register its own records");

    memset(claimed, 0, sizeof(claimed));
    memcpy(claimed, fz_mdns.nx_mdns_host_name, sizeof(claimed) - 1);

    before       = fz_sends;
    fz_delivered = 0;
    fz_pump(NX_MDNS_PKT_RX_EVENT);

    if (fz_sends == before)
        fz_fail("a response claiming this machine's name provoked nothing");

    if (strcmp(claimed, (const char *)fz_mdns.nx_mdns_host_name) == 0)
        fz_fail("the name was contested and the module kept it");

    fz_pool_check();
    fz_stop();
}


/* --------------------------------------------------------------- setup --- */

static void fz_setup(void)
{
    NX_INTERFACE *ifp = &fz_ip.nx_ip_interface[0];

    memset(&fz_ip, 0, sizeof(fz_ip));

    ifp -> nx_interface_valid            = NX_TRUE;
    ifp -> nx_interface_name             = "eth0";
    ifp -> nx_interface_link_up          = NX_TRUE;
    ifp -> nx_interface_ip_address       = FZ_OUR_IP;
    ifp -> nx_interface_ip_network_mask  = FZ_NETMASK;
    ifp -> nx_interface_ip_network       = FZ_OUR_IP & FZ_NETMASK;
    ifp -> nx_interface_ip_mtu_size      = 1500;
    ifp -> nx_interface_index            = 0;

    fz_ip.nx_ip_id                  = NX_IP_ID;
    fz_ip.nx_ip_default_packet_pool = &fz_pool;

    fz_local_cache = (ULONG *)malloc(FZ_LOCAL_CACHE_BYTES);
    fz_peer_cache  = (ULONG *)malloc(FZ_PEER_CACHE_BYTES);

    if (fz_local_cache == NULL || fz_peer_cache == NULL)
    {
        printf("fuzz_mdns: no memory for the caches\n");
        exit(1);
    }

    if (_nx_packet_pool_create(&fz_pool, "mdns", FZ_POOL_PAYLOAD,
                               (VOID *)fz_pool_area,
                               (ULONG)sizeof(fz_pool_area)) != NX_SUCCESS ||
        _nx_packet_pool_create(&fz_wire_pool, "wire", FZ_WIRE_PAYLOAD,
                               (VOID *)fz_wire_area,
                               (ULONG)sizeof(fz_wire_area)) != NX_SUCCESS)
    {
        printf("fuzz_mdns: the packet pools would not create\n");
        exit(1);
    }
}


int main(int argc, char **argv)
{
    int i;

    fz_setup();
    fz_selftest();

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0)
        {
            int s;

            for (s = 0; s < FZ_TOTAL_SEEDS; s++)
            {
                fz_run_seed(s);
                printf("  %-24s ok\n", fz_seed(s) -> name);
            }

            printf("fuzz_mdns: %d seed case(s), clean\n", FZ_TOTAL_SEEDS);
            return 0;
        }

        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            const char *want = argv[++i];
            int         s;

            for (s = 0; s < FZ_TOTAL_SEEDS; s++)
            {
                if (strcmp(fz_seed(s) -> name, want) == 0)
                {
                    fz_run_seed(s);
                    printf("fuzz_mdns: seed '%s', clean\n", want);
                    return 0;
                }
            }

            printf("fuzz_mdns: no seed case named '%s'\n", want);
            return 2;
        }

        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;
            FzwBuf        w;
            int           s;

            for (s = 0; s < FZ_TOTAL_SEEDS; s++)
                fz_run_seed(s);

            fzw_state = seed;

            for (n = 0; n < count; n++)
            {
                s = (int)fzw_below((unsigned)FZ_TOTAL_SEEDS);

                fzw_reset(&w);
                fz_seed(s) -> build(&w, FZ_LOCAL);
                fzw_mutate(&w);

                fz_case_name = fz_seed(s) -> name;

                fz_run_once(w.b, w.len);
            }

            printf("fuzz_mdns: %d seed(s) + %lu mutation(s) from seed %lu, "
                   "%lu datagram(s) parsed, clean\n", FZ_TOTAL_SEEDS, count,
                   seed, fz_cases);
            return 0;
        }
    }

    {
        static unsigned char buf[FZW_MAX];
        size_t               len = fread(buf, 1, sizeof(buf), stdin);

        fz_run_once(buf, len);
    }

    printf("fuzz_mdns: one datagram, clean\n");

    return 0;
}
