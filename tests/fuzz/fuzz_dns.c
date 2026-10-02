/*
 * AmiNetXDuo, host fuzz driver for the DNS response parser.
 *
 * Usage:
 *   fuzz_dns -s                  every seed case, named
 *   fuzz_dns -c NAME             one seed case by name
 *   fuzz_dns < datagram          one datagram from stdin
 *   fuzz_dns -r SEED COUNT       seeds plus mutations, no corpus needed
 *   fuzz_dns -t cache_drop       a server removal, then an insert (N-056)
 *   fuzz_dns -t ptr_owner        the PTR record's owner name (N-058)
 *   fuzz_dns -t cache_aaaa       a cached AAAA with zero octets, freed (N-057),
 *                                and AAAA identity in the string table
 *   fuzz_dns -t cache_aaaa_holes AAAA slots with zero holes, and binary
 *                                entries kept apart from name entries
 *   fuzz_dns -t cache_aaaa_interior  a freed interior :: slot, alone
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_udp.h"
#include "nx_packet.h"
#include "nxd_dns.h"

#include "fuzz_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The name every query asks for, so a seed can match the question section. */
#define FZ_QNAME        "test.example.com"
#define FZ_SERVER       IP_ADDRESS(10, 0, 0, 1)
#define FZ_OUR_IP       IP_ADDRESS(10, 0, 0, 17)
#define FZ_NETMASK      IP_ADDRESS(255, 255, 255, 0)

static NX_IP            fz_ip;
static NX_DNS           fz_dns;

/* A pool of its own for the datagrams that arrive: the DNS client's internal
   pool carries 512-byte payloads and a sender is not held to that. */
#define FZ_WIRE_PAYLOAD (FZW_MAX + 64)
#define FZ_WIRE_PACKETS 8
/* _Alignas because the pool is carved into NX_PACKETs and this is a HOST build:
   NX_PACKET holds 64-bit pointers and wants 8-byte alignment while the shim's
   ULONG is 32 bits.  On m68k the requirement is 4 and it does not arise. */
static _Alignas(NX_PACKET) ULONG
                        fz_wire_area[((FZ_WIRE_PAYLOAD + sizeof(NX_PACKET) +
                                      32) * FZ_WIRE_PACKETS) / sizeof(ULONG)];
static NX_PACKET_POOL   fz_wire_pool;

/* The client has no pool of its own any more: NX_DNS_CLIENT_USER_CREATE_PACKET_POOL
   means nx_dns_create() leaves nx_dns_packet_pool_ptr NULL and the caller sets
   it.  Kept SEPARATE from fz_wire_pool so the two leak messages below stay
   distinct -- a query packet that is never released should not read as an
   unreleased datagram.  NX_DNS_PACKET_PAYLOAD is what
   _nx_dns_packet_pool_set() checks against. */
#define FZ_QUERY_PACKETS 8
static _Alignas(NX_PACKET) ULONG
                        fz_query_area[((NX_DNS_PACKET_PAYLOAD + sizeof(NX_PACKET) +
                                      32) * FZ_QUERY_PACKETS) / sizeof(ULONG)];
static NX_PACKET_POOL   fz_query_pool;

/* Big enough that the cache path runs rather than failing to insert. */
static ULONG            fz_cache[512];

/* The datagram under test, and whether it has been handed over yet. */
static FzwBuf           fz_case;
static int              fz_delivered;
static int              fz_patch_id;    /* make the header ID match the query */

static unsigned long    fz_cases;
static const char      *fz_case_name = "stdin";


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

/* The mutex, through the checking wrappers the addon actually calls: only
   NetX Duo's error checking is disabled inside nxd_dns.c, not ThreadX's. */
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

UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT option)
{
    NX_PARAMETER_NOT_USED(g);
    NX_PARAMETER_NOT_USED(flags);
    NX_PARAMETER_NOT_USED(option);
    return TX_SUCCESS;
}

/*
 * Time has to advance.  _nx_dns_response_receive() decides when to give up by
 * subtracting tx_time_get() from the time it started, so a clock that stands
 * still turns "no more packets" into an endless loop.
 */
static ULONG fz_ticks;

ULONG _tx_time_get(VOID)
{
    return fz_ticks++;
}


/* --------------------------------------------------------------- UDP ----- */

/*
 * The socket, in as much detail as the DNS client can tell.  The internal
 * names, not the _nxe_ ones: nxd_dns.c defines NX_DISABLE_ERROR_CHECKING for
 * itself, so it calls straight through.
 */

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

    socket_ptr -> nx_udp_socket_port       = (port == NX_ANY_PORT) ? 49152 : port;
    socket_ptr -> nx_udp_socket_bound_next = socket_ptr;

    return NX_SUCCESS;
}

UINT _nx_udp_socket_unbind(NX_UDP_SOCKET *socket_ptr)
{
    socket_ptr -> nx_udp_socket_bound_next = NX_NULL;
    return NX_SUCCESS;
}

/* The query goes nowhere.  It still has to be released, or a leaked query
   packet would be reported as a leak in the parser. */
UINT _nxd_udp_socket_send(NX_UDP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                          NXD_ADDRESS *ip_address, UINT port)
{
    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(ip_address);
    NX_PARAMETER_NOT_USED(port);

    _nx_packet_release(packet_ptr);

    return NX_SUCCESS;
}

/* The IP header the stub hands up with each datagram; see the receive stub. */
static NX_IPV4_HEADER fz_ip_header;

/* Whether that header names the configured server.  Default yes. */
static int fz_from_server = 1;

UINT _nx_udp_socket_receive(NX_UDP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                            ULONG wait_option)
{
    NX_PACKET *p;
    UINT       status;

    NX_PARAMETER_NOT_USED(socket_ptr);
    NX_PARAMETER_NOT_USED(wait_option);

    if (fz_delivered)
        return NX_NO_PACKET;

    status = _nx_packet_allocate(&fz_wire_pool, &p, NX_UDP_PACKET, NX_NO_WAIT);
    if (status != NX_SUCCESS)
        return NX_NO_PACKET;

    if (fz_case.len > 0)
    {
        memcpy(p -> nx_packet_prepend_ptr, fz_case.b, fz_case.len);

        /* The query ID.  Patching it is what gets the datagram past the two ID
           gates and into the record loop; leaving it alone exercises the drop
           path, which is the other thing a hostile answer meets. */
        if (fz_patch_id && fz_case.len >= 2)
        {
            USHORT id = fz_dns.nx_dns_transmit_id;

            p -> nx_packet_prepend_ptr[0] = (UCHAR)(id >> 8);
            p -> nx_packet_prepend_ptr[1] = (UCHAR)id;
        }
    }

    p -> nx_packet_append_ptr    = p -> nx_packet_prepend_ptr + fz_case.len;
    p -> nx_packet_length        = (ULONG)fz_case.len;
    p -> nx_packet_ip_version    = NX_IP_VERSION_V4;
    p -> nx_packet_ip_interface  = &fz_ip.nx_ip_interface[0];

    /*
     * The client checks that an answer came from the server it asked, and the
     * real receive path fills nx_packet_ip_header in.  FZ_SERVER in host order,
     * which is the order that path leaves the header in.
     */
    fz_ip_header.nx_ip_header_source_ip =
        fz_from_server ? FZ_SERVER : IP_ADDRESS(10, 0, 0, 99);
    p -> nx_packet_ip_header = (UCHAR *)&fz_ip_header;

    /*
     * nxd_udp_source_extract() takes the UDP header from the longword eight
     * bytes before the payload, in host order: source port in the high half,
     * destination in the low.
     */
    {
        ULONG *udp = (ULONG *)(p -> nx_packet_prepend_ptr);

        *(udp - 2) = ((ULONG)NX_DNS_PORT << 16) | 0x0035UL;
    }
    p -> nx_packet_next          = NX_NULL;
    p -> nx_packet_last          = NX_NULL;

    fz_delivered = 1;
    *packet_ptr  = p;

    return NX_SUCCESS;
}

VOID _nx_ip_packet_deferred_receive(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    _nx_packet_release(packet_ptr);
}


/* --------------------------------------------------------- the drives ---- */

static void fz_fail(const char *what)
{
    printf("fuzz_dns: %s (case '%s', %lu cases in)\n", what, fz_case_name,
           fz_cases);
    fflush(stdout);
    abort();
}

static void fz_pool_check(void)
{
    if (fz_wire_pool.nx_packet_pool_available !=
        fz_wire_pool.nx_packet_pool_total)
        fz_fail("a received datagram was not released");

    if (fz_query_pool.nx_packet_pool_available !=
        fz_query_pool.nx_packet_pool_total)
        fz_fail("a query packet was not released");
}

/* One datagram, through everything the library exposes.  Each entry point is
   asked twice: the second call may be answered from the DNS cache. */
static void fz_run_once(const unsigned char *data, size_t len)
{
    UCHAR record[256];
    UCHAR name[NX_DNS_NAME_MAX + 1];
    ULONG address;
    UINT  count;
    int   pass;

    if (len > FZW_MAX)
        len = FZW_MAX;

    memcpy(fz_case.b, data, len);
    fz_case.len = len;

    memset(fz_cache, 0, sizeof(fz_cache));

    if (nx_dns_create(&fz_dns, &fz_ip, (UCHAR *)"example.com") != NX_SUCCESS)
        fz_fail("nx_dns_create failed");

    if (nx_dns_packet_pool_set(&fz_dns, &fz_query_pool) != NX_SUCCESS)
        fz_fail("the query pool was refused");

    (VOID)nx_dns_cache_initialize(&fz_dns, fz_cache, (UINT)sizeof(fz_cache));

    if (nx_dns_server_add(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("nx_dns_server_add failed");

    /* One try per server: the retry loop would only re-deliver the same
       datagram, and every case pays for it. */
    fz_dns.nx_dns_retries = 1;

    for (pass = 0; pass < 2; pass++)
    {
        fz_delivered = 0;
        address      = 0;
        (VOID)nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4);
    }

    for (pass = 0; pass < 2; pass++)
    {
        fz_delivered = 0;
        memset(name, 0, sizeof(name));
        (VOID)nx_dns_host_by_address_get(&fz_dns, IP_ADDRESS(10, 0, 0, 9),
                                         name, (UINT)sizeof(name), 4);

        /* A parser that reports a name must report a terminated one. */
        if (name[sizeof(name) - 1] != 0)
            fz_fail("the reverse lookup wrote past the name buffer's last byte");
    }

#ifdef FEATURE_NX_IPV6
    for (pass = 0; pass < 2; pass++)
    {
        fz_delivered = 0;
        count        = 0;
        memset(record, 0, sizeof(record));
        (VOID)nxd_dns_ipv6_address_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME,
                                               record, (UINT)sizeof(record),
                                               &count, 4);
    }
#else
    NX_PARAMETER_NOT_USED(record);
    NX_PARAMETER_NOT_USED(count);
#endif

    fz_pool_check();

    if (nx_dns_delete(&fz_dns) != NX_SUCCESS)
        fz_fail("nx_dns_delete failed");

    fz_cases++;
}

/* The harness has to be shown to reach the parser, or a sweep of malformed
   input proves only that the datagrams were thrown away. */
static void fz_selftest(void)
{
    FzwBuf w;
    ULONG  address = 0;
    UCHAR  name[NX_DNS_NAME_MAX + 1];

    fz_case_name = "selftest";
    fz_patch_id  = 1;

    if (nx_dns_create(&fz_dns, &fz_ip, (UCHAR *)"example.com") != NX_SUCCESS ||
        nx_dns_packet_pool_set(&fz_dns, &fz_query_pool) != NX_SUCCESS ||
        nx_dns_server_add(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("the DNS client would not start");

    fz_dns.nx_dns_retries = 1;

    fzw_reset(&w);
    fzs_a_answer(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len   = w.len;
    fz_delivered  = 0;

    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS)
        fz_fail("a well-formed A answer did not resolve");
    if (address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("a well-formed A answer resolved to the wrong address");

    /* The question echoed in the other case is the same question (RFC 4343),
       and resolvers do normalise before echoing. */
    fzw_reset(&w);
    fzs_question_upper(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len   = w.len;
    fz_delivered  = 0;
    address       = 0;

    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS)
        fz_fail("an answer echoing the question in capitals did not resolve");
    if (address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("an answer echoing the question in capitals gave the wrong "
                "address");

    /* An A record in the authority section is not an answer: nothing here ties
       its owner name to the question. */
    fzw_reset(&w);
    fzs_a_in_authority(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len   = w.len;
    fz_delivered  = 0;
    address       = 0;

    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) ==
        NX_SUCCESS)
        fz_fail("an A record in the authority section was taken as an answer");
    if (address != 0)
        fz_fail("an A record in the authority section wrote an address");

    /* And the same response with a real answer in front of it still resolves,
       to the answer and not to the authority record. */
    fzw_reset(&w);
    fzs_a_answer_plus_authority(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len   = w.len;
    fz_delivered  = 0;
    address       = 0;

    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS)
        fz_fail("an answer followed by an authority record did not resolve");
    if (address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("an authority record displaced the answer");

    fzw_reset(&w);
    fzs_ptr_answer_inaddr(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len  = w.len;
    fz_delivered = 0;
    memset(name, 0, sizeof(name));

    if (nx_dns_host_by_address_get(&fz_dns, IP_ADDRESS(10, 0, 0, 9), name,
                                   (UINT)sizeof(name), 4) != NX_SUCCESS)
        fz_fail("a well-formed PTR answer did not resolve");
    if (strcmp((const char *)name, "amiga.example.com") != 0)
        fz_fail("a well-formed PTR answer gave the wrong name");

    /* A PTR answer to a question about a different address.  The reverse path
       skipped the question section, so this used to answer the query. */
    fzw_reset(&w);
    fzs_ptr_wrong_question(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len  = w.len;
    fz_delivered = 0;
    memset(name, 0, sizeof(name));

    if (nx_dns_host_by_address_get(&fz_dns, IP_ADDRESS(10, 0, 0, 9), name,
                                   (UINT)sizeof(name), 4) == NX_SUCCESS)
        fz_fail("a PTR answer to another address's question was accepted");
    if (name[0] != 0)
        fz_fail("a rejected PTR answer left a name behind");

#ifdef FEATURE_NX_IPV6
    {
        NX_DNS_IPV6_ADDRESS answer[2];
        UINT                count = 0;

        fzw_reset(&w);
        fzs_aaaa_answer(&w, FZ_QNAME);
        memcpy(fz_case.b, w.b, w.len);
        fz_case.len  = w.len;
        fz_delivered = 0;
        memset(answer, 0, sizeof(answer));

        if (nxd_dns_ipv6_address_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME,
                                             answer, (UINT)sizeof(answer),
                                             &count, 4) != NX_SUCCESS)
            fz_fail("a well-formed AAAA answer did not resolve");
        if (count != 1 || answer[0].ipv6_address[0] != 0x00010203UL)
            fz_fail("a well-formed AAAA answer gave the wrong address");
    }
#endif

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
}

/*
 * A cached record has to expire even when it is asked for constantly: cache an
 * answer with a two-second TTL, then ask for it repeatedly with the clock
 * stepping less than a second and nothing further available from the wire.
 */
static void fz_cache_expiry_test(void)
{
    FzwBuf w;
    ULONG  address = 0;
    int    step;
    int    expired = 0;

    fz_case_name = "cache_expiry";
    fz_patch_id  = 1;

    memset(fz_cache, 0, sizeof(fz_cache));

    if (nx_dns_create(&fz_dns, &fz_ip, (UCHAR *)"example.com") != NX_SUCCESS ||
        nx_dns_packet_pool_set(&fz_dns, &fz_query_pool) != NX_SUCCESS ||
        nx_dns_cache_initialize(&fz_dns, fz_cache,
                                (UINT)sizeof(fz_cache)) != NX_SUCCESS ||
        nx_dns_server_add(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("the DNS client would not start");

    fz_dns.nx_dns_retries = 1;

    /* One answer off the wire, TTL two seconds, and then the wire goes quiet. */
    fzw_reset(&w);
    fzs_a_answer(&w, FZ_QNAME);
    /* The record's tail is TTL(4) RDLENGTH(2) RDATA(4), so the TTL starts ten
       bytes from the end. */
    fzw_patch16(&w, w.len - 10, 0);
    fzw_patch16(&w, w.len - 8, 2);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len  = w.len;
    fz_delivered = 0;

    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS)
        fz_fail("the answer that seeds the cache did not resolve");

    /* Nothing more arrives, so from here a success is a cache hit. */
    fz_delivered = 1;

    address = 0;
    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS)
        fz_fail("the record was not cached at all, so this proves nothing");
    if (address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("the cache returned the wrong address");

    /* NX_IP_PERIODIC_RATE is 50 here, so 20 ticks a step is well under the
       one second the old arithmetic needed before it charged anything. */
    for (step = 0; step < 60; step++)
    {
        fz_ticks += 20;

        address = 0;
        if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
            NX_SUCCESS)
        {
            expired = 1;
            break;
        }
    }

    if (!expired)
        fz_fail("a record with a two-second TTL never expired while it was "
                "being looked up");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
}

/* A fresh client with an empty cache and one server, for the contract tests. */
static void fz_contract_start(const char *name)
{
    fz_case_name = name;
    fz_patch_id  = 1;

    memset(fz_cache, 0, sizeof(fz_cache));

    if (nx_dns_create(&fz_dns, &fz_ip, (UCHAR *)"example.com") != NX_SUCCESS ||
        nx_dns_packet_pool_set(&fz_dns, &fz_query_pool) != NX_SUCCESS ||
        nx_dns_cache_initialize(&fz_dns, fz_cache,
                                (UINT)sizeof(fz_cache)) != NX_SUCCESS ||
        nx_dns_server_add(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("the DNS client would not start");

    fz_dns.nx_dns_retries = 1;
}

static void fz_contract_wire(FzwSeedFn build)
{
    FzwBuf w;

    fzw_reset(&w);
    build(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len  = w.len;
    fz_delivered = 0;
}

/*
 * N-056.  A server removal drops the cache, and the drop zeroed the head and
 * tail words _nx_dns_cache_initialize() sets, so the next insert walked the
 * string table with a zero tail and never left the loop.  The words are read
 * BEFORE anything inserts, so the unfixed client fails here rather than
 * hanging; the alarm in main() is the backstop.
 */
static void fz_cache_drop_test(void)
{
    ALIGN_TYPE *head_word = (ALIGN_TYPE *)fz_cache;
    ALIGN_TYPE *tail_word = (ALIGN_TYPE *)((UCHAR *)fz_cache +
                                           sizeof(fz_cache)) - 1;
    UCHAR      *head;
    UCHAR      *tail;
    ULONG       address = 0;

    fz_contract_start("cache_drop");

    fz_contract_wire(fzs_a_answer);
    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS)
        fz_fail("the answer that seeds the cache did not resolve");
    if (fz_dns.nx_dns_rr_count == 0)
        fz_fail("the answer was not cached, so the drop drops nothing");

    if (nx_dns_server_remove(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("nx_dns_server_remove failed");
    if (fz_dns.nx_dns_rr_count != 0)
        fz_fail("the server removal left records in the cache");

    if (*head_word != (ALIGN_TYPE)(head_word + 1))
        fz_fail("the dropped cache's head word is not the initial head "
                "(the next insert would never return)");
    if (*tail_word != (ALIGN_TYPE)tail_word)
        fz_fail("the dropped cache's tail word is not the initial tail "
                "(the next insert would never return)");

    /* The insert that used to spin. */
    if (nx_dns_server_add(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("nx_dns_server_add after the removal failed");

    fz_contract_wire(fzs_a_answer);
    address = 0;
    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS || address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("the lookup after the removal did not resolve");
    if (fz_dns.nx_dns_rr_count != 1)
        fz_fail("the lookup after the removal was not cached");

    /* The insert moved head up and tail down, and they have not crossed. */
    head = (UCHAR *)*head_word;
    tail = (UCHAR *)*tail_word;
    if (head <= (UCHAR *)(head_word + 1) || tail >= (UCHAR *)tail_word ||
        head > tail)
        fz_fail("the insert after the removal left head and tail wrong");

    /* And the record is found: nothing more comes off the wire. */
    fz_delivered = 1;
    address      = 0;
    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS || address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("the record inserted after the removal was not found");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
}

/* One reverse lookup of 10.0.0.9 against one crafted answer.  `want` NULL is
   a rejection: no name, and nothing cached.  Otherwise the name, and then the
   same name again from the cache with the wire silent. */
static void fz_ptr_case(const char *name, FzwSeedFn build, const char *want)
{
    UCHAR host[NX_DNS_NAME_MAX + 1];
    UINT  status;

    fz_contract_start(name);
    fz_contract_wire(build);
    memset(host, 0, sizeof(host));

    status = nx_dns_host_by_address_get(&fz_dns, IP_ADDRESS(10, 0, 0, 9),
                                        host, (UINT)sizeof(host), 4);

    if (want == NX_NULL)
    {
        if (status == NX_SUCCESS)
            fz_fail("a PTR record about another name was accepted");
        if (host[0] != 0)
            fz_fail("a rejected PTR answer left a name behind");
        if (fz_dns.nx_dns_rr_count != 0)
            fz_fail("a rejected PTR answer was cached");
    }
    else
    {
        if (status != NX_SUCCESS)
            fz_fail("a PTR answer about the name asked did not resolve");
        if (strcmp((const char *)host, want) != 0)
            fz_fail("a PTR answer resolved to the wrong name");
    }

    /* From here a success is a cache hit. */
    fz_delivered = 1;
    memset(host, 0, sizeof(host));
    status = nx_dns_host_by_address_get(&fz_dns, IP_ADDRESS(10, 0, 0, 9),
                                        host, (UINT)sizeof(host), 4);

    if (want == NX_NULL && status == NX_SUCCESS)
        fz_fail("a rejected PTR answer was served from the cache");
    if (want != NX_NULL &&
        (status != NX_SUCCESS || strcmp((const char *)host, want) != 0))
        fz_fail("an accepted PTR answer was not cached under the question");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
    printf("  %-30s ok\n", name);
}

/* N-058.  The reverse path took the first PTR-typed record in the answer
   section whatever its owner name, returned it, and cached it. */
static void fz_ptr_owner_test(void)
{
    fz_ptr_case("ptr_owner_compressed", fzs_ptr_answer_inaddr,
                "amiga.example.com");
    fz_ptr_case("ptr_owner_plain", fzs_ptr_owner_plain, "amiga.example.com");
    fz_ptr_case("ptr_cname_2317", fzs_ptr_cname_2317, "amiga.example.com");
    fz_ptr_case("ptr_owner_mismatch_then_match",
                fzs_ptr_owner_mismatch_then_match, "amiga.example.com");
    fz_ptr_case("ptr_owner_mismatch", fzs_ptr_owner_mismatch, NX_NULL);
    fz_ptr_case("ptr_cname_wrong_owner", fzs_ptr_cname_wrong_owner, NX_NULL);
}

/* An AAAA answer for `qname`: `addr` with TTL `ttl`. */
static void fz_aaaa_wire(const char *qname, const UCHAR *addr, ULONG ttl)
{
    FzwBuf w;

    fzw_reset(&w);
    fzw_hdr(&w, 0, FZW_QR | FZW_AA, 1, 1, 0, 0);
    fzw_question(&w, qname, FZW_TYPE_AAAA, FZW_CLASS_IN);
    fzw_ptr(&w, 12);
    fzw_u16(&w, FZW_TYPE_AAAA);
    fzw_u16(&w, FZW_CLASS_IN);
    fzw_u32(&w, ttl);
    fzw_u16(&w, 16);
    fzw_raw(&w, addr, 16);

    memcpy(fz_case.b, w.b, w.len);
    fz_case.len  = w.len;
    fz_delivered = 0;
}

/* One AAAA lookup.  wire: answer `addr` off the wire, else the wire is silent
   and only the cache can answer.  Returns whether `addr` came back. */
static int fz_aaaa_lookup(const char *qname, const UCHAR *addr, ULONG ttl,
                          int wire)
{
    NX_DNS_IPV6_ADDRESS answer[2];
    UINT                count = 0;
    int                 i;

    if (wire)
        fz_aaaa_wire(qname, addr, ttl);
    else
        fz_delivered = 1;

    memset(answer, 0, sizeof(answer));
    if (nxd_dns_ipv6_address_by_name_get(&fz_dns, (UCHAR *)qname, answer,
                                         (UINT)sizeof(answer), &count, 4) !=
        NX_SUCCESS || count != 1)
        return 0;

    for (i = 0; i < 4; i++)
    {
        ULONG want = ((ULONG)addr[4 * i] << 24) | ((ULONG)addr[4 * i + 1] << 16) |
                     ((ULONG)addr[4 * i + 2] << 8) | (ULONG)addr[4 * i + 3];

        if (answer[0].ipv6_address[i] != want)
            return 0;
    }

    return 1;
}

#define FZ_AAAA_OTHER   "other.example.com"

/* NX_IP_PERIODIC_RATE ticks a second; a TTL of 2 is gone after this. */
#define FZ_AAAA_EXPIRE  (3 * NX_IP_PERIODIC_RATE)

/*
 * N-057.  _nx_dns_cache_delete_string() re-derived an entry's size from its
 * bytes.  The AAAA address is 16 raw bytes, so a zero octet cut it short: the
 * wrong word was decremented as the use count, the slot was never freed, and
 * the string count and byte total drifted.  Each address here is cached with
 * a two-second TTL next to a long-lived neighbour, expired, and looked for.
 */
static void fz_cache_aaaa_case(const char *name, const UCHAR *addr)
{
    static const UCHAR other[16] = { 0x20, 0x01, 0x0d, 0xb8, 0x11, 0x22, 0x33,
                                     0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
                                     0xbb, 0xcc };
    static UCHAR snapshot[sizeof(fz_cache)];
    ALIGN_TYPE  *head_word = (ALIGN_TYPE *)fz_cache;
    ALIGN_TYPE  *tail_word = (ALIGN_TYPE *)((UCHAR *)fz_cache +
                                            sizeof(fz_cache)) - 1;
    ALIGN_TYPE   head0;
    ALIGN_TYPE   tail0;
    UINT         rr0, count0, bytes0;

    /* A: the neighbour first, the address under test at the tail below it. */
    fz_contract_start(name);

    if (!fz_aaaa_lookup(FZ_AAAA_OTHER, other, 300, 1))
        fz_fail("the neighbour AAAA did not resolve");

    head0  = *head_word;
    tail0  = *tail_word;
    rr0    = fz_dns.nx_dns_rr_count;
    count0 = fz_dns.nx_dns_string_count;
    bytes0 = fz_dns.nx_dns_string_bytes;
    memcpy(snapshot, fz_cache, sizeof(fz_cache));

    if (!fz_aaaa_lookup(FZ_QNAME, addr, 2, 1))
        fz_fail("the AAAA under test did not resolve");
    if (!fz_aaaa_lookup(FZ_QNAME, addr, 2, 0))
        fz_fail("the AAAA under test was not cached, so this proves nothing");

    fz_ticks += FZ_AAAA_EXPIRE;
    if (fz_aaaa_lookup(FZ_QNAME, addr, 2, 0))
        fz_fail("the AAAA under test did not expire");

    if (fz_dns.nx_dns_rr_count != rr0)
        fz_fail("the expired AAAA record was not deleted");
    if (fz_dns.nx_dns_string_count != count0 ||
        fz_dns.nx_dns_string_bytes != bytes0)
        fz_fail("the string count and bytes did not return to their prior "
                "values: the AAAA slot was not freed");
    if (*head_word != head0 || *tail_word != tail0)
        fz_fail("head and tail did not return to their prior values");

    /* The records sit above the head and the strings from the tail up; both
       regions are back to what they were, byte for byte. */
    if (memcmp((UCHAR *)fz_cache, snapshot, (size_t)((UCHAR *)head0 -
                                                     (UCHAR *)fz_cache)) != 0 ||
        memcmp((UCHAR *)tail0, snapshot + ((UCHAR *)tail0 - (UCHAR *)fz_cache),
               (size_t)((UCHAR *)fz_cache + sizeof(fz_cache) -
                        (UCHAR *)tail0)) != 0)
        fz_fail("the deletion changed a byte of the cache outside its slot");

    if (!fz_aaaa_lookup(FZ_AAAA_OTHER, other, 300, 0))
        fz_fail("the neighbour was lost from the cache");

    /* Reuse: the same address again, cached and found. */
    if (!fz_aaaa_lookup(FZ_QNAME, addr, 300, 1) ||
        !fz_aaaa_lookup(FZ_QNAME, addr, 300, 0))
        fz_fail("the cache could not take the address again");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);

    /* B: the address under test first, so its slot is not the tail when it
       goes; the tail walk crosses it when the neighbour expires after it. */
    fz_contract_start(name);

    if (!fz_aaaa_lookup(FZ_QNAME, addr, 2, 1) ||
        !fz_aaaa_lookup(FZ_AAAA_OTHER, other, 5, 1))
        fz_fail("the two AAAA answers did not resolve");

    fz_ticks += FZ_AAAA_EXPIRE;
    if (fz_aaaa_lookup(FZ_QNAME, addr, 2, 0))
        fz_fail("the AAAA under test did not expire");
    if (fz_dns.nx_dns_string_count != count0 ||
        fz_dns.nx_dns_string_bytes != bytes0)
        fz_fail("the string count and bytes did not drop by the AAAA "
                "entry's own size");

    fz_ticks += FZ_AAAA_EXPIRE;
    if (fz_aaaa_lookup(FZ_AAAA_OTHER, other, 5, 0))
        fz_fail("the neighbour did not expire");
    if (fz_dns.nx_dns_rr_count != 0 || fz_dns.nx_dns_string_count != 0 ||
        fz_dns.nx_dns_string_bytes != 0)
        fz_fail("an emptied cache still counts strings");
    if (*head_word != (ALIGN_TYPE)(head_word + 1) ||
        *tail_word != (ALIGN_TYPE)tail_word)
        fz_fail("an emptied cache did not return to its initial head and tail");

    if (!fz_aaaa_lookup(FZ_QNAME, addr, 300, 1) ||
        !fz_aaaa_lookup(FZ_QNAME, addr, 300, 0))
        fz_fail("the emptied cache could not take the address again");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
    printf("  %-30s ok\n", name);
}

/*
 * The AAAA address is shared with a stored entry only when every byte is the
 * same.  _nx_dns_cache_add_string() compared with _nx_dns_name_match(), which
 * folds case and stops at a zero byte, so an address differing from a cached
 * one only in the 0x20 bit of a letter-range byte was answered with the
 * cached one.  Domain names still share a slot whatever their case.
 */
static void fz_aaaa_pair(const char *name, const UCHAR *a, const UCHAR *b)
{
    fz_contract_start(name);

    if (!fz_aaaa_lookup("one.example.com", a, 300, 1) ||
        !fz_aaaa_lookup("two.example.com", b, 300, 1))
        fz_fail("the two AAAA answers did not resolve");

    /* Two names, two addresses. */
    if (fz_dns.nx_dns_string_count != 4)
        fz_fail("two distinct AAAA addresses were stored as one entry");

    if (!fz_aaaa_lookup("one.example.com", a, 300, 0))
        fz_fail("the first address did not come back from the cache");
    if (!fz_aaaa_lookup("two.example.com", b, 300, 0))
        fz_fail("the second address came back from the cache as the first");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
    printf("  %-30s ok\n", name);
}

static void fz_cache_aaaa_identity_test(void)
{
    static const UCHAR upper[16] = { 0x20, 0x01, 0x0d, 0xb8, 0x41, 0x42, 0x43,
                                     0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
                                     0x4b, 0x4c };
    static const UCHAR lower[16] = { 0x20, 0x01, 0x0d, 0xb8, 0x61, 0x42, 0x43,
                                     0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
                                     0x4b, 0x4c };
    static const UCHAR zero_a[16] = { 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x11, 0x22,
                                      0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                                      0xaa, 0x01 };
    static const UCHAR zero_b[16] = { 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x11, 0x22,
                                      0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                                      0xaa, 0x02 };
    FzwBuf w;
    ULONG  address = 0;
    UINT   count;

    fz_aaaa_pair("aaaa_case_bit", upper, lower);
    fz_aaaa_pair("aaaa_after_zero", zero_a, zero_b);

    /* The same name in two cases, an A record and an AAAA record: one name
       entry between them, as before. */
    fz_contract_start("name_case_dedup");

    fzw_reset(&w);
    fzs_a_answer(&w, FZ_QNAME);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len  = w.len;
    fz_delivered = 0;
    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)FZ_QNAME, &address, 4) !=
        NX_SUCCESS)
        fz_fail("the A answer did not resolve");
    count = fz_dns.nx_dns_string_count;

    if (!fz_aaaa_lookup("TEST.EXAMPLE.COM", upper, 300, 1))
        fz_fail("the AAAA answer in capitals did not resolve");
    if (fz_dns.nx_dns_string_count != count + 1)
        fz_fail("a name differing only in case was not shared");

    if (!fz_aaaa_lookup(FZ_QNAME, upper, 300, 0))
        fz_fail("the AAAA record was not found under the lower-case name");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
    printf("  %-30s ok\n", "name_case_dedup");
}

/* ------------------------------------------------- AAAA slot layout ---- */

/* The string table, as _nx_dns_cache_add_string() lays a slot out: the bytes,
   then CNT and LEN in the slot's last four bytes, LEN = (size & ~3) + 8. */
static USHORT fz_slot_len(const UCHAR *start, UINT size)
{
    return *(const USHORT *)(start + ((size & ~3U) + 8) - 2);
}

static USHORT fz_slot_cnt(const UCHAR *start, UINT size)
{
    return *(const USHORT *)(start + ((size & ~3U) + 8) - 4);
}

/* The cached record of `type`, the only one there is. */
static NX_DNS_RR *fz_rr_find(USHORT type)
{
    ALIGN_TYPE head_word;
    NX_DNS_RR *rr   = (NX_DNS_RR *)((ALIGN_TYPE *)fz_cache + 1);
    NX_DNS_RR *head;
    NX_DNS_RR *hit  = NX_NULL;

    memcpy(&head_word, fz_cache, sizeof(head_word));
    head = (NX_DNS_RR *)head_word;

    for (; rr < head; rr++)
    {
        if (rr -> nx_dns_rr_type != type)
            continue;
        if (hit != NX_NULL)
            fz_fail("more than one cached record of a type expected once");
        hit = rr;
    }

    if (hit == NX_NULL)
        fz_fail("an expected record is not in the cache");

    return hit;
}

/* The address as the AAAA path stores it: four host-order ULONGs. */
static void fz_aaaa_stored(const UCHAR *addr, ULONG out[4])
{
    int i;

    for (i = 0; i < 4; i++)
        out[i] = ((ULONG)addr[4 * i] << 24) | ((ULONG)addr[4 * i + 1] << 16) |
                 ((ULONG)addr[4 * i + 2] << 8) | (ULONG)addr[4 * i + 3];
}

/* The cache is back to what _nx_dns_cache_initialize() made it. */
static void fz_cache_is_empty(const char *when)
{
    ALIGN_TYPE *head_word = (ALIGN_TYPE *)fz_cache;
    ALIGN_TYPE *tail_word = (ALIGN_TYPE *)((UCHAR *)fz_cache +
                                           sizeof(fz_cache)) - 1;

    if (fz_dns.nx_dns_rr_count != 0 || fz_dns.nx_dns_string_count != 0 ||
        fz_dns.nx_dns_string_bytes != 0 ||
        *head_word != (ALIGN_TYPE)(head_word + 1) ||
        *tail_word != (ALIGN_TYPE)tail_word)
    {
        printf("fuzz_dns: after %s\n", when);
        fz_fail("the cache is not back to its initial head, tail and counts");
    }
}

#define FZ_HOLE_NAME    "hole.example.com"

/*
 * One address: cached, its slot read back field by field, expired, cached
 * again and dropped by a server removal.  The name is FZ_HOLE_NAME (16
 * characters, LEN 24, the same slot size as the address).
 */
static void fz_aaaa_hole_case(const char *name, const UCHAR *addr)
{
    ALIGN_TYPE *head_word = (ALIGN_TYPE *)fz_cache;
    ALIGN_TYPE *tail_word = (ALIGN_TYPE *)((UCHAR *)fz_cache +
                                           sizeof(fz_cache)) - 1;
    NX_DNS_RR  *rr;
    UCHAR      *a;
    UCHAR      *n;
    ULONG       want[4];

    fz_contract_start(name);
    fz_aaaa_stored(addr, want);

    if (!fz_aaaa_lookup(FZ_HOLE_NAME, addr, 2, 1))
        fz_fail("the AAAA answer did not resolve");

    rr = fz_rr_find(NX_DNS_RR_TYPE_AAAA);
    a  = (UCHAR *)rr -> nx_dns_rr_rdata.nx_dns_rr_rdata_aaaa.nx_dns_rr_aaaa_address;
    n  = rr -> nx_dns_rr_name;

    if (memcmp(a, want, 16) != 0)
        fz_fail("the stored address is not the address");
    if (fz_slot_len(a, 16) != 24 || fz_slot_cnt(a, 16) != 1)
        fz_fail("the address slot's LEN/CNT are not 24/1");
    if (strcmp((const char *)n, FZ_HOLE_NAME) != 0 ||
        fz_slot_len(n, 16) != 24 || fz_slot_cnt(n, 16) != 1)
        fz_fail("the name slot is not the name with LEN/CNT 24/1");

    /* The name went in first, at the top; the address under it, at the tail. */
    if (n + 24 != (UCHAR *)tail_word || a + 24 != n ||
        *tail_word != (ALIGN_TYPE)a)
        fz_fail("the two slots are not where the tail says");
    if (*head_word != (ALIGN_TYPE)((NX_DNS_RR *)(head_word + 1) + 1))
        fz_fail("head is not one record past the start");
    if (fz_dns.nx_dns_string_count != 2 || fz_dns.nx_dns_string_bytes != 48)
        fz_fail("the string count and bytes are not 2 and 48");

    if (!fz_aaaa_lookup(FZ_HOLE_NAME, addr, 2, 0))
        fz_fail("the address did not come back from the cache");

    fz_ticks += FZ_AAAA_EXPIRE;
    if (fz_aaaa_lookup(FZ_HOLE_NAME, addr, 2, 0))
        fz_fail("the record did not expire");
    fz_cache_is_empty("expiry");

    if (!fz_aaaa_lookup(FZ_HOLE_NAME, addr, 300, 1) ||
        !fz_aaaa_lookup(FZ_HOLE_NAME, addr, 300, 0))
        fz_fail("the address could not be cached again");

    if (nx_dns_server_remove(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("nx_dns_server_remove failed");
    fz_cache_is_empty("a server removal");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
    printf("  %-30s ok\n", name);
}

/*
 * An address whose sixteen bytes spell a sixteen-character name, cached with
 * that name.  The two are different kinds of entry and must not share a slot
 * in either order, nor when the name is spelt in another case.
 */
static void fz_aaaa_name_kind_case(const char *name, const char *qname,
                                   int address_first)
{
    static const char  spelt[] = "abcdefgh.ijklmno";
    UCHAR       text[16];
    FzwBuf      w;
    ULONG       address = 0;
    NX_DNS_RR  *rr_a;
    NX_DNS_RR  *rr_aaaa;
    UCHAR      *bin;

    int         i;

    /* The AAAA path stores the address as four host-order ULONGs, so the wire
       bytes are chosen to make the STORED bytes spell the name on either
       byte order. */
    for (i = 0; i < 4; i++)
    {
        ULONG v;

        memcpy(&v, spelt + 4 * i, 4);
        text[4 * i]     = (UCHAR)(v >> 24);
        text[4 * i + 1] = (UCHAR)(v >> 16);
        text[4 * i + 2] = (UCHAR)(v >> 8);
        text[4 * i + 3] = (UCHAR)v;
    }

    fz_contract_start(name);

    if (address_first && !fz_aaaa_lookup("other.example.com", text, 300, 1))
        fz_fail("the AAAA answer did not resolve");

    fzw_reset(&w);
    fzs_a_answer(&w, qname);
    memcpy(fz_case.b, w.b, w.len);
    fz_case.len  = w.len;
    fz_delivered = 0;
    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)qname, &address, 4) !=
        NX_SUCCESS)
        fz_fail("the A answer did not resolve");

    if (!address_first && !fz_aaaa_lookup("other.example.com", text, 300, 1))
        fz_fail("the AAAA answer did not resolve");

    rr_a    = fz_rr_find(NX_DNS_RR_TYPE_A);
    rr_aaaa = fz_rr_find(NX_DNS_RR_TYPE_AAAA);
    bin     = (UCHAR *)rr_aaaa -> nx_dns_rr_rdata.nx_dns_rr_rdata_aaaa.nx_dns_rr_aaaa_address;

    if (memcmp(bin, spelt, 16) != 0)
        fz_fail("the stored address does not spell the name, so this proves "
                "nothing");
    if (bin == rr_a -> nx_dns_rr_name)
        fz_fail("a binary address and a name share one slot");
    if (strcmp((const char *)rr_a -> nx_dns_rr_name, qname) != 0)
        fz_fail("the A record's name is not the name asked");
    if (fz_slot_cnt(bin, 16) != 1 || fz_slot_cnt(rr_a -> nx_dns_rr_name, 16) != 1)
        fz_fail("the address or the name slot is counted twice");
    if (fz_dns.nx_dns_string_count != 3)
        fz_fail("the name, the other name and the address are not 3 strings");

    /* Both still answer from the cache, each with its own bytes. */
    fz_delivered = 1;
    address      = 0;
    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)qname, &address, 4) !=
        NX_SUCCESS || address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("the A record did not come back from the cache");
    if (!fz_aaaa_lookup("other.example.com", text, 300, 0))
        fz_fail("the address did not come back from the cache");

    if (nx_dns_server_remove(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("nx_dns_server_remove failed");
    fz_cache_is_empty("a server removal");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
    printf("  %-30s ok\n", name);
}

/* Live slots walked down from the top by LEN, as add_string walks them: their
   number and total LEN have to be what the counters say. */
static void fz_cache_conserved(const char *when)
{
    UCHAR *top  = (UCHAR *)((ALIGN_TYPE *)((UCHAR *)fz_cache +
                                           sizeof(fz_cache)) - 1);
    UCHAR *tail = (UCHAR *)*(ALIGN_TYPE *)top;
    UCHAR *p;
    UINT   live = 0, bytes = 0;
    USHORT len;

    for (p = top; p > tail; p -= len)
    {
        len = *(USHORT *)(p - 2);
        if (len < 8 || len > (UINT)(p - tail))
            fz_fail("a slot's LEN does not fit the string table");
        if (*(USHORT *)(p - 4) != 0)
        {
            live++;
            bytes += len;
        }
    }

    if (p != tail || live != fz_dns.nx_dns_string_count ||
        bytes != fz_dns.nx_dns_string_bytes)
    {
        printf("fuzz_dns: after %s: %u live slots, %u bytes; counters %u, "
               "%u\n", when, live, bytes, (unsigned)fz_dns.nx_dns_string_count,
               (unsigned)fz_dns.nx_dns_string_bytes);
        fz_fail("the live slots do not add up to string_count/string_bytes");
    }
}

#define FZ_INNER_X      "xxxx.example.com"      /* 16: LEN 24 */
#define FZ_INNER_Y      "yyyy.example.com"
#define FZ_INNER_Z      "zzzz.example.com"

static void fz_a_wire_lookup(const char *qname, int wire)
{
    FzwBuf w;
    ULONG  address = 0;

    if (wire)
    {
        fzw_reset(&w);
        fzs_a_answer(&w, qname);
        memcpy(fz_case.b, w.b, w.len);
        fz_case.len  = w.len;
        fz_delivered = 0;
    }
    else
        fz_delivered = 1;

    if (nx_dns_host_by_name_get(&fz_dns, (UCHAR *)qname, &address, 4) !=
        NX_SUCCESS || address != IP_ADDRESS(10, 0, 0, 9))
        fz_fail("an A lookup did not resolve to its address");
}

/*
 * A FREED INTERIOR slot.  X's name at the top (shared by its A and AAAA
 * records), the AAAA :: below it, Y's name at the tail.  Only the :: record
 * expires, so its slot is freed with live slots on both sides.  Then either
 * a name of the same LEN or :: again goes in first.
 */
static void fz_aaaa_interior_case(const char *name, int name_first)
{
    static const UCHAR zero[16] = { 0 };
    static UCHAR       before[sizeof(fz_cache)];
    ALIGN_TYPE        *head_word = (ALIGN_TYPE *)fz_cache;
    ALIGN_TYPE        *tail_word = (ALIGN_TYPE *)((UCHAR *)fz_cache +
                                                  sizeof(fz_cache)) - 1;
    ALIGN_TYPE         head0, tail0;
    NX_DNS_RR         *rr;
    UCHAR             *xs, *ys, *zs, *slot, *again;
    UINT               count0, bytes0;
    int                i;

    fz_contract_start(name);

    fz_a_wire_lookup(FZ_INNER_X, 1);
    if (!fz_aaaa_lookup(FZ_INNER_X, zero, 2, 1))
        fz_fail("the AAAA :: answer did not resolve");
    fz_a_wire_lookup(FZ_INNER_Y, 1);

    rr   = fz_rr_find(NX_DNS_RR_TYPE_AAAA);
    slot = (UCHAR *)rr -> nx_dns_rr_rdata.nx_dns_rr_rdata_aaaa.nx_dns_rr_aaaa_address;
    xs   = rr -> nx_dns_rr_name;
    ys   = (UCHAR *)*tail_word;

    if (strcmp((const char *)xs, FZ_INNER_X) != 0 ||
        strcmp((const char *)ys, FZ_INNER_Y) != 0 ||
        xs + 24 != (UCHAR *)tail_word || slot + 24 != xs || ys + 24 != slot)
        fz_fail("the slots are not X, ::, Y from the top down");
    if (fz_slot_cnt(xs, 16) != 2 || fz_slot_cnt(slot, 16) != 1 ||
        fz_slot_cnt(ys, 16) != 1)
        fz_fail("CNT is not 2/1/1 for X, ::, Y");
    fz_cache_conserved("caching X, ::, Y");

    head0  = *head_word;
    tail0  = *tail_word;
    count0 = fz_dns.nx_dns_string_count;
    bytes0 = fz_dns.nx_dns_string_bytes;
    memcpy(before, fz_cache, sizeof(fz_cache));

    /* Only :: expires: its record is the middle one of three, too. */
    fz_ticks += FZ_AAAA_EXPIRE;
    if (fz_aaaa_lookup(FZ_INNER_X, zero, 2, 0))
        fz_fail("the AAAA :: record did not expire");

    for (i = 0; i < 22; i++)
        if (slot[i] != 0)
            fz_fail("the freed interior slot's bytes or CNT are not zero");
    if (fz_slot_len(slot, 16) != 24)
        fz_fail("the freed interior slot lost its LEN");
    if (*head_word != head0 || *tail_word != tail0)
        fz_fail("freeing an interior slot moved head or tail");
    if (fz_dns.nx_dns_string_count != count0 - 1 ||
        fz_dns.nx_dns_string_bytes != bytes0 - 24)
        fz_fail("string_count/bytes did not drop by exactly one slot");
    if (fz_slot_cnt(xs, 16) != 1)
        fz_fail("X's name did not give back the AAAA record's reference");
    if (memcmp(xs, before + (xs - (UCHAR *)fz_cache), 20) != 0 ||
        memcmp(ys, before + (ys - (UCHAR *)fz_cache), 24) != 0)
        fz_fail("a retained slot's bytes changed");
    fz_cache_conserved("freeing ::");

    fz_a_wire_lookup(FZ_INNER_X, 0);
    fz_a_wire_lookup(FZ_INNER_Y, 0);

    if (name_first)
    {
        /* A name of the same LEN takes the freed slot exactly; :: then goes
           on at the tail, and is not shared with it. */
        fz_a_wire_lookup(FZ_INNER_Z, 1);
        zs = NX_NULL;
        for (rr = (NX_DNS_RR *)(head_word + 1);
             rr < (NX_DNS_RR *)*head_word; rr++)
            if (rr -> nx_dns_rr_type == NX_DNS_RR_TYPE_A &&
                strcmp((const char *)rr -> nx_dns_rr_name, FZ_INNER_Z) == 0)
                zs = rr -> nx_dns_rr_name;
        if (zs != slot || zs == NX_NULL ||
            fz_slot_len(zs, 16) != 24 || fz_slot_cnt(zs, 16) != 1)
            fz_fail("the same-LEN name did not take the freed slot exactly");

        if (!fz_aaaa_lookup(FZ_INNER_X, zero, 300, 1))
            fz_fail("the AAAA :: did not resolve again");
        again = (UCHAR *)fz_rr_find(NX_DNS_RR_TYPE_AAAA) ->
                    nx_dns_rr_rdata.nx_dns_rr_rdata_aaaa.nx_dns_rr_aaaa_address;
        if (again == zs || again != ys - 24 || *tail_word != (ALIGN_TYPE)again)
            fz_fail(":: did not go on at the tail, apart from the name");
    }
    else
    {
        /* :: takes its own freed slot back; the name then goes on at the
           tail, and is not shared with it. */
        if (!fz_aaaa_lookup(FZ_INNER_X, zero, 300, 1))
            fz_fail("the AAAA :: did not resolve again");
        again = (UCHAR *)fz_rr_find(NX_DNS_RR_TYPE_AAAA) ->
                    nx_dns_rr_rdata.nx_dns_rr_rdata_aaaa.nx_dns_rr_aaaa_address;
        if (again != slot || fz_slot_cnt(again, 16) != 1 ||
            *tail_word != tail0)
            fz_fail(":: did not take its freed slot back exactly");

        fz_a_wire_lookup(FZ_INNER_Z, 1);
        zs = NX_NULL;
        for (rr = (NX_DNS_RR *)(head_word + 1);
             rr < (NX_DNS_RR *)*head_word; rr++)
            if (rr -> nx_dns_rr_type == NX_DNS_RR_TYPE_A &&
                strcmp((const char *)rr -> nx_dns_rr_name, FZ_INNER_Z) == 0)
                zs = rr -> nx_dns_rr_name;
        if (zs == NX_NULL || zs == again || zs != ys - 24 ||
            *tail_word != (ALIGN_TYPE)zs)
            fz_fail("the same-LEN name did not go on at the tail, apart");
    }

    if (memcmp(again, zero, 16) != 0)
        fz_fail("the re-added :: is not all zero");
    fz_cache_conserved("re-adding");

    /* Everything still answers byte-exactly, from the cache alone. */
    fz_a_wire_lookup(FZ_INNER_X, 0);
    fz_a_wire_lookup(FZ_INNER_Y, 0);
    fz_a_wire_lookup(FZ_INNER_Z, 0);
    if (!fz_aaaa_lookup(FZ_INNER_X, zero, 300, 0))
        fz_fail("the re-added :: did not come back from the cache");

    if (nx_dns_server_remove(&fz_dns, FZ_SERVER) != NX_SUCCESS)
        fz_fail("nx_dns_server_remove failed");
    fz_cache_is_empty("a server removal");

    fz_pool_check();
    (VOID)nx_dns_delete(&fz_dns);
    printf("  %-30s ok\n", name);
}

static void fz_cache_aaaa_holes_test(void)
{
    static const struct
    {
        const char *name;
        UCHAR       addr[16];
    } cases[] =
    {
        { "hole_all_zero",     { 0 } },
        { "hole_2001_db8__1",  { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 1 } },
        { "hole___1",          { 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 1 } },
        { "hole_fe80__",       { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0 } },
        { "hole_middle",       { 0x20, 0x01, 0x0d, 0xb8, 0x11, 0x22, 0, 0,
                                 0, 0, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 } },
        { "hole_printable",    { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                                 '.', 'i', 'j', 'k', 'l', 'm', 'n', 'o' } }
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        fz_aaaa_hole_case(cases[i].name, cases[i].addr);

    fz_aaaa_name_kind_case("kind_name_then_address", "abcdefgh.ijklmno", 0);
    fz_aaaa_name_kind_case("kind_address_then_name", "abcdefgh.ijklmno", 1);
    fz_aaaa_name_kind_case("kind_address_then_upper", "ABCDEFGH.IJKLMNO", 1);

    fz_aaaa_interior_case("interior_free_then_aaaa", 0);
    fz_aaaa_interior_case("interior_free_then_name", 1);
}

static void fz_cache_aaaa_test(void)
{
    static const struct
    {
        const char *name;
        UCHAR       addr[16];
    } cases[] =
    {
        { "aaaa_zero_free",  { 0x20, 0x01, 0x0d, 0xb8, 0x01, 0x02, 0x03, 0x04,
                               0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c } },
        { "aaaa_zero_at_0",  { 0x00, 0x01, 0x0d, 0xb8, 0x01, 0x02, 0x03, 0x04,
                               0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c } },
        { "aaaa_zero_at_5",  { 0x20, 0x01, 0x0d, 0xb8, 0x01, 0x00, 0x03, 0x04,
                               0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c } },
        { "aaaa_zero_at_10", { 0x20, 0x01, 0x0d, 0xb8, 0x01, 0x02, 0x03, 0x04,
                               0x05, 0x06, 0x00, 0x08, 0x09, 0x0a, 0x0b, 0x0c } },
        { "aaaa_zero_at_15", { 0x20, 0x01, 0x0d, 0xb8, 0x01, 0x02, 0x03, 0x04,
                               0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x00 } },
        { "aaaa_loopback",   { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } },
        { "aaaa_v4_mapped",  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff,
                               10, 0, 0, 9 } }
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        fz_cache_aaaa_case(cases[i].name, cases[i].addr);

    fz_cache_aaaa_identity_test();

    /* In the same ctest: the count of registered tests stays put. */
    fz_cache_aaaa_holes_test();
}

static void fz_run_seed(int which, int patch_id)
{
    FzwBuf w;

    fzw_reset(&w);
    fzw_seeds[which].build(&w, FZ_QNAME);

    fz_case_name = fzw_seeds[which].name;
    fz_patch_id  = patch_id;

    fz_run_once(w.b, w.len);
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

    fz_ip.nx_ip_id = NX_IP_ID;

    if (_nx_packet_pool_create(&fz_wire_pool, "wire", FZ_WIRE_PAYLOAD,
                               (VOID *)fz_wire_area,
                               (ULONG)sizeof(fz_wire_area)) != NX_SUCCESS)
    {
        printf("fuzz_dns: the wire packet pool would not create\n");
        exit(1);
    }

    if (_nx_packet_pool_create(&fz_query_pool, "query", NX_DNS_PACKET_PAYLOAD,
                               (VOID *)fz_query_area,
                               (ULONG)sizeof(fz_query_area)) != NX_SUCCESS)
    {
        printf("fuzz_dns: the query packet pool would not create\n");
        exit(1);
    }
}


int main(int argc, char **argv)
{
    int i;

    fz_setup();
    fz_selftest();
    fz_cache_expiry_test();

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
            const char *want = argv[++i];

            /* A regression of N-056 is a loop that never returns; the test
               detects it before the loop, and this turns any other hang into
               a failure rather than a stuck ctest. */
            alarm(30);

            if (strcmp(want, "cache_drop") == 0)
                fz_cache_drop_test();
            else if (strcmp(want, "ptr_owner") == 0)
                fz_ptr_owner_test();
            else if (strcmp(want, "cache_aaaa") == 0)
                fz_cache_aaaa_test();
            else if (strcmp(want, "cache_aaaa_holes") == 0)
                fz_cache_aaaa_holes_test();
            else if (strcmp(want, "cache_aaaa_interior") == 0)
            {
                fz_aaaa_interior_case("interior_free_then_aaaa", 0);
                fz_aaaa_interior_case("interior_free_then_name", 1);
            }
            else
            {
                printf("fuzz_dns: no contract test named '%s'\n", want);
                return 2;
            }

            printf("fuzz_dns: contract test '%s', clean\n", want);
            return 0;
        }

        if (strcmp(argv[i], "-s") == 0)
        {
            int s;

            for (s = 0; s < FZW_SEED_COUNT; s++)
            {
                fz_run_seed(s, 1);
                fz_run_seed(s, 0);
                printf("  %-24s ok\n", fzw_seeds[s].name);
            }

            printf("fuzz_dns: %d seed case(s), clean\n", FZW_SEED_COUNT);
            return 0;
        }

        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            const char *want = argv[++i];
            int         s;

            for (s = 0; s < FZW_SEED_COUNT; s++)
            {
                if (strcmp(fzw_seeds[s].name, want) == 0)
                {
                    fz_run_seed(s, 1);
                    fz_run_seed(s, 0);
                    printf("fuzz_dns: seed '%s', clean\n", want);
                    return 0;
                }
            }

            printf("fuzz_dns: no seed case named '%s'\n", want);
            return 2;
        }

        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;
            FzwBuf        w;
            int           s;

            /* The seeds first, always: a mutation sweep that never runs the
               known shapes is a sweep whose coverage nobody can state. */
            for (s = 0; s < FZW_SEED_COUNT; s++)
            {
                fz_run_seed(s, 1);
                fz_run_seed(s, 0);
            }

            fzw_state = seed;

            for (n = 0; n < count; n++)
            {
                s = (int)fzw_below((unsigned)FZW_SEED_COUNT);

                fzw_reset(&w);
                fzw_seeds[s].build(&w, FZ_QNAME);
                fzw_mutate(&w);

                fz_case_name = fzw_seeds[s].name;
                fz_patch_id  = (fzw_below(8) != 0);

                fz_run_once(w.b, w.len);
            }

            printf("fuzz_dns: %d seed(s) + %lu mutation(s) from seed %lu, "
                   "%lu datagram(s) parsed, clean\n", FZW_SEED_COUNT, count,
                   seed, fz_cases);
            return 0;
        }
    }

    {
        static unsigned char buf[FZW_MAX];
        size_t               len = fread(buf, 1, sizeof(buf), stdin);

        fz_patch_id = 1;
        fz_run_once(buf, len);
        fz_patch_id = 0;
        fz_run_once(buf, len);
    }

    printf("fuzz_dns: one datagram, clean\n");

    return 0;
}
